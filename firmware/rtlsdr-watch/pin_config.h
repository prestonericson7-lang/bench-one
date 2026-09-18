// Waveshare ESP32-S3-Touch-AMOLED-2.06 — pin map
// Verified against the V1.0 schematic and the ESP-IDF BSP, not the wiki.
#pragma once

#define XPOWERS_CHIP_AXP2101

// ---- Display: CO5300 AMOLED over QSPI ----
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   11
#define LCD_CS     12
#define LCD_RESET   8
#define LCD_TE     13     // real TE signal, unused by Arduino_GFX
#define LCD_WIDTH  410
#define LCD_HEIGHT 502
#define LCD_COL_OFFSET 22 // MANDATORY. Panel sits at column 0x16 in a 480px RAM.

// ---- Shared I2C: AXP2101 0x34, FT3168 0x38, PCF85063 0x51, QMI8658 0x6B ----
#define IIC_SDA    15
#define IIC_SCL    14

// ---- Touch: FT3168 ----
#define TP_INT     38     // active-low pulse. NOT RTC-capable, can't wake deep sleep.
#define TP_RESET    9     // vendor declares this and never drives it. We do.

// ---- Buttons ----
#define BTN_BOOT    0     // LOW = pressed
#define BTN_PWR    10     // HIGH = pressed (inverted!). 6s hold = hardware power off.

// ---- Misc ----
#define PIN_MOTOR  18     // vibration motor, undocumented by Waveshare
#define IMU_INT1   21     // only RTC-capable interrupt on the board
#define RTC_INT    39

// ---- Audio: I2S shared by ES8311 (out) and ES7210 (in) ----
#define I2S_MCLK   16
#define I2S_BCLK   41
#define I2S_LRCK   45
#define I2S_DOUT   40     // ESP32 -> ES8311
#define I2S_DIN    42     // ES7210 -> ESP32
#define PA_CTRL    46     // NS4150B amp enable, 10k pulldown (off at reset)
#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x40

// ---- SD / TF card (SDMMC 1-bit) ----
#define SD_CLK      2
#define SD_CMD      1
#define SD_D0       3
