/* ===========================================================================================
 *  esp32_worker.ino -- BENCH ONE: an ESP32-S3 that cannot break the machine
 * ===========================================================================================
 *
 *  WHAT THIS PROVES, ON HARDWARE
 *  ------------------------------
 *  The S3's WiFi stack will steal this core, without warning, for milliseconds at a time. Any
 *  design where this board must finish a computation on schedule is broken before it is
 *  written. So this sketch does the opposite: it holds a shard of the memory, scans as much of
 *  it as the RTOS allows between yields, and reports what it found and how far it got.
 *
 *  It runs the scan with WiFi ACTIVE and deliberately busy, then prints the stall distribution
 *  it actually suffered. If the longest stall is tens of milliseconds -- and it will be -- that
 *  is not a problem to fix. It is the number that justifies the architecture.
 *
 *  Flash one board to see the measurement. Flash several, set BENCH_NODE_ID differently on
 *  each, and they become a distributed associative memory over ESP-NOW with no access point,
 *  no router and no IP stack.
 *
 *
 *  THE TWO RULES THIS FILE EXISTS TO OBEY
 *  ---------------------------------------
 *  1. NEVER HOLD STATE THE CLUSTER IS WAITING ON. This node answers questions; it is never a
 *     link in a chain. If it vanishes mid-query the answer is less complete, never wrong.
 *  2. NEVER BLOCK THE RADIO. The scan yields between chunks, so the WiFi task always gets the
 *     core back inside a few hundred microseconds and the task watchdog is never starved.
 * ===========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

extern "C" {
#include "bench_hdc.h"
#include "bench_hdc_shard.h"
}

/* --- identity ---------------------------------------------------------------------------- */
#ifndef BENCH_NODE_ID
#define BENCH_NODE_ID 40          /* 40..54 for the fifteen S3s, per docs/11 addressing       */
#endif

/* --- shard ------------------------------------------------------------------------------- */
/* 128 vectors x 1 KB = 128 KB. Kept in INTERNAL SRAM deliberately: a scan touching PSRAM
 * competes with the WiFi stack for the same cache, which turns a 3 us comparison into a stall
 * of its own. PSRAM is for staging, never for the hot loop. */
#define SHARD_N     128
#define SHARD_BASE  ((BENCH_NODE_ID - 40) * SHARD_N)

static hd_t     g_store[SHARD_N];
static uint16_t g_label[SHARD_N];
static hd_mem_t g_mem;

/* hd_scan_t carries a 1 KB query, and hd_acc_t is 16 KB. Neither goes on a task stack --
 * that is a 16,432-byte stack frame and an instant overflow on an 8 KB FreeRTOS task. */
static hd_scan_t g_scan;
static hd_acc_t  g_acc;

/* --- stall measurement ------------------------------------------------------------------- */
static volatile uint32_t g_chunks    = 0;
static volatile uint32_t g_stall_max = 0;
static volatile uint32_t g_stall_1ms = 0;
static volatile uint32_t g_stall_10ms= 0;
static volatile uint64_t g_chunk_us  = 0;

static uint32_t g_rng = 0xBE0C0DE ^ (BENCH_NODE_ID * 2654435761u);

/* =========================================================================================
 * THE WORKER TASK -- pinned to core 1, WiFi lives on core 0
 * =======================================================================================*/
static void scan_to_deadline(hd_scan_t *s, uint32_t deadline_us)
{
    /* 16 vectors per chunk. Each comparison is 256 XOR + 256 popcount, so a chunk is a few
     * hundred microseconds -- short enough that the radio never waits on us, long enough that
     * the yield overhead stays under a few percent. */
    while (micros() < deadline_us) {
        const uint32_t t0 = micros();
        const int done = hd_scan_chunk(s, 16);
        const uint32_t dt = micros() - t0;

        g_chunks++;
        g_chunk_us += dt;
        if (dt > g_stall_max) g_stall_max = dt;
        if (dt > 1000)  g_stall_1ms++;      /* the chunk itself is ~200 us, so anything above */
        if (dt > 10000) g_stall_10ms++;     /* 1 ms is the radio having taken the core        */

        if (done) break;
        taskYIELD();                        /* <-- the whole fix, in one line */
    }
}

