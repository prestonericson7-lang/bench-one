/* ===========================================================================================
 *  psram_bank.c -- see psram_bank.h
 * ===========================================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "psram_bank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

/* ESP-PSRAM64H command set, SPI (not QPI) mode. */
#define PSRAM_CMD_READ      0x03u      /* 0 wait cycles, max 33 MHz                          */
#define PSRAM_CMD_FASTREAD  0x0Bu      /* 8 wait cycles, max 133 MHz                         */
#define PSRAM_CMD_WRITE     0x02u
#define PSRAM_CMD_RESET_EN  0x66u
#define PSRAM_CMD_RESET     0x99u
#define PSRAM_CMD_READ_ID   0x9Fu

/* -------------------------------------------------------------------------------------------
 * sysfs GPIO
 * ----------------------------------------------------------------------------------------- */
static int gpio_export(int n)
{
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", n);
    if (access(p, F_OK) == 0) return 0;                 /* already exported */

    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    char b[16];
    const int k = snprintf(b, sizeof(b), "%d", n);
    const ssize_t w = write(fd, b, (size_t)k);
    close(fd);
    /* EBUSY means someone already exported it, which is fine. */
    return (w == k || errno == EBUSY) ? 0 : -1;
}

static int gpio_out(int n)
{
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", n);
    int fd = open(p, O_WRONLY);
    if (fd < 0) return -1;
    const ssize_t w = write(fd, "out", 3);
    close(fd);
    return (w == 3) ? 0 : -1;
}

static int gpio_open_value(int n)
{
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", n);
    return open(p, O_WRONLY);
}

static inline void gpio_set(int fd, int v)
{
    if (fd >= 0) { const char c = v ? '1' : '0'; if (write(fd, &c, 1) != 1) { /* best effort */ } }
}

/* A settling delay after changing the address lines. The 74HC138 at 3.3 V is slow -- onsemi's
 * only guaranteed figure at this supply is the 2 V column, 195 ns enable-to-output at 85 C.
 * A microsecond is far past that and costs nothing, because this happens once per BANK. */
/* A short busy wait, NOT nanosleep.
 *
 * The first version of this called nanosleep(0, 1000). Linux does not sleep for one
 * microsecond; it rounds the request up to a timer tick and returns after roughly fifty to
 * sixty. That ran once per chunk, and at 25 MHz a chunk is twelve bytes, so reading one 1 KB
 * hypervector cost 86 sleeps -- about five milliseconds, all of it waiting. A microSD card
 * would have been faster, which would have made the whole stick pointless.
 *
 * What is actually being waited for is 74HC138 propagation, about 20 ns. A spin is the right
 * tool at that scale, and it needs no syscall. */
static void settle_ns(void)
{
    for (volatile int i = 0; i < 64; i++) { }
}

/* For the genuinely long waits -- udev creating a sysfs node, a chip's power-on reset. Here the
 * rounding does not matter because the wait is already thousands of times the tick. */
static void settle_us(long us)
{
    struct timespec t = { 0, us * 1000L };
    nanosleep(&t, NULL);
}

/* -------------------------------------------------------------------------------------------
 * OPEN / CLOSE
 * ----------------------------------------------------------------------------------------- */
int pb_open(psram_bank_t *b, const char *spidev, uint32_t hz, uint8_t nchips)
{
    return pb_open_ex(b, spidev, hz, nchips, 1);
}

