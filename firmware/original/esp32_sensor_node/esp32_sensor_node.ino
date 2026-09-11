/* ===========================================================================================
 *  esp32_sensor_node.ino  --  ESP32-S3 + MPU-6050, an ESP-NOW node in the fleet
 * ===========================================================================================
 *
 *  ROLE
 *  ----
 *  A leaf. It reads an IMU and reports what it saw. It decides nothing: the hub tells it which
 *  mode to run and at what rate, and the brain behind the hub decides what any of it means.
 *  Same mechanism/policy split as the rest of this project, one layer further out:
 *
 *      Teensy (brain)  <--UART-->  hub ESP32  <--ESP-NOW-->  THIS BOARD  <--I2C-->  MPU-6050
 *
 *  WIRING
 *  ------
 *      MPU-6050 / GY-521          ESP32-S3 #2
 *      -----------------          -----------
 *      VCC  --------------------> 3.3V   (NOT 5V -- see below)
 *      GND  --------------------> GND
 *      SCL  --------------------> GPIO9  (default Wire SCL)
 *      SDA  --------------------> GPIO8  (default Wire SDA)
 *      INT  --------------------> GPIO4  (data-ready; this is what makes timing honest)
 *      AD0  -- open or GND -----> address 0x68 (tie high for 0x69)
 *
 *  3.3 V, NOT 5 V. The module's I2C pull-ups tie to its own VCC, so a 5 V-powered module drives
 *  SDA and SCL to 5 V straight into pins that are not 5 V tolerant -- even though no 5 V wire
 *  ever touches the ESP32. The "3.3-5V" marking on these boards refers to their regulator, not
 *  to what they do to your bus.
 *
 *  WHY THE INT PIN IS NOT OPTIONAL
 *  -------------------------------
 *  You can read an MPU-6050 by polling, and for displaying orientation that is fine. It is
 *  useless for measuring latency. Polling timestamps the moment YOU LOOKED, not the moment the
 *  sample existed, so every measurement carries an unknown 0..interval of polling jitter --
 *  which at any realistic loop rate swamps the very air time we are trying to measure.
 *
 *  With INT wired, the data-ready interrupt timestamps the sample at the instant the sensor has
 *  it, and int_to_send_us becomes a real, defensible number.
 *
 *  WHAT THE ISR DOES, AND WHAT IT REFUSES TO DO
 *  --------------------------------------------
 *  It records a timestamp and sets a flag. That is all. It does not touch I2C (a blocking bus),
 *  and it does not call esp_now_send -- neither is safe from interrupt context, and doing either
 *  would trade a precise measurement for an unstable radio. loop() does the work; the ISR only
 *  captures WHEN.
 * ===========================================================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include "mesh_protocol.h"

/* ===========================================================================================
 * CONFIGURATION
 * =========================================================================================== */
#define SDA_PIN            8
#define SCL_PIN            9
#define INT_PIN            4
#define I2C_HZ             400000u

#define MPU_ADDR           0x68u
#define REG_SMPLRT_DIV     0x19u
#define REG_CONFIG         0x1Au
#define REG_GYRO_CONFIG    0x1Bu
#define REG_ACCEL_CONFIG   0x1Cu
#define REG_INT_PIN_CFG    0x37u
#define REG_INT_ENABLE     0x38u
#define REG_INT_STATUS     0x3Au
#define REG_ACCEL_XOUT     0x3Bu
#define REG_PWR_MGMT_1     0x6Bu
#define REG_WHO_AM_I       0x75u

/* The hub's WiFi STA MAC. Read from the board itself with esptool, not guessed.
 * A node also answers whoever last spoke to it, so this is the bootstrap address rather than a
 * hard dependency -- see gHubMac. */
static const uint8_t HUB_MAC[6] = { 0x28, 0x84, 0x85, 0x54, 0xE1, 0xAC };

