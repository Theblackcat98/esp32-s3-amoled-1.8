#include "bsp/board_bsp.h"

#include <Arduino_GFX_Library.h>
#include <SD_MMC.h>
#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <SensorPCF85063.hpp>
#include <SensorQMI8658.hpp>

#include "board_pins.h"

namespace bsp {

namespace {
XPowersPMU     g_pmu;
SensorQMI8658  g_imu;
SensorPCF85063 g_rtc;
BoardBsp       g_board;
}  // namespace

BoardBsp &board() { return g_board; }

void BoardBsp::resetPanelAndTouch() {
  if (!io_.isOnline()) return;

  io_.pinMode(board_pins::kExioPanelReset0, OUTPUT);
  io_.pinMode(board_pins::kExioPanelReset1, OUTPUT);
  io_.pinMode(board_pins::kExioTouchReset, OUTPUT);

  io_.digitalWrite(board_pins::kExioPanelReset0, LOW);
  io_.digitalWrite(board_pins::kExioPanelReset1, LOW);
  io_.digitalWrite(board_pins::kExioTouchReset, LOW);
  delay(20);
  io_.digitalWrite(board_pins::kExioPanelReset0, HIGH);
  io_.digitalWrite(board_pins::kExioPanelReset1, HIGH);
  io_.digitalWrite(board_pins::kExioTouchReset, HIGH);
  delay(50);
}

bool BoardBsp::initPmu() {
  if (!g_pmu.begin(Wire, board_pins::kI2cAddrPmuAxp2101, board_pins::kI2cSda, board_pins::kI2cScl)) {
    return false;
  }
  // Enable non-intrusive telemetry ADCs without altering factory OTP power rails.
  g_pmu.enableBattDetection();
  g_pmu.enableVbusVoltageMeasure();
  g_pmu.enableBattVoltageMeasure();
  g_pmu.enableSystemVoltageMeasure();
  return true;
}

bool BoardBsp::initImu() {
  if (!g_imu.begin(Wire, board_pins::kI2cAddrImuQmi8658L, board_pins::kI2cSda, board_pins::kI2cScl) &&
      !g_imu.begin(Wire, board_pins::kI2cAddrImuQmi8658H, board_pins::kI2cSda, board_pins::kI2cScl)) {
    return false;
  }
  g_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_250Hz, SensorQMI8658::LPF_MODE_0);
  g_imu.configGyroscope(SensorQMI8658::GYR_RANGE_512DPS, SensorQMI8658::GYR_ODR_224_2Hz, SensorQMI8658::LPF_MODE_3);
  g_imu.enableAccelerometer();
  g_imu.enableGyroscope();
  return true;
}

bool BoardBsp::initRtc() {
  return g_rtc.begin(Wire, board_pins::kI2cSda, board_pins::kI2cScl);
}

