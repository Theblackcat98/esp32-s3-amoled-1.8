#pragma once

#include <stdint.h>

// ============================================================================
// Waveshare ESP32-S3-Touch-AMOLED-1.8 — Hardware Revision V2 Pin & Bus Map
// ============================================================================
// Verified against Waveshare official V2 hardware specs:
//   - Display:     1.8" 368x448 QSPI AMOLED (CO5300, col_offset = 16)
//   - Touch:       CST820 Capacitive Touch (I2C 0x15, INT = GPIO21)
//   - IO Expander: TCA9554 (I2C 0x20)
//   - PMIC:        AXP2101 (I2C 0x34)
//   - 6-Axis IMU:  QMI8658 (I2C 0x6B)
//   - RTC:         PCF85063A (I2C 0x51)
//   - Audio Codec: ES8311 (I2C 0x18 + I2S + PA = GPIO46)
//   - Storage:     TF / MicroSD Slot (1-bit SDMMC + EXIO7)
// ============================================================================

namespace board_pins {

// --- 1.8" CO5300 QSPI AMOLED Display (368 x 448) ----------------------------
constexpr uint16_t kLcdWidth      = 368;
constexpr uint16_t kLcdHeight     = 448;
constexpr uint8_t  kLcdColOffset1 = 16;   // Required on V2 CO5300 368px panel
constexpr uint8_t  kLcdRowOffset1 = 0;
constexpr uint8_t  kLcdColOffset2 = 0;
constexpr uint8_t  kLcdRowOffset2 = 0;

constexpr int8_t kLcdCs    = 12;
constexpr int8_t kLcdSclk  = 11;
constexpr int8_t kLcdSdio0 = 4;
constexpr int8_t kLcdSdio1 = 5;
constexpr int8_t kLcdSdio2 = 6;
constexpr int8_t kLcdSdio3 = 7;

// --- Shared Onboard I2C Bus -------------------------------------------------
constexpr int8_t   kI2cSda    = 15;
constexpr int8_t   kI2cScl    = 14;
constexpr uint32_t kI2cFreqHz = 400000;

// 7-bit I2C Device Addresses on GPIO15/GPIO14
constexpr uint8_t kI2cAddrTouchCst820  = 0x15;
constexpr uint8_t kI2cAddrCodecEs8311  = 0x18;
constexpr uint8_t kI2cAddrIoTca9554    = 0x20;
constexpr uint8_t kI2cAddrPmuAxp2101   = 0x34;
constexpr uint8_t kI2cAddrRtcPcf85063  = 0x51;
constexpr uint8_t kI2cAddrImuQmi8658L  = 0x6B;
constexpr uint8_t kI2cAddrImuQmi8658H  = 0x6A;

// --- Capacitive Touch (CST820) ----------------------------------------------
constexpr int8_t kTouchInt = 21;

// --- TCA9554 I/O Expander Channels (EXIO0..EXIO7) ---------------------------
// V2 pulses EXIO0..EXIO2 LOW -> HIGH to reset/enable AMOLED + Touch, and drives
// EXIO7 HIGH to enable the microSD / TF card slot.
constexpr uint8_t kExioPanelReset0 = 0;
constexpr uint8_t kExioPanelReset1 = 1;
constexpr uint8_t kExioTouchReset  = 2;
constexpr uint8_t kExioSdCardEn    = 7;

// --- ES8311 Audio Codec + Onboard Mic & Speaker Amp -------------------------
constexpr int8_t kI2sMclk   = 16;
constexpr int8_t kI2sBclk   = 9;
constexpr int8_t kI2sWs     = 45;
constexpr int8_t kI2sDout   = 8;   // ESP32-S3 -> ES8311 DAC (Speaker)
constexpr int8_t kI2sDin    = 10;  // ES8311 ADC (Mic) -> ESP32-S3
constexpr int8_t kAudioPaEn = 46;  // Speaker Power Amplifier Enable (Active HIGH)

// --- TF / MicroSD Card (1-bit SDMMC) ----------------------------------------
constexpr int8_t kSdmmcClk  = 2;
constexpr int8_t kSdmmcCmd  = 1;
constexpr int8_t kSdmmcData = 3;

// --- Buttons & Native USB ---------------------------------------------------
constexpr int8_t kBootButton = 0;   // Active LOW BOOT button (PWR is routed via AXP2101)
constexpr int8_t kUsbDm      = 19;
constexpr int8_t kUsbDp      = 20;

}  // namespace board_pins
