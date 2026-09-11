/* ===========================================================================================
 *  attiny_guardian.ino -- the one processor that is not part of the machine
 * ===========================================================================================
 *
 *  WHY THIS CHIP EXISTS
 *  ---------------------
 *  An FPGA cannot reset itself. If the PL locks, or the PS hangs during boot, or a bitstream
 *  loads badly, there is nothing inside the chip able to recover it -- the thing that would do
 *  the recovering is the thing that is stuck. On a bench that means walking over and pulling
 *  the plug. On a 30-day training run at 3 a.m. it means the run is simply over.
 *
 *  So one ATtiny85 sits beside each FPGA doing nothing but watching. It shares no clock, no
 *  supply rail, no bus and no software with anything it monitors. That isolation IS the
 *  feature: a watchdog that can be taken down by the fault it is watching for is decoration.
 *
 *      PB0  <- HEARTBEAT   the FPGA's PS toggles this. Silence means trouble.
 *      PB1  -> PROG_B      pulse low to force a full bitstream reload
 *      PB2  -> SRST_B      pulse low to reset the PS only
 *      PB3  -> POWER_EN    last resort: cold cycle the whole board. LEAVE THIS UNCONNECTED
 *                          on a first build -- cutting an FPGA's supply needs a high-side load
 *                          switch sized for the board, and an undersized one fails closed or
 *                          browns the rail out slowly, which is worse than not cycling at all.
 *                          With PB3 floating the escalation still does SRST and PROG, which is
 *                          where nearly all real recoveries happen, and then latches dead.
 *      PB4  -> STATUS LED
 *      PB5     left as RESET -- see below
 *
 *
 *  THE ESCALATION, AND WHY IT IS AN ESCALATION
 *  --------------------------------------------
 *  Recovery attempts are ordered from least to most destructive, and each is given time to
 *  work before the next is tried. A watchdog that jumps straight to a power cut turns a 200 ms
 *  glitch into a 40-second reboot, and does it repeatedly.
 *
 *      miss 1-3   ->  nothing. wait. a busy PS can be late.
 *      miss 4     ->  SRST_B. the PS restarts, the bitstream survives.
 *      miss 8     ->  PROG_B. full reconfiguration.
 *      miss 12    ->  power cycle.
 *      miss 16+   ->  STOP. hold power off and flash the LED forever.
 *
 *  That last state matters more than the others. A board that fails four recoveries has a
 *  hardware fault, and a watchdog that keeps power-cycling it forever will happily destroy it
 *  overnight. Giving up and saying so is the correct behaviour.
 *
 *
 *  FUSES -- READ THIS BEFORE BURNING ANYTHING
 *  -------------------------------------------
 *    * LEAVE PB5 AS RESET. Fusing it to GPIO (RSTDISBL) gets you a sixth pin and costs you the
 *      ability to ever reprogram the chip without a high-voltage programmer. Five pins is
 *      enough for this job.
 *    * Set BODLEVEL to 2.7 V, NOT 4.3 V. This chip runs from the FPGA's 3.3 V rail so its
 *      outputs match PROG_B and SRST_B without level shifting -- and a 4.3 V brown-out
 *      threshold on a 3.3 V supply holds the AVR in reset forever. It would look exactly like
 *      a dead chip. 2.7 V still catches a genuinely sagging rail, which is the point: a
 *      browning-out AVR must not drive PROG_B randomly while everything else is failing too.
 *    * At 3.3 V the part is rated to 10 MHz, so the 8 MHz internal oscillator is in spec.
 *      Do NOT fuse it to a 16 MHz PLL clock at this voltage.
 *    * Internal 8 MHz RC is fine. Nothing here is timing critical; the watchdog oscillator is
 *      separate silicon anyway.
 *
 *  Program with a USBasp (~$3) or an Arduino as ISP. Board: "ATtiny85 @ 8 MHz internal".
 *
 *  PROGRAM IT BEFORE IT GOES IN THE CIRCUIT. ISP uses PB0, PB1, PB2 and RESET, and this sketch
 *  uses PB0, PB1 and PB2 for the heartbeat and the two reset lines. A programmer attached while
 *  those wires are connected fights the FPGA board for the same three pins. Put the AVR in a
 *  DIP-8 socket, program it on a breadboard, then seat it. You will want the socket anyway.
 *
 *
 *  THE OUTPUTS ARE OPEN-DRAIN, DELIBERATELY
 *  -----------------------------------------
 *  PROG_B and SRST_B on a Zynq are active-low lines with their own pull-ups, and JTAG or a
 *  reset button may drive them too. This never drives them HIGH -- it releases them by going
 *  to input mode. Two devices driving the same net in opposite directions is how you lose an
 *  FPGA, and it would happen the first time you plugged in a JTAG cable while this was running.
 * ===========================================================================================
 */

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#define PIN_HEARTBEAT  PB0
#define PIN_PROG_B     PB1
#define PIN_SRST_B     PB2
#define PIN_POWER_EN   PB3
#define PIN_LED        PB4

/* Heartbeat expected at least this often. The FPGA's PS toggles PB0 from a cron job or the
 * orchestrator; 8 seconds of silence is one missed tick, not a crisis. */
#define TICK_SECONDS   8

#define MISS_SRST      4
#define MISS_PROG      8
#define MISS_POWER    12
#define MISS_GIVEUP   16

