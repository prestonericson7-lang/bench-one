// SDR — the watch IS the controller for the RTL-SDR system.
// Sends the FULL control surface over ESP-NOW to the E32R40T bridge, which relays
// it down the UART to the Teensy engine; mirrors spectrum/telemetry/detections back.
// Every LinkCmd the engine accepts is exposed here. Panel touch drives it all
// (the E32R40T's touch is dead; this one works).
#include "ui.h"
#include "sdr_link.h"

// ---- band presets (mirror of the Teensy config.h BAND_PRESETS) ----
struct Band { const char* name; uint32_t hz; uint32_t sr; uint8_t demod; };
static const Band BANDS[] = {
  { "FM Broadcast", 100100000UL, 2400000UL, 2 }, { "NOAA Wx 1", 162400000UL, 250000UL, 1 },
  { "Airband AM",   124000000UL,  250000UL, 3 }, { "2m Ham",    145000000UL, 250000UL, 1 },
  { "70cm Ham",     435000000UL,  250000UL, 1 }, { "ISM 433",   433920000UL,1024000UL, 0 },
  { "ISM 915",      915000000UL, 2400000UL, 0 }, { "ADS-B 1090",1090000000UL,2400000UL,0 },
  { "Marine VHF",   156800000UL,  250000UL, 1 }, { "PMR446",    446000000UL, 250000UL, 1 },
};
static const int BAND_N = sizeof(BANDS)/sizeof(BANDS[0]);

static const uint32_t STEPS[]    = { 1000UL, 10000UL, 100000UL, 1000000UL, 10000000UL };
static const char*    STEP_LBL[] = { "1k", "10k", "100k", "1M", "10M" };
static const int      STEP_N = 5;

static const uint32_t SRS[]    = { 250000UL, 1024000UL, 2048000UL, 2400000UL, 3200000UL };
static const char*    SR_LBL[] = { "250k", "1.024M", "2.048M", "2.4M", "3.2M" };
static const int      SR_N = 5;

static const uint8_t  FFTLOG[] = { 8, 9, 10, 11 };
static const char*    FFT_LBL[] = { "256", "512", "1024", "2048" };
static const int      FFT_N = 4;

static const char* TUNER[]  = { "unknown","R820T","R820T2","R860","FC0012","E4000" };
static const char* MODEN[]  = { "idle","spectrum","sweep","detect","demod" };
static const char* DEMODN[] = { "off","NBFM","WBFM","AM","USB","LSB" };

// capture / system action codes (event user-data)
enum { ACT_CAP_START=0, ACT_CAP_STOP, ACT_CAP_SAVE, ACT_CAP_DISCARD, ACT_REINIT, ACT_PERSIST };

// ---- widgets ----
static lv_obj_t *lblLink=NULL, *chart=NULL, *lblSpan=NULL, *lblFreq=NULL, *lblStep=NULL;
static lv_obj_t *lblMode=NULL, *roller=NULL, *lblRead=NULL, *lblCfg=NULL;
static lv_obj_t *swGain=NULL, *swBias=NULL, *sldGain=NULL, *lblGain=NULL, *lblToast=NULL;
static lv_obj_t *lblTxStat=NULL, *lblTxFreq=NULL;
static lv_chart_series_t *ser = NULL;
static int stepIdx = 2;                 // 100k
static uint32_t txFreqLocal = 10000000UL;
static int cfgSquelchT = -350;          // tenth-dBFS mirror (telemetry has no squelch field)
static uint32_t lastReqMs = 0, toastMs = 0;

static void toast(const char* s){ if(lblToast){ lv_label_set_text(lblToast, s); toastMs = millis(); } }

