#!/usr/bin/env python3
"""Offline stdlib mock server implementing llama-server's OpenAI Realtime GA /v1/realtime schema.

Matches tools/server/server-realtime.cpp behavior:
  - Optional Bearer token auth (--api-key) returning HTTP 401 on mismatch
  - session.created with 16 kHz input PCM16, 24 kHz output PCM16, turn_detection=null
  - session.update validation (locks instructions/tools/type after first audio/message)
  - input_audio_buffer.append & input_audio_buffer.commit
  - response.create streaming response.output_text.delta + response.output_audio.delta (24 kHz)
  - response.cancel emitting 2 realistic in-flight tail deltas before response.done(status="cancelled")
  - Optional --inject-harness flag emitting harness.agent.info & harness.tool.status events
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import math
import os
import secrets
import struct
from typing import Any, Optional

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def rand_id(prefix: str) -> str:
    return f"{prefix}_{secrets.token_hex(6)}"


async def read_ws_frame(reader: asyncio.StreamReader) -> Optional[tuple[int, bytes]]:
    chunks: list[bytes] = []
    first_opcode = None
    while True:
        try:
            hdr = await reader.readexactly(2)
        except (asyncio.IncompleteReadError, ConnectionError):
            return None
        b1, b2 = hdr[0], hdr[1]
        fin = (b1 & 0x80) != 0
        opcode = b1 & 0x0F
        masked = (b2 & 0x80) != 0
        length = b2 & 0x7F
        if length == 126:
            ext = await reader.readexactly(2)
            length = struct.unpack("!H", ext)[0]
        elif length == 127:
            ext = await reader.readexactly(8)
            length = struct.unpack("!Q", ext)[0]
        mask_key = await reader.readexactly(4) if masked else b""
        payload = await reader.readexactly(length) if length else b""
        if masked and payload:
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

        if opcode in (0x8, 0x9, 0xA):
            return opcode, payload

        if first_opcode is None:
            first_opcode = opcode
        chunks.append(payload)
        if fin:
            return first_opcode, b"".join(chunks)


def build_ws_frame(opcode: int, payload: bytes) -> bytes:
    out = bytearray([0x80 | (opcode & 0x0F)])
    n = len(payload)
    if n < 126:
        out.append(n)
    elif n <= 0xFFFF:
        out.append(126)
        out.extend(struct.pack("!H", n))
    else:
        out.append(127)
        out.extend(struct.pack("!Q", n))
    out.extend(payload)
    return bytes(out)


class MockRealtimeSession:
    def __init__(
        self,
        writer: asyncio.StreamWriter,
        chunk_ms: int = 40,
        inject_harness: bool = False,
        model: str = "lfm3-audio-2b",
    ) -> None:
        self.writer = writer
        self.chunk_ms = chunk_ms
        self.inject_harness = inject_harness
        self.model = model
        self.sess_id = rand_id("sess")
        self.in_rate = 16000
        self.out_rate = 24000
        self.instructions = ""
        self.out_modalities: Optional[list[str]] = None
        self.started = False
        self.has_audio = False
        self.turn_open = False
        self.audio_bytes = 0
        self.last_item_id: Optional[str] = None
        self.send_lock = asyncio.Lock()
        self.resp_task: Optional[asyncio.Task] = None
        self.cancel_event: Optional[asyncio.Event] = None
        self.closed = False

    async def send_json(self, obj: dict[str, Any]) -> None:
        if self.closed:
            return
        payload = dict(obj)
        payload.setdefault("event_id", rand_id("event"))
        frame = build_ws_frame(0x1, json.dumps(payload).encode("utf-8"))
        async with self.send_lock:
            if not self.closed:
                self.writer.write(frame)
                await self.writer.drain()

    async def send_error(self, message: str, err_type: str = "invalid_request_error", code: Optional[str] = None) -> None:
        await self.send_json(
            {
                "type": "error",
                "error": {"type": err_type, "code": code, "message": message, "param": None},
            }
        )

    def session_obj(self) -> dict[str, Any]:
        return {
            "id": self.sess_id,
            "object": "realtime.session",
            "type": "realtime",
            "model": self.model,
            "instructions": self.instructions,
            "output_modalities": self.out_modalities,
            "max_output_tokens": "inf",
            "tools": [],
            "tool_choice": "auto",
            "audio": {
                "input": {
                    "format": {"type": "audio/pcm", "rate": self.in_rate},
                    "turn_detection": None,
                },
                "output": {
                    "format": {"type": "audio/pcm", "rate": self.out_rate},
                },
            },
        }

    async def commit_audio(self) -> bool:
        if not self.has_audio:
            await self.send_error("input_audio_buffer.commit: the buffer is empty", code="input_audio_buffer_commit_empty")
            return False
        self.has_audio = False
        prev = self.last_item_id
        self.last_item_id = rand_id("item")
        await self.send_json(
            {
                "type": "input_audio_buffer.committed",
                "previous_item_id": prev,
                "item_id": self.last_item_id,
            }
        )
        item = {
            "id": self.last_item_id,
            "object": "realtime.item",
            "type": "message",
            "role": "user",
            "status": "completed",
            "content": [{"type": "input_audio"}],
        }
        await self.send_json({"type": "conversation.item.added", "previous_item_id": prev, "item": item})
        await self.send_json({"type": "conversation.item.done", "previous_item_id": prev, "item": item})
        return True

    async def run_response(self, resp_id: str, item_id: str, cancel_ev: asyncio.Event, turn_bytes: int) -> None:
        words = ["Acknowledged", " your", " voice", " input", " over", " the", " local", " realtime", " link."]
        n_chunks = 20
        samples_per_chunk = int(self.out_rate * self.chunk_ms / 1000)

        await self.send_json(
            {
                "type": "response.created",
                "response": {
                    "id": resp_id,
                    "object": "realtime.response",
                    "status": "in_progress",
                    "output_modalities": self.out_modalities or ["text", "audio"],
                    "output": [],
                },
            }
        )

        if self.inject_harness and turn_bytes > 0:
            call_id = rand_id("call")
            await self.send_json({"type": "harness.tool.status", "call_id": call_id, "name": "local_ping", "state": "running"})
            await asyncio.sleep(0.03)
            await self.send_json({"type": "harness.tool.status", "call_id": call_id, "name": "local_ping", "state": "done"})

        msg_item = {
            "id": item_id,
            "object": "realtime.item",
            "type": "message",
            "role": "assistant",
            "status": "in_progress",
            "content": [],
        }
        await self.send_json(
            {"type": "response.output_item.added", "response_id": resp_id, "output_index": 0, "item": msg_item}
        )
        await self.send_json(
            {
                "type": "response.content_part.added",
                "response_id": resp_id,
                "item_id": item_id,
                "output_index": 0,
                "content_index": 0,
                "part": {"type": "output_text", "text": ""},
            }
        )
        await self.send_json(
            {
                "type": "response.content_part.added",
                "response_id": resp_id,
                "item_id": item_id,
                "output_index": 0,
                "content_index": 1,
                "part": {"type": "output_audio", "transcript": ""},
            }
        )

        full_text = []
        cancelled = False
        phase = 0.0
        freq = 330.0
        for idx in range(n_chunks):
            if cancel_ev.is_set():
                cancelled = True
                # Emit 2 realistic tail deltas before finalizing cancelled response
                for tail_i in range(2):
                    pcm = self._synth_pcm(samples_per_chunk, freq, phase)
                    phase += 2.0 * math.pi * freq * samples_per_chunk / self.out_rate
                    await self.send_json(
                        {
                            "type": "response.output_audio.delta",
                            "response_id": resp_id,
                            "item_id": item_id,
                            "output_index": 0,
                            "content_index": 1,
                            "delta": base64.b64encode(pcm).decode("ascii"),
                        }
                    )
                    await asyncio.sleep(0.005)
                break

            if idx < len(words):
                w = words[idx]
                full_text.append(w)
                await self.send_json(
                    {
                        "type": "response.output_text.delta",
                        "response_id": resp_id,
                        "item_id": item_id,
                        "output_index": 0,
                        "content_index": 0,
                        "delta": w,
                    }
                )

            pcm = self._synth_pcm(samples_per_chunk, freq, phase)
            phase += 2.0 * math.pi * freq * samples_per_chunk / self.out_rate
            await self.send_json(
                {
                    "type": "response.output_audio.delta",
                    "response_id": resp_id,
                    "item_id": item_id,
                    "output_index": 0,
                    "content_index": 1,
                    "delta": base64.b64encode(pcm).decode("ascii"),
                }
            )
            await asyncio.sleep(0.012)

        text_str = "".join(full_text)
        status = "cancelled" if cancelled else "completed"
        await self.send_json(
            {
                "type": "response.output_text.done",
                "response_id": resp_id,
                "item_id": item_id,
                "output_index": 0,
                "content_index": 0,
                "text": text_str,
            }
        )
        await self.send_json(
            {
                "type": "response.content_part.done",
                "response_id": resp_id,
                "item_id": item_id,
                "output_index": 0,
                "content_index": 0,
                "part": {"type": "output_text", "text": text_str},
            }
        )
        await self.send_json(
            {
                "type": "response.output_audio.done",
                "response_id": resp_id,
                "item_id": item_id,
                "output_index": 0,
                "content_index": 1,
            }
        )
        await self.send_json(
            {
                "type": "response.content_part.done",
                "response_id": resp_id,
                "item_id": item_id,
                "output_index": 0,
                "content_index": 1,
                "part": {"type": "output_audio", "transcript": ""},
            }
        )
        done_item = {
            "id": item_id,
            "object": "realtime.item",
            "type": "message",
            "role": "assistant",
            "status": status,
            "content": [
                {"type": "output_text", "text": text_str},
                {"type": "output_audio", "transcript": ""},
            ],
        }
        await self.send_json(
            {"type": "response.output_item.done", "response_id": resp_id, "output_index": 0, "item": done_item}
        )
        self.last_item_id = item_id
        await self.send_json(
            {
                "type": "response.done",
                "response": {
                    "id": resp_id,
                    "object": "realtime.response",
                    "status": status,
                    "output": [done_item],
                    "usage": {"output_tokens": 24},
                },
            }
        )

    @staticmethod
    def _synth_pcm(n_samples: int, freq_hz: float, phase0: float) -> bytes:
        buf = bytearray(n_samples * 2)
        step = 2.0 * math.pi * freq_hz / 24000.0
        for i in range(n_samples):
            val = int(9000.0 * math.sin(phase0 + step * i))
            struct.pack_into("<h", buf, i * 2, val)
        return bytes(buf)

    async def handle_event(self, ev: dict[str, Any]) -> None:
        ev_type = ev.get("type", "")
        if ev_type == "session.update":
            sess = ev.get("session", {})
            if "instructions" in sess and isinstance(sess["instructions"], str):
                if self.started:
                    await self.send_error("session.update: instructions cannot change after the first audio or message")
                else:
                    self.instructions = sess["instructions"]
            if "output_modalities" in sess:
                self.out_modalities = sess["output_modalities"]
            await self.send_json({"type": "session.updated", "session": self.session_obj()})
            if self.inject_harness:
                await self.send_json(
                    {
                        "type": "harness.agent.info",
                        "id": "ember",
                        "name": "Ember",
                        "role": "Harness Test",
                        "accent_color": "#10B981",
                        "orb_style": "rings",
                    }
                )
        elif ev_type == "input_audio_buffer.append":
            b64 = ev.get("audio", "")
            try:
                raw = base64.b64decode(b64, validate=True)
            except Exception:
                await self.send_error("invalid base64 audio")
                return
            if len(raw) >= 2:
                self.started = True
                self.turn_open = True
                self.has_audio = True
                self.audio_bytes += len(raw)
        elif ev_type == "input_audio_buffer.commit":
            await self.commit_audio()
        elif ev_type == "input_audio_buffer.clear":
            self.has_audio = False
            self.audio_bytes = 0
            await self.send_json({"type": "input_audio_buffer.cleared"})
        elif ev_type == "response.create":
            if self.resp_task and not self.resp_task.done():
                await self.resp_task
            if self.has_audio:
                if not await self.commit_audio():
                    return
            elif not self.turn_open:
                await self.send_error("response.create: commit audio or create a message first", code="conversation_empty")
                return
            turn_bytes = self.audio_bytes
            self.audio_bytes = 0
            self.turn_open = False
            self.cancel_event = asyncio.Event()
            self.resp_task = asyncio.create_task(
                self.run_response(rand_id("resp"), rand_id("item"), self.cancel_event, turn_bytes)
            )
        elif ev_type == "response.cancel":
            if self.cancel_event is not None:
                self.cancel_event.set()
        elif ev_type.startswith("harness."):
            pass
        else:
            await self.send_error(f"unknown event type: {ev_type}")


async def handle_client(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    api_key: str,
    chunk_ms: int,
    inject_harness: bool,
) -> None:
    try:
        raw = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10.0)
    except Exception:
        writer.close()
        return

    lines = raw.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) < 2:
        writer.close()
        return
    method, target = parts[0], parts[1]
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    if target.split("?", 1)[0] == "/health":
        body = b'{"status":"ok"}\n'
        writer.write(
            b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
            + body
        )
        await writer.drain()
        writer.close()
        return

    if api_key:
        auth = headers.get("authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else auth
        if token != api_key:
            body = b'{"error":{"message":"invalid api key"}}\n'
            writer.write(
                b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
                + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
                + body
            )
            await writer.drain()
            writer.close()
            return

    if method != "GET" or target.split("?", 1)[0] != "/v1/realtime" or headers.get("upgrade", "").lower() != "websocket":
        writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        await writer.drain()
        writer.close()
        return

    ws_key = headers.get("sec-websocket-key", "")
    accept = base64.b64encode(hashlib.sha1((ws_key + WS_GUID).encode("ascii")).digest()).decode("ascii")
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    writer.write(resp.encode("ascii"))
    await writer.drain()

    session = MockRealtimeSession(writer, chunk_ms=chunk_ms, inject_harness=inject_harness)
    await session.send_json({"type": "session.created", "session": session.session_obj()})

    try:
        while True:
            frame = await read_ws_frame(reader)
            if frame is None:
                break
            opcode, payload = frame
            if opcode == 0x8:
                async with session.send_lock:
                    writer.write(build_ws_frame(0x8, b""))
                    await writer.drain()
                break
            if opcode == 0x9:
                async with session.send_lock:
                    writer.write(build_ws_frame(0xA, payload))
                    await writer.drain()
                continue
            if opcode == 0x1:
                try:
                    ev = json.loads(payload.decode("utf-8"))
                except Exception:
                    await session.send_error("invalid JSON")
                    continue
                await session.handle_event(ev)
    finally:
        session.closed = True
        if session.cancel_event is not None:
            session.cancel_event.set()
        if session.resp_task is not None and not session.resp_task.done():
            session.resp_task.cancel()
        writer.close()


async def run_server(args: argparse.Namespace) -> None:
    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, args.api_key, args.chunk_ms, args.inject_harness),
        args.host,
        args.port,
        reuse_address=True,
    )
    print(f"mock /v1/realtime listening on ws://{args.host}:{args.port}/v1/realtime", flush=True)
    async with server:
        await server.serve_forever()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=18082)
    p.add_argument("--api-key", default="")
    p.add_argument("--chunk-ms", type=int, default=40)
    p.add_argument("--inject-harness", action="store_true")
    try:
        asyncio.run(run_server(p.parse_args()))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
