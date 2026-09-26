/*
 * tg_teensy.cpp -- Teensy 4.1 platform layer (tg_plat.h): USB CDC, the 16-bit parallel bus of
 * SPEC section 3, microseconds, LED. Everything else is portable (tg_core.c, geom.c, gpu_setup.c).
 *
 * Bus pins (checked at compile time against the Teensy core's core_pins.h, and again at boot
 * against the core's pin table -- if either check fails the bus is never driven):
 *   D0..D15 = pins 19 18 14 15 40 41 17 16 22 23 20 21 38 39 26 27 = GPIO6 bits 16..31
 *   SOR = pin 3 (GPIO9 bit 5), STROBE = pin 2 (GPIO9 bit 4), BUSY = pin 4 (GPIO9 bit 6, INPUT_PULLUP)
 * One transfer: D[15:0] with one GPIO6_DR_TOGGLE write, SOR with GPIO9_DR_SET/CLEAR, DSB, wait
 * >= 30 ns (DWT cycle counter), toggle STROBE (GPIO9_DR_TOGGLE), DSB, wait >= 60 ns. 48 transfers
 * per record, low half first, SOR = 1 only on the first. STROBE always returns to 0 after a
 * record (48 = even number of toggles), which is the level the FPGA's pull-down gives it, so
 * switching the pins between input and output never creates a STROBE edge.
 * Output pads: DSE(1) (~150 ohm, about the impedance of a jumper wire: series-terminated, clean
 * monotonic edges at the far end), slow slew, SPEED(0). Plenty for 30/60 ns timing; change
 * BUS_PAD if the wiring is ever made on a PCB with controlled impedance.
 */
#if defined(__IMXRT1062__)

#include <Arduino.h>
#include "tg_common.h"

#define PIN_STROBE 2
#define PIN_SOR    3
#define PIN_BUSY   4
#define PIN_LED    13
#define SOR_BIT    (1u << 5)
#define STROBE_BIT (1u << 4)
#define BUSY_BIT   (1u << 6)
#define BUS_PAD    (IOMUXC_PAD_DSE(1) | IOMUXC_PAD_SPEED(0))
#define SETUP_NS   30u
#define HOLD_NS    60u

static const uint8_t data_pins[16] = {19, 18, 14, 15, 40, 41, 17, 16, 22, 23, 20, 21, 38, 39, 26, 27};

/* Compile-time check of the SPEC 3 table against this core's core_pins.h (Teensy 4.1 section). */
#if !defined(ARDUINO_TEENSY41)
#error "tg_teensy.cpp: build for Teensy 4.1 (fqbn teensy:avr:teensy41)"
#endif
static_assert(CORE_PIN19_BIT == 16 && CORE_PIN18_BIT == 17 && CORE_PIN14_BIT == 18 && CORE_PIN15_BIT == 19,
              "bus D0..D3 must be GPIO6 bits 16..19");
static_assert(CORE_PIN40_BIT == 20 && CORE_PIN41_BIT == 21 && CORE_PIN17_BIT == 22 && CORE_PIN16_BIT == 23,
              "bus D4..D7 must be GPIO6 bits 20..23");
static_assert(CORE_PIN22_BIT == 24 && CORE_PIN23_BIT == 25 && CORE_PIN20_BIT == 26 && CORE_PIN21_BIT == 27,
              "bus D8..D11 must be GPIO6 bits 24..27");
static_assert(CORE_PIN38_BIT == 28 && CORE_PIN39_BIT == 29 && CORE_PIN26_BIT == 30 && CORE_PIN27_BIT == 31,
              "bus D12..D15 must be GPIO6 bits 28..31");
static_assert(CORE_PIN2_BIT == 4 && CORE_PIN3_BIT == 5 && CORE_PIN4_BIT == 6,
              "STROBE/SOR/BUSY must be GPIO9 bits 4/5/6");

static bool     g_pins_ok;          /* boot-time pin table check passed */
static bool     g_driving;
static uint32_t g_cur_data;         /* D[15:0] currently in GPIO6_DR[31:16] */
static uint32_t g_cyc_setup, g_cyc_hold;

static inline void dsb(void) { asm volatile("dsb" ::: "memory"); }

static uint32_t ns_to_cycles(uint32_t ns)
{
    uint32_t mhz = F_CPU_ACTUAL / 1000000u;
    return (mhz * ns + 999u) / 1000u + 2u;      /* round up, +2 cycles for the loop itself */
}

/* Boot-time check: the core's pin table must agree with the SPEC 3 table (runtime proof that
 * digital pin n really is the GPIO6/GPIO9 bit we toggle). */
