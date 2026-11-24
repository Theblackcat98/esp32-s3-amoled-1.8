# Hardware Notes — Waveshare ESP32-S3-Touch-AMOLED-1.8 (Revision V2)

## 1. V1 vs. V2 Differences

Waveshare transitioned this board to **Hardware Revision V2** in May 2026:
- **Display:** `CO5300` QSPI AMOLED `368×448` (`col_offset1 = 16`, even-pixel `rounder_cb`) — **not** V1 `SH8601`.
- **Touch:** `CST820` on shared I2C (`0x15`, `INT = GPIO21`) — **not** V1 `FT3168` (`0x38`).
- **BSP Component:** `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3` auto-probes `CST816S`/`CST820` at `0x15` vs `FT5x06` at `0x38` and applies `x_gap = 16` when `0x15` responds.

---

## 2. Verified Pin Map & Shared I2C Bus

| Subsystem | Device / Spec | GPIOs / I2C Address | Notes |
| --- | --- | --- | --- |
| **MCU** | ESP32-S3R8 (240 MHz) | 16 MB QIO Flash, 8 MB OPI PSRAM | `qio_opi` @ 80 MHz |
| **QSPI Display** | `CO5300` (368×448 RGB565) | `CS=12`, `SCLK=11`, `D0..D3=4,5,6,7` | Brightness via cmd `0x51` (`0..255`), no PWM pin |
| **Shared I2C (`I2C_NUM_0`)** | 400 kHz Fast Mode | `SDA=15`, `SCL=14` | Shared by all 6 onboard I2C peripherals |
| **Touch** | `CST820` | `0x15`, `INT=21` | Reset via TCA9554 `EXIO2` |
| **Audio Codec + Amp** | `ES8311` + speaker PA | `0x18`, `MCLK=16`, `BCLK=9`, `WS=45`, `DOUT=8`, `DIN=10`, `PA_EN=46` | `PA_EN` active HIGH |
| **I/O Expander** | `TCA9554` | `0x20` | `EXIO0..2` panel/touch resets (LOW 20ms → HIGH 50ms); `EXIO7` TF power |
| **PMIC** | `AXP2101` | `0x34` | Power rails, VBUS, battery gauge, `PWR` button |
| **RTC** | `PCF85063A` | `0x51` | Real-time clock |
| **6-Axis IMU** | `QMI8658` | `0x6B` (alt `0x6A`) | Accel + Gyro |
| **microSD** | 1-bit SDMMC | `CLK=2`, `CMD=1`, `D0=3` | Enable `EXIO7 = HIGH` first |
| **Buttons / USB** | `BOOT` + `PWR` + Native USB | `BOOT=GPIO0`, `USB D-/D+=19/20` | USB-Serial/JTAG console |

Expected I2C bus scan on a healthy V2 unit:
```text
0x15 (CST820)  0x18 (ES8311)  0x20 (TCA9554)  0x34 (AXP2101)  0x51 (PCF85063A)  0x6B (QMI8658)
```

---

## 3. ES8311 Register `0x44 = 0x48` & ESP-SR Channel Order

Running full-duplex AEC without clock drift requires hardware-synchronized playback reference inside the same I2S RX frame:

1. Open the shared I2S channel (`I2S_NUM_0`) in **16 kHz, 16-bit, Stereo (`I2S_SLOT_MODE_STEREO`)** full-duplex mode before initializing `esp_codec_dev`.
2. Write **ES8311 register `0x44` = `0x48`** (`ADCDAT_SEL = 4` = `DACL + ADC`) after `esp_codec_dev_open()`, and read back register `0x44` to confirm `0x48`.
3. Resulting stereo I2S RX frame layout:
   - **Left Channel (even `int16` index `2*i`)** = Exact digital **DAC Loopback (`R`)**
   - **Right Channel (odd `int16` index `2*i + 1`)** = **Microphone ADC (`M`)**
4. **Critical:** Because Left = Reference (`R`) and Right = Mic (`M`), ESP-SR AFE **must** be initialized with `input_format = "RM"` (NOT `"MR"`):
   ```c
   afe_config_t *cfg = afe_config_init("RM", NULL, AFE_TYPE_FD, AFE_MODE_LOW_COST);
   cfg->aec_init = true;
   cfg->aec_mode = AEC_MODE_FD_LOW_COST;
   cfg->wakenet_init = false;
   ```
5. Keep I2S TX continuously fed (either resampled 16 kHz playback or zeros) so the ES8311 DAC reference channel remains valid and clock-locked at all times.

---

## 4. AXP2101 PMIC Safety & Register Map (`0x34`)

**Rule:** Never rewrite AXP2101 LDO/DCDC voltage registers. Factory OTP power rails power the V2 board cleanly on boot.

Read-only telemetry + safe power-key / shutdown registers used in `board_pmu.c`:
- `0x00` (`STATUS1`): Bit 3 (`0x08`) = Battery Present; Bit 5 (`0x20`) = VBUS Good.
- `0x01` (`STATUS2`): Bit 3 == 0 (when VBUS Good) = VBUS In; `(val >> 5) == 0x01` = Charging.
- `0x30` (`ADC_CHANNEL_CTRL`): Read-modify-write to set bits 0 (VBAT), 2 (VBUS), 3 (VSYS).
- `0x68` (`BAT_DET_CTRL`): Read-modify-write bit 0 = 1 (enable battery detection).
- `0x34` / `0x35`: 13-bit VBAT in mV = `((reg34 & 0x1F) << 8) | reg35`.
- `0xA4`: Fuel gauge SoC percentage (`0..100`).
- `0x41` (`INTEN2`) / `0x49` (`INTSTS2`): Bit 3 (`0x08`) = `PWR` short press IRQ, Bit 2 (`0x04`) = `PWR` long press IRQ (clear by writing 1).
- `0x10` (`COMMON_CONFIG`): Set bit 0 (`0x01`) for clean software shutdown (`board_pmu_shutdown()`).

---

## 5. AEC & VAD Tuning Notes

Because the microphone and speaker sit centimeters apart inside the enclosure:
- Start with speaker volume `75%` and mic PGA gain `30 dB`. If the speaker saturates the mic capsule during loud bursts, lower speaker volume (`60%–70%`) or mic gain (`24 dB`) before raising VAD thresholds.
- Dual-threshold barge-in:
  - Normal `LISTENING` onset: `vad_min_speech_ms = 128`, local confirmation holdback = `300 ms`, pre-roll = `350 ms`, end-of-turn silence = `650 ms`.
  - During `SPEAKING` / `THINKING`: require `+6 dB` above noise floor AND `>= 220 ms` continuous speech before firing `response.cancel`.

---

## 6. USB-Serial/JTAG Flashing & Monitor Quirks

- Native USB-C (`GPIO19/20`) handles both flashing and `115200` baud logging.
- If a board is stuck in a crash loop or deep sleep, hold **`BOOT` (`GPIO0`)**, tap **`PWR`** (or replug USB-C), release `BOOT`, then run `pio run -t upload`.
- Always use `pio device monitor` (or `idf.py monitor`) rather than naive serial tools that toggle DTR/RTS and accidentally reset the chip into download mode.
