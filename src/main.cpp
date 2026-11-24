#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board_pins.h"
#include "bsp/board_bsp.h"
#include "ui/app_ui.h"

namespace {

constexpr uint32_t kDrawBufLines = board_pins::kLcdHeight / 2;
constexpr uint32_t kDrawBufPx    = board_pins::kLcdWidth * kDrawBufLines;

lv_disp_draw_buf_t g_draw_buf;
lv_color_t        *g_buf1 = nullptr;
lv_color_t        *g_buf2 = nullptr;
bsp::TouchPoint    g_last_touch{};

void lvglFlushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  auto *gfx = bsp::board().display();
  if (gfx) {
    const uint32_t w = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t h = static_cast<uint32_t>(area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(&color_p->full), w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(&color_p->full), w, h);
#endif
  }
  lv_disp_flush_ready(disp);
}

// CO5300 AMOLED requires even-aligned CASET/RASET window coordinates on partial updates.
void lvglRounderCb(lv_disp_drv_t * /*disp_drv*/, lv_area_t *area) {
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

void lvglTouchReadCb(lv_indev_drv_t * /*indev_drv*/, lv_indev_data_t *data) {
  bsp::TouchPoint pt{};
  if (bsp::board().touch().read(pt, board_pins::kLcdWidth, board_pins::kLcdHeight) && pt.pressed) {
    g_last_touch  = pt;
    data->state   = LV_INDEV_STATE_PR;
    data->point.x = static_cast<lv_coord_t>(pt.x);
    data->point.y = static_cast<lv_coord_t>(pt.y);
  } else {
    g_last_touch.pressed = false;
    data->state          = LV_INDEV_STATE_REL;
  }
}

bool initLvglPort() {
  lv_init();

  // Allocate double draw buffers in 8MB Octal PSRAM (falls back to heap if PSRAM disabled).
  g_buf1 = static_cast<lv_color_t *>(heap_caps_malloc(kDrawBufPx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_buf2 = static_cast<lv_color_t *>(heap_caps_malloc(kDrawBufPx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!g_buf1) {
    g_buf1 = static_cast<lv_color_t *>(malloc(board_pins::kLcdWidth * 40 * sizeof(lv_color_t)));
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, nullptr, board_pins::kLcdWidth * 40);
  } else {
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, g_buf2, kDrawBufPx);
  }

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res    = board_pins::kLcdWidth;
  disp_drv.ver_res    = board_pins::kLcdHeight;
  disp_drv.flush_cb   = lvglFlushCb;
  disp_drv.rounder_cb = lvglRounderCb;
  disp_drv.draw_buf   = &g_draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvglTouchReadCb;
  lv_indev_drv_register(&indev_drv);

  return g_buf1 != nullptr;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // Never block firmware execution if USB serial monitor isn't open
  delay(150);

  Serial.println("\n=== Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 ===");
  Serial.printf("Flash: %u MB | PSRAM: %u MB | CPU: %u MHz\n",
                ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / (1024 * 1024),
                ESP.getCpuFreqMHz());

  if (!bsp::board().begin()) {
    Serial.println("[FATAL] Board BSP initialization failed!");
  }

  if (!initLvglPort()) {
    Serial.println("[FATAL] LVGL port initialization failed!");
  }

  ui::init(bsp::board());
}

void loop() {
  lv_timer_handler();

  static uint32_t       last_telemetry_ms = 0;
  static bsp::Telemetry telemetry{};

  const uint32_t now = millis();
  if (now - last_telemetry_ms >= 100) {
    last_telemetry_ms = now;
    bsp::board().pollTelemetry(telemetry);
    ui::update(telemetry, g_last_touch);
  }

  delay(4);
}
