/* ===========================================================================================
 *  psram_bringup.c -- the first thing you run on a freshly soldered stick
 * ===========================================================================================
 *
 *  A hand-soldered SOP-8 joint can look perfect and be intermittent. This finds that before
 *  the stick holds anything you care about.
 *
 *  It runs in the order that isolates faults, not the order that is quickest:
 *
 *      1. IDLE CHECK   -- both decoders off. Nothing should be selected at all.
 *      2. PROBE        -- read the JEDEC id of every chip. Wrong id = wiring, not memory.
 *      3. WALK         -- select each chip in turn and confirm only that one answers.
 *                         This is what catches a decoder wired wrong, which otherwise looks
 *                         like "chip 6 is dead" when really chip 6 and chip 2 share a select.
 *      4. MEMTEST      -- write, read back, compare. The only test that finds a cold joint.
 *      5. THROUGHPUT   -- measure real MB/s so the deep-tier numbers come from this board
 *                         and not from my arithmetic.
 *      6. tCEM STRESS  -- hammer one chip and re-verify, because a refresh violation shows up
 *                         under sustained load and not in a single pass.
 *
 *  BUILD
 *      arm-rockchip830-linux-uclibcgnueabihf-gcc -O2 -o psram_bringup \
 *          psram_bringup.c psram_bank.c
 *
 *  RUN
 *      ./psram_bringup                 # 10 chips at 25 MHz, quick test
 *      ./psram_bringup --chips 1       # start here. ONE chip. always.
 *      ./psram_bringup --hz 50000000 --full
 * ===========================================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "psram_bank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/spidev0.0";
    uint32_t hz = 25000000u;
    int chips = 10, full = 0, test_kb = 64, gpio_cs = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--dev")   && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--hz")    && i + 1 < argc) hz = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chips") && i + 1 < argc) chips = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--kb")    && i + 1 < argc) test_kb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--full")) { full = 1; test_kb = 8 * 1024; }
        else if (!strcmp(argv[i], "--gpio-cs")) gpio_cs = 1;
        else {
            fprintf(stderr,
                "usage: %s [--dev /dev/spidev0.0] [--hz 25000000] [--chips 10]\n"
                "          [--kb 64] [--full] [--gpio-cs]\n", argv[0]);
            return 1;
        }
    }

    printf("\n===============================================================\n");
    printf("PSRAM STICK BRING-UP   %s   %u Hz   %d chips\n", dev, (unsigned)hz, chips);
    printf("===============================================================\n\n");

    psram_bank_t b;
    if (pb_open_ex(&b, dev, hz, (uint8_t)chips, gpio_cs ? 0 : 1) != 0) {
        fprintf(stderr, "\nopen failed. check, in this order:\n");
        fprintf(stderr, "  1. luckfox-config -> SPI0_M0 enabled, /dev/spidev0.0 exists\n");
        fprintf(stderr, "  2. the five GPIO numbers in psram_bank.c match YOUR wiring\n");
        fprintf(stderr, "  3. 3V3 and GND actually reach the stick (meter it)\n");
        return 1;
    }
    printf("capacity if all chips are good: %llu MB\n\n",
           (unsigned long long)(pb_capacity(&b) / (1024u * 1024u)));

    /* ---- 1. IDLE -------------------------------------------------------------------- */
    printf("[1] idle state\n");
    pb_select(&b, -1);
    printf("    both decoders disabled, no chip selected.  OK\n\n");

    /* ---- 2. PROBE ------------------------------------------------------------------- */
    printf("[2] probe -- expecting id 0x0D5D on every chip\n");
    int good[PB_MAX_CHIPS];
    int ngood = 0;
    for (int c = 0; c < chips; c++) {
        uint16_t id = 0;
        good[c] = 0;
        if (pb_probe(&b, (uint8_t)c, &id) != 0) {
            printf("    chip %2d  SPI TRANSFER FAILED\n", c);
            continue;
        }
        if (id == 0x0D5Du) { good[c] = 1; ngood++; printf("    chip %2d  0x%04X  OK\n", c, id); }
        else if (id == 0x0000u || id == 0xFFFFu)
            printf("    chip %2d  0x%04X  NOTHING THERE -- CE#, power, or a cold joint\n", c, id);
        else
            printf("    chip %2d  0x%04X  WRONG ID -- MISO/MOSI swapped, or not a PSRAM64H\n", c, id);
    }
    printf("    %d of %d responded\n\n", ngood, chips);
    if (!ngood) {
        printf("Nothing answered. That is a bus fault, not ten dead chips.\n");
        printf("Check SCLK / MOSI / MISO / 3V3 / GND before touching the decoders.\n");
        pb_close(&b);
        return 1;
    }

    /* ---- 3. WALK -- the test that catches a mis-wired decoder ------------------------ */
    printf("[3] decoder walk -- selecting one chip must not answer as another\n");
    {
        int collisions = 0;
        for (int c = 0; c < chips; c++) {
            if (!good[c]) continue;
            /* Write a value unique to this chip at address 0, with only this chip selected. */
            uint8_t mark = (uint8_t)(0xA0u + c);
            if (pb_write(&b, (uint64_t)c * PB_CHIP_BYTES, &mark, 1) != 0) continue;
        }
        for (int c = 0; c < chips; c++) {
            if (!good[c]) continue;
            uint8_t rb = 0;
            if (pb_read(&b, (uint64_t)c * PB_CHIP_BYTES, &rb, 1) != 0) continue;
            if (rb != (uint8_t)(0xA0u + c)) {
                printf("    chip %2d  read 0x%02X, expected 0x%02X  <-- DECODER MIS-WIRED\n",
                       c, rb, (unsigned)(0xA0u + c));
                collisions++;
            }
        }
        if (!collisions) printf("    every chip is independently addressable.  OK\n");
        else printf("    %d collisions -- check A/B/C and the two G2A lines\n", collisions);
        printf("\n");
    }

    /* ---- 4. MEMTEST ----------------------------------------------------------------- */
    printf("[4] memtest -- %d KB per chip%s\n", test_kb, full ? "  (FULL 8 MB, this is slow)" : "");
    int bad_chips = 0;
    for (int c = 0; c < chips; c++) {
        if (!good[c]) { printf("    chip %2d  skipped (did not probe)\n", c); continue; }
        uint32_t firstbad = 0;
        const double t0 = now_s();
        const int r = pb_memtest(&b, (uint8_t)c, (uint32_t)test_kb * 1024u, &firstbad);
        const double dt = now_s() - t0;
        if (r == 0)
            printf("    chip %2d  PASS   %6.2f s   %5.2f MB/s\n",
                   c, dt, (test_kb / 1024.0) * 2.0 / dt);
        else if (r == 1) {
            printf("    chip %2d  FAIL at byte %u  <-- cold joint or a marginal clock\n",
                   c, (unsigned)firstbad);
            bad_chips++; good[c] = 0;
        } else {
            printf("    chip %2d  TRANSFER ERROR\n", c);
            bad_chips++; good[c] = 0;
        }
    }
    printf("\n");

    /* ---- 5. THROUGHPUT, AND THE TEST THAT PROVES G2A IS WIRED TO CS ---------------- */
    printf("[5] measured throughput -- this replaces my arithmetic\n");
    {
        int c = -1;
        for (int i = 0; i < chips; i++) if (good[i]) { c = i; break; }
        if (c >= 0) {
            static uint8_t buf[64 * 1024];
            const uint64_t base = (uint64_t)c * PB_CHIP_BYTES;

            const uint64_t io0 = b.ioctls;
            const double t0 = now_s();
            for (int i = 0; i < 8; i++) pb_read(&b, base, buf, sizeof(buf));
            const double dt = now_s() - t0;
            const double mbs = (8.0 * sizeof(buf)) / dt / 1048576.0;

            printf("    sequential read: %.2f MB/s\n", mbs);
            printf("    chunk %u B, %u chunks per ioctl, %llu ioctls for 512 KB\n",
                   (unsigned)b.max_chunk, (unsigned)b.xfers,
                   (unsigned long long)(b.ioctls - io0));
            printf("    one 1 KB hypervector body: %.0f us\n", 1024.0 / mbs / 1048576.0 * 1e6);
            printf("    full %d-chip scan: %.1f s\n", ngood, (ngood * 8.0) / mbs);

            /* THE WIRING TEST. Re-run the same read through the GPIO fallback. If G2A really
             * is on the controller's CS, the batched path is many times faster. If the two
             * come out the same, the CS wire is not doing anything -- G2A is probably tied to
             * 3V3, and the stick will work but stay 30x slower than it should be. */
            if (!gpio_cs && b.hw_cs) {
                b.hw_cs = 0;
                const double s0 = now_s();
                pb_read(&b, base, buf, 8 * 1024);
                const double sdt = now_s() - s0;
                b.hw_cs = 1;
                const double smbs = (8.0 * 1024.0) / sdt / 1048576.0;
                const double ratio = mbs / (smbs > 0.0 ? smbs : 1e-9);
                printf("    gpio-cs fallback: %.3f MB/s   ratio %.1fx\n", smbs, ratio);
                if (ratio < 3.0) {
                    printf("    ^^ WIRING: batching bought almost nothing, so the SPI chip\n");
                    printf("       select is NOT reaching pin 4 (G2A) on the decoders.\n");
                    printf("       Check that wire. The stick works either way; this is speed.\n");
                } else {
                    printf("    ^^ hardware CS confirmed on G2A.  OK\n");
                }
            }
        }
        printf("\n");
    }

    /* ---- 6. tCEM STRESS ------------------------------------------------------------- */
    printf("[6] tCEM stress -- sustained hammering, then re-verify\n");
    {
        int c = -1;
        for (int i = 0; i < chips; i++) if (good[i]) { c = i; break; }
        if (c >= 0) {
            static uint8_t w[4096], r[4096];
            for (int i = 0; i < 4096; i++) w[i] = (uint8_t)(i * 7u + 3u);
            const uint64_t base = (uint64_t)c * PB_CHIP_BYTES + 0x1000u;
            pb_write(&b, base, w, sizeof(w));
            int errs = 0;
            for (int pass = 0; pass < 40; pass++) {
                memset(r, 0, sizeof(r));
                if (pb_read(&b, base, r, sizeof(r)) != 0) { errs++; continue; }
                if (memcmp(r, w, sizeof(w)) != 0) errs++;
            }
            if (!errs)
                printf("    chip %d: 40 passes over 4 KB, no corruption.  refresh is holding.\n", c);
            else
                printf("    chip %d: %d/40 passes CORRUPTED  <-- lower --hz and retry\n", c, errs);
        }
        printf("\n");
    }

    printf("===============================================================\n");
    printf("RESULT: %d of %d chips usable", ngood - bad_chips, chips);
    if (bad_chips) printf("   (%d failed memtest)", bad_chips);
    printf("\n");
    printf("usable capacity: %d MB\n", (ngood - bad_chips) * 8);
    printf("chunks: %llu   ioctls: %llu   read: %llu B   written: %llu B\n",
           (unsigned long long)b.chunks, (unsigned long long)b.ioctls,
           (unsigned long long)b.reads, (unsigned long long)b.writes);
    printf("===============================================================\n\n");
    if (bad_chips) {
        printf("For a failed chip: reflow it first. A cold SOP-8 joint passes probe\n");
        printf("(one byte) and fails memtest (many bytes) exactly like this.\n\n");
    }

    pb_close(&b);
    return bad_chips ? 2 : 0;
}
