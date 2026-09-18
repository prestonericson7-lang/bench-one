// Channel heatmap: sit in promiscuous mode, hop 1..13, and count how many frames
// land on each channel. The bars show where 2.4 GHz is busy -- find the loud AP or
// pick a clear channel at a glance. Pure RX.
#include "ui.h"
#include "radio.h"

#define NCH 13
static volatile uint32_t cnt[NCH + 1];      // 1..13
static portMUX_TYPE hmux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *bars[NCH + 1];
static lv_obj_t *nums[NCH + 1];
static lv_obj_t *statusLbl = NULL, *busiest = NULL;

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  LV_UNUSED(type);
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  uint8_t ch = pkt->rx_ctrl.channel;
  if (ch >= 1 && ch <= NCH) { portENTER_CRITICAL(&hmux); cnt[ch]++; portEXIT_CRITICAL(&hmux); }
}

void app_heatmap_open(lv_obj_t *body) {
  for (int i = 0; i <= NCH; i++) cnt[i] = 0;

  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statusLbl, "counting frames per channel...");

  lv_obj_t *card = ui_card(body);
  lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);

  lv_obj_t *strip = lv_obj_create(card);
  lv_obj_set_size(strip, LV_PCT(100), 150);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(strip, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

  for (int c = 1; c <= NCH; c++) {
    lv_obj_t *col = lv_obj_create(strip);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b = lv_obj_create(col);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, 3);
    lv_obj_set_style_bg_color(b, COL_BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    bars[c] = b;

    lv_obj_t *n = lv_label_create(col);
    lv_label_set_text_fmt(n, "%d", c);
    lv_obj_set_style_text_color(n, COL_TEXT_2, LV_PART_MAIN);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_12, LV_PART_MAIN);
    nums[c] = n;
  }

  busiest = lv_label_create(card);
  lv_obj_set_style_text_color(busiest, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(busiest, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(busiest, "");

  radio_start(sniff, true);
}

void app_heatmap_tick(void) {
  radio_tick();
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 500) return;
  last = now;

  uint32_t c[NCH + 1]; uint32_t total = 0, mx = 1; int mxc = 1;
  portENTER_CRITICAL(&hmux);
  for (int i = 1; i <= NCH; i++) c[i] = cnt[i];
  portEXIT_CRITICAL(&hmux);
  for (int i = 1; i <= NCH; i++) { total += c[i]; if (c[i] > mx) { mx = c[i]; mxc = i; } }

  for (int i = 1; i <= NCH; i++) {
    int h = 3 + (int)((c[i] * 132UL) / mx);
    if (h > 135) h = 135;
    lv_obj_set_height(bars[i], h);
    bool hot = (c[i] == mx && mx > 1);
    lv_obj_set_style_bg_color(bars[i], hot ? COL_ORANGE : COL_BLUE, LV_PART_MAIN);
  }
  if (statusLbl) lv_label_set_text_fmt(statusLbl, "%lu frames  ·  scanning ch %d",
                                       (unsigned long)total, radio_channel());
  if (busiest) lv_label_set_text_fmt(busiest, "busiest: channel %d", mxc);
}

void app_heatmap_close(void) {
  radio_stop();
  statusLbl = busiest = NULL;
}
