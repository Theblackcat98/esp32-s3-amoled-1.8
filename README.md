# ESP32-S3-Touch-AMOLED-1.8 (V2) Firmware

Starter firmware and Board Support Package (BSP) for the **Waveshare ESP32-S3-Touch-AMOLED-1.8 V2** development unit.

## Hardware Specifications (Revision V2)

| Subsystem | Part / Interface | Pins / Address |
| --- | --- | --- |
| **MCU** | ESP32-S3R8 Dual-Core Xtensa LX7 @ 240 MHz | Native USB-C (`D-` = GPIO19, `D+` = GPIO20) |
| **Memory** | **16 MB** QIO Flash + **8 MB** Octal (OPI) PSRAM | `qio_opi`, `app3M_fat9M_16MB.csv` |
| **AMOLED Display** | 1.8" `368 × 448` QSPI (**CO5300**) | `CS=12, SCLK=11, SDIO0..3=4,5,6,7`, `col_offset1=16` |
| **Capacitive Touch** | **CST820** (I2C) | `0x15` (`SDA=15, SCL=14, INT=21`) |
| **I/O Expander** | **TCA9554** 8-bit GPIO (I2C) | `0x20` (`EXIO0..2` = Display/Touch reset, `EXIO7` = TF enable) |
| **PMIC** | **AXP2101** Power Management (I2C) | `0x34` (Battery, rails, VBUS, `PWR` button) |
| **6-Axis IMU** | **QMI8658** Accel + Gyro (I2C) | `0x6B` |
| **RTC** | **PCF85063A** Real-Time Clock (I2C) | `0x51` |
| **Audio Codec** | **ES8311** + Onboard Mic & Speaker (I2C + I2S) | `0x18`, `MCLK=16, BCLK=9, WS=45, DOUT=8, DIN=10, PA=46` |
| **TF / MicroSD** | 1-bit SDMMC | `CLK=2, CMD=1, D0=3` + `EXIO7=HIGH` |
| **Buttons** | `BOOT` (`GPIO0`, active-low) + `PWR` (via `AXP2101`) | Side physical buttons |

> **Important V2 Hardware Notes**
> - **V2 vs V1:** Your unit is hardware **V2** (`CO5300` display + `CST820` touch), not V1 (`SH8601` + `FT3168`).
> - **Column Offset & Even Rounding:** `CO5300` at `368×448` requires `col_offset1 = 16` and 2-pixel aligned partial flush coordinates (`lvglRounderCb` in `src/main.cpp`).
> - **Non-blocking USB CDC:** `Serial.setTxTimeoutMs(0)` is set at boot so battery-powered operation never stalls waiting for a host serial monitor.

## Project Layout

```text
├── boards/
│   └── waveshare_esp32_s3_touch_amoled_1_8_v2.json   # Custom PlatformIO board manifest
├── include/
│   ├── board_pins.h          # Complete V2 pinout & I2C address constants
│   ├── lv_conf.h             # LVGL 8.4 config (RGB565 byte-swapped for QSPI DMA)
│   └── bsp/
│       ├── board_bsp.h       # Unified Board Support Package facade
│       ├── cst820_touch.h    # Single-burst CST820 touch driver (0x15)
│       ├── es8311_audio.h    # ES8311 codec + I2S speaker/mic driver (0x18)
│       └── tca9554_io.h      # TCA9554 I/O expander driver (0x20)
├── src/
│   ├── main.cpp              # Entry point + LVGL PSRAM double-buffered port
│   ├── bsp/                  # Hardware bring-up & peripheral polling
│   └── ui/                   # Starter true-black AMOLED hardware dashboard
└── platformio.ini
```

## Build, Flash & Monitor

```bash
# Build firmware
pio run

# Flash over USB-C (plug in normally; if in boot loop, hold BOOT while tapping PWR/plugging in)
pio run -t upload

# Open serial monitor (115200 baud, exception decoder enabled)
pio device monitor
```
