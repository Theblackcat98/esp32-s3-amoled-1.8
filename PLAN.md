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
|        Waveshare ESP32-S3-Touch-AMOLED-1.8       |    ws://<HOST_IP>:8080/v1/realtime |        LAN Host Computer            |
|                                                  | <================================> |                                     |
|  [MIC] --> ES8311 (Reg 0x44: L=DAC_ref, R=ADC)   |   Uplink:   PCM16 16kHz (Base64)   |  llama-server (llama.cpp >= v1.0)   |
|               |                                  |             + response.cancel      |   - Model: LFM3-Audio-2B / Omni-2B  |
|               v                                  |                                    |   - Endpoint: /v1/realtime (WS)     |
|        ESP-SR AFE (AEC + NS + VAD @ 16kHz)       |   Downlink: PCM16 24kHz (Base64)   |   - Modality: Audio-in -> Audio-out |
|               |                                  |             + transcript deltas    |                                     |
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
- **Target Runtime:** Stock `llama.cpp` **`v1.0.0+`** (realtime audio shipped in `v0.8.0` and stabilized through `v0.9.0`/`v1.0.0`).
- **Endpoint:** `ws://<LAN_IP>:8080/v1/realtime` — implements the OpenAI Realtime WebSocket event schema for conversation, transcription, and tool calls.
- **LAN Security Requirement (`v1.0.0+`):** Binding `llama-server` to a non-loopback address (`--host 0.0.0.0`) with tool support enabled requires an explicit `--api-key`. The ESP32 client must pass `Authorization: Bearer <API_KEY>` in the WebSocket handshake headers.

### Model Options & Full-Duplex Reality Check

