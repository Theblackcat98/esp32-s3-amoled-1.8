# AGENTS.md — Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2)

## Target Hardware Truth (Do Not Mix Up With V1)

This repository targets **Hardware Revision V2** of the Waveshare ESP32-S3-Touch-AMOLED-1.8:
- **MCU / Memory:** ESP32-S3R8 (`240 MHz`), `16 MB` QIO Flash, `8 MB` Octal (`qio_opi`) PSRAM (`boards/waveshare_esp32_s3_touch_amoled_1_8_v2.json`).
- **Display:** `CO5300` QSPI `368×448` AMOLED (`CS=12, SCLK=11, SDIO0..3=4,5,6,7`). **Must** pass `col_offset1 = 16` and keep LVGL partial redraw areas aligned to even pixel boundaries (`rounder_cb`). Note that `Arduino_GFX @ 1.4.9` takes `(bus, rst, rotation, ips, width, height, col_offset1, ...)`.
- **Touch:** `CST820` on I2C `0x15` (`SDA=15, SCL=14, INT=21`), **not** V1's `FT3168`.
- **I/O Expander:** `TCA9554` on I2C `0x20`. Pulse `EXIO0, EXIO1, EXIO2` LOW -> HIGH before display/touch init; drive `EXIO7` HIGH before `SD_MMC.begin()`.
- **Shared I2C Bus (`SDA=15, SCL=14`):** `0x15` (`CST820`), `0x18` (`ES8311`), `0x20` (`TCA9554`), `0x34` (`AXP2101`), `0x51` (`PCF85063A`), `0x6B` (`QMI8658`). Always share `Wire` — never install a competing raw IDF I2C driver on port 0.
- **PMIC Safety:** Do **not** blindly rewrite AXP2101 LDO voltages; factory OTP rails power the board cleanly at boot.
- **USB Serial Safety:** Always keep `Serial.setTxTimeoutMs(0)` enabled when `ARDUINO_USB_CDC_ON_BOOT=1` so standalone battery operation never blocks on `Serial.print`.

## Architecture & Build Conventions

This repository contains two complementary firmware targets plus host tooling:

1. **Primary Voice Agent Firmware (`firmware/`, ESP-IDF 5.5.5 via PlatformIO + `pioarduino`):**
   - Configured in `firmware/platformio.ini` (`src_dir = main`, `framework = espidf`, `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3`, `espressif/esp-sr ^2.6.2`, `espressif/esp_websocket_client ^1.8.0`, `lvgl/lvgl ^9.2.0`).
   - Verify compilation with `cd firmware && pio run`.
   - ES8311 reg `0x44 = 0x48` outputs stereo `(L = DAC_ref, R = ADC_mic)`, so ESP-SR AFE **must** use `input_format = "RM"` (`AFE_TYPE_FD`, `AEC_MODE_FD_LOW_COST`, `models = NULL`).
   - OpenAI Realtime GA WebSocket client uses `response.output_audio.delta` / `response.output_text.delta` (never beta `response.audio.*`), filters by `response_id`, and keeps audio/WS buffers in Octal PSRAM.
   - UI uses a true-black AMOLED **Editorial Teleprompter (Variant 2B)** theme with **zero cyan anywhere** and pure semantic `app_state_t` colors (no Orb Customizer).
   - Never start `llama-server` / `lfm3.sh` in automated tests; test server protocol against `server/mock_realtime_server.py`.

2. **Root Hardware Bring-Up Diagnostic (`platformio.ini`, Arduino + LVGL 8.4):**
   - Keep hardware drivers and pin definitions strictly inside `include/board_pins.h`, `include/bsp/`, and `src/bsp/`.
   - Keep application screens/widgets inside `src/ui/` consuming `bsp::BoardBsp` rather than touching I2C/SPI registers directly.
   - Verify root starter changes compile cleanly with `pio run`.
