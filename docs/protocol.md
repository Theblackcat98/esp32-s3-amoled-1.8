# Wire Protocol Specification — `/v1/realtime` (OpenAI Realtime GA + `harness.*`)

This document specifies the exact JSON-over-WebSocket protocol spoken between the **ESP32-S3-Touch-AMOLED-1.8 (V2)** client and `llama-server` (`llama.cpp/tools/server/server-realtime.cpp`), plus the backwards-compatible `harness.*` extensions reserved for Phase 2.

---

## 1. Connection & Handshake

- **Endpoint:** `ws://<HOST_IP>:<PORT>/v1/realtime?model=lfm3-audio-2b`
  - Default LAN host in PoC: `ws://192.168.1.199:8082/v1/realtime?model=lfm3-audio-2b`
- **Authentication:** Optional HTTP upgrade header when `--api-key` is enabled on the server:
  ```http
  Authorization: Bearer <API_KEY>
  ```
- **Server-Sent `session.created`:**
  ```json
  {
    "event_id": "event_...",
    "type": "session.created",
    "session": {
      "id": "sess_...",
      "object": "realtime.session",
      "type": "realtime",
      "model": "lfm3-audio-2b",
      "instructions": "",
      "output_modalities": null,
      "max_output_tokens": "inf",
      "tools": [],
      "tool_choice": "auto",
      "audio": {
        "input": {
          "format": { "type": "audio/pcm", "rate": 16000 },
          "turn_detection": null
        },
        "output": {
          "format": { "type": "audio/pcm", "rate": 24000 }
        }
      }
    }
  }
  ```

### Important Constraints (`server-realtime.cpp`)
1. `turn_detection` is always `null` — the client **must** perform VAD, turn-commit (`input_audio_buffer.commit` + `response.create`), and barge-in (`response.cancel`).
2. `audio.input.format.rate` (**16000 Hz** PCM16 mono) is fixed; `llama-server` does **not** resample uplink audio.
3. `audio.output.format.rate` (**24000 Hz** PCM16 mono for LFM3-Audio) is downsampled 3:2 to 16 kHz on the ESP32-S3.
4. `instructions` and `tools` are locked once the first audio chunk or user message arrives (`started = true`).
5. Do **not** send `voice` or `audio.input.transcription.language` in `realtime` sessions (`language` is for `transcription` sessions only).

---

## 2. Client `session.update`

Immediately upon receiving `session.created`, the client sends:

```json
{
  "type": "session.update",
  "session": {
    "type": "realtime",
    "instructions": "Respond with interleaved text and audio. You are Ember, a concise local voice assistant...",
    "output_modalities": ["text", "audio"]
  }
}
```

- For `LFM3-Audio-2B`, `instructions` **must** begin with `"Respond with interleaved text and audio."` when requesting interleaved speech + captions.
- Server responds with `session.updated`.

---

## 3. Audio Turn Lifecycle (GA Event Names)

> **Warning:** `llama-server` implements the OpenAI Realtime **GA** event schema:
> - Audio deltas arrive as **`response.output_audio.delta`** (NOT `response.audio.delta`).
> - Text deltas arrive as **`response.output_text.delta`** (NOT `response.audio_transcript.delta`).

### Uplink (Client → Server)
1. **Speech onset gate:** Hold the first ~300 ms of VAD-confirmed speech locally (keeping a 350 ms pre-roll ring buffer) so coughs/clicks do not open false server turns.
2. **Stream audio (`input_audio_buffer.append`):**
   ```json
   {
     "type": "input_audio_buffer.append",
     "audio": "<base64 PCM16 16kHz mono>"
   }
   ```
3. **End of utterance (`input_audio_buffer.commit` + `response.create`):**
   ```json
   { "type": "input_audio_buffer.commit" }
   ```
   ```json
   { "type": "response.create" }
   ```
   *(If `awaiting_cancel` is active from a recent barge-in, defer `response.create` until the cancelled `response.done` arrives.)*

### Downlink (Server → Client)
- `input_audio_buffer.committed`
- `response.created` → `{ "response": { "id": "resp_...", "status": "in_progress" } }`
- `response.output_item.added` / `response.content_part.added`
- `response.output_text.delta` → `{ "response_id": "resp_...", "delta": "..." }`
- `response.output_audio.delta` → `{ "response_id": "resp_...", "delta": "<base64 PCM16 24kHz mono>" }`
- `response.output_text.done` / `response.output_audio.done`
- `response.done` → `{ "response": { "id": "resp_...", "status": "completed" | "cancelled" } }`
- `error` → `{ "error": { "type": "...", "message": "..." } }`

---

## 4. Barge-In & `response_id` Sequencing

```text
  SPEAKING (active_resp = R1)
      │
      │  User speaks >= 220 ms at +6 dB margin (or taps orb)
      ▼
  1. audio_engine_flush_playback()      (mute/clear jitter buffer immediately)
  2. active_resp = "" ; awaiting_cancel = true
  3. send {"type": "response.cancel"}
      │
      ├──► Ignore any remaining response.output_audio.delta / output_text.delta for R1
      │
      ▼
  4. User finishes speaking while awaiting_cancel == true:
     send {"type": "input_audio_buffer.commit"}
     set pending_response = true
      │
      ▼
  5. Receive response.done (id=R1, status="cancelled"):
     awaiting_cancel = false
     if pending_response:
         pending_response = false
         send {"type": "response.create"}
```

This matches `server-realtime.cpp` and `webUIs/lfm3-voice-chat/index.html` and prevents stale audio frames or premature `response.create` collisions.

---

## 5. Phase 2 Extension Events (`harness.*`)

Unknown JSON `type` strings are silently ignored by the PoC client unless they match the `harness.*` contract below:

### Server → Client
- **`harness.agent.info`** — Updates the AMOLED Agent Card and orb appearance dynamically:
  ```json
  {
    "type": "harness.agent.info",
    "id": "ember",
    "name": "Ember",
    "role": "Home Ops",
    "accent_color": "#F59E0B",
    "orb_style": "rings"
  }
  ```
  - `orb_style`: `"pulse"` | `"rings"` | `"halo"` (optional).
  - `accent_color`: `#RRGGBB` hex string (optional).
- **`harness.tool.status`** — Updates UI state to `TOOL_CALLING` and shows tool progress in the caption area:
  ```json
  {
    "type": "harness.tool.status",
    "call_id": "call_123",
    "name": "calendar_create",
    "state": "running"
  }
  ```
  - `state`: `"running"` | `"done"` | `"error"`.
- **`harness.tool.confirm`** — Prompts an `[Allow]` / `[Deny]` modal on the 368×448 touchscreen:
  ```json
  {
    "type": "harness.tool.confirm",
    "call_id": "call_123",
    "name": "door_unlock",
    "summary": "Unlock front door?"
  }
  ```

### Client → Server
- **`harness.tool.confirm_result`** — Sent when the user taps `[Allow]` or `[Deny]`:
  ```json
  {
    "type": "harness.tool.confirm_result",
    "call_id": "call_123",
    "approved": true
  }
  ```
- **`harness.agent.switch`** — Sent when the user swipes left/right on the center Agent Card:
  ```json
  {
    "type": "harness.agent.switch",
    "direction": "next"
  }
  ```