// ---- helpers ----
static lv_obj_t* flex_row(lv_obj_t *parent, int h) {
  if (h < 56) h = 56;                     // comfortable min tap height (no baby-finger buttons)
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_size(r, LV_PCT(100), h);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(r, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}
static lv_obj_t* mk_btn(lv_obj_t *row, const char *txt, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = lv_obj_create(row);
  lv_obj_set_flex_grow(b, 1);
  lv_obj_set_height(b, 52);
  lv_obj_set_style_bg_color(b, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 2, LV_PART_MAIN);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_set_style_text_color(l, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
  return b;
}
static void card_title(lv_obj_t *card, const char *txt, lv_color_t c) {
  lv_obj_t *l = lv_label_create(card);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, c, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
}
static lv_obj_t* mk_switch(lv_obj_t *row, bool on, lv_color_t c, lv_event_cb_t cb) {
  lv_obj_t *sw = lv_switch_create(row);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, c, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return sw;
}
static void row_label(lv_obj_t *row, const char *txt) {
  lv_obj_t *l = lv_label_create(row);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
}

// ---- event callbacks ----
static void tune_cb(lv_event_t *e){ sdr_cmd_i32(CMD_TUNE_STEP, (int)(intptr_t)lv_event_get_user_data(e) * (int32_t)STEPS[stepIdx]); buzz(10); }
static void step_cb(lv_event_t *e){ LV_UNUSED(e); stepIdx=(stepIdx+1)%STEP_N; if(lblStep) lv_label_set_text_fmt(lblStep,"step %s",STEP_LBL[stepIdx]); buzz(10); }
static void mode_cb(lv_event_t *e){ sdr_cmd_u8(CMD_SET_MODE,(uint8_t)(intptr_t)lv_event_get_user_data(e)); buzz(15); }
static void demod_cb(lv_event_t *e){ sdr_cmd_u8(CMD_SET_DEMOD,(uint8_t)(intptr_t)lv_event_get_user_data(e)); buzz(12); }
static void sr_cb(lv_event_t *e){ int i=(int)(intptr_t)lv_event_get_user_data(e); sdr_cmd_u32(CMD_SET_SR,SRS[i]); buzz(12); }
static void fft_cb(lv_event_t *e){ uint8_t lg=(uint8_t)(intptr_t)lv_event_get_user_data(e); uint8_t a[3]={lg,0,2}; sdr_send_cmd(CMD_SET_FFT,a,3); buzz(12); }
static void direct_cb(lv_event_t *e){ sdr_cmd_u8(CMD_SET_DIRECT,(uint8_t)(intptr_t)lv_event_get_user_data(e)); buzz(12); }
static void ppm_cb(lv_event_t *e){ int d=(int)(intptr_t)lv_event_get_user_data(e); const SdrState*s=sdr_link_state(); int16_t v=(int16_t)(s->ppm+d); uint8_t a[2]; lp_puti16(a,v); sdr_send_cmd(CMD_SET_PPM,a,2); buzz(10); }
static void sq_cb(lv_event_t *e){ int d=(int)(intptr_t)lv_event_get_user_data(e); cfgSquelchT+=d*10; if(cfgSquelchT<-800)cfgSquelchT=-800; if(cfgSquelchT>0)cfgSquelchT=0; uint8_t a[2]; lp_puti16(a,(int16_t)cfgSquelchT); sdr_send_cmd(CMD_SET_SQUELCH,a,2); buzz(10); }
static void band_cb(lv_event_t *e){ LV_UNUSED(e); if(!roller)return; int i=(int)lv_roller_get_selected(roller); if(i<0||i>=BAND_N)return; sdr_cmd_band(BANDS[i].hz,BANDS[i].sr,BANDS[i].demod); sdr_cmd_u8(CMD_SET_MODE,LMODE_SPECTRUM); toast("tuned"); buzz(20); }
static void gain_cb(lv_event_t *e){ lv_obj_t*sw=(lv_obj_t*)lv_event_get_target(e); bool a=lv_obj_has_state(sw,LV_STATE_CHECKED); int16_t v=sldGain?(int16_t)lv_slider_get_value(sldGain):297; sdr_cmd_set_gain(a,v); buzz(10); }
static void gainval_cb(lv_event_t *e){ lv_obj_t*s=(lv_obj_t*)lv_event_get_target(e); int v=lv_slider_get_value(s); if(lblGain)lv_label_set_text_fmt(lblGain,"%.1f dB",v/10.0); if(swGain&&!lv_obj_has_state(swGain,LV_STATE_CHECKED)) sdr_cmd_set_gain(false,(int16_t)v); }
static void bias_cb(lv_event_t *e){ lv_obj_t*sw=(lv_obj_t*)lv_event_get_target(e); sdr_cmd_u8(CMD_SET_BIAST, lv_obj_has_state(sw,LV_STATE_CHECKED)?1:0); buzz(10); }
static void tx_mode_cb(lv_event_t *e) {
  uint8_t m = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  sdr_cmd_tx_set(m, txFreqLocal);
  toast(m==0?"TX off":m==1?"TX CW":"TX AM");
  buzz(15);
}
static void tx_freq_cb(lv_event_t *e) {
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  int64_t f = (int64_t)txFreqLocal + d * (int64_t)STEPS[stepIdx];
  if (f < 1000) f = 1000;
  if (f > 75000000LL) f = 75000000LL;
  txFreqLocal = (uint32_t)f;
  const SdrState *s = sdr_link_state();
  if (s->txMode > 0) sdr_cmd_tx_set(s->txMode, txFreqLocal);
  if (lblTxFreq) lv_label_set_text_fmt(lblTxFreq, "%.3f MHz", txFreqLocal / 1e6);
  buzz(10);
}
static void tx_ptt_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED)  { sdr_cmd_tx_key(true);  buzz(8); }
  if (code == LV_EVENT_RELEASED) { sdr_cmd_tx_key(false); buzz(8); }
}
static void cap_cb(lv_event_t *e){
  switch((int)(intptr_t)lv_event_get_user_data(e)){
    case ACT_CAP_START:  { uint8_t k=0; sdr_send_cmd(CMD_CAP_START,&k,1); toast("capturing"); } break;
    case ACT_CAP_STOP:   sdr_send_cmd(CMD_CAP_STOP,nullptr,0); toast("stopped"); break;
    case ACT_CAP_SAVE:   sdr_send_cmd(CMD_CAP_SAVE,nullptr,0); toast("saving\xE2\x80\xA6"); break;
    case ACT_CAP_DISCARD:sdr_send_cmd(CMD_CAP_DISCARD,nullptr,0); toast("discarded"); break;
    case ACT_REINIT:     sdr_send_cmd(CMD_REBOOT_SDR,nullptr,0); toast("re-init SDR\xE2\x80\xA6"); break;
    case ACT_PERSIST:    { uint8_t s=1; sdr_send_cmd(CMD_PERSIST,&s,1); toast("settings saved"); } break;
  }
  buzz(15);
}

