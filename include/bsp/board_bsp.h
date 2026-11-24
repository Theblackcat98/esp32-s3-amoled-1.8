#pragma once

#include <Arduino.h>

#include "bsp/cst820_touch.h"
#include "bsp/es8311_audio.h"
#include "bsp/tca9554_io.h"

class Arduino_CO5300;

namespace bsp {

struct Telemetry {
  // Memory
  uint32_t flash_mb     = 0;
  uint32_t psram_mb     = 0;
  uint32_t free_heap_kb = 0;

  // Power (AXP2101)
  bool     pmu_online    = false;
  bool     vbus_present  = false;
  bool     batt_present  = false;
  bool     charging      = false;
  uint16_t batt_mv       = 0;
  uint16_t sys_mv        = 0;
  int      batt_pct      = -1;

  // Motion (QMI8658)
  bool  imu_online = false;
  float acc_x      = 0.0f;
  float acc_y      = 0.0f;
  float acc_z      = 0.0f;
  float gyr_x      = 0.0f;
  float gyr_y      = 0.0f;
  float gyr_z      = 0.0f;

  // RTC (PCF85063A)
  bool     rtc_online = false;
  uint16_t year       = 2026;
  uint8_t  month      = 1;
  uint8_t  day        = 1;
  uint8_t  hour       = 0;
  uint8_t  minute     = 0;
  uint8_t  second     = 0;

  // Storage (TF / SD_MMC)
  bool     sd_mounted = false;
  uint32_t sd_size_mb = 0;

  // Peripherals & Buttons
  bool    io_exp_online = false;
  bool    touch_online  = false;
  bool    audio_online  = false;
  bool    boot_pressed  = false;
  uint8_t mic_level_pct = 0;
};

class BoardBsp {
 public:
  bool begin();
  void pollTelemetry(Telemetry &out);

  void setBrightness(uint8_t level);
  uint8_t brightness() const { return brightness_; }

  bool probeSdCard();
  bool playTestChime();

  Arduino_CO5300 *display() { return gfx_; }
  Cst820Touch    &touch() { return touch_; }
  Tca9554Io      &ioExpander() { return io_; }
  Es8311Audio    &audio() { return audio_; }

 private:
  void resetPanelAndTouch();
  bool initPmu();
  bool initImu();
  bool initRtc();

  Arduino_CO5300 *gfx_        = nullptr;
  Tca9554Io       io_;
  Cst820Touch     touch_;
  Es8311Audio     audio_;

  uint8_t  brightness_ = 220;
  bool     pmu_online_ = false;
  bool     imu_online_ = false;
  bool     rtc_online_ = false;
  bool     sd_mounted_ = false;
  uint32_t sd_size_mb_ = 0;
};

BoardBsp &board();

}  // namespace bsp
