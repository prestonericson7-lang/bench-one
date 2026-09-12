/* ===========================================================================================
 *  esp32s3_node -- an ESP32-S3 as another worker on the same chain
 *
 *  WHY THESE BOARDS EARN A PLACE
 *
 *  Each one carries its own PSRAM on the module, reached through the chip's own memory controller
 *  rather than a hand-timed bus. That distinction is the whole reason they are usable here. This
 *  project already established that wifi jitter of half a millisecond is nothing at 50 ms and fatal at
 *  8 us, and that timed peripherals must stay off ESP32s -- so this node never bit-bangs anything. It
 *  reads memory-mapped PSRAM, which the controller clocks whether an interrupt lands or not.
 *
 *  Six of them at 8 MB each is 48 MB, which at one bit a weight is 384 million more parameters.
 *
 *  THE SAME CHAIN, THE SAME PROTOCOL
 *
 *  Upstream on Serial1, downstream on Serial2, one image for every node, and the address taken at
 *  power-up from whatever comes down the wire:
 *
 *      A <n>       take n if unnumbered, offer n+1 downstream
 *      @<n> <cmd>  run cmd on node n, or pass the line down untouched and relay the answer back
 *
 *  A Teensy and an ESP32 are indistinguishable to the host. That is the point: the host addresses
 *  nodes, not board types, and a chain can be any mixture.
 *
 *  THE ARITHMETIC, BY TABLE RATHER THAN BY MULTIPLY
 *
 *  The Teensy computes one bit a weight with SMLAD because it has SMLAD. This chip does not, but it has
 *  something better for this particular shape: 512 kB of fast internal SRAM, which is enough to hold
 *  the answer for every possible weight byte.
 *
 *  A bit means -1 or +1, so a byte of eight weights against eight known activations has exactly 256
 *  possible partial sums. The activation vector wraps every 2048 weights, which is every 256 bytes, so
 *  there are 256 distinct activation groups. 256 groups by 256 byte values is 65536 sums -- 128 kB as
 *  int16, built once at start-up.
 *
 *  Then a byte of weights costs one load and one add instead of eight multiply-accumulates. Eight
 *  weights per table lookup. It is exact, not an approximation: the table holds the same integer the
 *  multiplies would have produced, which is why the host can verify this node against the same
 *  reference as every other.
 *
 *  It is deliberately NOT what the Teensy does. Batching is what makes the Teensy fast, and a table is
 *  per activation vector, so sixty-four slots would want sixty-four tables. This node is fast at batch
 *  one and the Teensy is fast at batch sixty-four, and the host can put work where it pays.
 * ======================================================================================== */

#include <Arduino.h>

#define NACT        2048          /* the activation vector, same length every node uses */
#define GROUPS      256           /* 2048 weights / 8 per byte = 256 bytes before it wraps */
#define UP          Serial1       /* upstream: toward the head */
#define DOWN        Serial2       /* downstream: toward the tail */
#define UP_RX       18
#define UP_TX       17
#define DOWN_RX     16
#define DOWN_TX     15
#define LINKBAUD    1000000

static int8_t   xvec[NACT];
static int32_t  xsum;
static int16_t *table;            /* GROUPS * 256, in internal SRAM, never in PSRAM */
static uint8_t *weights;          /* in PSRAM */
static size_t   wbytes;

static uint8_t  g_addr;
static char     line[96];
static uint8_t  ln;
static char     out[128];

/* ---------------------------------------------------------------------------------------------
 *  the table
 *
 *  entry[g][b] is the sum over the set bits of b of the activations for group g, already doubled and
 *  already carrying its share of the -sum(x) term, so the kernel adds and does nothing else.
 * ------------------------------------------------------------------------------------------ */
static bool build_table(void)
{
    table = (int16_t *)malloc((size_t)GROUPS * 256 * sizeof(int16_t));
    if (!table) return false;
    for (int g = 0; g < GROUPS; g++) {
        const int8_t *a = &xvec[(g * 8) & (NACT - 1)];
        int32_t base = 0;
        for (int i = 0; i < 8; i++) base += a[i];      /* the -1 contribution of every clear bit */
        for (int b = 0; b < 256; b++) {
            int32_t set = 0;
            for (int i = 0; i < 8; i++)
                if ((b >> i) & 1) set += a[i];
            /* sum (2*bit - 1) * a  =  2 * (sum of set) - (sum of all eight) */
            table[g * 256 + b] = (int16_t)(2 * set - base);
        }
    }
    return true;
}

static int64_t mac1_table(const uint8_t *w, size_t nbytes)
{
    int64_t acc = 0;
    size_t g = 0;
    for (size_t j = 0; j < nbytes; j++) {
        acc += table[g * 256 + w[j]];
        g = (g + 1) & (GROUPS - 1);
    }
    return acc;
}

/* the same rule every node fills its memory with, so the host can predict any node's answer */
static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

/* ---------------------------------------------------------------------------------------------
 *  the link
 * ------------------------------------------------------------------------------------------ */
static void say(const char *s)
{
    UP.println(s);
    Serial.print("< "); Serial.println(s);
}

/* A node in the middle is a wire with a buffer. It must not interpret, reformat or reorder anything --
 * the reply belongs to the host. */