// ---- app lifecycle ----
void app_sdr_open(lv_obj_t *body) {
  sdr_link_begin();
  stepIdx = 2; cfgSquelchT = -350; lastReqMs = 0; toastMs = 0;

  // LINK
  lv_obj_t *lc = ui_card(body);
  card_title(lc, "LINK  \xC2\xB7  RTL-SDR", COL_TEAL);
  lblLink = lv_label_create(lc);
  lv_label_set_long_mode(lblLink, LV_LABEL_LONG_WRAP); lv_obj_set_width(lblLink, LV_PCT(100));
  lv_obj_set_style_text_color(lblLink, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblLink, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(lblLink, "connecting\xE2\x80\xA6");
  lblToast = lv_label_create(lc);
  lv_obj_set_style_text_color(lblToast, COL_YELLOW, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblToast, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblToast, "");

  // SPECTRUM
  lv_obj_t *sc = ui_card(body);
  card_title(sc, "SPECTRUM", COL_GREEN);
  chart = lv_chart_create(sc);
  lv_obj_set_size(chart, LV_PCT(100), 150);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
  lv_chart_set_point_count(chart, LINK_WATCH_SPEC_BINS);
  lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 255);
  lv_chart_set_div_line_count(chart, 4, 0);
  lv_obj_set_style_bg_color(chart, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
  lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
  ser = lv_chart_add_series(chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
  lblSpan = lv_label_create(sc);
  lv_obj_set_style_text_color(lblSpan, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblSpan, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblSpan, "\xE2\x80\x94");

  // TUNE
  lv_obj_t *tc = ui_card(body);
  card_title(tc, "TUNE", COL_BLUE);
  lblFreq = lv_label_create(tc);
  lv_obj_set_style_text_color(lblFreq, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblFreq, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_label_set_text(lblFreq, "--- MHz");
  lv_obj_t *tr = flex_row(tc, 46);
  mk_btn(tr, LV_SYMBOL_MINUS, tune_cb, (void*)(intptr_t)-1);
  lv_obj_t *sb = mk_btn(tr, "step", step_cb, NULL);
  lblStep = lv_obj_get_child(sb, 0); lv_label_set_text_fmt(lblStep, "step %s", STEP_LBL[stepIdx]);
  mk_btn(tr, LV_SYMBOL_PLUS, tune_cb, (void*)(intptr_t)1);

  // MODE + demod
  lv_obj_t *mc = ui_card(body);
  card_title(mc, "MODE", COL_INDIGO);
  lblMode = lv_label_create(mc);
  lv_obj_set_style_text_color(lblMode, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblMode, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblMode, "mode --");
  lv_obj_t *mr = flex_row(mc, 42);
  mk_btn(mr, "SPEC",  mode_cb, (void*)(intptr_t)LMODE_SPECTRUM);
  mk_btn(mr, "SWEEP", mode_cb, (void*)(intptr_t)LMODE_SWEEP);
  mk_btn(mr, "DET",   mode_cb, (void*)(intptr_t)LMODE_DETECT);
  mk_btn(mr, "DEMOD", mode_cb, (void*)(intptr_t)LMODE_DEMOD);
  lv_obj_t *dr = flex_row(mc, 42);
  mk_btn(dr, "NBFM", demod_cb, (void*)(intptr_t)1);
  mk_btn(dr, "WBFM", demod_cb, (void*)(intptr_t)2);
  mk_btn(dr, "AM",   demod_cb, (void*)(intptr_t)3);
  mk_btn(dr, "USB",  demod_cb, (void*)(intptr_t)4);
  mk_btn(dr, "LSB",  demod_cb, (void*)(intptr_t)5);

  // BANDS
  lv_obj_t *bc = ui_card(body);
  card_title(bc, "BANDS", COL_VIOLET);
  roller = lv_roller_create(bc);
  { char opts[512]; opts[0]=0; for(int i=0;i<BAND_N;i++){ strcat(opts,BANDS[i].name); if(i<BAND_N-1)strcat(opts,"\n"); } lv_roller_set_options(roller,opts,LV_ROLLER_MODE_NORMAL); }
  lv_roller_set_visible_row_count(roller, 3);
  lv_obj_set_width(roller, LV_PCT(100));
  lv_obj_set_style_bg_color(roller, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(roller, COL_VIOLET, LV_PART_SELECTED);
  lv_obj_set_style_text_font(roller, &lv_font_montserrat_16, LV_PART_MAIN);
  mk_btn(flex_row(bc, 42), "Tune band", band_cb, NULL);

  // SAMPLE RATE
  lv_obj_t *rc2 = ui_card(body);
  card_title(rc2, "SAMPLE RATE", COL_TEAL);
  lv_obj_t *rr = flex_row(rc2, 42);
  for (int i=0;i<SR_N;i++) mk_btn(rr, SR_LBL[i], sr_cb, (void*)(intptr_t)i);

  // FRONT END: auto gain + manual gain + bias-tee
  lv_obj_t *gc = ui_card(body);
  card_title(gc, "FRONT END", COL_ORANGE);
  lv_obj_t *gr = flex_row(gc, 40);
  row_label(gr, "Auto gain (AGC)");
  swGain = mk_switch(gr, true, COL_GREEN, gain_cb);
  sldGain = lv_slider_create(gc);
  lv_obj_set_width(sldGain, LV_PCT(100));
  lv_slider_set_range(sldGain, 0, 496);
  lv_slider_set_value(sldGain, 297, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sldGain, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sldGain, COL_ORANGE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sldGain, COL_ORANGE, LV_PART_KNOB);
  lv_obj_add_event_cb(sldGain, gainval_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lblGain = lv_label_create(gc);
  lv_obj_set_style_text_color(lblGain, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblGain, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblGain, "29.7 dB  (manual)");
  lv_obj_t *br = flex_row(gc, 40);
  row_label(br, "Bias-tee 4.5V (LNA)");
  swBias = mk_switch(br, false, COL_ORANGE, bias_cb);

  // CONFIG: PPM, FFT, squelch, direct sampling
  lv_obj_t *cc = ui_card(body);
  card_title(cc, "CONFIG", COL_YELLOW);
  lblCfg = lv_label_create(cc);
  lv_obj_set_style_text_color(lblCfg, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblCfg, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblCfg, "ppm -- \xC2\xB7 fft -- \xC2\xB7 sq -- \xC2\xB7 direct --");
  lv_obj_t *pr = flex_row(cc, 40);
  row_label(pr, "PPM");
  mk_btn(pr, LV_SYMBOL_MINUS, ppm_cb, (void*)(intptr_t)-1);
  mk_btn(pr, LV_SYMBOL_PLUS,  ppm_cb, (void*)(intptr_t)1);
  lv_obj_t *sqr = flex_row(cc, 40);
  row_label(sqr, "Squelch");
  mk_btn(sqr, LV_SYMBOL_MINUS, sq_cb, (void*)(intptr_t)-1);
  mk_btn(sqr, LV_SYMBOL_PLUS,  sq_cb, (void*)(intptr_t)1);
  lv_obj_t *fr = flex_row(cc, 40);
  for (int i=0;i<FFT_N;i++) mk_btn(fr, FFT_LBL[i], fft_cb, (void*)(intptr_t)FFTLOG[i]);
  lv_obj_t *dsr = flex_row(cc, 40);
  mk_btn(dsr, "Direct OFF", direct_cb, (void*)(intptr_t)0);
  mk_btn(dsr, "I",          direct_cb, (void*)(intptr_t)1);
  mk_btn(dsr, "Q",          direct_cb, (void*)(intptr_t)2);

  // CAPTURE + system
  lv_obj_t *cap = ui_card(body);
  card_title(cap, "CAPTURE  \xC2\xB7  SD", COL_RED);
  lv_obj_t *cr1 = flex_row(cap, 42);
  mk_btn(cr1, "REC",     cap_cb, (void*)(intptr_t)ACT_CAP_START);
  mk_btn(cr1, "STOP",    cap_cb, (void*)(intptr_t)ACT_CAP_STOP);
  mk_btn(cr1, "SAVE",    cap_cb, (void*)(intptr_t)ACT_CAP_SAVE);
  mk_btn(cr1, "DISCARD", cap_cb, (void*)(intptr_t)ACT_CAP_DISCARD);
  lv_obj_t *cr2 = flex_row(cap, 42);
  mk_btn(cr2, "Re-init SDR",   cap_cb, (void*)(intptr_t)ACT_REINIT);
  mk_btn(cr2, "Save settings", cap_cb, (void*)(intptr_t)ACT_PERSIST);

  // TX · GPIO RF
  lv_obj_t *txc = ui_card(body);
  card_title(txc, "TX \xC2\xB7 GPIO RF", COL_RED);
  lblTxFreq = lv_label_create(txc);
  lv_obj_set_style_text_color(lblTxFreq, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblTxFreq, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_label_set_text_fmt(lblTxFreq, "%.3f MHz", txFreqLocal / 1e6);
  lv_obj_t *txmr = flex_row(txc, 42);
  mk_btn(txmr, "OFF", tx_mode_cb, (void*)(intptr_t)0);
  mk_btn(txmr, "CW",  tx_mode_cb, (void*)(intptr_t)1);
  mk_btn(txmr, "AM",  tx_mode_cb, (void*)(intptr_t)2);
  lv_obj_t *txfr = flex_row(txc, 42);
  mk_btn(txfr, LV_SYMBOL_MINUS, tx_freq_cb, (void*)(intptr_t)-1);
  mk_btn(txfr, LV_SYMBOL_PLUS,  tx_freq_cb, (void*)(intptr_t)1);
  lv_obj_t *ptt = lv_obj_create(txc);
  lv_obj_set_size(ptt, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(ptt, COL_RED, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ptt, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ptt, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(ptt, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(ptt, 12, LV_PART_MAIN);
  lv_obj_remove_flag(ptt, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ptt, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ptt, tx_ptt_cb, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(ptt, tx_ptt_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *pl = lv_label_create(ptt);
  lv_label_set_text(pl, "PTT  (CW KEY)");
  lv_obj_center(pl);
  lv_obj_set_style_text_color(pl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, LV_PART_MAIN);
  lblTxStat = lv_label_create(txc);
  lv_obj_set_style_text_color(lblTxStat, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblTxStat, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblTxStat, "TX off");

  // READOUT
  lv_obj_t *ro = ui_card(body);
  card_title(ro, "READOUT", COL_TEXT_2);
  lblRead = lv_label_create(ro);
  lv_label_set_long_mode(lblRead, LV_LABEL_LONG_WRAP); lv_obj_set_width(lblRead, LV_PCT(100));
  lv_obj_set_style_text_color(lblRead, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblRead, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(lblRead, "waiting for telemetry\xE2\x80\xA6");

  sdr_send_req(REQ_HELLO);
  sdr_send_req(REQ_TELEMETRY);
  sdr_cmd_u8(CMD_SET_MODE, LMODE_SPECTRUM);
}

void app_sdr_tick(void) {
  sdr_link_poll();                       // always drain the ESP-NOW ring + keep the link alive
  const SdrState *s = sdr_link_state();
  uint32_t now = millis();

  if (now - lastReqMs > 1000) { lastReqMs = now; sdr_send_req(REQ_TELEMETRY); if (!s->linkUp) sdr_send_req(REQ_HELLO); }

  if (lblToast && toastMs && now - toastMs > 2500) { lv_label_set_text(lblToast, ""); toastMs = 0; }

  if (lblLink) {
    const char *tn = TUNER[s->tunerType < 6 ? s->tunerType : 0];
    if (!s->linkUp) { lv_label_set_text(lblLink, LV_SYMBOL_WARNING "  link down \xE2\x80\x94 is the E32R40T powered?"); lv_obj_set_style_text_color(lblLink, COL_RED, LV_PART_MAIN); }
    else if (s->tunerType == 0xFF || !s->sdrPresent) { lv_label_set_text(lblLink, LV_SYMBOL_OK "  bridge up \xC2\xB7 no dongle (SIM)"); lv_obj_set_style_text_color(lblLink, COL_YELLOW, LV_PART_MAIN); }
    else { lv_label_set_text_fmt(lblLink, LV_SYMBOL_OK "  online \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 fw %u.%u", tn, s->sdrStreaming?"streaming":"idle", s->fwMajor, s->fwMinor); lv_obj_set_style_text_color(lblLink, COL_GREEN, LV_PART_MAIN); }
  }

  if (chart && ser && s->specBins > 0) {
    uint16_t n = s->specBins; if (n > LINK_WATCH_SPEC_BINS) n = LINK_WATCH_SPEC_BINS;
    for (uint16_t i=0;i<LINK_WATCH_SPEC_BINS;i++) lv_chart_set_series_value_by_id(chart, ser, i, i<n ? s->specMag[i] : 0);
    lv_chart_refresh(chart);
    if (lblSpan) lv_label_set_text_fmt(lblSpan, "%.3f MHz \xC2\xB7 span %.2f MHz", s->specCentre/1e6, s->specSpan/1e6);
  }
  if (lblFreq && s->tlmValid) lv_label_set_text_fmt(lblFreq, "%.3f MHz", s->tunedHz/1e6);
  if (lblMode && s->tlmValid) lv_label_set_text_fmt(lblMode, "mode %s \xC2\xB7 demod %s \xC2\xB7 %.3f MS/s",
      MODEN[s->mode<5?s->mode:0], DEMODN[s->demod<6?s->demod:0], s->sampleRateHz/1e6);
  if (lblCfg && s->tlmValid) {
    static const char* DS[] = {"off","I","Q"};
    lv_label_set_text_fmt(lblCfg, "ppm %d \xC2\xB7 sq %.0f dBFS \xC2\xB7 direct %s \xC2\xB7 gain %s%.1f dB",
      s->ppm, cfgSquelchT/10.0, DS[s->directSamp<3?s->directSamp:0], s->gainAuto?"auto ":"", s->gainTenthDb/10.0);
  }
  if (s->tlmValid && s->txFreqHz > 0) txFreqLocal = s->txFreqHz;
  if (lblTxFreq && s->tlmValid) lv_label_set_text_fmt(lblTxFreq, "%.3f MHz", txFreqLocal / 1e6);
  if (lblTxStat && s->tlmValid) {
    static const char* TXM[] = {"off","CW","AM"};
    if (s->txActive)
      lv_label_set_text_fmt(lblTxStat, "TX %s  %.3f MHz  ACTIVE", TXM[s->txMode<3?s->txMode:0], s->txFreqHz/1e6);
    else
      lv_label_set_text_fmt(lblTxStat, "TX %s  %.3f MHz", TXM[s->txMode<3?s->txMode:0], s->txFreqHz/1e6);
  }
  if (lblRead && s->tlmValid) {
    lv_label_set_text_fmt(lblRead,
      "RSSI %.1f dBFS  floor %.1f\nSDR %u mV  %d mA \xC2\xB7 %.1f\xC2\xB0""C  fan %u%% %u rpm\n"
      "SD %s  \xC2\xB7  GPS %s %u sats",
      s->rssiTenthDbfs/10.0, s->floorTenthDbfs/10.0,
      s->busMv, s->curMa, s->dieTenthC/10.0, (unsigned)(s->fanDuty*100/255), s->fanRpm,
      s->sdOk?"ok":"--", s->gpsValid?"fix":"no fix", s->gpsSats);
  }
}

void app_sdr_close(void) {
  sdr_link_end();
  lblLink = chart = lblSpan = lblFreq = lblStep = lblMode = roller = lblRead = NULL;
  lblCfg = swGain = swBias = sldGain = lblGain = lblToast = lblTxStat = lblTxFreq = NULL;
  ser = NULL;
}
