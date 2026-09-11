/* ===========================================================================================
 *  usblink_teensy -- how fast can a Luckfox actually feed a Teensy?
 * ===========================================================================================
 *
 *  WHY THIS NUMBER DECIDES AN ARCHITECTURE
 *  --------------------------------------
 *  The proposal is that a Luckfox holds the model and does all the talking, and the Teensy does nothing
 *  but arithmetic. Whether that is an improvement or a bottleneck comes down to one unmeasured number:
 *  how many megabytes per second crosses the USB link between them.
 *
 *  The comparison it has to win:
 *
 *      Teensy reading its own PSRAM, measured             18.2 MB/s effective
 *      Teensy reading DDR3 through the FPGA, cfgB         15.2 MB/s effective
 *      Teensy nibble unpack, measured                     39.3 MB/s
 *
 *  If the link carries more than about 20 MB/s it can replace the Teensy's own external memory and the
 *  architecture is sound. If it carries 8, the Luckfox is a bottleneck wearing the costume of a
 *  helper, and every weight it forwards is slower than the Teensy fetching that weight itself.
 *
 *  Both ends are USB 2.0 High Speed, 480 Mb/s on the wire, so 60 MB/s is the theoretical ceiling and
 *  bulk CDC realistically lands well under it. That gap is the whole question, and it is why this is
 *  measured rather than divided.
 *
 *  WHAT IS MEASURED, AND WHY THE DIRECTIONS ARE NOT SYMMETRIC
 *  ---------------------------------------------------------
 *  IN   host -> Teensy. **The direction that matters**, because this is weights arriving.
 *  OUT  Teensy -> host. Results leaving. Small in the real workload, measured for completeness.
 *  RTT  a small round trip, which prices the per-message cost rather than the per-byte cost.
 *
 *  The two directions do not have to be equal and usually are not: different endpoints, different
 *  buffering, and on the host side a completely different code path.
 *
 *  THE PROTOCOL, DELIBERATELY TRIVIAL
 *  ----------------------------------
 *  Single ASCII command bytes so the link can be driven by hand from a terminal when something is
 *  wrong, which is when a binary framing layer costs the most and helps the least.
 *
 *      'I' <4-byte LE count>   receive that many bytes, verify them, reply with "ok <bad> <us>"
 *      'O' <4-byte LE count>   send that many bytes of the same pattern
 *      'P'                     reply 'p' immediately. One round trip.
 *      'V'                     reply with a version line
 *
 *  The pattern is byte i == (i * 0x9D + 0x3B) & 0xFF, which is the same generator used everywhere else
 *  in this project. Verification is not optional decoration: a CDC link left in the default Linux line
 *  discipline silently turns 0x0A into 0x0D 0x0A, and a throughput number taken over a link that is
 *  corrupting data is worse than no number. The host side must set raw mode. If the byte count comes
 *  back wrong, that is almost always why.
 *
 *  RUN
 *      Flash this. Then on the Luckfox, build and run usblink_host.c against /dev/ttyACM0.
 * ========================================================================================= */

#include <Arduino.h>

#define BLK 4096
static uint8_t buf[BLK];

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9D + 0x3B); }

/* Block until n bytes have arrived or the link goes quiet for a second. Returning short rather than
 * hanging forever matters: a wedged benchmark tells you nothing, a short read tells you where it
 * stopped. */
static uint32_t read_exact(uint8_t *p, uint32_t n)
{
    uint32_t got = 0;
    uint32_t last = millis();
    while (got < n) {
        int r = Serial.read();
        if (r < 0) {
            if (millis() - last > 1000) break;
            continue;
        }
        p[got++] = (uint8_t)r;
        last = millis();
    }
    return got;
}

static uint32_t read_u32(void)
{
    uint8_t b[4];
    if (read_exact(b, 4) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void setup()
{
    Serial.begin(115200);          /* baud is ignored on USB CDC; it is here to satisfy the API */
    for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(i);
}

void loop()
{
    int c = Serial.read();
    if (c < 0) return;

    if (c == 'V') {
        Serial.printf("usblink_teensy 1 cpu=%lu\n", (unsigned long)(F_CPU_ACTUAL / 1000000));
        return;
    }

    if (c == 'P') { Serial.write('p'); return; }

    if (c == 'I') {
        uint32_t want = read_u32();
        uint32_t bad = 0, got = 0;
        uint32_t t0 = micros();
        while (got < want) {
            uint32_t n = want - got; if (n > BLK) n = BLK;
            uint32_t r = read_exact(buf, n);
            if (r == 0) break;
            for (uint32_t i = 0; i < r; i++) if (buf[i] != pat(got + i)) bad++;
            got += r;
        }
        uint32_t us = micros() - t0;
        /* Restore the reference block, which the receive loop overwrote. */
        for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(i);
        Serial.printf("ok %lu %lu %lu\n", (unsigned long)got, (unsigned long)bad,
                      (unsigned long)us);
        return;
    }

    if (c == 'O') {
        uint32_t want = read_u32();
        uint32_t sent = 0;
        while (sent < want) {
            uint32_t n = want - sent; if (n > BLK) n = BLK;
            /* The pattern has to stay continuous across block boundaries or the host cannot verify
             * it, so the block is regenerated at its true offset rather than reused. */
            for (uint32_t i = 0; i < n; i++) buf[i] = pat(sent + i);
            Serial.write(buf, n);
            sent += n;
        }
        Serial.flush();
        return;
    }
}