bool BoardBsp::probeSdCard() {
  if (io_.isOnline()) {
    io_.pinMode(board_pins::kExioSdCardEn, OUTPUT);
    io_.digitalWrite(board_pins::kExioSdCardEn, HIGH);
    delay(20);
  }

  SD_MMC.setPins(board_pins::kSdmmcClk, board_pins::kSdmmcCmd, board_pins::kSdmmcData);
  if (!SD_MMC.begin("/sdcard", true /* mode1bit */, false /* format_if_mount_failed */)) {
    sd_mounted_ = false;
    sd_size_mb_ = 0;
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    sd_mounted_ = false;
    sd_size_mb_ = 0;
    return false;
  }

  sd_mounted_ = true;
  sd_size_mb_ = static_cast<uint32_t>(SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}

bool BoardBsp::begin() {
  pinMode(board_pins::kBootButton, INPUT_PULLUP);

  Wire.begin(board_pins::kI2cSda, board_pins::kI2cScl, board_pins::kI2cFreqHz);

  // 1. TCA9554 I/O expander -> pulse AMOLED & CST820 reset lines
  if (io_.begin(Wire, board_pins::kI2cAddrIoTca9554)) {
    resetPanelAndTouch();
  } else {
    Serial.println("[BSP] WARN: TCA9554 (0x20) not detected");
  }

  // 2. Power Management (AXP2101)
  pmu_online_ = initPmu();
  Serial.printf("[BSP] AXP2101 PMIC (0x34): %s\n", pmu_online_ ? "OK" : "NOT FOUND");

  // 3. CO5300 1.8" 368x448 QSPI AMOLED (V2 requires col_offset1 = 16)
  auto *bus = new Arduino_ESP32QSPI(
      board_pins::kLcdCs,
      board_pins::kLcdSclk,
      board_pins::kLcdSdio0,
      board_pins::kLcdSdio1,
      board_pins::kLcdSdio2,
      board_pins::kLcdSdio3);

  gfx_ = new Arduino_CO5300(
      bus,
      GFX_NOT_DEFINED,
      0 /* rotation */,
      false /* ips */,
      board_pins::kLcdWidth,
      board_pins::kLcdHeight,
      board_pins::kLcdColOffset1,
      board_pins::kLcdRowOffset1,
      board_pins::kLcdColOffset2,
      board_pins::kLcdRowOffset2);

  if (!gfx_->begin()) {
    Serial.println("[BSP] ERROR: CO5300 AMOLED init failed");
    return false;
  }
  gfx_->fillScreen(BLACK);
  gfx_->setBrightness(brightness_);
  Serial.println("[BSP] CO5300 368x448 QSPI AMOLED: OK");

  // 4. CST820 Capacitive Touch (0x15)
  const bool touch_ok = touch_.begin(Wire, board_pins::kI2cAddrTouchCst820, board_pins::kTouchInt);
  Serial.printf("[BSP] CST820 Touch (0x15, id=0x%02X): %s\n", touch_.chipId(), touch_ok ? "OK" : "NOT FOUND");

  // 5. QMI8658 6-Axis IMU (0x6B) & PCF85063A RTC (0x51)
  imu_online_ = initImu();
  rtc_online_ = initRtc();
  Serial.printf("[BSP] QMI8658 IMU: %s | PCF85063 RTC: %s\n",
                imu_online_ ? "OK" : "NOT FOUND",
                rtc_online_ ? "OK" : "NOT FOUND");

  // 6. ES8311 Audio Codec + Mic + Speaker Amp
  const bool audio_ok = audio_.begin(Wire, 16000, 75);
  Serial.printf("[BSP] ES8311 Codec (0x18): %s\n", audio_ok ? "OK" : "NOT FOUND");

  // 7. Non-blocking TF / MicroSD probe
  probeSdCard();
  Serial.printf("[BSP] TF Card (SDMMC): %s (%lu MB)\n",
                sd_mounted_ ? "MOUNTED" : "NONE",
                static_cast<unsigned long>(sd_size_mb_));

  return true;
}

void BoardBsp::setBrightness(uint8_t level) {
  brightness_ = level;
  if (gfx_) {
    gfx_->setBrightness(level);
  }
}

bool BoardBsp::playTestChime() {
  return audio_.playChime();
}

void BoardBsp::pollTelemetry(Telemetry &out) {
  out.flash_mb      = ESP.getFlashChipSize() / (1024UL * 1024UL);
  out.psram_mb      = ESP.getPsramSize() / (1024UL * 1024UL);
  out.free_heap_kb  = ESP.getFreeHeap() / 1024UL;
  out.io_exp_online = io_.isOnline();
  out.touch_online  = touch_.isOnline();
  out.audio_online  = audio_.isOnline();
  out.boot_pressed  = (digitalRead(board_pins::kBootButton) == LOW);
  out.sd_mounted    = sd_mounted_;
  out.sd_size_mb    = sd_size_mb_;

  out.pmu_online = pmu_online_;
  if (pmu_online_) {
    out.vbus_present = g_pmu.isVbusIn();
    out.batt_present = g_pmu.isBatteryConnect();
    out.charging     = g_pmu.isCharging();
    out.batt_mv      = g_pmu.getBattVoltage();
    out.sys_mv       = g_pmu.getSystemVoltage();
    out.batt_pct     = out.batt_present ? g_pmu.getBatteryPercent() : -1;
  }

  out.imu_online = imu_online_;
  if (imu_online_ && g_imu.getDataReady()) {
    IMUdata acc{}, gyr{};
    if (g_imu.getAccelerometer(acc.x, acc.y, acc.z)) {
      out.acc_x = acc.x;
      out.acc_y = acc.y;
      out.acc_z = acc.z;
    }
    if (g_imu.getGyroscope(gyr.x, gyr.y, gyr.z)) {
      out.gyr_x = gyr.x;
      out.gyr_y = gyr.y;
      out.gyr_z = gyr.z;
    }
  }

  out.rtc_online = rtc_online_;
  if (rtc_online_) {
    RTC_DateTime dt = g_rtc.getDateTime();
    out.year        = dt.getYear();
    out.month       = dt.getMonth();
    out.day         = dt.getDay();
    out.hour        = dt.getHour();
    out.minute      = dt.getMinute();
    out.second      = dt.getSecond();
  }

  if (audio_.isOnline()) {
    out.mic_level_pct = audio_.sampleMicLevel();
  }
}

}  // namespace bsp