int pb_open_ex(psram_bank_t *b, const char *spidev, uint32_t hz, uint8_t nchips, int hw_cs)
{
    memset(b, 0, sizeof(*b));
    b->hw_cs = hw_cs ? 1 : 0;
    b->gpio_a = 56; b->gpio_b = 57; b->gpio_c = 4;      /* header 16 / 17 / 18 */
    b->gpio_en0 = 47; b->gpio_en1 = 46;                  /* header 10 / 11      */
    b->fd_a = b->fd_b = b->fd_c = b->fd_en0 = b->fd_en1 = -1;
    b->spi_fd = -1;
    b->selected = -1;
    b->nchips = (nchips > PB_MAX_CHIPS) ? PB_MAX_CHIPS : nchips;
    b->spi_hz = hz;

    /* tCEM is 8 us MAX with CE# low. At `hz` bits/second, 1 bit takes 1/hz seconds, and a
     * transfer costs (4 command+address bytes + len) * 8 bits. Solve for len, then take 60%
     * as margin against scheduler jitter -- this is Linux, and a preemption in the middle of
     * a transfer extends the CE#-low window by however long the kernel feels like. */
    const uint32_t bits_in_8us = (uint32_t)((uint64_t)hz * 8u / 1000000u);
    uint32_t max = bits_in_8us / 8u;                     /* bytes that fit in one 8 us window */
    max = (max > 4u) ? (max - 4u) : 0u;                  /* minus the 4 command+address bytes */
    max = (max * 6u) / 10u;                              /* 60% margin for scheduler jitter   */
    if (max > 256u) max = 256u;                          /* keeps the ioctl buffer small      */

    /* NO FLOOR HERE. An earlier version clamped this up to 8 bytes, which quietly overrode the
     * tCEM calculation it sits next to: at 10 MHz an 8-byte payload holds CE# low for 9.6 us
     * against an 8 us limit, and at 1 MHz for 96 us. That is not a slow bus, it is silent data
     * corruption, because a PSRAM whose refresh has been blocked returns plausible wrong bytes
     * and reports nothing. Below about 5 MHz not even a single byte fits in the window, so the
     * honest answer is to refuse to open rather than to run and rot. */
    if (max < 1u) {
        fprintf(stderr,
            "psram: %u Hz is too slow to meet the 8 us tCEM limit.\n"
            "  At this clock even one payload byte holds CE# low too long, and the\n"
            "  chip's refresh stalls. Use 10 MHz or more; 25-50 MHz is intended.\n",
            (unsigned)hz);
        return -1;
    }
    b->max_chunk = max;

    {   /* Report what was actually chosen -- a 12-byte chunk at 25 MHz is correct and looks
         * alarming, so print it rather than let someone "fix" it later. */
        const double ce_us = (4.0 + (double)max) * 8.0 / (double)hz * 1e6;
        printf("psram: %u Hz, chunk %u B, CE# low %.2f us per transfer (limit 8.00)\n",
               (unsigned)hz, (unsigned)max, ce_us);
    }

    const int pins[5] = { b->gpio_a, b->gpio_b, b->gpio_c, b->gpio_en0, b->gpio_en1 };
    for (int i = 0; i < 5; i++) {
        if (gpio_export(pins[i]) != 0) {
            fprintf(stderr, "psram: cannot export gpio %d: %s\n", pins[i], strerror(errno));
            return -1;
        }
        settle_us(2000);                                 /* udev needs a moment to chmod it */
        if (gpio_out(pins[i]) != 0) {
            fprintf(stderr, "psram: cannot set gpio %d to output\n", pins[i]);
            return -1;
        }
    }
    b->fd_a   = gpio_open_value(b->gpio_a);
    b->fd_b   = gpio_open_value(b->gpio_b);
    b->fd_c   = gpio_open_value(b->gpio_c);
    b->fd_en0 = gpio_open_value(b->gpio_en0);
    b->fd_en1 = gpio_open_value(b->gpio_en1);
    if (b->fd_a < 0 || b->fd_b < 0 || b->fd_c < 0 || b->fd_en0 < 0 || b->fd_en1 < 0) {
        fprintf(stderr, "psram: cannot open gpio value files\n");
        return -1;
    }

    /* IDLE FIRST, before SPI even exists. Both decoders disabled means every CE# is high;
     * anything else and a chip could sit with CE# low from the moment power came up. */
    gpio_set(b->fd_en0, 1);
    gpio_set(b->fd_en1, 1);

    b->spi_fd = open(spidev, O_RDWR);
    if (b->spi_fd < 0) {
        fprintf(stderr, "psram: %s: %s\n", spidev, strerror(errno));
        fprintf(stderr, "  enable SPI0_M0 with luckfox-config, then check /dev/spidev0.0\n");
        return -1;
    }
    uint8_t mode = SPI_MODE_0, bits = 8;
    if (ioctl(b->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(b->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(b->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        fprintf(stderr, "psram: spidev config failed: %s\n", strerror(errno));
        return -1;
    }

    /* ONE ioctl SHOULD MOVE A WHOLE HYPERVECTOR.
     *
     * A body is HD_BYTES = 1024 and a tCEM-legal chunk at 25 MHz is 12 bytes, so 86 chunks.
     * spidev's argument size limit allows a few hundred transfers per message; 128 keeps the
     * buffers small and still gets a 1 KB read down to one or two kernel entries. */
    b->xfers = 4096u / (4u + b->max_chunk);
    if (b->xfers > 128u) b->xfers = 128u;
    if (b->xfers < 1u)   b->xfers = 1u;

    {
        const size_t stride = 4u + b->max_chunk;
        const size_t bytes  = stride * b->xfers;
        b->txbuf = (uint8_t *)malloc(bytes);
        b->rxbuf = (uint8_t *)malloc(bytes);
        b->xarr  = malloc(sizeof(struct spi_ioc_transfer) * b->xfers);
        if (!b->txbuf || !b->rxbuf || !b->xarr) {
            fprintf(stderr, "psram: out of memory for transfer buffers\n");
            return -1;
        }
        printf("psram: %s, %u chunks per ioctl, %u B of transfer buffer\n",
               b->hw_cs ? "hardware CS on G2A" : "GPIO CS (slow fallback)",
               (unsigned)b->xfers, (unsigned)(bytes * 2u));
    }
    return 0;
}

void pb_close(psram_bank_t *b)
{
    pb_select(b, -1);
    int *fds[5] = { &b->fd_a, &b->fd_b, &b->fd_c, &b->fd_en0, &b->fd_en1 };
    for (int i = 0; i < 5; i++) if (*fds[i] >= 0) { close(*fds[i]); *fds[i] = -1; }
    if (b->spi_fd >= 0) { close(b->spi_fd); b->spi_fd = -1; }
    free(b->txbuf); b->txbuf = NULL;
    free(b->rxbuf); b->rxbuf = NULL;
    free(b->xarr);  b->xarr  = NULL;
}

uint64_t pb_capacity(const psram_bank_t *b)
{
    return (uint64_t)b->nchips * PB_CHIP_BYTES;
}

/* -------------------------------------------------------------------------------------------
 * SELECT -- break before make
 * ----------------------------------------------------------------------------------------- */
int pb_select(psram_bank_t *b, int chip)
{
    /* BREAK. Both decoders off before anything moves. This is the only ordering that
     * guarantees no chip sees a select pulse it was not meant to see. */
    gpio_set(b->fd_en0, 1);
    gpio_set(b->fd_en1, 1);

    if (chip < 0) { b->selected = -1; return 0; }
    if (chip >= (int)b->nchips) return -1;

    const int local = chip & 7;
    gpio_set(b->fd_a, local & 1);
    gpio_set(b->fd_b, (local >> 1) & 1);
    gpio_set(b->fd_c, (local >> 2) & 1);

    /* Each gpio_set above is a write() to sysfs, which costs microseconds. By the time the
     * third one returns, the address lines have been stable for far longer than the decoder
     * needs. The spin is belt and braces, not the actual guarantee. */
    settle_ns();

    if (chip < 8) gpio_set(b->fd_en0, 0);
    else          gpio_set(b->fd_en1, 0);

    b->selected = chip;
    return 0;
}

/* -------------------------------------------------------------------------------------------
 * TRANSFER
 * ----------------------------------------------------------------------------------------- */
static int spi_xfer(int fd, const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t hz)
{
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof(t));
    t.tx_buf = (unsigned long)tx;
    t.rx_buf = (unsigned long)rx;
    t.len = len;
    t.speed_hz = hz;
    t.bits_per_word = 8;
    return (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) ? -1 : 0;
}

/* -------------------------------------------------------------------------------------------
 * THE BATCHED PATH -- many tCEM-legal chunks in one ioctl
 * -----------------------------------------------------------------------------------------
 * tCEM says CE# must go high between chunks. It does not say software has to be the thing that
 * raises it. With the controller's CS wired to G2A, spidev raises it: cs_change on a transfer
 * means "deassert CS after this one", and a single SPI_IOC_MESSAGE can carry a whole array of
 * transfers. So the CE# pulse train is generated by the SPI hardware, one kernel entry for the
 * lot, instead of two sysfs writes and a rounded-up sleep per twelve bytes.
 *
 * The last transfer in the array keeps cs_change = 0 on purpose. In spidev the flag inverts its
 * meaning on the final transfer: setting it there asks the controller to LEAVE CS asserted
 * after the message, which would hold one chip's CE# low indefinitely -- precisely the failure
 * this whole file exists to avoid.
 * ----------------------------------------------------------------------------------------- */
static int pb_batch(psram_bank_t *b, uint8_t cmd, uint32_t off,
                    const uint8_t *src, uint8_t *dst, uint32_t len, uint32_t nx)
{
    struct spi_ioc_transfer *x = (struct spi_ioc_transfer *)b->xarr;
    const uint32_t stride = 4u + b->max_chunk;
    uint32_t used = 0, done = 0;

    memset(x, 0, (size_t)nx * sizeof(*x));

    while (done < len && used < nx) {
        uint32_t n = len - done;
        if (n > b->max_chunk) n = b->max_chunk;

        uint8_t *tx = b->txbuf + (size_t)used * stride;
        tx[0] = cmd;
        tx[1] = (uint8_t)((off + done) >> 16);
        tx[2] = (uint8_t)((off + done) >> 8);
        tx[3] = (uint8_t)(off + done);
        if (src) memcpy(tx + 4, src + done, n);
        else     memset(tx + 4, 0, n);

        x[used].tx_buf        = (unsigned long)tx;
        x[used].rx_buf        = dst ? (unsigned long)(b->rxbuf + (size_t)used * stride) : 0ul;
        x[used].len           = 4u + n;
        x[used].speed_hz      = b->spi_hz;
        x[used].bits_per_word = 8;
        x[used].cs_change     = 1;      /* corrected below for the final transfer */
        x[used].delay_usecs   = 1;      /* CE# high long enough for the refresh to run.
                                         * tCPH needs 18 ns; this is a udelay inside the SPI
                                         * driver, so 1 us costs no syscall and buys margin. */
        used++;
        done += n;
    }
    if (!used) return -1;
    x[used - 1].cs_change   = 0;        /* see the note above -- do NOT leave CS asserted */
    x[used - 1].delay_usecs = 0;

    if (ioctl(b->spi_fd, SPI_IOC_MESSAGE(used), x) < 0) return -1;
    b->ioctls++;
    b->chunks += used;

    if (dst) {
        uint32_t o = 0;
        for (uint32_t i = 0; i < used; i++) {
            const uint32_t n = x[i].len - 4u;
            memcpy(dst + o, b->rxbuf + (size_t)i * stride + 4u, n);
            o += n;
        }
    }
    return (int)done;
}

int pb_read(psram_bank_t *b, uint64_t addr, void *dst, uint32_t len)
{
    uint8_t *out = (uint8_t *)dst;
    if (addr + len > pb_capacity(b)) return -1;

    while (len) {
        const uint8_t  chip = (uint8_t)(addr / PB_CHIP_BYTES);
        const uint32_t off  = (uint32_t)(addr % PB_CHIP_BYTES);

        uint32_t n = len;
        if (off + n > PB_CHIP_BYTES) n = PB_CHIP_BYTES - off;   /* never cross a chip */

        /* SELECT ONCE PER CHIP, not once per chunk. In hw_cs mode the decoder stays armed and
         * the controller's CS does the pulsing, so the expensive sysfs writes happen a handful
         * of times per megabyte instead of a hundred times per kilobyte. */
        if (pb_select(b, (int)chip) != 0) return -1;

        int r;
        if (b->hw_cs) {
            r = pb_batch(b, PSRAM_CMD_READ, off, NULL, out, n, b->xfers);
        } else {
            /* Fallback: G2A tied high, so CE# can only be raised by GPIO. Correct, and slow. */
            uint32_t c = n; if (c > b->max_chunk) c = b->max_chunk;
            uint8_t *tx = b->txbuf, *rx = b->rxbuf;
            tx[0] = PSRAM_CMD_READ;
            tx[1] = (uint8_t)(off >> 16); tx[2] = (uint8_t)(off >> 8); tx[3] = (uint8_t)off;
            memset(tx + 4, 0, c);
            r = (spi_xfer(b->spi_fd, tx, rx, 4u + c, b->spi_hz) == 0) ? (int)c : -1;
            if (r > 0) { memcpy(out, rx + 4, c); b->chunks++; }
            pb_select(b, -1);
        }
        if (r <= 0) { pb_select(b, -1); return -1; }

        out += r; addr += (uint32_t)r; len -= (uint32_t)r;
        b->reads += (uint32_t)r;
    }
    pb_select(b, -1);                   /* IDLE. rule 3: never leave a bank armed. */
    return 0;
}

int pb_write(psram_bank_t *b, uint64_t addr, const void *src, uint32_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    if (addr + len > pb_capacity(b)) return -1;

    while (len) {
        const uint8_t  chip = (uint8_t)(addr / PB_CHIP_BYTES);
        const uint32_t off  = (uint32_t)(addr % PB_CHIP_BYTES);

        uint32_t n = len;
        if (off + n > PB_CHIP_BYTES) n = PB_CHIP_BYTES - off;

        if (pb_select(b, (int)chip) != 0) return -1;

        int r;
        if (b->hw_cs) {
            r = pb_batch(b, PSRAM_CMD_WRITE, off, in, NULL, n, b->xfers);
        } else {
            uint32_t c = n; if (c > b->max_chunk) c = b->max_chunk;
            uint8_t *tx = b->txbuf;
            tx[0] = PSRAM_CMD_WRITE;
            tx[1] = (uint8_t)(off >> 16); tx[2] = (uint8_t)(off >> 8); tx[3] = (uint8_t)off;
            memcpy(tx + 4, in, c);
            r = (spi_xfer(b->spi_fd, tx, NULL, 4u + c, b->spi_hz) == 0) ? (int)c : -1;
            if (r > 0) b->chunks++;
            pb_select(b, -1);
        }
        if (r <= 0) { pb_select(b, -1); return -1; }

        in += r; addr += (uint32_t)r; len -= (uint32_t)r;
        b->writes += (uint32_t)r;
    }
    pb_select(b, -1);
    return 0;
}

int pb_probe(psram_bank_t *b, uint8_t chip, uint16_t *id_out)
{
    if (chip >= b->nchips) return -1;

    /* Reset first. A chip left in QPI mode by earlier code answers nothing on one wire, and
     * that presents as "the chip is dead" when it is merely in the wrong mode. */
    uint8_t c;
    if (pb_select(b, (int)chip) != 0) return -1;
    c = PSRAM_CMD_RESET_EN; spi_xfer(b->spi_fd, &c, NULL, 1, b->spi_hz);
    pb_select(b, -1);
    if (pb_select(b, (int)chip) != 0) return -1;
    c = PSRAM_CMD_RESET;    spi_xfer(b->spi_fd, &c, NULL, 1, b->spi_hz);
    pb_select(b, -1);
    settle_us(200);

    uint8_t tx[10], rx[10];
    memset(tx, 0, sizeof(tx));
    tx[0] = PSRAM_CMD_READ_ID;                  /* then 3 address bytes, then the id bytes */
    if (pb_select(b, (int)chip) != 0) return -1;
    const int r = spi_xfer(b->spi_fd, tx, rx, 8, b->spi_hz);
    pb_select(b, -1);
    if (r != 0) return -1;

    if (id_out) *id_out = (uint16_t)((rx[4] << 8) | rx[5]);   /* MFID, KGD */
    return 0;
}

int pb_memtest(psram_bank_t *b, uint8_t chip, uint32_t bytes, uint32_t *first_bad_out)
{
    if (chip >= b->nchips) return -1;
    if (bytes > PB_CHIP_BYTES) bytes = PB_CHIP_BYTES;

    const uint64_t base = (uint64_t)chip * PB_CHIP_BYTES;
    static uint8_t wbuf[256], rbuf[256];
    uint32_t seed = 0xA5A50000u ^ chip;

    for (uint32_t off = 0; off < bytes; off += sizeof(wbuf)) {
        uint32_t n = (bytes - off > sizeof(wbuf)) ? (uint32_t)sizeof(wbuf) : (bytes - off);
        uint32_t s = seed + off;
        for (uint32_t i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            wbuf[i] = (uint8_t)(s >> 24);
        }
        if (pb_write(b, base + off, wbuf, n) != 0) return -1;
        memset(rbuf, 0, n);
        if (pb_read(b, base + off, rbuf, n) != 0) return -1;
        for (uint32_t i = 0; i < n; i++) {
            if (rbuf[i] != wbuf[i]) {
                if (first_bad_out) *first_bad_out = off + i;
                return 1;                        /* mismatch: the address, not just "failed" */
            }
        }
    }
    return 0;
}
