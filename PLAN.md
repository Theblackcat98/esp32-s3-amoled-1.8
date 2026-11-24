# PLAN.md — ESP32-S3 AMOLED Full-Duplex Voice Agent (PoC → Harness)

## 1. Overview & Objectives

### Proof of Concept (Phase 1 — Now)
Build a working voice-agent proof of concept on the **Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2)** that:
1. Connects to the local 2.4 GHz Wi-Fi network.
2. Displays the **active AI agent** (identity, persona/name, connection/battery status, and live audio state) on the 1.8" AMOLED touchscreen.
3. Launches a session on demand ("start up the app and talk") and communicates directly with a **`llama-server` (`llama.cpp`)** instance running an audio model on a computer on the LAN.
4. Operates in **full-duplex voice mode**—simultaneous microphone capture and speaker playback with Acoustic Echo Cancellation (AEC) and Voice Activity Detection (VAD) barge-in so the user can speak naturally and interrupt the agent mid-response.

### Future Evolution (Phase 2 — Harness Integration)
Design the PoC firmware and wire protocol around a clean boundary (**OpenAI Realtime WebSocket schema**) so a dedicated **LAN Agent Harness** can later be inserted between the ESP32-S3 and `llama-server` to provide:
- Tool calling & execution (MCP servers, local APIs, smart-home/OS actions)
- Multi-agent routing and dynamic persona switching on the display
- Long-term memory, session persistence, and optional touchscreen approval prompts for sensitive tool calls

### Explicit Non-Goals for PoC
- On-device wake-word detection (WakeNet) or offline ASR/TTS
- Cloud API dependencies or internet routing
- Tool execution or MCP orchestration inside the ESP32 firmware
- Production Wi-Fi provisioning UI (BLE/SoftAP) — local compile-time/NVS config is sufficient for PoC

---

## 2. Hardware Profile (Verified from Target Device)

From the device label, this unit is the **Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2)**.

> **Critical V1 vs. V2 Hardware Warning:** Waveshare transitioned this board to **V2** in May 2026. V2 replaces the V1 `SH8601` display / `FT3168` touch controller (`0x38`) with **`CO5300` / `CST820` (`0x15`)**. Any older V1 example code or pin/driver configs will fail with a blank screen or unresponsive touch. Use Waveshare's official managed ESP-IDF BSP component (`waveshare/esp32_s3_touch_amoled_1_8`), which supports V2.

### Core Specifications & Onboard Peripherals

| Subsystem | Chip / Spec | Bus / Address | Role in PoC / Future |
| :--- | :--- | :--- | :--- |
| **MCU** | ESP32-S3R8 (Dual-core Xtensa LX7 @ 240 MHz) | — | Core 0: Wi-Fi/WS; Core 1: Audio/AEC/UI |
| **Memory** | 512 KB Internal SRAM + **8 MB Octal PSRAM** + **16 MB Flash** | QIO/OPI 80 MHz | SRAM: DMA/Wi-Fi/AEC; PSRAM: LVGL/buffers |
| **Radio** | 2.4 GHz Wi-Fi 4 (`802.11 b/g/n`) + BLE 5 | Onboard antenna | LAN WebSocket connection (**2.4 GHz SSID required**) |
| **Display** | 1.8" AMOLED, **368×448** RGB565, **CO5300** driver | QSPI (40 MHz) | Agent card, state indicator, waveform, captions |
| **Touch** | **CST820** capacitive touch | Shared I2C (`0x15`) | Start/Stop session, Mute, Manual interrupt, future agent switcher |
| **Audio Codec** | **ES8311** (single-channel ADC + DAC) + onboard MIC & SPK amp | I2C (`0x18`) + I2S | Full-duplex mic capture, speaker output, hardware DAC loopback for AEC |
| **PMIC** | **AXP2101** | Shared I2C (`0x34`) | Power rails, battery level %, USB-C charging status |
| **IO Expander** | **TCA9554** | Shared I2C (`0x20`) | Auxiliary control pins (e.g., SD CS / peripheral resets) |
| **6-Axis IMU** | **QMI8658** (Accel + Gyro) | Shared I2C (`0x6B`) | *Future:* Raise-to-wake / tap gestures |
| **RTC** | **PCF85063** | Shared I2C (`0x51`) | *Future:* Offline clock / timestamps |
| **Storage** | microSD (TF Card slot) | SDMMC 1-bit | *Future:* Agent avatars, local chime assets, debug logs |
| **Buttons / USB** | `BOOT`, `PWR`, Native USB-C (USB-Serial/JTAG) | GPIO 19/20 (USB) | Flashing/logs, hardware mute/wake shortcuts |

