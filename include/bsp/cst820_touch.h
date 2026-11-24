#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace bsp {

struct TouchPoint {
  bool     pressed   = false;
  uint8_t  fingers   = 0;
  uint8_t  gesture   = 0;
  uint16_t x         = 0;
  uint16_t y         = 0;
};

// Fast single-burst I2C driver for the V2 CST820 capacitive touch controller (0x15).
class Cst820Touch {
 public:
  bool begin(TwoWire &wire, uint8_t addr = 0x15, int8_t int_pin = -1) {
    wire_    = &wire;
    addr_    = addr;
    int_pin_ = int_pin;

    if (int_pin_ >= 0) {
      pinMode(int_pin_, INPUT_PULLUP);
    }

    // Probe device ID register (0xA7 -> 0xB7 on CST820; CST816 family returns 0xB4..0xB6).
    uint8_t chip_id = 0;
    if (!readBytes(kRegChipId, &chip_id, 1)) {
      online_ = false;
      return false;
    }
    chip_id_ = chip_id;

    // Disable auto-sleep during active UI and enable periodic touch interrupt pulses.
    writeReg(kRegDisAutoSleep, 0xFF);
    writeReg(kRegIrqCtl, 0x60);

    online_ = true;
    return true;
  }

  bool isOnline() const { return online_; }
  uint8_t chipId() const { return chip_id_; }

  bool read(TouchPoint &pt, uint16_t max_w = 368, uint16_t max_h = 448) {
    pt = TouchPoint{};
    if (!online_) return false;

    uint8_t buf[6] = {0};
    if (!readBytes(kRegGestureId, buf, sizeof(buf))) {
      return false;
    }

    const uint8_t fingers = buf[1] & 0x0F;
    pt.gesture            = buf[0];
    pt.fingers            = fingers;

    if (fingers == 0 || fingers > 2) {
      pt.pressed = false;
      return true;
    }

    uint16_t raw_x = static_cast<uint16_t>(((buf[2] & 0x0F) << 8) | buf[3]);
    uint16_t raw_y = static_cast<uint16_t>(((buf[4] & 0x0F) << 8) | buf[5]);

    if (raw_x >= max_w) raw_x = max_w - 1;
    if (raw_y >= max_h) raw_y = max_h - 1;

    pt.pressed = true;
    pt.x       = raw_x;
    pt.y       = raw_y;
    return true;
  }

 private:
  static constexpr uint8_t kRegGestureId    = 0x01;
  static constexpr uint8_t kRegChipId       = 0xA7;
  static constexpr uint8_t kRegIrqCtl       = 0xFA;
  static constexpr uint8_t kRegDisAutoSleep = 0xFE;

  bool writeReg(uint8_t reg, uint8_t val) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(val);
    return wire_->endTransmission() == 0;
  }

  bool readBytes(uint8_t reg, uint8_t *buf, size_t len) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom(addr_, static_cast<uint8_t>(len)) != len) return false;
    for (size_t i = 0; i < len; ++i) {
      buf[i] = wire_->read();
    }
    return true;
  }

  TwoWire *wire_    = nullptr;
  uint8_t  addr_    = 0x15;
  int8_t   int_pin_ = -1;
  uint8_t  chip_id_ = 0;
  bool     online_  = false;
};

}  // namespace bsp