/* ESP-NOW peers MUST share a channel. This is the single most common way a mesh silently fails:
 * everything initialises without error, every send returns ESP_OK, and nothing is ever received,
 * because the two radios are listening on different channels. The hub can change this at runtime
 * via MESH_MSG_CONFIG when it joins an access point and inherits the AP's channel. */
#define DEFAULT_CHANNEL    1

#define DEFAULT_RATE_HZ    200u
#define STATUS_PERIOD_MS   5000u

/* ===========================================================================================
 * STATE
 * =========================================================================================== */
static volatile uint32_t gIsrTimeUs   = 0;   /* esp_timer at the data-ready edge  */
static volatile uint32_t gIsrCount    = 0;
static volatile bool     gSampleReady = false;

static uint8_t   gMode       = MESH_MODE_ON_INTERRUPT;
static uint16_t  gRateHz     = DEFAULT_RATE_HZ;
static uint8_t   gBatchSize  = MESH_MAX_SAMPLES;
static uint8_t   gHubMac[6];

/* Counters for the two ways this node can stop producing data WITHOUT any error appearing.
 *
 * THE FAILURE THAT MOTIVATED THEM. After an ESP32 reset the node came up with its I2C
 * initialisation failing, so gSensorOk was false and the sampling block was skipped entirely.
 * But the MPU does NOT reset when the ESP32 does -- it kept the sample-rate and interrupt
 * configuration written before the reset, and kept pulsing its data-ready line at exactly the
 * configured rate. So the heartbeat showed irq climbing at a perfect 202 Hz while sent stayed
 * frozen at 0, which reads like a transmit problem and is actually a sensor problem.
 *
 * A live interrupt from a sensor you cannot talk to is one of the most misleading signals in
 * embedded work: the one number that looks healthiest is the one proving the sensor was
 * configured EARLIER, not that it is readable NOW. */
static uint32_t  gReadFail = 0;   /* mpuRead() returned false                                */
static uint32_t  gSkipped  = 0;   /* loop saw a sample but could not act on it               */
static uint8_t   gChannel    = DEFAULT_CHANNEL;

static MeshSampleMsg gOut;
static uint8_t   gPending    = 0;      /* samples accumulated in gOut          */
static uint16_t  gSeq        = 0;
static uint8_t   gDropped    = 0;
static uint32_t  gFirstUs    = 0;

static bool      gSensorOk   = false;
static uint8_t   gWhoAmI     = 0;
static uint32_t  gSent = 0, gSendFail = 0, gRecv = 0;
static uint32_t  gLastStatus = 0;

/* ===========================================================================================
 * MPU-6050
 * =========================================================================================== */
static bool mpuWrite(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool mpuRead(uint8_t reg, uint8_t *buf, uint8_t n)
{
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((int)MPU_ADDR, (int)n) != n) {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

/* The data-ready ISR. Timestamp and flag, nothing else -- see the note in the file header. */
static void IRAM_ATTR onDataReady(void)
{
    gIsrTimeUs   = (uint32_t)esp_timer_get_time();
    gSampleReady = true;
    gIsrCount++;
}

static bool mpuBegin(void)
{
    uint8_t div;

    if (!mpuRead(REG_WHO_AM_I, &gWhoAmI, 1)) {
        return false;
    }

    /* Wake it. The MPU-6050 boots into sleep and returns all zeros until PWR_MGMT_1 is cleared,
     * which looks exactly like a dead sensor and is the single most common bring-up mistake. */
    if (!mpuWrite(REG_PWR_MGMT_1, 0x00)) {
        return false;
    }
    delay(100);

    /* DLPF at 44 Hz. This also drops the internal sample rate from 8 kHz to 1 kHz, which is what
     * makes SMPLRT_DIV divide from 1 kHz below -- the two registers interact, and getting it
     * wrong silently gives you 8x the intended rate. */
    mpuWrite(REG_CONFIG, 0x03);
    mpuWrite(REG_GYRO_CONFIG, 0x00);    /* +/-250 deg/s */
    mpuWrite(REG_ACCEL_CONFIG, 0x00);   /* +/-2 g       */

    div = (gRateHz == 0u || gRateHz > 1000u) ? 0u : (uint8_t)((1000u / gRateHz) - 1u);
    mpuWrite(REG_SMPLRT_DIV, div);

    /* Active-high, push-pull, pulse (not latched). A latched interrupt stays asserted until the
     * status register is read, which is safer against a missed edge but costs an extra I2C
     * transaction per sample -- and at 1 kHz that bus traffic starts to matter. */
    mpuWrite(REG_INT_PIN_CFG, 0x00);
    mpuWrite(REG_INT_ENABLE, 0x01);     /* DATA_RDY_EN */

    return true;
}

/* ===========================================================================================
 * ESP-NOW
 * =========================================================================================== */
static void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status == ESP_NOW_SEND_SUCCESS) {
        gSent++;
    } else {
        /* This means the MAC layer got no acknowledgement. ESP-NOW does not retransmit, so the
         * packet is simply gone -- which is why the sequence number exists. */
        gSendFail++;
    }
}

