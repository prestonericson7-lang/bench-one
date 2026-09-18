// Bug sweep: the mics as an instrument. Live sound-level meter plus a spectrum
// bar view reaching into the near-ultrasonic, so you can spot hidden emitters
// (a switching whine, a camera's oscillator harmonic) a human can't hear.
#include "ui.h"
#include "audio_mic.h"

#define RATE 48000
#define BARS 24
static lv_obj_t *splLbl = NULL, *peakLbl = NULL, *barObj[BARS];
static lv_obj_t *specCard = NULL, *note = NULL;
static int16_t  samp[AUDIO_FFT_N];
static float    mag[AUDIO_FFT_N/2];
static bool     micOk = false;

void app_bugsweep_open(lv_obj_t *body) {
  micOk = audio_start(RATE);

  lv_obj_t *card = ui_card(body);
  lv_obj_t *cl = lv_label_create(card);
  lv_obj_set_style_text_color(cl, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(cl, micOk ? "SOUND LEVEL" : "MIC OFFLINE");
  splLbl = lv_label_create(card);
  lv_obj_set_style_text_color(splLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(splLbl, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(splLbl, micOk ? "-- dB" : "no signal");
  peakLbl = lv_label_create(card);
  lv_obj_set_style_text_color(peakLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(peakLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(peakLbl, "peak: --");

  // spectrum: a row of bars, low freq left -> ~22 kHz right
  specCard = ui_card(body);
  lv_obj_set_style_pad_all(specCard, 10, LV_PART_MAIN);
  lv_obj_t *sl = lv_label_create(specCard);
  lv_obj_set_style_text_color(sl, COL_BLUE, LV_PART_MAIN);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(sl, "SPECTRUM  0 - 22 kHz");

  lv_obj_t *strip = lv_obj_create(specCard);
  lv_obj_set_size(strip, LV_PCT(100), 90);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(strip, 3, LV_PART_MAIN);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < BARS; i++) {
    lv_obj_t *b = lv_obj_create(strip);
    lv_obj_set_size(b, 9, 4);
    lv_obj_set_style_bg_color(b, i >= BARS-4 ? COL_VIOLET : COL_BLUE, LV_PART_MAIN); // top bars = ultrasonic
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    barObj[i] = b;
  }

  lv_obj_t *nc = ui_card(body);
  note = lv_label_create(nc);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_style_text_color(note, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(note, "the violet bars are the near-ultrasonic top end. dB is relative "
                          "(dBFS), not calibrated SPL. steady tones nobody's making out loud "
                          "are worth a look.");
}

void app_bugsweep_tick(void) {
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[bugsweep] mic=%d\n",(int)micOk);} }
  if (!micOk) return;
  int n = audio_read(samp, AUDIO_FFT_N);
  if (n < AUDIO_FFT_N) return;

  float db = audio_rms_db(samp, n);
  lv_label_set_text_fmt(splLbl, "%.0f dB", db);

  float binHz = audio_fft(samp, AUDIO_FFT_N, RATE, mag);
  int half = AUDIO_FFT_N/2;

  // bucket the bins into BARS log-ish groups, track peak freq
  float pk = 0; int pkBin = 1;
  for (int i = 1; i < half; i++) if (mag[i] > pk) { pk = mag[i]; pkBin = i; }
  lv_label_set_text_fmt(peakLbl, "peak: %.1f kHz", (pkBin * binHz) / 1000.0f);

  for (int b = 0; b < BARS; b++) {
    int lo = 1 + (b * (half-1)) / BARS;
    int hi = 1 + ((b+1) * (half-1)) / BARS;
    float m = 0; for (int i = lo; i < hi; i++) if (mag[i] > m) m = mag[i];
    int h = (int)(m * 10.0f); if (h < 3) h = 3; if (h > 84) h = 84;
    lv_obj_set_height(barObj[b], h);
  }
}

void app_bugsweep_close(void) {
  audio_stop();
  splLbl = peakLbl = specCard = note = NULL;
}
