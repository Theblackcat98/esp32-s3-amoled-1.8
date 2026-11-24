# Phase 2 — LAN Agent Harness (Future Boundary)

The ESP32-S3 firmware is intentionally a **thin realtime I/O terminal**: it handles 16 kHz full-duplex I2S audio, ES8311 DAC loopback AEC, ESP-SR VAD/barge-in, 24→16 kHz downlink resampling, and the LVGL 9 AMOLED Orb UI. All prompt routing, memory, and tool execution stay off-device.

## Integration Boundary

```text
  ESP32-S3 (Ember UI)                         LAN Agent Harness                        llama-server
  ws://<HARNESS_IP>:8000/v1/realtime  <====>  - Proxies /v1/realtime GA events <====>  ws://127.0.0.1:8082/v1/realtime
                                              - Injects tools & system prompts
                                              - Intercepts function_call items
                                              - Executes MCP / local actions
                                              - Emits harness.* UI metadata
```

## Supported `harness.*` Hooks Already Wired in Firmware

The ESP32-S3 WebSocket parser (`firmware/main/net/realtime_ws.c`) already supports:

1. **`harness.agent.info`** (Server → Device): Updates displayed agent name, role subtitle, orb hex accent color (arbitrary `#RRGGBB`), and orb style (`"pulse"` | `"rings"` | `"halo"`).
2. **`harness.tool.status`** (Server → Device): Switches UI state to `TOOL_CALLING`, updates the live caption with the active tool name, and restores normal state when `"done"` or `"error"`.
3. **`harness.tool.confirm`** (Server → Device) → **`harness.tool.confirm_result`** (Device → Server): Displays a modal `[Allow]` / `[Deny]` prompt on the 1.8" AMOLED touchscreen for sensitive actions.
4. **`harness.agent.switch`** (Device → Server): Sent on horizontal card swipe (`"direction": "next" | "prev"`) so the Harness can swap active personas and answer with an updated `harness.agent.info`.

See [docs/protocol.md](../docs/protocol.md) for full JSON schemas.