static volatile uint8_t  g_beats   = 0;    /* edges seen since the last check */
static          uint8_t  g_misses  = 0;
static          uint8_t  g_last    = 0;
static          uint8_t  g_recover = 0;    /* how many recoveries attempted this session */
static          uint8_t  g_dead    = 0;

/* Pin-change interrupt: the heartbeat may arrive while we are asleep, which is most of the
 * time. Counting edges in the ISR means a single pulse anywhere in the 8-second window is
 * enough -- polling would miss a pulse narrower than the wake interval. */
ISR(PCINT0_vect)
{
    const uint8_t now = (PINB >> PIN_HEARTBEAT) & 1u;
    if (now != g_last) { g_last = now; if (g_beats < 200u) g_beats++; }
}

ISR(WDT_vect) { }                          /* wake only */

/* Release a line: input, no pull-up. The board's own pull-up takes it high. */
static inline void release(uint8_t pin)
{
    DDRB  &= (uint8_t)~(1u << pin);
    PORTB &= (uint8_t)~(1u << pin);
}
/* Assert a line low: output, driven 0. */
static inline void assert_low(uint8_t pin)
{
    PORTB &= (uint8_t)~(1u << pin);
    DDRB  |= (uint8_t)(1u << pin);
}

/* A 1 ms delay that does not need <util/delay.h>'s compile-time constant folding. */
static void _delay_ms_1(void)
{
    /* 8 MHz / 4 cycles per loop = 2000 iterations per millisecond */
    for (volatile uint16_t i = 0; i < 2000; i++) { }
}

static void pulse_low(uint8_t pin, uint16_t ms)
{
    assert_low(pin);
    while (ms--) { _delay_ms_1(); }
    release(pin);
}

static void led(uint8_t on)
{
    if (on) { DDRB |= (1u << PIN_LED); PORTB |= (1u << PIN_LED); }
    else    { PORTB &= (uint8_t)~(1u << PIN_LED); DDRB |= (1u << PIN_LED); }
}

static void wdt_sleep_8s(void)
{
    /* WDT runs from its OWN 128 kHz oscillator, so it keeps time even if the main clock
     * stops. That independence is the reason this chip is trustworthy. */
    MCUSR = 0;
    WDTCR |= (1u << WDCE) | (1u << WDE);
    WDTCR  = (1u << WDIE) | (1u << WDP3) | (1u << WDP0);   /* ~8 s, interrupt not reset */

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sei();
    sleep_cpu();
    sleep_disable();
}

int main(void)
{
    /* POWER ON FIRST, and release both reset lines, before anything else. If this chip resets
     * for its own reasons the FPGA must not be dragged down with it. */
    PORTB |= (1u << PIN_POWER_EN);
    DDRB  |= (1u << PIN_POWER_EN);
    release(PIN_PROG_B);
    release(PIN_SRST_B);
    led(0);

    DDRB  &= (uint8_t)~(1u << PIN_HEARTBEAT);      /* heartbeat is an input, no pull-up:  */
    PORTB &= (uint8_t)~(1u << PIN_HEARTBEAT);      /* the FPGA drives it push-pull        */

    GIMSK |= (1u << PCIE);
    PCMSK |= (1u << PIN_HEARTBEAT);
    g_last = (PINB >> PIN_HEARTBEAT) & 1u;

    /* Give the FPGA a full boot before expecting anything. A Zynq loading Linux from SD takes
     * tens of seconds, and a watchdog that fires during first boot never lets it finish --
     * which looks exactly like a board that cannot boot. */
    for (uint8_t i = 0; i < 8; i++) { led(i & 1u); wdt_sleep_8s(); }
    g_beats = 0;

    for (;;) {
        wdt_sleep_8s();

        if (g_dead) {                       /* fault latched: power off, flash forever */
            PORTB &= (uint8_t)~(1u << PIN_POWER_EN);
            led(1); _delay_ms_1(); led(0);
            continue;
        }

        cli();
        const uint8_t beats = g_beats;
        g_beats = 0;
        sei();

        if (beats) {
            g_misses = 0;
            led(0);
            continue;
        }

        g_misses++;
        led(1);

        if (g_misses == MISS_SRST) {
            pulse_low(PIN_SRST_B, 50);      /* PS reset. bitstream survives. cheapest fix. */
            g_recover++;
        } else if (g_misses == MISS_PROG) {
            pulse_low(PIN_PROG_B, 300);     /* full reconfiguration */
            g_recover++;
        } else if (g_misses == MISS_POWER) {
            PORTB &= (uint8_t)~(1u << PIN_POWER_EN);
            for (uint16_t i = 0; i < 3000; i++) _delay_ms_1();   /* 3 s, rails must drain */
            PORTB |= (1u << PIN_POWER_EN);
            g_recover++;
            g_misses = 0;                   /* the cold boot gets a fresh budget */
            for (uint8_t i = 0; i < 8; i++) wdt_sleep_8s();      /* and time to boot */
            g_beats = 0;
        } else if (g_misses >= MISS_GIVEUP || g_recover >= 4u) {
            /* Four failed recoveries is a hardware fault, not a glitch. Stop. Continuing to
             * power-cycle a broken board all night is how a fault becomes damage. */
            g_dead = 1;
        }
    }
}