/* IDF5 signature: the source MAC arrives inside esp_now_recv_info_t, not as a bare pointer.
 * This changed from core 2.x and is a compile error rather than a silent bug, thankfully. */
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    gRecv++;
    if (len < 1) {
        return;
    }

    /* Learn who is talking to us. A node that only ever answers a hardcoded MAC cannot be
     * adopted by a different hub without reflashing; learning the address makes the fleet
     * self-organising, and the hardcoded default is only a bootstrap. */
    memcpy(gHubMac, info->src_addr, 6);

    switch (data[0]) {
    case MESH_MSG_PING:
        /* Echo IMMEDIATELY, from the callback, carrying the token back unchanged. Any work done
         * before replying would be measured by the hub as air time and would corrupt the one
         * number this message exists to produce. The token guards against a late pong being
         * counted as a fast one -- the same stale-response hazard SEQ solves on the UART. */
        if (len >= (int)sizeof(MeshPingMsg)) {
            MeshPingMsg pong;
            memcpy(&pong, data, sizeof(pong));
            pong.type = MESH_MSG_PONG;
            esp_now_send(info->src_addr, (const uint8_t *)&pong, sizeof(pong));
        }
        break;

    case MESH_MSG_CONFIG:
        if (len >= (int)sizeof(MeshConfigMsg)) {
            MeshConfigMsg cfg;
            memcpy(&cfg, data, sizeof(cfg));
            gMode = cfg.mode;
            if (cfg.rate_hz > 0u) {
                gRateHz = cfg.rate_hz;
                uint8_t div = (gRateHz > 1000u) ? 0u : (uint8_t)((1000u / gRateHz) - 1u);
                mpuWrite(REG_SMPLRT_DIV, div);
            }
            if (cfg.batch > 0u) {
                gBatchSize = (cfg.batch > MESH_MAX_SAMPLES) ? MESH_MAX_SAMPLES : cfg.batch;
            }
            gPending = 0;
            Serial.printf("[cfg] mode=%u rate=%uHz batch=%u\n",
                          (unsigned)gMode, (unsigned)gRateHz, (unsigned)gBatchSize);
        }
        break;

    case MESH_MSG_IDENTIFY:
        {
            MeshHelloMsg hello;
            hello.type      = MESH_MSG_HELLO;
            hello.fw_major  = MESH_FW_MAJOR;
            hello.fw_minor  = MESH_FW_MINOR;
            hello.caps      = MESH_CAP_IMU;
            hello.sensor_ok = gSensorOk ? 1u : 0u;
            hello.who_am_i  = gWhoAmI;
            esp_now_send(info->src_addr, (const uint8_t *)&hello, sizeof(hello));
        }
        break;

    default:
        break;
    }
}

