#include "touch_cal.h"
#include "pin_config.h"
#include "ui.h"        // USBSerial
#include <SD_MMC.h>

// screenX = a*rawX + b*rawY + c ; screenY = d*rawX + e*rawY + f. Identity default.
static float g_a = 1, g_b = 0, g_c = 0, g_d = 0, g_e = 1, g_f = 0;
static bool  g_cal = false;

void tc_set(float a, float b, float c, float d, float e, float f) {
  g_a = a; g_b = b; g_c = c; g_d = d; g_e = e; g_f = f; g_cal = true;
}

void tc_apply(int32_t rawx, int32_t rawy, int32_t *sx, int32_t *sy) {
  *sx = (int32_t)(g_a * (float)rawx + g_b * (float)rawy + g_c + 0.5f);
  *sy = (int32_t)(g_d * (float)rawx + g_e * (float)rawy + g_f + 0.5f);
}

bool tc_calibrated(void) { return g_cal; }

static bool sd_up(void) {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  return SD_MMC.begin("/sdcard", true /*1-bit*/, false);
}

void tc_begin(void) {
  if (!sd_up()) { USBSerial.println("[touchcal] no SD; identity map"); return; }
  File f = SD_MMC.open("/touchcal.dat", FILE_READ);
  if (f) {
    char buf[128]; int n = f.readBytesUntil('\n', buf, sizeof(buf) - 1); buf[n] = 0;
    float a, b, c, d, e, g;
    if (sscanf(buf, "TCAL2 %f %f %f %f %f %f", &a, &b, &c, &d, &e, &g) == 6) {
      g_a = a; g_b = b; g_c = c; g_d = d; g_e = e; g_f = g; g_cal = true;
      USBSerial.println("[touchcal] loaded 3-point affine from SD");
    }
    f.close();
  }
  SD_MMC.end();
}

bool tc_save(float a, float b, float c, float d, float e, float f) {
  tc_set(a, b, c, d, e, f);
  if (!sd_up()) { USBSerial.println("[touchcal] save FAILED: no SD"); return false; }
  File fp = SD_MMC.open("/touchcal.dat", FILE_WRITE);
  bool ok = false;
  if (fp) {
    fp.printf("TCAL2 %.8f %.8f %.4f %.8f %.8f %.4f\n", a, b, c, d, e, f);
    fp.flush(); fp.close(); ok = true;
    USBSerial.println("[touchcal] saved /touchcal.dat");
  }
  SD_MMC.end();
  return ok;
}