static bool check_pins(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t p = data_pins[i];
        if (portOutputRegister(p) != &GPIO6_DR || digitalPinToBitMask(p) != (1u << (16 + i)))
            return false;
    }
    if (portOutputRegister(PIN_SOR) != &GPIO9_DR || digitalPinToBitMask(PIN_SOR) != SOR_BIT)
        return false;
    if (portOutputRegister(PIN_STROBE) != &GPIO9_DR || digitalPinToBitMask(PIN_STROBE) != STROBE_BIT)
        return false;
    if (portOutputRegister(PIN_BUSY) != &GPIO9_DR || digitalPinToBitMask(PIN_BUSY) != BUSY_BIT)
        return false;
    return true;
}

static void pins_input(void)
{
    for (int i = 0; i < 16; i++)
        pinMode(data_pins[i], INPUT);
    pinMode(PIN_SOR, INPUT);
    pinMode(PIN_STROBE, INPUT);
}

extern "C" {

uint32_t tgp_micros(void) { return micros(); }
uint32_t tgp_cpu_mhz(void) { return F_CPU_ACTUAL / 1000000u; }

int tgp_usb_read(void *buf, uint32_t max)
{
    int n = usb_serial_available();
    if (n <= 0)
        return 0;
    if ((uint32_t)n > max)
        n = (int)max;
    n = usb_serial_read(buf, (uint32_t)n);
    return n > 0 ? n : 0;
}

void tgp_usb_write(const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {                                  /* usb_serial_write gives up after its 120 ms timeout */
        uint32_t chunk = n > 4096u ? 4096u : n;
        int w = usb_serial_write(p, chunk);
        if (w <= 0)
            return;                              /* host not listening: drop the rest */
        p += w;
        n -= (uint32_t)w;
    }
}

void tgp_usb_flush(void) { usb_serial_flush_output(); }

int tgp_bus_busy(void)
{
    return (GPIO9_PSR & BUSY_BIT) ? 1 : 0;
}

int tgp_bus_drive(int enable)
{
    if (!enable || !g_pins_ok) {                 /* a failed pin check means: never drive */
        pins_input();
        g_driving = false;
        return 0;
    }
    g_cyc_setup = ns_to_cycles(SETUP_NS);
    g_cyc_hold = ns_to_cycles(HOLD_NS);
    GPIO6_DR_CLEAR = 0xFFFF0000u;               /* D = 0, SOR = 0, STROBE = 0 before enabling */
    GPIO9_DR_CLEAR = SOR_BIT | STROBE_BIT;
    g_cur_data = 0;
    for (int i = 0; i < 16; i++) {
        pinMode(data_pins[i], OUTPUT);
        *(digital_pin_to_info_PGM[data_pins[i]].pad) = BUS_PAD;
    }
    pinMode(PIN_SOR, OUTPUT);
    *(digital_pin_to_info_PGM[PIN_SOR].pad) = BUS_PAD;
    pinMode(PIN_STROBE, OUTPUT);
    *(digital_pin_to_info_PGM[PIN_STROBE].pad) = BUS_PAD;
    g_driving = true;
    return 1;
}

int tgp_bus_send(const uint32_t rec[24])
{
    if (!g_driving)
        return -1;                               /* never happens: the core checks first */
    const uint32_t cs = g_cyc_setup, ch = g_cyc_hold;
    uint32_t prev = g_cur_data, t;
    for (int i = 0; i < 24; i++) {
        const uint32_t w = rec[i];
        for (int half = 0; half < 2; half++) {
            const uint32_t v = half ? (w >> 16) : (w & 0xFFFFu);
            GPIO6_DR_TOGGLE = ((prev ^ v) & 0xFFFFu) << 16;   /* all 16 data bits in one write */
            prev = v;
            if (i == 0) {                        /* SOR = 1 on transfer 0 only */
                if (half)
                    GPIO9_DR_CLEAR = SOR_BIT;
                else
                    GPIO9_DR_SET = SOR_BIT;
            }
            dsb();
            t = ARM_DWT_CYCCNT;
            while (ARM_DWT_CYCCNT - t < cs) {
            }
            GPIO9_DR_TOGGLE = STROBE_BIT;       /* one edge = one transfer */
            dsb();
            t = ARM_DWT_CYCCNT;
            while (ARM_DWT_CYCCNT - t < ch) {
            }
        }
    }
    g_cur_data = prev;
    return 0;
}

void tgp_led(int on)
{
    if (g_pins_ok)                               /* else teensy_gpu.ino blinks the fast error pattern */
        digitalWriteFast(PIN_LED, on ? HIGH : LOW);
}

void tgp_idle(void) {}

} /* extern "C" */

/* called from setup() in teensy_gpu.ino */
void tg_teensy_begin(void)
{
    pins_input();                                /* D/SOR/STROBE high-impedance from the start */
    pinMode(PIN_BUSY, INPUT_PULLUP);             /* unconfigured / absent FPGA reads as busy */
    pinMode(PIN_LED, OUTPUT);
    digitalWriteFast(PIN_LED, LOW);
    g_pins_ok = check_pins();
    Serial.begin(115200);                        /* USB CDC: the baud rate is ignored */
}

bool tg_teensy_pins_ok(void) { return g_pins_ok; }

#endif /* __IMXRT1062__ */
