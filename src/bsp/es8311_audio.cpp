#include "bsp/es8311_audio.h"

#include <driver/i2s.h>
#include <math.h>

#include "board_pins.h"

namespace bsp {

namespace {
constexpr i2s_port_t kI2sPort = I2S_NUM_0;

// ES8311 Register Addresses
constexpr uint8_t kRegReset   = 0x00;
constexpr uint8_t kRegClk01   = 0x01;
constexpr uint8_t kRegClk02   = 0x02;
constexpr uint8_t kRegClk03   = 0x03;
constexpr uint8_t kRegClk04   = 0x04;
constexpr uint8_t kRegClk05   = 0x05;
constexpr uint8_t kRegClk06   = 0x06;
constexpr uint8_t kRegClk07   = 0x07;
constexpr uint8_t kRegClk08   = 0x08;
constexpr uint8_t kRegSdpIn   = 0x09;
constexpr uint8_t kRegSdpOut  = 0x0A;
constexpr uint8_t kRegSys0D   = 0x0D;
constexpr uint8_t kRegSys0E   = 0x0E;
constexpr uint8_t kRegSys12   = 0x12;
constexpr uint8_t kRegSys13   = 0x13;
constexpr uint8_t kRegSys14   = 0x14;
constexpr uint8_t kRegAdc16   = 0x16;
constexpr uint8_t kRegAdc17   = 0x17;
constexpr uint8_t kRegAdc1C   = 0x1C;
constexpr uint8_t kRegDac32   = 0x32;
constexpr uint8_t kRegDac37   = 0x37;
}  // namespace

bool Es8311Audio::begin(TwoWire &wire, uint32_t sample_rate, uint8_t volume_pct) {
  wire_        = &wire;
  sample_rate_ = sample_rate;

  pinMode(board_pins::kAudioPaEn, OUTPUT);
  digitalWrite(board_pins::kAudioPaEn, LOW);  // Keep amp muted until I2S + DAC stabilize

  uint8_t probe = 0;
  if (!readReg(kRegReset, probe)) {
    online_ = false;
    return false;
  }

  if (!initI2s(sample_rate_)) {
    online_ = false;
    return false;
  }

  if (!initCodec(sample_rate_)) {
    online_ = false;
    return false;
  }

  setVolume(volume_pct);
  setAmplifierEnabled(true);
  online_ = true;
  return true;
}

bool Es8311Audio::initI2s(uint32_t sample_rate) {
  if (i2s_ready_) {
    i2s_driver_uninstall(kI2sPort);
    i2s_ready_ = false;
  }

  i2s_config_t i2s_cfg = {};
  i2s_cfg.mode                 = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  i2s_cfg.sample_rate          = sample_rate;
  i2s_cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  i2s_cfg.dma_buf_count        = 6;
  i2s_cfg.dma_buf_len          = 256;
  i2s_cfg.use_apll             = false;
  i2s_cfg.tx_desc_auto_clear   = true;
  i2s_cfg.fixed_mclk           = sample_rate * 256;

  if (i2s_driver_install(kI2sPort, &i2s_cfg, 0, nullptr) != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pin_cfg = {};
  pin_cfg.mck_io_num   = board_pins::kI2sMclk;
  pin_cfg.bck_io_num   = board_pins::kI2sBclk;
  pin_cfg.ws_io_num    = board_pins::kI2sWs;
  pin_cfg.data_out_num = board_pins::kI2sDout;
  pin_cfg.data_in_num  = board_pins::kI2sDin;

  if (i2s_set_pin(kI2sPort, &pin_cfg) != ESP_OK) {
    i2s_driver_uninstall(kI2sPort);
    return false;
  }

  i2s_zero_dma_buffer(kI2sPort);
  i2s_ready_ = true;
  return true;
}

bool Es8311Audio::initCodec(uint32_t /*sample_rate*/) {
  // Soft reset and power-up sequence (16 kHz / MCLK = 256 * Fs = 4.096 MHz, 16-bit I2S slave)
  if (!writeReg(kRegReset, 0x1F)) return false;
  delay(5);
  if (!writeReg(kRegReset, 0x00)) return false;
  if (!writeReg(kRegReset, 0x80)) return false;

  // Clock manager: MCLK from pin, 256*Fs dividers (pre_div=1, mult=1x, adc/dac_div=1, bclk_div=4, osr=0x10)
  writeReg(kRegClk01, 0x3F);
  writeReg(kRegClk02, 0x00);
  writeReg(kRegClk03, 0x10);
  writeReg(kRegClk04, 0x10);
  writeReg(kRegClk05, 0x00);
  writeReg(kRegClk06, 0x03);
  writeReg(kRegClk07, 0x00);
  writeReg(kRegClk08, 0xFF);

  // Slave mode, 16-bit I2S SDP in/out (3 << 2 = 0x0C)
  uint8_t reg00 = 0;
  if (readReg(kRegReset, reg00)) {
    writeReg(kRegReset, static_cast<uint8_t>(reg00 & 0xBF));
  }
  writeReg(kRegSdpIn, 0x0C);
  writeReg(kRegSdpOut, 0x0C);

  // Power up analog circuitry, PGA, ADC modulator, DAC, and headphone/line driver
  writeReg(kRegSys0D, 0x01);
  writeReg(kRegSys0E, 0x02);
  writeReg(kRegSys12, 0x00);
  writeReg(kRegSys13, 0x10);
  writeReg(kRegAdc1C, 0x6A);
  writeReg(kRegDac37, 0x08);

  // Enable onboard analog microphone + moderate ADC gain
  writeReg(kRegAdc17, 0xC8);
  writeReg(kRegSys14, 0x1A);
  writeReg(kRegAdc16, 0x03);
  return true;
}

bool Es8311Audio::setVolume(uint8_t volume_pct) {
  if (volume_pct > 100) volume_pct = 100;
  volume_pct_ = volume_pct;
  const uint8_t reg_val = (volume_pct == 0)
                              ? 0
                              : static_cast<uint8_t>((static_cast<uint16_t>(volume_pct) * 256U / 100U) - 1U);
  return writeReg(kRegDac32, reg_val);
}

void Es8311Audio::setAmplifierEnabled(bool enabled) {
  digitalWrite(board_pins::kAudioPaEn, enabled ? HIGH : LOW);
}

void Es8311Audio::playTone(float freq_hz, uint32_t duration_ms, float amplitude) {
  if (!online_ || !i2s_ready_) return;

  constexpr size_t kChunkFrames = 128;
  int16_t stereo_buf[kChunkFrames * 2];
  const uint32_t total_frames = (sample_rate_ * duration_ms) / 1000U;
  uint32_t frames_sent        = 0;
  const float two_pi_f        = 2.0f * static_cast<float>(M_PI) * freq_hz / static_cast<float>(sample_rate_);

  while (frames_sent < total_frames) {
    const size_t n = min<size_t>(kChunkFrames, total_frames - frames_sent);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t idx = frames_sent + i;
      // Smooth attack/release envelope to avoid speaker clicks
      float env = 1.0f;
      if (idx < 160) env = static_cast<float>(idx) / 160.0f;
      if (total_frames - idx < 240) env = min(env, static_cast<float>(total_frames - idx) / 240.0f);

      const int16_t sample   = static_cast<int16_t>(sinf(two_pi_f * static_cast<float>(idx)) * 32767.0f * amplitude * env);
      stereo_buf[i * 2]      = sample;
      stereo_buf[i * 2 + 1]  = sample;
    }
    size_t bytes_written = 0;
    i2s_write(kI2sPort, stereo_buf, n * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(50));
    frames_sent += n;
  }
}

bool Es8311Audio::playChime() {
  if (!online_ || !i2s_ready_) return false;
  playTone(659.25f, 75, 0.25f);  // E5
  playTone(880.00f, 110, 0.25f); // A5
  i2s_zero_dma_buffer(kI2sPort);
  return true;
}

uint8_t Es8311Audio::sampleMicLevel() {
  if (!online_ || !i2s_ready_) return 0;

  int16_t buf[128];
  size_t bytes_read = 0;
  if (i2s_read(kI2sPort, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(10)) != ESP_OK || bytes_read < 4) {
    return 0;
  }

  const size_t samples = bytes_read / sizeof(int16_t);
  uint64_t sum_sq      = 0;
  size_t   used        = 0;
  for (size_t i = 0; i < samples; i += 2) {
    const int32_t s = buf[i];
    sum_sq += static_cast<uint64_t>(s * s);
    ++used;
  }
  if (used == 0) return 0;

  const float rms  = sqrtf(static_cast<float>(sum_sq) / static_cast<float>(used));
  const float norm = min(100.0f, (rms / 6000.0f) * 100.0f);
  return static_cast<uint8_t>(norm);
}

bool Es8311Audio::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(board_pins::kI2cAddrCodecEs8311);
  wire_->write(reg);
  wire_->write(val);
  return wire_->endTransmission() == 0;
}

bool Es8311Audio::readReg(uint8_t reg, uint8_t &val) {
  wire_->beginTransmission(board_pins::kI2cAddrCodecEs8311);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom(board_pins::kI2cAddrCodecEs8311, static_cast<uint8_t>(1)) != 1) return false;
  val = wire_->read();
  return true;
}

}  // namespace bsp
