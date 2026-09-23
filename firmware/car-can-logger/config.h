// ============================================================
//  config.h -- Teensy 4.1 CAN logger for the BMW F30 330e
//
//  HARDWARE LISTEN-ONLY. Both controllers are brought up with
//  FlexCAN_T4's LISTEN_ONLY mode, which puts the peripheral in silent
//  mode: it never asserts a dominant bit, so it cannot transmit, ACK,
//  or error-frame the bus. This is enforced by the controller, not by
//  our code, so a firmware bug cannot put traffic on the car's bus.
// ============================================================
#pragma once
#include <Arduino.h>

// ---------------- buses ----------------
// Teensy 4.1 has CAN1/CAN2/CAN3. Default: CAN1 on a 500k powertrain-class
// bus, CAN2 on a 100k body-class bus. Verify the actual bit rate with the
// logic analyser before wiring -- a wrong rate just yields zero frames.
#define BUS1_BAUD        500000UL
#define BUS2_BAUD        100000UL
#define ENABLE_BUS2      1

// ---------------- capture buffer ----------------
// Ring lives in RAM2 (DMAMEM) so it does not crowd the fast RAM1.
// A record is 10 bytes + payload (<=8), so 128 KB buffers ~7000 frames --
// seconds of headroom to ride out an SD write stall without dropping.
#define RING_BYTES       (128u * 1024u)

// SD is written in large sequential chunks, never per-frame. Small frequent
// writes cause 10-100x write amplification and make a power cut far more
// likely to land mid-write.
#define SD_BLOCK_BYTES   16384u

// ---------------- bus sleep ----------------
// F-series buses sleep and the car expects them to. When traffic stops we
// flush, close the file and idle, so the logger sleeps with the car instead
// of holding a file open and burning current.
#define IDLE_FLUSH_MS    2000UL     // quiet this long -> flush to SD
#define IDLE_SLEEP_MS    15000UL    // quiet this long -> close file, idle

// ---------------- ID census (the decoding tool) ----------------
// For every CAN ID we track how often it appears and WHICH BITS EVER CHANGE.
// Bits that never change are structure/constants; bits that move are the
// actual signals. This is how an undocumented bus gets decoded.
#define ID_TABLE_MAX     768

// ---------------- storage ----------------
#define LOG_MAGIC        "CANLOG2\n"   // 8 bytes
#define SD_CS            BUILTIN_SDCARD