static bool meshBegin(uint8_t channel)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false, 0);

    /* Pin the channel explicitly. Without this the radio sits on whatever channel it last used,
     * and a mesh whose members disagree about the channel initialises perfectly and never
     * exchanges a single packet -- every send returns ESP_OK and nothing arrives. */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    gChannel = channel;

    if (esp_now_init() != ESP_OK) {
        return false;
    }
    esp_now_register_send_cb(onEspNowSent);
    esp_now_register_recv_cb(onEspNowRecv);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, gHubMac, 6);
    peer.channel = channel;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        return false;
    }
    return true;
}

static void sendPending(uint16_t intToSendUs)
{
    if (gPending == 0u) {
        return;
    }
    gOut.type           = MESH_MSG_SAMPLE;
    gOut.mode           = gMode;
    gOut.seq            = gSeq++;
    gOut.t_first_us     = gFirstUs;
    gOut.int_to_send_us = intToSendUs;
    gOut.count          = gPending;
    gOut.dropped        = gDropped;

    /* Send only the bytes actually used. A fixed 204-byte send for a single sample would waste
     * ~90% of the air time, and air time is the resource under test. */
    uint16_t bytes = (uint16_t)(offsetof(MeshSampleMsg, samples) + (gPending * sizeof(MeshSample)));
    esp_now_send(gHubMac, (const uint8_t *)&gOut, bytes);

    gPending = 0;
    gDropped = 0;
}

/* ===========================================================================================
 * SETUP / LOOP
 * =========================================================================================== */
void setup(void)
{
    Serial.begin(115200);
    {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 1500u) { }
    }

    memcpy(gHubMac, HUB_MAC, 6);

    Serial.println();
    Serial.println("===========================================================");
    Serial.println(" ESP32-S3 sensor node  --  ESP-NOW fleet member");
    Serial.printf(  " firmware %u.%u, I2C SDA=%d SCL=%d, INT=GPIO%d\n",
                    MESH_FW_MAJOR, MESH_FW_MINOR, SDA_PIN, SCL_PIN, INT_PIN);
    Serial.println("===========================================================");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("[node] my MAC : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[node] hub MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  gHubMac[0], gHubMac[1], gHubMac[2], gHubMac[3], gHubMac[4], gHubMac[5]);

    Wire.begin(SDA_PIN, SCL_PIN, I2C_HZ);
    gSensorOk = mpuBegin();
    if (gSensorOk) {
        Serial.printf("[imu]  MPU-6050 ok, WHO_AM_I=0x%02X, %u Hz, data-ready IRQ enabled\n",
                      (unsigned)gWhoAmI, (unsigned)gRateHz);
    } else {
        Serial.println("[imu]  NOT FOUND. Check SDA=GPIO8, SCL=GPIO9, VCC=3.3V, GND shared.");
    }

    pinMode(INT_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(INT_PIN), onDataReady, RISING);

    if (meshBegin(DEFAULT_CHANNEL)) {
        Serial.printf("[mesh] ESP-NOW up on channel %u\n", (unsigned)gChannel);
    } else {
        Serial.println("[mesh] ESP-NOW FAILED to initialise");
    }

    /* Announce ourselves unprompted, exactly as the radio does to the brain over the UART. A hub
     * that restarts should not have to poll to discover who is out there. */
    MeshHelloMsg hello;
    hello.type      = MESH_MSG_HELLO;
    hello.fw_major  = MESH_FW_MAJOR;
    hello.fw_minor  = MESH_FW_MINOR;
    hello.caps      = MESH_CAP_IMU;
    hello.sensor_ok = gSensorOk ? 1u : 0u;
    hello.who_am_i  = gWhoAmI;
    esp_now_send(gHubMac, (const uint8_t *)&hello, sizeof(hello));

    Serial.println("[node] running");
}

