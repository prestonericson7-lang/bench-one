// Ultrasonic beacon detector: listen in the near-ultrasonic band (17-22 kHz)
// for the inaudible tones stores and ads use to silently track phones
// (ultrasonic cross-device tracking). Flags sustained energy up there that a
// human can't hear. Real counter-surveillance.
#include "ui.h"
#include "audio_mic.h"

#define RATE 48000
static lv_obj_t *bigLbl = NULL, *bandLbl = NULL, *bar = NULL, *note = NULL, *peakLbl = NULL;
static int16_t  samp[AUDIO_FFT_N];
static float    mag[AUDIO_FFT_N/2];
static bool     micOk = false;
static int      hotStreak = 0;

void app_ultra_open(lv_obj_t *body) {
  hotStreak = 0;
  micOk = audio_start(RATE);

  bigLbl = lv_label_create(body);
  lv_obj_set_width(bigLbl, LV_PCT(100));
  lv_obj_set_style_text_align(bigLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(bigLbl, &lv_font_montserrat_28, LV_PART_MAIN);
  if (!micOk) {
    lv_obj_set_style_text_color(bigLbl, COL_ORANGE, LV_PART_MAIN);
    lv_label_set_text(bigLbl, "MIC OFFLINE");
  } else {
    lv_obj_set_style_text_color(bigLbl, COL_GREEN, LV_PART_MAIN);
    lv_label_set_text(bigLbl, "LISTENING");
  }

  bandLbl = lv_label_create(body);
  lv_obj_set_width(bandLbl, LV_PCT(100));
  lv_obj_set_style_text_align(bandLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(bandLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(bandLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(bandLbl, micOk ? "17-22 kHz band" : "ES7210 returned no signal");

  lv_obj_t *card = ui_card(body);
  lv_obj_t *cl = lv_label_create(card);
  lv_obj_set_style_text_color(cl, COL_VIOLET, LV_PART_MAIN);
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(cl, "NEAR-ULTRASONIC ENERGY");
  bar = lv_bar_create(card);
  lv_obj_set_size(bar, LV_PCT(100), 16);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, COL_VIOLET, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
  peakLbl = lv_label_create(card);
  lv_obj_set_style_text_color(peakLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(peakLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(peakLbl, "peak: --");

  lv_obj_t *nc = ui_card(body);
  note = lv_label_create(nc);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_style_text_color(note, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(note, "the tiny mic tops out near 22 kHz, so this catches near-ultrasonic "
                          "beacons, not true >24 kHz. sustained tones you can't hear here are "
                          "worth suspecting.");
}

void app_ultra_tick(void) {
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[ultra] mic=%d\n",(int)micOk);} }
  if (!micOk) return;
  int n = audio_read(samp, AUDIO_FFT_N);
  if (n < AUDIO_FFT_N) return;
  float binHz = audio_fft(samp, AUDIO_FFT_N, RATE, mag);

  int lo = (int)(17000 / binHz), hi = (int)(22000 / binHz);
  if (hi > AUDIO_FFT_N/2) hi = AUDIO_FFT_N/2;
  float band = 0, total = 0, pk = 0; int pkBin = lo;
  for (int i = 1; i < AUDIO_FFT_N/2; i++) {
    total += mag[i];
    if (i >= lo && i <= hi) { band += mag[i]; if (mag[i] > pk) { pk = mag[i]; pkBin = i; } }
  }
  float ratio = total > 0.0001f ? band / total : 0;   // fraction of energy up top
  int pct = (int)(ratio * 300); if (pct > 100) pct = 100;
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_label_set_text_fmt(peakLbl, "peak: %.1f kHz", (pkBin * binHz) / 1000.0f);

  bool hot = (ratio > 0.12f && pk > 2.0f);
  hotStreak = hot ? hotStreak + 1 : 0;

  if (hotStreak >= 3) {
    lv_label_set_text(bigLbl, "! BEACON !");
    lv_obj_set_style_text_color(bigLbl, COL_RED, LV_PART_MAIN);
    lv_label_set_text(bandLbl, "inaudible tone detected");
    static uint32_t lb = 0; uint32_t now = millis();
    if (now - lb > 800) { lb = now; buzz(50); }
  } else {
    lv_label_set_text(bigLbl, "LISTENING");
    lv_obj_set_style_text_color(bigLbl, COL_GREEN, LV_PART_MAIN);
    lv_label_set_text(bandLbl, "17-22 kHz band");
  }
}

void app_ultra_close(void) {
  audio_stop();
  bigLbl = bandLbl = bar = note = peakLbl = NULL;
}