static void worker_task(void *)
{
    for (;;) {
        /* A question the coordinator would normally send. Generated locally here so the
         * measurement runs with one board on the bench. */
        hd_t query;
        hd_random(query, &g_rng);

        static uint16_t qid = 0;
        qid++;

        hd_scan_begin(&g_scan, &g_mem, query, BENCH_NODE_ID, qid, SHARD_BASE);

        const uint32_t t0 = micros();
        scan_to_deadline(&g_scan, t0 + 50000u);   /* 50 ms budget, then answer regardless */
        const uint32_t total = micros() - t0;

        static uint32_t report = 0;
        if (++report % 20 == 0) {
            Serial.printf("q%-5u scanned %3u/%3u (%3u%% cov)  best=%ld d=%lu  %lu us\n",
                          qid, g_scan.result.scanned, g_scan.result.total,
                          (unsigned)(hd_scan_coverage(&g_scan) * 100 / 255),
                          (long)hd_partial_slot(&g_scan.result),
                          (unsigned long)g_scan.result.best_dist,
                          (unsigned long)total);
        }

        uint8_t wire[HD_PARTIAL_WIRE_BYTES];
        hd_partial_pack(&g_scan.result, wire);
        esp_now_send(NULL, wire, sizeof(wire));   /* broadcast; no peer = harmless no-op */

        vTaskDelay(1);
    }
}

/* =========================================================================================
 * SETUP
 * =======================================================================================*/
void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println("\n==========================================================");
    Serial.printf ("BENCH ONE -- S3 shard worker, node %d\n", BENCH_NODE_ID);
    Serial.println("==========================================================");
    Serial.printf("CPU          : %lu MHz, %d cores\n",
                  (unsigned long)getCpuFrequencyMhz(), 2);
    Serial.printf("internal heap: %u bytes free\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.printf("PSRAM        : %u bytes\n", (unsigned)ESP.getPsramSize());
    Serial.printf("shard        : %d vectors x %d bytes = %d KB, global base %d\n",
                  SHARD_N, HD_BYTES, (SHARD_N * HD_BYTES) / 1024, SHARD_BASE);

    /* --- fill the shard. Every node derives its vectors from a seed, so no hypervector is
     * ever sent over the air to populate a memory. --- */
    hd_mem_init(&g_mem, g_store, g_label, SHARD_N);
    for (int i = 0; i < SHARD_N; i++) {
        hd_t v;
        uint32_t s = 0x51EED000u + SHARD_BASE + i;
        hd_random(v, &s);
        hd_mem_add(&g_mem, v, (uint16_t)(SHARD_BASE + i));
    }
    Serial.printf("shard filled : %u vectors\n", g_mem.n);

    /* --- sanity: the substrate must behave before anything distributed is attempted --- */
    {
        hd_t a, b, c;
        uint32_t s = 7;
        hd_random(a, &s); hd_random(b, &s);
        hd_bind(c, a, b);
        hd_t back; hd_bind(back, c, b);
        bool inv = true;
        for (int i = 0; i < HD_WORDS; i++) if (back[i] != a[i]) inv = false;
        Serial.printf("bind inverse : %s\n", inv ? "OK" : "FAIL");
        Serial.printf("orthogonality: unrelated pair hamming %lu (expect ~%d)\n",
                      (unsigned long)hd_hamming(a, b), HD_BITS / 2);
        Serial.printf("self distance: %lu (expect 0)\n", (unsigned long)hd_hamming(a, a));
    }

    /* --- bring the radio up. This is the thing that will steal the core. --- */
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);        /* no power save: worst case for stalls, which is
                                           * exactly what we want to measure against */
    if (esp_now_init() == ESP_OK) {
        esp_now_peer_info_t p = {};
        memset(p.peer_addr, 0xFF, 6);     /* broadcast */
        p.channel = 0; p.encrypt = false;
        esp_now_add_peer(&p);
        Serial.println("esp-now      : up (broadcast)");
    } else {
        Serial.println("esp-now      : init FAILED (scan still runs)");
    }

    Serial.println("\n--- scanning with the radio active ---------------------");
    Serial.println("Watch the us column. Chunk work is ~200 us; anything far above");
    Serial.println("that is WiFi taking the core. The scan survives it by design.\n");

    /* Compute on core 1. WiFi's task and its interrupts live on core 0 -- this reduces the
     * collision rate and does NOT eliminate it, which is why the chunked scan exists as well. */
    xTaskCreatePinnedToCore(worker_task, "hdc", 8192, NULL, 3, NULL, 1);
}

void loop()
{
    delay(10000);
    const uint32_t ch = g_chunks;
    if (!ch) return;
    Serial.println("\n--- stall report --------------------------------------");
    Serial.printf("chunks run        : %lu\n", (unsigned long)ch);
    Serial.printf("mean chunk        : %lu us\n", (unsigned long)(g_chunk_us / ch));
    Serial.printf("worst chunk       : %lu us   <-- the radio\n", (unsigned long)g_stall_max);
    Serial.printf("chunks over 1 ms  : %lu (%.2f%%)\n",
                  (unsigned long)g_stall_1ms, 100.0 * g_stall_1ms / ch);
    Serial.printf("chunks over 10 ms : %lu (%.2f%%)\n",
                  (unsigned long)g_stall_10ms, 100.0 * g_stall_10ms / ch);
    Serial.println("Every one of those stalls cost coverage. None of them cost correctness.");
    Serial.println("-------------------------------------------------------\n");
}