void loop(void)
{
    uint32_t now = millis();

    /* Count the sample we are about to ignore BEFORE deciding to ignore it, so a node that is
     * interrupting but unreadable is distinguishable from one that is simply idle. */
    if (gSampleReady && (!gSensorOk) && gMode != MESH_MODE_IDLE) {
        gSkipped++;
        gSampleReady = false;
    }

    if (gSampleReady && gSensorOk && gMode != MESH_MODE_IDLE) {
        uint8_t  raw[14];
        uint32_t isrUs;

        /* Snapshot the ISR timestamp before clearing the flag, so a sample arriving mid-read
         * cannot make us attribute the wrong instant to this packet. */
        noInterrupts();
        isrUs        = gIsrTimeUs;
        gSampleReady = false;
        interrupts();

        if (!mpuRead(REG_ACCEL_XOUT, raw, 14)) {
            gReadFail++;
        } else {
            if (gPending < gBatchSize && gPending < MESH_MAX_SAMPLES) {
                MeshSample *sm = &gOut.samples[gPending];
                sm->ax = (int16_t)((raw[0]  << 8) | raw[1]);
                sm->ay = (int16_t)((raw[2]  << 8) | raw[3]);
                sm->az = (int16_t)((raw[4]  << 8) | raw[5]);
                /* raw[6..7] is temperature, skipped: it changes far slower than the sample rate
                 * and would cost 2 bytes on every packet to say nothing new. */
                sm->gx = (int16_t)((raw[8]  << 8) | raw[9]);
                sm->gy = (int16_t)((raw[10] << 8) | raw[11]);
                sm->gz = (int16_t)((raw[12] << 8) | raw[13]);
                if (gPending == 0u) {
                    gFirstUs = isrUs;
                }
                gPending++;
            } else if (gDropped < 255u) {
                /* The node could not keep up. SAY SO in the next packet rather than silently
                 * thinning the stream -- a quietly decimated dataset is worse than a gap you
                 * can see, because nothing downstream can tell it happened. */
                gDropped++;
            }

            if (gMode == MESH_MODE_ON_INTERRUPT ||
                gPending >= gBatchSize || gPending >= MESH_MAX_SAMPLES) {
                uint32_t nowUs = (uint32_t)esp_timer_get_time();
                uint32_t delta = nowUs - gFirstUs;   /* unsigned: wrap-safe */
                sendPending((uint16_t)((delta > 65535u) ? 65535u : delta));
            }
        }
    }

    if ((uint32_t)(now - gLastStatus) >= STATUS_PERIOD_MS) {
        gLastStatus = now;
        Serial.printf("[node] up %lus ch=%u mode=%u irq=%lu sent=%lu fail=%lu recv=%lu "
                      "sensor=%s(0x%02X) readfail=%lu skipped=%lu\n",
                      (unsigned long)(now / 1000u), (unsigned)gChannel, (unsigned)gMode,
                      (unsigned long)gIsrCount, (unsigned long)gSent,
                      (unsigned long)gSendFail, (unsigned long)gRecv,
                      gSensorOk ? "ok" : "DOWN", (unsigned)gWhoAmI,
                      (unsigned long)gReadFail, (unsigned long)gSkipped);

        /* A node that cannot read its sensor must not merely log it -- it should keep TRYING.
         * Re-running init costs a few I2C transactions once every status period, and it turns a
         * permanently dead node into one that recovers by itself when a marginal connection
         * settles or a brown-out clears. Silence after a transient fault is a design choice, and
         * it is the wrong one. */
        if (!gSensorOk || gReadFail > 0u) {
            Serial.printf("[node] sensor unusable -- retrying init\n");
            if (mpuBegin()) {
                Serial.printf("[node] sensor recovered, WHO_AM_I=0x%02X\n", (unsigned)gWhoAmI);
                gReadFail = 0;
                gSkipped  = 0;
            }
        }
    }
}