static void relay_reply(uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        while (DOWN.available()) {
            const char c = (char)DOWN.read();
            UP.write(c);
            if (c == 0x0A) return;
        }
    }
    UP.println("E timeout");
}

static long arg(const char *s, int which)
{
    int seen = 0;
    for (const char *p = s; *p; p++)
        if (*p == ' ') { seen++; if (seen == which) return atol(p + 1); }
    return 0;
}

static void handle(const char *c)
{
    Serial.print("> "); Serial.println(c);

    if (c[0] == '@') {
        const char *sp = c;
        while (*sp && *sp != ' ') sp++;
        const long want = atol(c + 1);
        if (g_addr && want == (long)g_addr && *sp) {
            c = sp + 1;
        } else {
            DOWN.println(c);
            relay_reply(600000ul);
            return;
        }
    }

    if ((c[0] == 'A' || c[0] == 'a') && (c[1] == ' ' || c[1] == 0)) {
        const long want = atol(c + 1);
        if (!g_addr && want > 0) {
            g_addr = (uint8_t)want;
            snprintf(out, sizeof(out), "A %u here", g_addr);
            say(out);
            char nxt[16];
            snprintf(nxt, sizeof(nxt), "A %ld", want + 1);
            DOWN.println(nxt);
            return;
        }
        DOWN.println(c);
        relay_reply(5000ul);
        return;
    }

    switch (c[0]) {
    case 'I': case 'i':
        snprintf(out, sizeof(out),
                 "I bench-one esp32s3 1 addr %u psram %lu bits 1 fcpu %lu",
                 g_addr, (unsigned long)wbytes, (unsigned long)getCpuFrequencyMhz() * 1000000ul);
        say(out);
        break;

    case 'F': case 'f': {
        const uint32_t a = (uint32_t)arg(c, 1);
        uint32_t len = (uint32_t)arg(c, 2);
        if (a + len > wbytes) len = (a < wbytes) ? (uint32_t)(wbytes - a) : 0;
        const uint32_t t0 = micros();
        for (uint32_t i = 0; i < len; i++) weights[a + i] = pat(a + i);
        snprintf(out, sizeof(out), "F %lu", (unsigned long)(micros() - t0));
        say(out);
        break;
    }

    case 'V': case 'v': {
        const uint32_t a = (uint32_t)arg(c, 1);
        uint32_t len = (uint32_t)arg(c, 2);
        if (a + len > wbytes) len = (a < wbytes) ? (uint32_t)(wbytes - a) : 0;
        uint32_t bad = 0;
        const uint32_t t0 = micros();
        for (uint32_t i = 0; i < len; i++) if (weights[a + i] != pat(a + i)) bad++;
        snprintf(out, sizeof(out), "V %lu %lu",
                 (unsigned long)bad, (unsigned long)(micros() - t0));
        say(out);
        break;
    }

    case 'M': case 'm': {
        const uint32_t a = (uint32_t)arg(c, 1);
        uint32_t len = (uint32_t)arg(c, 2);
        const long bits = arg(c, 3);
        if (a + len > wbytes) len = (a < wbytes) ? (uint32_t)(wbytes - a) : 0;
        if (bits != 1) { say("E bits"); break; }
        const uint32_t t0 = micros();
        const int64_t sum = mac1_table(weights + a, len);
        const uint32_t us = micros() - t0;
        snprintf(out, sizeof(out), "M %lld %lu", (long long)sum, (unsigned long)us);
        say(out);
        break;
    }

    default:
        say("E unknown");
        break;
    }
}

void setup()
{
    Serial.begin(115200);
    UP.begin(LINKBAUD, SERIAL_8N1, UP_RX, UP_TX);
    DOWN.begin(LINKBAUD, SERIAL_8N1, DOWN_RX, DOWN_TX);
    delay(300);

    for (int i = 0; i < NACT; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    xsum = 0;
    for (int i = 0; i < NACT; i++) xsum += xvec[i];

    /* The table must live in internal SRAM. In PSRAM it would be one external access per weight byte,
     * which is the cost the table exists to avoid. */
    if (!build_table()) {
        Serial.println("table allocation failed -- this node cannot compute");
    }

    /* Every byte of PSRAM the module has, less a margin, is weights. */
    wbytes = ESP.getFreePsram();
    if (wbytes > 256 * 1024) wbytes -= 256 * 1024;
    weights = (uint8_t *)ps_malloc(wbytes);
    if (!weights) { wbytes = 0; Serial.println("no PSRAM -- this node is a relay only"); }

    Serial.println("esp32s3_node up. math only; the host decides everything else.");
    Serial.print("  psram for weights: "); Serial.print((unsigned long)wbytes);
    Serial.print(" bytes = "); Serial.print((unsigned long)(wbytes * 8 / 1000000));
    Serial.println(" million parameters at one bit");
    Serial.println("  chain: upstream Serial1, downstream Serial2, address unset.");
}

void loop()
{
    while (UP.available()) {
        const char ch = (char)UP.read();
        if (ch == '\n' || ch == '\r') {
            if (ln) { line[ln] = 0; handle(line); ln = 0; }
        } else if (ln < sizeof(line) - 1) {
            line[ln++] = ch;
        }
    }
}
