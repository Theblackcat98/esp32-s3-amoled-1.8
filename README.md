# ESP32-S3-Touch-AMOLED-1.8 (V2) — Full-Duplex Voice Agent

Full-duplex realtime voice agent and Board Support Package (BSP) for the **Waveshare ESP32-S3-Touch-AMOLED-1.8 V2** development unit, targeting a LAN `llama-server` (`LiquidAI/LFM3-Audio-2B`) over `/v1/realtime` WebSockets.

## Repository Overview

- `firmware/` — **Primary ESP-IDF v5.5.5 Voice Agent firmware** (PlatformIO + `pioarduino`, Waveshare V2 BSP, ESP-SR full-duplex AEC + WebRTC NS/VAD, live speaker volume + high-volume echo guard, remappable hardware buttons, LVGL 9 true-black Editorial Teleprompter multi-turn UI).
- `server/` — Host `llama-server` launcher (`run-llama-server.sh`), Python CLI test client (`test_realtime_cli.py`), stdlib offline `/v1/realtime` mock server (`mock_realtime_server.py`), and Ember system prompt.
- `docs/` — Wire protocol specification (`docs/protocol.md`) and V2 hardware/AEC tuning notes (`docs/hardware-v2-notes.md`).
- `harness/` — Phase 2 LAN Agent Harness boundary specification (`harness/README.md`).
- `src/` + `include/` + `platformio.ini` — Root Arduino + LVGL 8.4 V2 hardware diagnostic starter.

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
├── docs/
│   ├── protocol.md           # OpenAI Realtime GA + harness.* JSON schemas
│   └── hardware-v2-notes.md  # V2 pinout, AXP2101 map, ES8311 reg 0x44 & AEC tuning
├── server/
│   ├── run-llama-server.sh   # Reproducible LFM3-Audio-2B llama-server launcher
│   ├── mock_realtime_server.py # Stdlib offline /v1/realtime GA mock server
│   ├── test_realtime_cli.py  # Desktop test client (--synthetic / --wav / --mic)
│   └── prompts/default_agent.txt
├── harness/
│   └── README.md             # Phase 2 LAN Agent Harness boundary contract
├── firmware/                 # Primary ESP-IDF v5.5 Voice Agent (pioarduino)
│   ├── platformio.ini        # src_dir = main, framework = espidf
│   ├── sdkconfig.defaults    # PSRAM OPI, ESP-SR, LVGL 9, Wi-Fi tuning
│   ├── partitions.csv        # 16 MB flash partition table
│   ├── test_host/            # Host C unit tests for resampler & ring buffer
│   └── main/                 # board/, audio/, net/, session/, ui/
├── include/ & src/           # Root Arduino + LVGL 8.4 V2 diagnostic starter
└── platformio.ini
```

## Build, Configure & Test

### 1. Voice Agent Firmware (`firmware/`)

```bash
# Copy local Wi-Fi / LAN server template (gitignored)
cp firmware/main/local_config.h.example firmware/main/local_config.h
# Edit SSID, password, LAN server IP (default 192.168.1.199:8082), optional API key

# Build ESP-IDF 5.5 Voice Agent firmware
cd firmware && pio run

# Flash & monitor over USB-C
pio run -t upload && pio device monitor
```

### 2. Server & Offline Verification

```bash
# Run offline mock server + synthetic client verification
python3 server/mock_realtime_server.py --port 18082 &
python3 server/test_realtime_cli.py --url http://127.0.0.1:18082 --synthetic

# Run host DSP & echo-guard unit tests (24k->16k resampler, ring buffer, high-vol guard)
gcc -Wall -Wextra -Werror -O2 -I firmware/main/audio \
  firmware/test_host/test_audio_dsp.c \
  firmware/main/audio/audio_resampler.c \
  firmware/main/audio/audio_ringbuf.c \
  firmware/main/audio/audio_guard.c -lm -o /tmp/test_audio_dsp && /tmp/test_audio_dsp

# Launch live LFM3-Audio-2B server on LAN (manual GPU run)
./server/run-llama-server.sh
```

### 3. On-Device Volume, Button Remapping & High-Volume Echo Guard

- **Speaker Volume & Settings:** Tap the top-bar volume pill (`75%`, enlarged `94×48` touch hitbox) or `Setup` in `READY` to adjust live **ES8311** speaker volume (`0–100%` via slider or `±5%` buttons, persisted in NVS), Echo Guard, and button bindings.
- **High-Volume Echo Guard (`Auto` / `Strict` / `Always`):** Prevents loud speaker playback and acoustic hangover tails from interrupting the agent's own voice:
  - **Auto (default):** Uses a dynamic speaker-coupled echo gate below `80%` volume and automatically enables a half-duplex shield at/above `80%` (configurable `60–100%`).
  - **Strict:** Always shields the mic from playback self-triggers at any volume (interrupt via center `Interrupt` pill or mapped `Interrupt` button).
  - **Always:** Forces full-duplex voice barge-in at all volumes with aggressive volume-scaled echo gating.
- **Button Remapping:** Open **Settings -> Buttons** to assign `BOOT Short`, `BOOT Long`, `PWR Short`, or `PWR Long` to `Talk / Mute`, `Start / Stop`, `Mute Mic`, `Interrupt`, `Vol +10%`, `Vol -10%`, `Echo Guard`, `Dim Screen`, `Power Off`, or `None`.

### 4. Root Hardware Diagnostic Starter

```bash
pio run
pio run -t upload
pio device monitor
```
