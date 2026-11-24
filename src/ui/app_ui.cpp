#include "app_ui.h"

#include <lvgl.h>
#include <stdio.h>

namespace ui {

namespace {
bsp::BoardBsp *g_board = nullptr;

lv_obj_t *g_clock_lbl = nullptr;
lv_obj_t *g_power_lbl = nullptr;
lv_obj_t *g_mem_lbl   = nullptr;
lv_obj_t *g_imu_lbl   = nullptr;
lv_obj_t *g_bus_lbl   = nullptr;
lv_obj_t *g_touch_lbl = nullptr;
lv_obj_t *g_mic_bar   = nullptr;

lv_obj_t *makeCard(lv_obj_t *parent, lv_coord_t h) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_height(card, h);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x11161F), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x232D3F), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

void onBrightnessChanged(lv_event_t *e) {
  if (!g_board) return;
  lv_obj_t *slider = lv_event_get_target(e);
  g_board->setBrightness(static_cast<uint8_t>(lv_slider_get_value(slider)));
}

void onChimeClicked(lv_event_t * /*e*/) {
  if (g_board) {
    g_board->playTestChime();
  }
}

void onSdProbeClicked(lv_event_t * /*e*/) {
  if (g_board) {
    g_board->probeSdCard();
  }
}
}  // namespace

