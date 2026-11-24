#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace bsp {

// Lightweight I2C driver for the onboard TCA9554 / PCA9554 8-bit GPIO expander (0x20).
class Tca9554Io {
 public:
  bool begin(TwoWire &wire, uint8_t addr = 0x20) {
    wire_ = &wire;
    addr_ = addr;
    if (!readReg(kRegOutput, out_cache_) || !readReg(kRegConfig, cfg_cache_)) {
      online_ = false;
      return false;
    }
    online_ = true;
    return true;
  }

  bool isOnline() const { return online_; }

  bool pinMode(uint8_t pin, uint8_t mode) {
    if (!online_ || pin > 7) return false;
    if (mode == OUTPUT) {
      cfg_cache_ &= static_cast<uint8_t>(~(1U << pin));
    } else {
      cfg_cache_ |= static_cast<uint8_t>(1U << pin);
    }
    return writeReg(kRegConfig, cfg_cache_);
  }

  bool digitalWrite(uint8_t pin, uint8_t level) {
    if (!online_ || pin > 7) return false;
    if (level) {
      out_cache_ |= static_cast<uint8_t>(1U << pin);
    } else {
      out_cache_ &= static_cast<uint8_t>(~(1U << pin));
    }
    return writeReg(kRegOutput, out_cache_);
  }

  int digitalRead(uint8_t pin) {
    if (!online_ || pin > 7) return LOW;
    uint8_t val = 0;
    if (!readReg(kRegInput, val)) return LOW;
    return (val & (1U << pin)) ? HIGH : LOW;
  }

 private:
  static constexpr uint8_t kRegInput  = 0x00;
  static constexpr uint8_t kRegOutput = 0x01;
  static constexpr uint8_t kRegConfig = 0x03;

  bool writeReg(uint8_t reg, uint8_t val) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(val);
    return wire_->endTransmission() == 0;
  }

  bool readReg(uint8_t reg, uint8_t &out) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom(addr_, static_cast<uint8_t>(1)) != 1) return false;
    out = wire_->read();
    return true;
  }

  TwoWire *wire_     = nullptr;
  uint8_t  addr_     = 0x20;
  uint8_t  out_cache_ = 0xFF;
  uint8_t  cfg_cache_ = 0xFF;
  bool     online_   = false;
};

}  // namespace bsp