### Reference Pin Map (Verify Against Official V2 BSP Header on Bring-Up)

| Interface | Signals & GPIOs | Notes |
| :--- | :--- | :--- |
| **QSPI Display** | `SCLK=11`, `CS=12`, `D0..D3=4,5,6,7` | Brightness controlled via CO5300 command (no separate PWM backlight) |
| **Shared I2C** | `SDA=15`, `SCL=14`, `TP_INT=21` | Shared by CST820, ES8311, AXP2101, TCA9554, QMI8658, PCF85063 |
| **I2S Audio (ES8311)** | `MCLK=16`, `BCLK=9`, `WS/LRCK=45`, `DOUT/DIN=8/10`, `PA_EN=46` | Verify MCU-vs-Codec `DIN`/`DOUT` direction (`8` vs `10`) via BSP `12_i2s_codec` example |
| **microSD** | `CLK=2`, `CMD=1`, `DATA=3` | Optional for PoC |

---

## 3. System Architecture & Harness Evolution

### Phase 1: Proof of Concept (Direct LAN Connection)

```text
+--------------------------------------------------+         LAN Wi-Fi (2.4 GHz)        +-------------------------------------+
|        Waveshare ESP32-S3-Touch-AMOLED-1.8       |    ws://<HOST_IP>:8082/v1/realtime |        LAN Host Computer            |
|                                                  | <================================> |  192.168.1.199 (RX 7900 XT 20GB)   |
|  [MIC] --> ES8311 (Reg 0x44: L=DAC_ref, R=ADC)   |   Uplink:   PCM16 16kHz (Base64)   |  llama-server (llama.cpp >= 0.8.0)  |
|               |  input_format = "RM"             |             + response.cancel      |   - Model: LFM3-Audio-2B-Q4_K_M     |
|               v                                  |                                    |   - Endpoint: /v1/realtime (WS)     |
|     ESP-SR AFE ("RM", AEC_FD + NS + VAD @ 16kHz) |   Downlink: PCM16 24kHz (Base64)   |   - response.output_audio.delta     |
|               |                                  |             + output_text.delta    |   - response.output_text.delta      |
|   Downlink -> Resampler (24k->16k) -> ES8311 SPK |                                    +-------------------------------------+
|   State/Text -> LVGL 9 UI (CO5300 + CST820)      |
+--------------------------------------------------+
```

### Phase 2: Future Architecture (With Agent Harness)

```text
+---------------------------+     ws://<HARNESS_IP>:8000/v1/realtime     +-----------------------------------+        +------------------+
|  ESP32-S3 AMOLED Device   | <========================================> |        LAN Agent Harness          | <====> |   llama-server   |
|  (Thin Realtime Client)   |  OpenAI Realtime WS + UI Metadata Events   |  - Agent Registry & System Prompts|        |  (Audio / LLM    |
|  - Same audio/AEC engine  |  (agent.profile, tool.status, confirmations)| - Tool Router & MCP Execution    |        |   Inference)     |
|  - Renders dynamic agent  |                                            |  - Memory / Context Management    |        +------------------+
+---------------------------+                                            |  - Optional Binary Opus Transport |
                                                                         +-----------------------------------+
```

**Key Architectural Principle:** Keep the ESP32-S3 a **thin realtime I/O terminal**. It handles audio capture, echo cancellation, local barge-in detection, audio playback, and UI rendering. It never embeds tool schemas, prompt logic, or model-specific quirks—so swapping `llama-server` for the future Agent Harness is primarily a server-URL configuration change.

---

## 4. LAN Inference Server (`llama.cpp`) Plan

### Runtime Selection & Capabilities
- **Target Runtime:** Stock `llama.cpp` **`0.8.0-dev+` / `v1.0.0+`** (verified local build at `/home/nick/Documents/Models/llama.cpp/build/bin/llama-server` on ROCm0 RX 7900 XT 20 GB).
- **Endpoint:** `ws://<LAN_IP>:8082/v1/realtime` — implements the OpenAI Realtime **GA** WebSocket event schema (`tools/server/server-realtime.cpp`).
- **LAN Auth Support:** When `--api-key` is supplied to `llama-server` (or a Phase 2 Harness), the ESP32 client passes `Authorization: Bearer <API_KEY>` in the WebSocket handshake headers.

### Model Options & Full-Duplex Reality Check