void init(bsp::BoardBsp &board) {
  g_board = &board;

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);  // True OLED black
  lv_obj_set_style_pad_all(scr, 12, 0);
  lv_obj_set_style_pad_row(scr, 8, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Header Card (Title + RTC + Battery)
  lv_obj_t *hdr = makeCard(scr, 78);
  lv_obj_t *title = lv_label_create(hdr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
  lv_label_set_text(title, "ESP32-S3 AMOLED 1.8 V2");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  g_clock_lbl = lv_label_create(hdr);
  lv_obj_set_style_text_font(g_clock_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_clock_lbl, lv_color_hex(0x94A3B8), 0);
  lv_label_set_text(g_clock_lbl, "RTC --:--:--");
  lv_obj_align(g_clock_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  g_power_lbl = lv_label_create(hdr);
  lv_obj_set_style_text_font(g_power_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_power_lbl, lv_color_hex(0x4ADE80), 0);
  lv_label_set_text(g_power_lbl, "USB");
  lv_obj_align(g_power_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

  // Memory & Storage + Bus Health Card
  lv_obj_t *sys_card = makeCard(scr, 86);
  g_mem_lbl = lv_label_create(sys_card);
  lv_obj_set_style_text_color(g_mem_lbl, lv_color_hex(0xE2E8F0), 0);
  lv_label_set_text(g_mem_lbl, "Flash: 16MB | PSRAM: 8MB | TF: --");
  lv_obj_align(g_mem_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  g_bus_lbl = lv_label_create(sys_card);
  lv_obj_set_style_text_font(g_bus_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(g_bus_lbl, lv_color_hex(0x94A3B8), 0);
  lv_label_set_text(g_bus_lbl, "I2C: IO -- | Touch -- | PMU -- | IMU -- | Audio --");
  lv_obj_align(g_bus_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // 6-Axis IMU + Touch + Mic Card
  lv_obj_t *sensor_card = makeCard(scr, 110);
  g_imu_lbl = lv_label_create(sensor_card);
  lv_obj_set_style_text_color(g_imu_lbl, lv_color_hex(0xF8FAFC), 0);
  lv_label_set_text(g_imu_lbl, "ACC:  0.00,  0.00,  0.00 g\nGYR:   0.0,   0.0,   0.0 dps");
  lv_obj_align(g_imu_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  g_touch_lbl = lv_label_create(sensor_card);
  lv_obj_set_style_text_font(g_touch_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(g_touch_lbl, lv_color_hex(0xFBBF24), 0);
  lv_label_set_text(g_touch_lbl, "Touch: idle");
  lv_obj_align(g_touch_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  g_mic_bar = lv_bar_create(sensor_card);
  lv_obj_set_size(g_mic_bar, 120, 10);
  lv_bar_set_range(g_mic_bar, 0, 100);
  lv_obj_set_style_bg_color(g_mic_bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_mic_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
  lv_obj_align(g_mic_bar, LV_ALIGN_BOTTOM_RIGHT, 0, -2);

  // Brightness Slider Card
  lv_obj_t *bri_card = makeCard(scr, 66);
  lv_obj_t *bri_lbl = lv_label_create(bri_card);
  lv_obj_set_style_text_font(bri_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(bri_lbl, lv_color_hex(0x94A3B8), 0);
  lv_label_set_text(bri_lbl, "AMOLED Brightness");
  lv_obj_align(bri_lbl, LV_ALIGN_TOP_LEFT, 0, -2);

  lv_obj_t *slider = lv_slider_create(bri_card);
  lv_obj_set_width(slider, lv_pct(100));
  lv_slider_set_range(slider, 15, 255);
  lv_slider_set_value(slider, board.brightness(), LV_ANIM_OFF);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_add_event_cb(slider, onBrightnessChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  // Action Buttons Row (Speaker Chime + Re-scan TF Card)
  lv_obj_t *btn_row = lv_obj_create(scr);
  lv_obj_set_size(btn_row, lv_pct(100), 52);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_set_style_pad_all(btn_row, 0, 0);
  lv_obj_set_style_pad_column(btn_row, 10, 0);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *chime_btn = lv_btn_create(btn_row);
  lv_obj_set_height(chime_btn, lv_pct(100));
  lv_obj_set_flex_grow(chime_btn, 1);
  lv_obj_set_style_bg_color(chime_btn, lv_color_hex(0x0284C7), 0);
  lv_obj_set_style_radius(chime_btn, 12, 0);
  lv_obj_add_event_cb(chime_btn, onChimeClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *chime_lbl = lv_label_create(chime_btn);
  lv_label_set_text(chime_lbl, "Speaker Chime");
  lv_obj_center(chime_lbl);

  lv_obj_t *sd_btn = lv_btn_create(btn_row);
  lv_obj_set_height(sd_btn, lv_pct(100));
  lv_obj_set_flex_grow(sd_btn, 1);
  lv_obj_set_style_bg_color(sd_btn, lv_color_hex(0x334155), 0);
  lv_obj_set_style_radius(sd_btn, 12, 0);
  lv_obj_add_event_cb(sd_btn, onSdProbeClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sd_lbl = lv_label_create(sd_btn);
  lv_label_set_text(sd_lbl, "Check TF Card");
  lv_obj_center(sd_lbl);
}

void update(const bsp::Telemetry &t, const bsp::TouchPoint &last_touch) {
  if (!g_clock_lbl) return;

  if (t.rtc_online) {
    lv_label_set_text_fmt(g_clock_lbl, "RTC %04u-%02u-%02u %02u:%02u:%02u",
                          t.year, t.month, t.day, t.hour, t.minute, t.second);
  } else {
    lv_label_set_text(g_clock_lbl, "RTC offline");
  }

  if (t.pmu_online) {
    if (t.batt_present && t.batt_pct >= 0) {
      lv_label_set_text_fmt(g_power_lbl, "%d%% (%u mV)%s",
                            t.batt_pct, t.batt_mv, t.charging ? " CHG" : "");
    } else {
      lv_label_set_text_fmt(g_power_lbl, "%s %u mV",
                            t.vbus_present ? "USB" : "SYS", t.sys_mv);
    }
  } else {
    lv_label_set_text(g_power_lbl, "PMU --");
  }

  if (t.sd_mounted) {
    lv_label_set_text_fmt(g_mem_lbl, "Flash %luM | PSRAM %luM | TF %luM",
                          static_cast<unsigned long>(t.flash_mb),
                          static_cast<unsigned long>(t.psram_mb),
                          static_cast<unsigned long>(t.sd_size_mb));
  } else {
    lv_label_set_text_fmt(g_mem_lbl, "Flash %luM | PSRAM %luM | Heap %luK",
                          static_cast<unsigned long>(t.flash_mb),
                          static_cast<unsigned long>(t.psram_mb),
                          static_cast<unsigned long>(t.free_heap_kb));
  }

  lv_label_set_text_fmt(g_bus_lbl, "IO:%s  TP:%s  PMU:%s  IMU:%s  ES8311:%s",
                        t.io_exp_online ? "OK" : "--",
                        t.touch_online ? "OK" : "--",
                        t.pmu_online ? "OK" : "--",
                        t.imu_online ? "OK" : "--",
                        t.audio_online ? "OK" : "--");

  char imu_buf[96];
  snprintf(imu_buf, sizeof(imu_buf),
           "ACC: %+5.2f, %+5.2f, %+5.2f g\nGYR: %+5.1f, %+5.1f, %+5.1f dps",
           t.acc_x, t.acc_y, t.acc_z, t.gyr_x, t.gyr_y, t.gyr_z);
  lv_label_set_text(g_imu_lbl, imu_buf);

  if (last_touch.pressed) {
    lv_label_set_text_fmt(g_touch_lbl, "Touch: (%u, %u)%s",
                          last_touch.x, last_touch.y, t.boot_pressed ? " [BOOT]" : "");
  } else {
    lv_label_set_text_fmt(g_touch_lbl, "Touch: idle%s", t.boot_pressed ? " [BOOT]" : "");
  }

  lv_bar_set_value(g_mic_bar, t.mic_level_pct, LV_ANIM_ON);
}

}  // namespace ui