| Option | Runtime + Model | Duplex Behavior | Hardware Target | Best For |
| :--- | :--- | :--- | :--- | :--- |
| **Option A (Recommended PoC)** | Stock `llama-server` + **`LiquidAI/LFM3-Audio-2B`** (or **`LFM3-Omni-2B`**) GGUF + `mmproj` | **Interruptible Full-Duplex:** Audio prefills as it streams in; output streams back at 24 kHz PCM16; client VAD triggers instant barge-in via `response.cancel`. Supports tool calls natively. | Consumer GPU or modern Apple Silicon (must sustain **>1.0x real-time** audio token generation to avoid playback stutter) | Fast PoC directly against stock `llama-server` with a clean OpenAI Realtime contract. |
| **Option B (True Simultaneous Duplex)** | **`llama.cpp-omni`** (fork) + **`MiniCPM-o 4.5`** (9B GGUF stack) | **Simultaneous Overlapping Duplex:** Model continuously listens and generates overlapping audio/backchannels natively. | ≥12 GB NVIDIA GPU or ≥24 GB Apple M4 Max | Exploring true simultaneous duplex if interruptible turn-taking feels too rigid (requires adapting to `llama.cpp-omni`'s wire protocol). |

### Critical `llama-server /v1/realtime` Constraints to Design Around
1. **No Server-Side VAD (`turn_detection: null`):** `llama-server` does **not** detect speech start/stop or emit `input_audio_buffer.speech_started`. End-of-turn (`input_audio_buffer.commit` + `response.create`) and barge-in (`response.cancel`) are **100% the ESP32 client's responsibility**. Calling `response.cancel` cleanly aborts active generation while preserving session history.
2. **Sample-Rate Asymmetry & No Auto-Resampling:**
   - Uplink must match `audio.input.format.rate` reported in `session.created` (typically **16 kHz PCM16 mono**); `llama-server` does not resample mismatched input rates.
   - Downlink audio (`response.audio.delta`) arrives as **24 kHz PCM16 mono** (for LFM3-Audio).
3. **Real-Time Factor (RTF) Sensitivity:** If the host machine generates audio slower than 1.0× real time (e.g., on an underpowered iGPU), streamed playback will underrun and crackle. Validate RTF ≥ 1.2× on the LAN host before debugging ESP32 playback.
4. **Input Validation Hygiene:** Ensure `session.update` JSON payloads strictly conform to expected object schemas (especially once `tools` are added in Phase 2) and pin a recent `llama.cpp` build containing the realtime error-handling fixes (`>= v1.0.0`).

### Target Wire Protocol Sequence (PoC)
1. **Connect:** `ws://<LAN_IP>:8080/v1/realtime` with `Authorization: Bearer <API_KEY>`.
2. **Handshake:** Receive `session.created` → read `audio.input.format.rate` → send `session.update` configuring:
   - `session.type = "realtime"`
   - `instructions` (agent persona system prompt)
   - Output voice/language (e.g., `female` / `male` per `GET /props` voices list)
   - Output modalities (`["audio", "text"]` so the AMOLED can also render live captions)
3. **User Speaks:** Stream 20–40 ms PCM16 16 kHz frames via `input_audio_buffer.append` (Base64).
4. **End of Utterance (Client VAD Silence Threshold):** Send `input_audio_buffer.commit` followed by `response.create`.
5. **Agent Answers:** Receive `response.audio.delta` (decode Base64 → downsample 24 kHz→16 kHz → playback ring buffer) and optional `response.audio_transcript.delta` (update UI caption) until `response.done`.
6. **Barge-In (User Speaks During Agent Playback):**
   - Local AEC + VAD triggers speech start while state == `SPEAKING`.
   - Immediately mute/flush the local speaker jitter buffer.
   - Send `response.cancel` over WebSocket.
   - Flush pre-roll buffer (~250 ms of captured speech) via `input_audio_buffer.append` so the first word is never lost.

---

## 5. ESP32-S3 Firmware Architecture

### Framework Choice: ESP-IDF (v5.5+)
Use **ESP-IDF (C/C++)** rather than Arduino for this project because:
- First-class support for **ESP-SR** (`esp_afe` / `esp_aec` / `vadnet`) compiled with ESP32-S3 SIMD acceleration
- Official Waveshare managed BSP (`waveshare/esp32_s3_touch_amoled_1_8`) supporting V2 CO5300 + CST820 + LVGL 9
- Deterministic FreeRTOS dual-core pinning and explicit Internal SRAM vs. Octal PSRAM allocation (`heap_caps_malloc`)

### Full-Duplex Audio & AEC Pipeline (The Critical Engineering Piece)

Because the microphone and speaker sit centimeters apart inside the small black enclosure, naive full-duplex causes the model to hear itself and self-interrupt immediately.

1. **Single Shared Full-Duplex I2S Bus @ 16 kHz, 16-bit:**
   - ESP-SR AEC requires **16 kHz** input. Running the single ES8311 I2S bus at 16 kHz eliminates uplink resampling completely.
2. **ES8311 Hardware Digital Feedback Loopback for Sample-Aligned AEC:**
   - Software ring-buffer echo references tend to drift between DMA TX and RX.
   - Configure **ES8311 register `0x44` (`ADCDAT_SEL` = `0x48`)** via I2C on initialization so the codec outputs a **stereo I2S RX frame** where:
     - **Left Channel (`R`)** = exact digital DAC loopback (what the speaker is playing)
     - **Right Channel (`M`)** = ADC microphone capture (room + speaker echo + user voice)
   - Because both channels share the same I2S frame clock, the echo reference is sample-aligned with zero drift.
   - *(Fallback plan: If acoustic enclosure delay or gain mismatch needs compensation, pair with a fixed-sample delay offset or software reference buffer tuning.)*
3. **ESP-SR Audio Front-End (AFE):**
   - Input format: `"MR"` (1 mic + 1 playback reference).
   - Mode: **`AEC_MODE_FD_LOW_COST`** (linear adaptive filter + nonlinear residual echo suppression, recommended by Espressif for full-duplex conversation).
   - Enable **Noise Suppression (NS)** and **VAD** (`AFE_VAD`) inside AFE.
4. **Pre-Roll Circular Buffer (250–300 ms):**
   - Continuously retain the last ~300 ms of AEC-cleaned 16 kHz audio in PSRAM. When VAD transitions from silence → speech (especially during barge-in), send the pre-roll first so the user's opening syllable is preserved.
5. **Downlink Resampling & Jitter Buffer:**
   - Decode incoming Base64 `response.audio.delta` chunks (24 kHz PCM16) in PSRAM.
   - Apply a lightweight **3:2 streaming downsampler (24 kHz → 16 kHz)** (linear interpolation with simple low-pass filter or `esp-dsp` polyphase FIR).
   - Feed a **150–250 ms jitter ring buffer** before starting speaker playback to absorb LAN Wi-Fi and token-generation burstiness.
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

Use a **true-black (`#000000`) AMOLED theme** to maximize contrast, reduce power draw, and minimize burn-in.

### Screen Wireframe (`368 × 448` Portrait)

```text
+----------------------------------+  y = 0
| [WiFi]  LAN: 12ms       [87% ⚡] |  Status Bar (AXP2101 battery, WS status)
+----------------------------------+  y = 44
|                                  |
|          ╭────────────╮          |
|          │   ( ◉‿◉ )  │          |  Agent Avatar / Identity Badge
|          │  Orb/Ring  │          |  (Animated ring pulses with mic/spk RMS)
|          ╰────────────╯          |
|                                  |
|            "NOVA"                |  Active Agent Name (Large, crisp font)
|      Local Voice Assistant       |  Agent Role / Model Subtitle
|                                  |
|        ● LISTENING ...           |  State Pill (color-coded)
+----------------------------------+  y = 300
|  "Sure, I can help you plan      |  Scrolling 2–3 line live caption /
|   that hardware test..."         |  status area (optional transcript)
+----------------------------------+  y = 376
|  [  MUTE  ]        [ END / STOP] |  Large touch targets (CST820)
+----------------------------------+  y = 448
```

### UI States & Visual Mapping

| State | Ring / Indicator Color | Meaning |
| :--- | :--- | :--- |
| `BOOTING` / `WIFI_WAIT` | Dim White / Amber | Initializing BSP, AXP2101, ES8311, joining Wi-Fi |
| `READY` (App Idle) | Soft Gray | Connected to Wi-Fi; tap **"Start Talking"** (or press `BOOT`) to open realtime session |
| `LISTENING` | Cyan / Teal | Mic active, AEC+VAD listening; ring animates to user voice RMS |
| `THINKING` | Amber (spinning) | Utterance committed (`input_audio_buffer.commit`), awaiting first audio delta |
| `SPEAKING` | Emerald / Violet | Agent speaking; ring animates to playback RMS; **AEC+VAD armed for barge-in** |
| `INTERRUPTED` | Flash White → Cyan | Barge-in triggered; local buffer flushed & `response.cancel` sent |
| `TOOL_CALLING` *(Phase 2)* | Indigo Pulse | Harness executing tool (caption shows e.g. *"Running: get_weather"*) |
| `ERROR` / `RECONNECTING` | Red | Server unreachable or WS dropped; auto-backoff reconnect + manual retry button |

### Physical Controls & Power Behavior
- **Touch (`CST820`):**
  - Tap center orb during `SPEAKING` → manual force-interrupt (backup for noisy environments).
  - Bottom buttons → **Mute Mic** (hard mute before uplink) and **Start / End Session**.
- **Side Buttons:**
  - `BOOT` short-press → Toggle Mute; long-press → End/Start session.
  - `PWR` short-press → Dim/wake AMOLED (CO5300 brightness register); long-press → Clean shutdown via AXP2101.
- **Auto-Dim:** Dim display after 30 s of `READY` idle to protect the AMOLED panel.

---

## 7. Future Harness Integration Contract (Phase 2)

To avoid rewriting firmware when moving from raw `llama-server` to a full tool-capable Agent Harness:

1. **Single Transport Seam:** The Harness exposes the exact same `/v1/realtime` WebSocket endpoint to the ESP32-S3 and manages upstream connections to `llama-server` (and optionally a stronger separate reasoning/tool LLM alongside the fast audio model).
2. **Backwards-Compatible Extension Events:** The ESP32 parser silently ignores unknown JSON event types in PoC, and recognizes optional custom `harness.*` events once the Harness is introduced:
   - `harness.agent.info` → `{ "id": "nova", "name": "Nova", "role": "Home Ops", "accent_color": "#00E5FF", "avatar_id": "orb_1" }` (dynamically updates the AMOLED Agent Card).
   - `harness.tool.status` → `{ "call_id": "...", "name": "calendar_create", "state": "running" | "done" | "error" }` (transitions UI to `TOOL_CALLING` state and shows subtle progress feedback).
   - `harness.tool.confirm` *(optional future)* → Prompts a `[Allow]` / `[Deny]` modal on the 1.8" touchscreen before running destructive tools.
   - `harness.agent.switch` *(client → server)* → Triggered by swiping left/right on the AMOLED Agent Card to switch between configured agents.
3. **Tool Lifecycle Stays Off-Device:** When `llama-server` emits a function call, the Harness intercepts it, executes the tool (via MCP or native plugins), submits the tool result back to the model, and streams the model's spoken follow-up audio back to the ESP32.
4. **Optional Future Transport Upgrade:** If Wi-Fi latency or Base64 overhead becomes limiting, add a negotiated binary WebSocket sub-protocol (`audio/opus` or raw binary PCM frames) between ESP32 and the Harness without altering the audio/AEC pipeline.

---

## 8. Proposed Repository Layout

```text
voice-agent-amoled/
├── PLAN.md                         # This architectural plan
├── docs/
│   ├── protocol.md                 # Exact /v1/realtime + harness.* JSON event schemas
│   └── hardware-v2-notes.md        # Verified V2 pinout, I2C scan output, AEC tuning notes
├── server/
│   ├── run-llama-server.sh         # Reproducible llama-server launch script + flags
│   ├── test_realtime_cli.py        # Desktop Python test client (mic/spk) to validate server independently
│   └── prompts/
│       └── default_agent.txt       # PoC agent persona system prompt
├── harness/                        # Phase 2 placeholder (Python/TypeScript WS proxy + tool/MCP runner)
│   └── README.md
└── firmware/                       # ESP-IDF v5.5+ project
    ├── CMakeLists.txt
    ├── partitions.csv              # 16 MB Flash layout (App + OTA + NVS + SPIFFS/LittleFS assets)
    ├── sdkconfig.defaults          # PSRAM OPI, ESP-SR, LVGL 9, Wi-Fi tuning
    ├── sdkconfig.defaults.local    # (gitignored) Wi-Fi SSID/PASS, LAN_SERVER_IP, API_KEY
    └── main/
        ├── idf_component.yml       # waveshare/esp32_s3_touch_amoled_1_8, espressif/esp-sr, lvgl
        ├── app_main.c
        ├── board/                  # BSP init, ES8311 reg 0x44 setup, AXP2101 battery monitor
        ├── audio/                  # Full-duplex I2S driver, ESP-SR AFE/AEC/VAD, 24k->16k resampler, ring buffers
        ├── net/                    # Wi-Fi STA manager, /v1/realtime WS client, JSON/Base64 codec
        ├── session/                # Conversation & barge-in state machine
        └── ui/                     # LVGL 9 Agent Card, waveform widget, touch/button handlers
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

## 11. Open Decisions to Finalize Before Implementation

1. **LAN Host Specs & Model Selection:** What GPU/CPU + VRAM does the LAN computer have? (Determines whether to start with `LFM3-Audio-2B` on stock `llama-server` vs. `MiniCPM-o 4.5` on `llama.cpp-omni`.)
2. **Power Setup for PoC:** Running tethered via USB-C only, or is a 3.7V LiPo cell connected to the MX1.25 header (enabling untethered AXP2101 battery display)?
3. **Session Start UX:** Should booting the device land on a `"Tap to Connect"` Agent Card, or immediately auto-connect and enter `LISTENING` mode on boot?
4. **LAN Discovery:** Hardcoded static LAN IP in gitignored `sdkconfig.defaults.local` for PoC, or mDNS (`esp_mdns`) discovery of `_llama._tcp.local`?
5. **Initial Agent Persona:** Name, visual theme/color, and system prompt for the default PoC agent displayed on the 1.8" AMOLED screen.