| Option | Runtime + Model | Duplex Behavior | Hardware Target | Best For |
| :--- | :--- | :--- | :--- | :--- |
| **Option A (Selected PoC)** | Stock `llama-server` + **`LiquidAI/LFM3-Audio-2B-Q4_K_M`** + `mmproj` | **Interruptible Full-Duplex:** Audio prefills as it streams in; output streams back at 24 kHz PCM16; client VAD triggers instant barge-in via `response.cancel`. Supports tool calls natively. | Local host RX 7900 XT (20 GB VRAM, sustains >1.0× real-time audio generation) | Fast PoC directly against stock `llama-server` with a clean OpenAI Realtime GA contract. |
| **Option B (True Simultaneous Duplex)** | **`llama.cpp-omni`** (fork) + **`MiniCPM-o 4.5`** (9B GGUF stack) | **Simultaneous Overlapping Duplex:** Model continuously listens and generates overlapping audio/backchannels natively. | ≥12 GB NVIDIA GPU or ≥24 GB Apple M4 Max | Exploring true simultaneous duplex if interruptible turn-taking feels too rigid (requires adapting to `llama.cpp-omni`'s wire protocol). |

### Critical `llama-server /v1/realtime` Constraints to Design Around
1. **No Server-Side VAD (`turn_detection: null`):** `llama-server` does **not** detect speech start/stop or emit `input_audio_buffer.speech_started`. End-of-turn (`input_audio_buffer.commit` + `response.create`) and barge-in (`response.cancel`) are **100% the ESP32 client's responsibility**. Calling `response.cancel` cleanly aborts active generation while preserving session history.
2. **OpenAI Realtime GA Event Names:** `llama-server` emits **`response.output_audio.delta`** / **`response.output_audio.done`** and **`response.output_text.delta`** / **`response.output_text.done`** (not beta `response.audio.*` / `response.audio_transcript.*`).
3. **Sample-Rate Asymmetry & No Auto-Resampling:**
   - Uplink must match `audio.input.format.rate` reported in `session.created` (**16 kHz PCM16 mono**); `llama-server` does not resample mismatched input rates.
   - Downlink audio (`response.output_audio.delta`) arrives as **24 kHz PCM16 mono** (for LFM3-Audio).
4. **LFM3-Audio `session.update` Schema & Instructions Lock:**
   - `instructions` must begin with `"Respond with interleaved text and audio."` alongside `output_modalities: ["text", "audio"]`.
   - Do **not** send unsupported `voice` or transcription-only `language` fields in `realtime` mode; `instructions` and `tools` are locked once the first audio/message arrives.
5. **Barge-In `response_id` Filtering & Sequencing:**
   - After `response.cancel`, `llama-server` may emit 1–2 in-flight tail deltas before `response.done` (`status: "cancelled"`). The client filters incoming deltas against the active `response_id`, clears active `response_id` and sets `awaiting_cancel = true` immediately on cancel, holds the first ~300 ms of speech before sending `input_audio_buffer.append` (to filter transient coughs/clicks), and defers `response.create` (`pending_response = true`) until the cancelled `response.done` settles.
6. **Real-Time Factor (RTF) Sensitivity:** Validate RTF ≥ 1.2× on the LAN host (or mock harness) before debugging ESP32 playback.

### Target Wire Protocol Sequence (PoC)
1. **Connect:** `ws://<LAN_IP>:8082/v1/realtime?model=lfm3-audio-2b` with optional `Authorization: Bearer <API_KEY>`.
2. **Handshake:** Receive `session.created` → read `audio.input.format.rate` & `audio.output.format.rate` → send `session.update` configuring:
   - `session.type = "realtime"`
   - `instructions` (starting with `"Respond with interleaved text and audio."`)
   - `output_modalities = ["text", "audio"]`
3. **User Speaks:** Hold ~300 ms onset confirmation (while keeping ~350 ms pre-roll), then stream 20–40 ms PCM16 16 kHz frames via `input_audio_buffer.append` (Base64).
4. **End of Utterance (Client VAD Silence Threshold ~650 ms):** Send `input_audio_buffer.commit` followed by `response.create` (or defer `response.create` until cancelled `response.done` if `awaiting_cancel` is active).
5. **Agent Answers:** Receive `response.output_audio.delta` (decode Base64 → downsample 24 kHz→16 kHz → playback ring buffer) and `response.output_text.delta` (update UI caption) matching active `response_id` until `response.done`.
6. **Barge-In (User Speaks During Agent Playback):**
   - Local AEC + VAD triggers speech start while state == `SPEAKING` (+6 dB / 220 ms hysteresis).
   - Immediately mute/flush the local speaker jitter buffer, clear active `response_id`, set `awaiting_cancel = true`, and send `response.cancel` over WebSocket.
   - Flush pre-roll buffer (~350 ms of captured speech) via `input_audio_buffer.append` once utterance confirms so the first word is never lost.

---

## 5. ESP32-S3 Firmware Architecture

### Framework Choice: ESP-IDF (v5.5+ via PlatformIO + `pioarduino`)
Use **ESP-IDF 5.5 (`pioarduino` `55.03.312-1`, `framework = espidf`, `src_dir = main`)** in `firmware/` because:
- First-class support for **ESP-SR** (`esp_afe` / `esp_aec` / WebRTC NS+VAD) compiled with ESP32-S3 SIMD acceleration
- Official Waveshare managed BSP (`waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3`) supporting V2 CO5300 + CST820 + LVGL 9
- Deterministic FreeRTOS dual-core pinning and explicit Internal SRAM vs. Octal PSRAM allocation (`heap_caps_malloc`)
- Built cleanly with `cd firmware && pio run` alongside the existing root Arduino V2 hardware-diagnostic starter

### Full-Duplex Audio & AEC Pipeline (The Critical Engineering Piece)

Because the microphone and speaker sit centimeters apart inside the small black enclosure, naive full-duplex causes the model to hear itself and self-interrupt immediately.

1. **Single Shared Full-Duplex I2S Bus @ 16 kHz, 16-bit Stereo:**
   - ESP-SR AEC requires **16 kHz** input. Running the single ES8311 I2S bus at 16 kHz eliminates uplink resampling completely.
2. **ES8311 Hardware Digital Feedback Loopback for Sample-Aligned AEC:**
   - Software ring-buffer echo references tend to drift between DMA TX and RX.
   - Configure **ES8311 register `0x44` (`ADCDAT_SEL` = `0x48`)** via I2C on initialization so the codec outputs a **stereo I2S RX frame** where:
     - **Left Channel (`R`)** = exact digital DAC loopback (what the speaker is playing)
     - **Right Channel (`M`)** = ADC microphone capture (room + speaker echo + user voice)
   - Because both channels share the same I2S frame clock, the echo reference is sample-aligned with zero drift.
   - *(Fallback plan: If acoustic enclosure delay or gain mismatch needs compensation, pair with a fixed-sample delay offset or software reference buffer tuning.)*
3. **ESP-SR Audio Front-End (AFE):**
   - Input format: **`"RM"`** (Left = playback reference `R`, Right = microphone `M`, matching ES8311 `0x44 = 0x48` interleaved frames).
   - Init: `afe_config_init("RM", NULL, AFE_TYPE_FD, AFE_MODE_LOW_COST)` with `wakenet_init = false`, `aec_mode = AEC_MODE_FD_LOW_COST` (linear adaptive filter + nonlinear residual echo suppression), WebRTC Noise Suppression (`AFE_NS_MODE_WEBRTC`), and WebRTC VAD (`vad_model_name = NULL`).
4. **Pre-Roll Circular Buffer (350 ms) & 300 ms Onset Confirmation:**
   - Continuously retain the last ~350 ms of AEC-cleaned 16 kHz audio in PSRAM. When VAD transitions from silence → speech and holds for 300 ms (or 220 ms + 6 dB margin during `SPEAKING`), flush the pre-roll first so the user's opening syllable is preserved without false cough/click triggers.
5. **Downlink Resampling & Jitter Buffer:**
   - Decode incoming Base64 `response.output_audio.delta` chunks (24 kHz PCM16) in PSRAM.
   - Apply a lightweight **3:2 streaming downsampler (24 kHz → 16 kHz)** (3-tap low-pass + linear interpolation).
   - Feed a **180 ms jitter pre-buffer / 256 KB ring buffer** before starting speaker playback to absorb LAN Wi-Fi and token-generation burstiness.
   - Manage speaker amp `PA_EN` (GPIO 46) cleanly to prevent boot/shutdown pops.

### FreeRTOS Task & Dual-Core Pinning

| Core | Task | Priority | Responsibility |
| :--- | :--- | :--- | :--- |
| **Core 0** | `wifi` / `lwip` (IDF sys) | System | 2.4 GHz Wi-Fi station + TCP stack |
| **Core 0** | `ws_rx_task` | High | `esp_websocket_client` frame reassembly, JSON event dispatch, Base64 decode, push to downlink audio queue / UI event queue |
| **Core 0** | `ws_tx_task` | Medium-High | Pop AEC-cleaned 16 kHz frames from uplink queue, Base64 encode, emit `input_audio_buffer.append` / `commit` / `cancel` |
| **Core 1** | `audio_io_task` | Very High (Real-Time) | Full-duplex I2S DMA read/write, split stereo `L/R`, feed AFE, pull resampled playback PCM |
| **Core 1** | `afe_vad_task` | High | Run ESP-SR AEC + NS + VAD, drive conversation state machine & barge-in interrupts |
| **Core 1** | `lvgl_ui_task` | Low-Medium | 30 FPS LVGL 9 rendering (CO5300 QSPI DMA), CST820 touch events, AXP2101 telemetry polling (1 Hz) |

### Memory & Bandwidth Budget
- **Internal SRAM (~370 KB usable after boot):** Reserve strictly for Wi-Fi buffers, I2S/SPI DMA descriptors, FreeRTOS task stacks, and ESP-SR AEC scratch memory.
- **Octal PSRAM (8 MB):** Place LVGL partial draw buffers (e.g., 2× `368×48` RGB565 stripes), WebSocket RX assembly buffer (64–128 KB), Base64 scratch buffers, uplink pre-roll buffer, and downlink jitter buffer (256 KB).
- **LAN Wi-Fi Bandwidth:**
  - Uplink: 16 kHz × 2 B = 32 KB/s raw → ~46 KB/s Base64+JSON (~370 kbps).
  - Downlink: 24 kHz × 2 B = 48 KB/s raw → ~68 KB/s Base64+JSON (~545 kbps).
  - Combined ~1 Mbps — well within ESP32-S3 Wi-Fi 4 throughput, provided `TCP_NODELAY` is enabled and Wi-Fi power-save (`WIFI_PS_NONE`) is disabled during active voice sessions to keep latency low.

---

## 6. Display & Interaction Design (368×448 AMOLED)

Use a **true-black (`#000000`) AMOLED theme** to maximize contrast, reduce power draw, and minimize burn-in. **No cyan is used anywhere in the palette.**

### Screen Wireframe (`368 × 448` Portrait)

```text
+----------------------------------+  y = 0
| [WiFi]  12ms    [Orb*]  [87% ⚡] |  Status Bar (Wi-Fi/RTT, Orb Customizer, AXP2101 battery)
+----------------------------------+  y = 44
|                                  |
|          ╭────────────╮          |
|          │   (  ◉  )  │          |  Customizable Agent Orb (Tap: start/interrupt;
|          │  Orb/Ring  │          |  Long-press: customize color/style; Swipe: switch agent)
|          ╰────────────╯          |
|                                  |
|            "EMBER"               |  Active Agent Name (Large, crisp font)
|       Local Voice Agent          |  Agent Role / Status Subtitle
|                                  |
|        ● LISTENING ...           |  State Pill (color-coded)
+----------------------------------+  y = 300
|  "Sure, I can help you plan      |  Scrolling 2–3 line live caption /
|   that hardware test..."         |  status area (response.output_text.delta)
+----------------------------------+  y = 376
|  [  MUTE  ]    [ START / STOP ]  |  Large touch targets (CST820)
+----------------------------------+  y = 448
```

### UI States & Visual Mapping

| State | Ring / Indicator Color | Meaning |
| :--- | :--- | :--- |
| `BOOTING` / `WIFI_WAIT` | Dim White / Warm Amber | Initializing BSP, AXP2101, ES8311, joining Wi-Fi |
| `READY` (App Idle) | Soft Slate / Dim Accent | Connected to Wi-Fi; tap **"Start Talking"** / Orb (or press `BOOT`) to open realtime session |
| `LISTENING` | Active Orb Accent (`#F59E0B` default) | Mic active, AEC+VAD listening; orb animates to user voice RMS |
| `THINKING` | Gold (`#EAB308`, spinning arc) | Utterance committed (`input_audio_buffer.commit`), awaiting first audio delta |
| `SPEAKING` | Emerald (`#10B981`) or Active Accent | Agent speaking; orb animates to playback RMS; **AEC+VAD armed for barge-in** |
| `INTERRUPTED` | Flash White → Active Accent | Barge-in triggered; local buffer flushed & `response.cancel` sent |
| `TOOL_CALLING` *(Phase 2)* | Indigo / Violet (`#8B5CF6`) Pulse | Harness executing tool (caption shows e.g. *"Running: get_weather"*) |
| `ERROR` / `RECONNECTING` | Crimson (`#EF4444`) | Server unreachable or WS dropped; auto-backoff reconnect + manual retry button |

### Customizable Orb & Physical Controls
- **On-Device Orb Customizer (Persisted in NVS):**
  - Open via top-bar `Orb` button or long-pressing the center orb.
  - **6 Curated Non-Cyan Presets:** Amber (`#F59E0B`, default), Emerald (`#10B981`), Violet (`#8B5CF6`), Coral (`#FB7185`), Gold (`#EAB308`), Crimson (`#EF4444`) — plus arbitrary `#RRGGBB` overrides from `harness.agent.info`.
  - **3 Orb Styles:** `Pulse Orb` (breathing solid core + halo), `Sonar Rings` (concentric reactive ripples), and `Arc Halo` (rotating segmented energy ring).
- **Touch (`CST820`):**
  - Tap center orb in `READY` → start session; tap during `SPEAKING` → manual force-interrupt (backup for noisy environments).
  - Horizontal swipe on center card → emit `harness.agent.switch` (Phase 2).
  - Bottom buttons → **Mute Mic** (hard mute before uplink) and **Start Talking / End**.
- **Side Buttons:**
  - `BOOT` short-press → Toggle Mute (or Start session if `READY`); long-press → End/Start session.
  - `PWR` short-press → Dim/wake AMOLED (CO5300 brightness register); long-press → Clean shutdown via AXP2101 (`0x10` bit 0).
- **Auto-Dim:** Dim display after 30 s of `READY` idle to protect the AMOLED panel.

---

## 7. Future Harness Integration Contract (Phase 2)

To avoid rewriting firmware when moving from raw `llama-server` to a full tool-capable Agent Harness:

1. **Single Transport Seam:** The Harness exposes the exact same `/v1/realtime` WebSocket endpoint to the ESP32-S3 and manages upstream connections to `llama-server` (and optionally a stronger separate reasoning/tool LLM alongside the fast audio model).
2. **Backwards-Compatible Extension Events:** The ESP32 parser silently ignores unknown JSON event types in PoC, and recognizes optional custom `harness.*` events once the Harness is introduced:
   - `harness.agent.info` → `{ "id": "ember", "name": "Ember", "role": "Home Ops", "accent_color": "#F59E0B", "orb_style": "rings" }` (dynamically updates the AMOLED Agent Card and orb).
   - `harness.tool.status` → `{ "call_id": "...", "name": "calendar_create", "state": "running" | "done" | "error" }` (transitions UI to `TOOL_CALLING` state and shows subtle progress feedback).
   - `harness.tool.confirm` *(optional future)* → Prompts a `[Allow]` / `[Deny]` modal on the 1.8" touchscreen before running destructive tools, returning `harness.tool.confirm_result`.
   - `harness.agent.switch` *(client → server)* → Triggered by swiping left/right on the AMOLED Agent Card to switch between configured agents.
3. **Tool Lifecycle Stays Off-Device:** When `llama-server` emits a function call, the Harness intercepts it, executes the tool (via MCP or native plugins), submits the tool result back to the model, and streams the model's spoken follow-up audio back to the ESP32.
4. **Optional Future Transport Upgrade:** If Wi-Fi latency or Base64 overhead becomes limiting, add a negotiated binary WebSocket sub-protocol (`audio/opus` or raw binary PCM frames) between ESP32 and the Harness without altering the audio/AEC pipeline.

---

## 8. Proposed Repository Layout

```text
esp32-s3-amoled-1.8/
├── PLAN.md                         # This architectural plan
├── platformio.ini / src/           # Root Arduino V2 hardware diagnostic starter
├── docs/
│   ├── protocol.md                 # Exact /v1/realtime GA + harness.* JSON event schemas
│   └── hardware-v2-notes.md        # Verified V2 pinout, I2C map, AXP2101 & AEC tuning notes
├── server/
│   ├── run-llama-server.sh         # Reproducible llama-server launch script + flags
│   ├── mock_realtime_server.py     # Stdlib Python /v1/realtime mock server for offline CI tests
│   ├── test_realtime_cli.py        # Desktop Python test client (--synthetic / --wav / --mic)
│   └── prompts/
│       └── default_agent.txt       # PoC agent persona system prompt (Ember)
├── harness/                        # Phase 2 placeholder (Python/TypeScript WS proxy + tool/MCP runner)
│   └── README.md
└── firmware/                       # Primary ESP-IDF v5.5+ Voice Agent (PlatformIO + pioarduino)
    ├── platformio.ini              # pioarduino 55.03.312-1 (IDF 5.5.5), src_dir = main
    ├── CMakeLists.txt
    ├── partitions.csv              # 16 MB Flash layout (NVS + OTA + SPIFFS)
    ├── sdkconfig.defaults          # PSRAM OPI, ESP-SR, LVGL 9, Wi-Fi tuning
    ├── sdkconfig.defaults.local    # (gitignored) Wi-Fi SSID/PASS, LAN_SERVER_IP, API_KEY
    ├── test_host/                  # Host-compiled unit tests (resampler + PSRAM ring buffer)
    └── main/
        ├── idf_component.yml       # waveshare/esp32_s3_touch_amoled_1_8, espressif/esp-sr, esp_websocket_client, lvgl
        ├── Kconfig.projbuild       # Menuconfig Voice Agent & Orb defaults
        ├── app_config.h            # Unified config + optional local_config.h overrides
        ├── app_main.c
        ├── board/                  # BSP init, AXP2101 telemetry/PWR/shutdown, BOOT button
        ├── audio/                  # Full-duplex I2S, ES8311 reg 0x44, ESP-SR AFE("RM")/AEC/VAD, 24k->16k resampler
        ├── net/                    # Wi-Fi STA manager, /v1/realtime GA WS client, JSON/Base64 codec
        ├── session/                # Conversation & barge-in state machine
        └── ui/                     # LVGL 9 Agent Card, customizable non-cyan orb + NVS theme, touch/confirm modals
```

---

## 9. Phased Implementation Roadmap & Acceptance Criteria

### Milestone 0 — Hardware Bring-Up & V2 Verification
- **Tasks:**
  - Verify ESP-IDF `v5.5+` toolchain and flash Waveshare's `00_board_check` / `00_bsp_quickstart`.
  - Run an I2C bus scan confirming all V2 addresses (`0x15` CST820, `0x18` ES8311, `0x20` TCA9554, `0x34` AXP2101, `0x51` PCF85063, `0x6B` QMI8658).
  - Verify CO5300 display rendering, CST820 touch coordinates, and ES8311 playback + mic recording (`12_i2s_codec`).
  - Test ES8311 register `0x44 = 0x48` stereo loopback capture and verify L-channel contains DAC loopback and R-channel contains mic signal.
- **Done When:** Board boots cleanly over USB-C, draws 368×448 test UI, registers touch, plays test audio, and captures aligned stereo `DAC_ref + MIC` samples.

### Milestone 1 — Standalone LAN `llama-server` Validation
- **Tasks:**
  - Install/build `llama.cpp` `v1.0.0+` on the LAN host and pull chosen GGUF audio model + `mmproj` (e.g., `LFM3-Audio-2B`).
  - Start `llama-server --host 0.0.0.0 --port 8080 --api-key <key>`.
  - Validate conversational audio-in → audio-out, barge-in (`response.cancel`), sample rates, and generation speed (measure RTF and time-to-first-audio) using a minimal desktop Python WebSocket client (`server/test_realtime_cli.py`).
- **Done When:** Desktop client holds a multi-turn spoken conversation over LAN with RTF > 1.0× and clean mid-sentence interruption.

### Milestone 2 — ESP32-S3 End-to-End Voice Loop (Half-Duplex Baseline)
- **Tasks:**
  - Implement Wi-Fi STA connection (2.4 GHz, `WIFI_PS_NONE`), `/v1/realtime` WebSocket client, and `session.update`.
  - Implement the Agent Display UI (Agent Card + states + touch buttons).
  - Wire up push-to-talk / tap-to-talk first (mic muted during playback) with 24 kHz → 16 kHz downlink resampling and jitter buffering.
- **Done When:** You can tap to talk on the ESP32-S3, see the agent transition `LISTENING → THINKING → SPEAKING`, and hear clean, glitch-free spoken responses from the LAN `llama-server`.

### Milestone 3 — Full-Duplex AEC, Client VAD & Barge-In
- **Tasks:**
  - Enable simultaneous I2S RX/TX with ES8311 register `0x44` hardware reference loopback.
  - Integrate ESP-SR AFE (`AEC_MODE_FD_LOW_COST`, NS, VAD) + 250 ms pre-roll ring buffer.
  - Tune speaker output gain, mic PGA gain, and VAD barge-in sensitivity so the speaker never self-triggers VAD inside the enclosure while human speech interrupts cleanly within ~200–400 ms.
  - Implement barge-in path: instant playback flush + `response.cancel` + pre-roll uplink.
- **Done When:** Hands-free multi-turn conversation works continuously without pushing buttons, zero self-echo triggers occur across 10 consecutive turns, and speaking over the agent interrupts it cleanly without losing the first word.

### Milestone 4 — PoC Polish & Reliability
- **Tasks:**
  - Add WebSocket auto-reconnect with exponential backoff and clear UI status.
  - Display live battery % / charging icon from AXP2101 and LAN latency indicator.
  - Add idle AMOLED auto-dim and clean `BOOT`/`PWR` button controls.
  - Record latency metrics (VAD-end → first audio byte → first speaker sample) via serial logs.
- **Done When:** Device runs reliably untethered on battery (if installed) or USB-C for 15+ minutes of continuous conversational testing with graceful recovery if `llama-server` restarts.

### Milestone 5 — Future Agent Harness & Tool Execution (Post-PoC)
- **Tasks:**
  - Stand up LAN Harness WebSocket proxy sitting between ESP32-S3 and `llama-server`.
  - Wire up tool definitions (MCP / local tools), agent profiles, and `harness.*` UI metadata events.
  - Add swipeable multi-agent selector and `TOOL_CALLING` UI feedback on the ESP32-S3.

---

## 10. Key Engineering Risks & Mitigations

| Risk | Impact | Mitigation |
| :--- | :--- | :--- |
| **Acoustic echo / self-interruption in tiny case** | Speaker audio triggers VAD, causing infinite barge-in loops | Use ES8311 reg `0x44` sample-aligned DAC loopback + ESP-SR `AEC_MODE_FD_LOW_COST`; cap max speaker volume below enclosure distortion threshold; raise VAD trigger threshold slightly during `SPEAKING` state (dual-threshold barge-in). |
| **Sample-rate mismatch (16 kHz I2S vs. 24 kHz model output)** | Pitch-shifted audio or server rejection | Keep I2S bus + AEC fixed at 16 kHz; validate `session.created` input rate dynamically; perform 3:2 downsampling (24→16 kHz) on ESP32 downlink. |
| **LAN model generation slower than real time (RTF < 1.0)** | Choppy/crackling playback on speaker | Benchmark model + quantization (`Q4_K_M` vs `Q8_0`) on host GPU in Milestone 1 before firmware tuning; add configurable jitter buffer (150–300 ms). |
| **Internal SRAM exhaustion / heap fragmentation** | Wi-Fi drops, AEC crash, or `LoadProhibited` | Allocate LVGL buffers, WS RX payloads, and audio ring buffers strictly in 8 MB Octal PSRAM; avoid per-frame `malloc`/`cJSON` allocations on the audio hot path (reuse static/pool buffers). |
| **Wi-Fi 2.4 GHz latency spikes** | Delayed barge-in or audio stutter | Disable modem sleep (`WIFI_PS_NONE`) during active sessions, enable `TCP_NODELAY`, keep LAN host on wired Ethernet. |
| **macOS/host serial port auto-reset quirk** | Opening serial monitor drops ESP32-S3 into download mode via USB-JTAG DTR/RTS | Document proper `idf.py monitor` / serial-capture flags in `docs/hardware-v2-notes.md`. |

---

## 11. Finalized Decisions

1. **LAN Host Specs & Model Selection:** Local host (`192.168.1.199`, Ryzen 7 9700X + AMD RX 7900 XT 20 GB) running stock `llama.cpp` (`0.8.0-dev+` ROCm build) with **`LFM3-Audio-2B-Q4_K_M.gguf` + `mmproj`** on port **`8082`**.
2. **Power Setup for PoC:** Auto-detected at runtime via AXP2101 (`isBatteryConnect()` / `0x00` bit 3) — shows live LiPo `%` + charging status when connected, or USB-C `VBUS` status when tethered.
3. **Session Start UX:** Boot to `"Tap to Talk"` (`READY`) Agent Card; tap the orb or `START TALKING` button (or press `BOOT`) to open `/v1/realtime` and enter `LISTENING`.
4. **LAN Discovery:** Static LAN IP (`192.168.1.199:8082`) configured via Kconfig defaults and overridden in gitignored `firmware/sdkconfig.defaults.local` / `firmware/main/local_config.h`.
5. **Initial Agent Persona & Orb:** **"EMBER"** (`Local Voice Agent`), default warm Amber (`#F59E0B`), **no cyan anywhere**, with an on-device Orb Customizer (6 curated non-cyan color presets + 3 orb styles) persisted in NVS.
