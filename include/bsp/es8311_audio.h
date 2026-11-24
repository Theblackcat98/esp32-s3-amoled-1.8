#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace bsp {

class Es8311Audio {
 public:
  bool begin(TwoWire &wire, uint32_t sample_rate = 16000, uint8_t volume_pct = 75);
  bool isOnline() const { return online_; }

  bool setVolume(uint8_t volume_pct);
  uint8_t volume() const { return volume_pct_; }

  void setAmplifierEnabled(bool enabled);

  // Synthesize a short two-note confirmation chime over the onboard speaker.
  bool playChime();

  // Sample microphone input briefly and return normalized 0..100 RMS level.
  uint8_t sampleMicLevel();

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t &val);
  bool initCodec(uint32_t sample_rate);
  bool initI2s(uint32_t sample_rate);
  void playTone(float freq_hz, uint32_t duration_ms, float amplitude = 0.28f);

  TwoWire *wire_        = nullptr;
  uint32_t sample_rate_ = 16000;
  uint8_t  volume_pct_  = 75;
  bool     online_      = false;
  bool     i2s_ready_   = false;
};

}  // namespace bsp
