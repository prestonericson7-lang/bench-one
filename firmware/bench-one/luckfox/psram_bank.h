/* ===========================================================================================
 *  psram_bank.h -- 10 ESP-PSRAM64H behind two 74HC138s on one Luckfox SPI bus
 * ===========================================================================================
 *
 *  WHAT THIS DRIVES
 *  -----------------
 *      SCLK / MOSI / MISO   shared by all ten chips        (spidev, /dev/spidev0.0)
 *      CE# per chip         from a 74HC138 output, active low
 *      A / B / C            three GPIOs, shared by both decoders
 *      EN0 / EN1            one GPIO per decoder -> G2B, active low. Exactly one ever low.
 *      SPI0_CS0             the controller's own chip select -> G2A on BOTH decoders
 *
 *  A 74HC138 has two active-low enables that AND together, and using both is what makes this
 *  fast. G2B says WHICH bank of eight; G2A says WHEN. Because G2A is the real SPI chip select,
 *  the per-chunk CE# pulse that tCEM demands is produced by the SPI controller inside a single
 *  ioctl instead of by two sysfs writes per chunk. Same part count, one wire moved, about 30x
 *  the throughput. It also means an idle or unbooted Luckfox leaves CS high, so every CE# is
 *  high, with no software involved at all.
 *
 *  Ten chips, one bus, 80 MB. Capacity scales with chip count; BANDWIDTH DOES NOT --
 *  only one chip is ever active. At 1-bit SPI and 50 MHz that is 6.25 MB/s, so a full
 *  80 MB scan takes about 12.8 seconds. That is the deep tier and it is meant to be slow.
 *
 *
 *  THE THREE RULES, AND WHY EACH ONE IS NOT OPTIONAL
 *  --------------------------------------------------
 *
 *  1. tCEM -- CE# MUST NOT STAY LOW LONGER THAN 8 MICROSECONDS.
 *     PSRAM is DRAM with a refresh controller hidden inside. Holding CE# low blocks that
 *     refresh, and the chip quietly loses data. It does not report an error; it returns
 *     plausible wrong bytes. Every transfer here is chunked so no single CE# assertion can
 *     exceed the limit, and the chunk size is computed from the clock rather than guessed.
 *
 *  2. BREAK BEFORE MAKE.
 *     The three address lines do not change simultaneously. Change them while a decoder is
 *     enabled and its output walks across intermediate values -- a few nanoseconds of CE# low
 *     on a chip nobody asked for, which is more than enough to start a transaction on it.
 *     So: disable both decoders, set the address, let it settle, enable one. Always.
 *
 *  3. IDLE MEANS BOTH DECODERS DISABLED.
 *     Not "the last chip we used". A bank left selected is a chip whose CE# stays low
 *     indefinitely, which is rule 1 violated for as long as the machine is idle -- the worst
 *     possible case, because it happens exactly when nobody is watching.
 *
 *
 *  WHY GPIO IS DRIVEN THROUGH sysfs AND NOT libgpiod
 *  --------------------------------------------------
 *  The stock Luckfox Buildroot image ships neither libgpiod nor its headers, and the SDK
 *  toolchain is uClibc so a glibc build of it will not run. sysfs is always there. It is
 *  slower per toggle (a write() each), which is why the address is set once per BANK and not
 *  once per transfer.
 * ===========================================================================================
 */

#ifndef PSRAM_BANK_H
#define PSRAM_BANK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_MAX_CHIPS   16
#define PB_CHIP_BYTES  (8u * 1024u * 1024u)     /* ESP-PSRAM64H = 64 Mbit = 8 MByte */

/* Sysfs GPIO numbers, not header pins. bank = gpio_bank*32 + group*8 + index.
 * Defaults match the shipped diagram:
 *      A   = header 16 = GPIO1_D0 = 1*32+3*8+0 = 56
 *      B   = header 17 = GPIO1_D1 = 57
 *      C   = header 18 = GPIO0_A4 = 4
 *      EN0 = header 10 = GPIO1_B7 = 1*32+1*8+7 = 47
 *      EN1 = header 11 = GPIO1_B6 = 46
 * Check these against YOUR board before running -- a wrong number here silently drives the
 * wrong pin, and the symptom is a chip that reads back as all-ones. */
typedef struct {
    int      gpio_a, gpio_b, gpio_c;
    int      gpio_en0, gpio_en1;
    int      fd_a, fd_b, fd_c, fd_en0, fd_en1;   /* held open: reopening per toggle is slow */

    int      spi_fd;
    uint32_t spi_hz;
    uint8_t  nchips;

    /* Largest payload allowed in one CE#-low window, derived from spi_hz so the 8 us tCEM
     * limit holds at whatever clock the bus actually runs. */
    uint32_t max_chunk;

    /* HOW CE# IS PULSED BETWEEN CHUNKS. See the note above -- this is the difference between
     * a deep tier that is slow and one that is unusable.
     *   hw_cs = 1  the SPI controller's own CS drives both decoders' G2A, so one ioctl carries
     *              many chunks and every CE# pulse happens in hardware. Requires the wiring in
     *              docs/15. This is the default.
     *   hw_cs = 0  CE# is pulsed by writing sysfs GPIO between chunks. Works with G2A tied to
     *              3V3, costs ~9 syscalls per chunk, and is roughly 30x slower. */
    int      hw_cs;
    uint32_t xfers;            /* chunks batched into one ioctl when hw_cs                    */
    uint8_t *txbuf, *rxbuf;    /* sized in pb_open; never on the stack                        */
    void    *xarr;             /* struct spi_ioc_transfer[xfers]                              */
    uint64_t ioctls;           /* measured, so the guide's numbers come from your board        */

    uint64_t reads, writes, chunks;
    int      selected;                            /* -1 = idle, both decoders off */
} psram_bank_t;

/* Open spidev and export/configure every GPIO. Returns 0 on success.
 * `spidev` is typically "/dev/spidev0.0". */
int  pb_open(psram_bank_t *b, const char *spidev, uint32_t hz, uint8_t nchips);

/* Same, but choose how CE# is pulsed. hw_cs = 1 is what pb_open uses. Pass 0 only if you tied
 * G2A high instead of running the controller's CS to it. */
int  pb_open_ex(psram_bank_t *b, const char *spidev, uint32_t hz, uint8_t nchips, int hw_cs);
void pb_close(psram_bank_t *b);

/* Select one chip (0..nchips-1), or -1 for idle. Break-before-make is enforced inside. */
int  pb_select(psram_bank_t *b, int chip);

/* Linear address across the whole stick: chip = addr / 8 MB. Chunked to respect tCEM, and
 * the chip is deselected before returning. */
int  pb_read (psram_bank_t *b, uint64_t addr, void *dst, uint32_t len);
int  pb_write(psram_bank_t *b, uint64_t addr, const void *src, uint32_t len);

/* Read the JEDEC id of one chip. 0x0D5D is an ESP-PSRAM64H; anything else means the chip is
 * not wired, not powered, or the decoder is addressing the wrong one. Run this on all ten
 * before trusting a single byte of data. */
int  pb_probe(psram_bank_t *b, uint8_t chip, uint16_t *id_out);

/* Write a pattern, read it back, and compare -- per chip. This is the only thing that proves
 * a hand-soldered stick, because a marginal joint reads correctly right up until it does not. */
int  pb_memtest(psram_bank_t *b, uint8_t chip, uint32_t bytes, uint32_t *first_bad_out);

uint64_t pb_capacity(const psram_bank_t *b);

#ifdef __cplusplus
}
#endif
#endif /* PSRAM_BANK_H */
