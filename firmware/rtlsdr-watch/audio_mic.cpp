#include "audio_mic.h"
#include "ui.h"   // USBSerial
#include "pin_config.h"
#include <Wire.h>
#include <ESP_I2S.h>
#include <math.h>

// ES7210 init: I2S SLAVE, Philips 16-bit, MCLK = 256*fs, MIC1+MIC2 -> SDOUT1,
// MICBIAS on, PGA 30 dB. Register values cross-checked across Espressif
// esp_codec_dev, esp-adf v2.6 audio_hal and ESPHome (datasheet-literal set).
struct rv_t { uint8_t reg, val; };
static const rv_t ES7210_INIT[] = {
  {0x00,0xFF},{0x00,0x41},{0x01,0x3F},
  {0x09,0x30},{0x0A,0x30},
  {0x23,0x2A},{0x22,0x0A},{0x20,0x0A},{0x21,0x2A},
  {0x08,0x10},                          // slave mode
  {0x40,0x43},{0x41,0x70},{0x42,0x70},  // analog on, micbias 2.87V
  {0x07,0x20},{0x02,0xC1},{0x04,0x01},{0x05,0x00},   // 256*fs clocking
  {0x11,0x60},{0x12,0x00},              // 16-bit Philips, ADC12 -> SDOUT1
  {0x01,0x34},                          // open ADC12 clock path
  {0x43,0x1A},{0x44,0x1A},{0x45,0x00},{0x46,0x00},   // MIC1/2 select + 30dB
  {0x47,0x08},{0x48,0x08},{0x49,0x08},{0x4A,0x08},
  {0x06,0x00},{0x4B,0x00},{0x4C,0xFF},  // power up MIC1/2, MIC3/4 off
  {0x00,0x71},{0x00,0x41},              // pulse ADC reset -> running
};

static I2SClass i2s;
static bool  running = false;
static bool  micOk   = false;

static void es7210_w(uint8_t r, uint8_t v) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(r); Wire.write(v);
  Wire.endTransmission();
}
static uint8_t es7210_r(uint8_t r) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(r);
  Wire.endTransmission(false);
  Wire.requestFrom((int)ES7210_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

bool audio_start(uint32_t rate) {
  if (running) return micOk;

  // Presence check: chip ID regs 0x3D/0x3E read "72"/"10".
  uint8_t id1 = es7210_r(0x3D), id2 = es7210_r(0x3E);
  bool present = (id1 == 0x72 && id2 == 0x10);
  USBSerial.printf("[mic] ES7210 id=%02X %02X present=%d\n", id1, id2, present);

  for (unsigned i = 0; i < sizeof(ES7210_INIT)/sizeof(ES7210_INIT[0]); i++)
    es7210_w(ES7210_INIT[i].reg, ES7210_INIT[i].val);
  delay(10);

  // ESP32 is the I2S MASTER (provides MCLK/BCLK/LRCK); ES7210 is the slave.
  i2s.setPins(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_DIN, I2S_MCLK);
  bool ok = i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT,
                      I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);  // 2 mics -> stereo
  if (!ok) { running = false; micOk = false; USBSerial.println("[mic] I2S begin FAILED"); return false; }
  running = true;

  // Sample a little and see if anything moves -- a dead ES7210 gives a flat DC
  // line, which we report honestly rather than drawing a fake spectrum.
  int16_t probe[128];
  int got = audio_read(probe, 128);
  int16_t mn = 32767, mx = -32768;
  for (int i = 0; i < got; i++) { if (probe[i] < mn) mn = probe[i]; if (probe[i] > mx) mx = probe[i]; }
  micOk = present && got > 0 && (mx - mn) > 4;   // any real signal has some spread
  USBSerial.printf("[mic] i2s got=%d spread=%d -> %s\n", got, (int)(mx-mn), micOk?"OK":"OFFLINE");
  return micOk;
}

void audio_stop(void) {
  if (!running) return;
  i2s.end();
  running = false;
  micOk = false;
}

bool audio_mic_ok(void) { return micOk; }

// Stereo frames come in interleaved L/R (MIC1/MIC2); we keep MIC1 (left).
int audio_read(int16_t *dst, int maxSamples) {
  if (!running) return 0;
  static int16_t buf[AUDIO_FFT_N * 2];
  int want = maxSamples * 2;
  if (want > AUDIO_FFT_N * 2) want = AUDIO_FFT_N * 2;
  size_t got = i2s.readBytes((char *)buf, want * sizeof(int16_t));
  int frames = got / (2 * sizeof(int16_t));
  for (int i = 0; i < frames; i++) dst[i] = buf[i * 2];
  return frames;
}

// In-place iterative radix-2 FFT on float re[]/im[]. n must be a power of 2.
static void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t;
                       t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cwr = 1, cwi = 0;
      for (int k = 0; k < len/2; k++) {
        float ur = re[i+k],        ui = im[i+k];
        float vr = re[i+k+len/2]*cwr - im[i+k+len/2]*cwi;
        float vi = re[i+k+len/2]*cwi + im[i+k+len/2]*cwr;
        re[i+k] = ur+vr; im[i+k] = ui+vi;
        re[i+k+len/2] = ur-vr; im[i+k+len/2] = ui-vi;
        float ncwr = cwr*wr - cwi*wi; cwi = cwr*wi + cwi*wr; cwr = ncwr;
      }
    }
  }
}

float audio_fft(const int16_t *samples, int n, uint32_t rate, float *mag) {
  static float re[AUDIO_FFT_N], im[AUDIO_FFT_N];
  if (n > AUDIO_FFT_N) n = AUDIO_FFT_N;
  for (int i = 0; i < n; i++) {
    // Hann window to cut spectral leakage
    float w = 0.5f * (1 - cosf(2 * (float)M_PI * i / (n - 1)));
    re[i] = samples[i] * w;
    im[i] = 0;
  }
  fft(re, im, n);
  for (int i = 0; i < n/2; i++) mag[i] = sqrtf(re[i]*re[i] + im[i]*im[i]) / n;
  return (float)rate / n;
}

float audio_rms_db(const int16_t *samples, int n) {
  if (n <= 0) return -120;
  double sum = 0;
  for (int i = 0; i < n; i++) sum += (double)samples[i] * samples[i];
  double rms = sqrt(sum / n);
  if (rms < 1) rms = 1;
  return 20.0f * log10f(rms / 32768.0f);   // dBFS (<= 0)
}
