#!/usr/bin/env python3
"""Desktop /v1/realtime GA test client (stdlib WebSocket + numpy + optional pyaudio).

Modes:
  --synthetic     Non-interactive protocol, latency, and barge-in self-test (no audio hardware needed)
  --wav FILE      Stream one or more PCM16 WAV files as turns and write output WAV(s)
  --mic           Interactive full-duplex desktop microphone/speaker conversation via PyAudio
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import os
import secrets
import struct
import sys
import time
import urllib.parse
import wave
from pathlib import Path
from typing import Any, Optional

import numpy as np

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
DEFAULT_PROMPT_PATH = Path(__file__).resolve().parent / "prompts" / "default_agent.txt"


class StdlibWsClient:
    """Minimal async RFC 6455 WebSocket client using Python stdlib only."""

    def __init__(self, url: str, api_key: str = "") -> None:
        self.url = url
        self.api_key = api_key
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None
        self._send_lock = asyncio.Lock()

    async def connect(self) -> None:
        parsed = urllib.parse.urlparse(self.url)
        if parsed.scheme not in ("ws", "http"):
            raise ValueError(f"Only ws:// (or http:// mapped to ws://) supported: {self.url}")
        host = parsed.hostname or "127.0.0.1"
        port = parsed.port or 80
        path = parsed.path or "/v1/realtime"
        if parsed.query:
            path = f"{path}?{parsed.query}"

        self.reader, self.writer = await asyncio.open_connection(host, port)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        lines = [
            f"GET {path} HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        if self.api_key:
            lines.append(f"Authorization: Bearer {self.api_key}")
        self.writer.write(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        await self.writer.drain()

        raw_hdr = await asyncio.wait_for(self.reader.readuntil(b"\r\n\r\n"), 10.0)
        status_line = raw_hdr.decode("latin-1").split("\r\n", 1)[0]
        if " 101 " not in status_line:
            raise PermissionError(f"WebSocket upgrade rejected: {status_line}")
        expected_accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        if expected_accept not in raw_hdr.decode("latin-1"):
            raise RuntimeError("Invalid Sec-WebSocket-Accept from server")

    async def send_json(self, obj: dict[str, Any]) -> None:
        assert self.writer is not None
        payload = json.dumps(obj).encode("utf-8")
        frame = self._build_client_frame(0x1, payload)
        async with self._send_lock:
            self.writer.write(frame)
            await self.writer.drain()

    async def recv_json(self, timeout: float = 30.0) -> dict[str, Any]:
        assert self.reader is not None and self.writer is not None
        chunks: list[bytes] = []
        while True:
            hdr = await asyncio.wait_for(self.reader.readexactly(2), timeout)
            b1, b2 = hdr[0], hdr[1]
            fin = (b1 & 0x80) != 0
            opcode = b1 & 0x0F
            masked = (b2 & 0x80) != 0
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", await self.reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", await self.reader.readexactly(8))[0]
            mask = await self.reader.readexactly(4) if masked else b""
            payload = await self.reader.readexactly(length) if length else b""
            if masked and payload:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x8:
                raise EOFError("WebSocket closed by peer")
            if opcode == 0x9:
                async with self._send_lock:
                    self.writer.write(self._build_client_frame(0xA, payload))
                    await self.writer.drain()
                continue
            if opcode == 0xA:
                continue
            chunks.append(payload)
            if fin:
                return json.loads(b"".join(chunks).decode("utf-8"))

    async def close(self) -> None:
        if self.writer is not None:
            try:
                async with self._send_lock:
                    self.writer.write(self._build_client_frame(0x8, b""))
                    await self.writer.drain()
            except Exception:
                pass
            self.writer.close()

    @staticmethod
    def _build_client_frame(opcode: int, payload: bytes) -> bytes:
        mask = secrets.token_bytes(4)
        n = len(payload)
        out = bytearray([0x80 | (opcode & 0x0F)])
        if n < 126:
            out.append(0x80 | n)
        elif n <= 0xFFFF:
            out.append(0x80 | 126)
            out.extend(struct.pack("!H", n))
        else:
            out.append(0x80 | 127)
            out.extend(struct.pack("!Q", n))
        out.extend(mask)
        out.extend(bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        return bytes(out)


def load_instructions(cli_arg: Optional[str]) -> str:
    if cli_arg:
        return cli_arg
    if DEFAULT_PROMPT_PATH.is_file():
        return DEFAULT_PROMPT_PATH.read_text(encoding="utf-8").strip()
    return "Respond with interleaved text and audio. Keep answers short and clear."


def ws_url_from_base(base: str, model: str) -> str:
    url = base.rstrip("/")
    if url.startswith("http://"):
        url = "ws://" + url[7:]
    elif url.startswith("https://"):
        url = "wss://" + url[8:]
    if not url.endswith("/v1/realtime"):
        url = f"{url}/v1/realtime"
    sep = "&" if "?" in url else "?"
    return f"{url}{sep}model={urllib.parse.quote(model)}"


def read_wav_mono(path: str, target_rate: int) -> np.ndarray:
    with wave.open(path, "rb") as wf:
        if wf.getsampwidth() != 2:
            raise ValueError(f"{path}: expected 16-bit PCM WAV")
        raw = wf.readframes(wf.getnframes())
        x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        x = x.reshape(-1, wf.getnchannels()).mean(axis=1)
        sr = wf.getframerate()
    if sr != target_rate:
        n = int(round(len(x) * target_rate / sr))
        x = np.interp(np.arange(n) * sr / target_rate, np.arange(len(x)), x)
    return np.clip(x, -32768, 32767).astype(np.int16)


def write_wav_mono(path: str, pcm: np.ndarray, rate: int) -> None:
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm.tobytes())


async def handshake(ws: StdlibWsClient, instructions: str) -> tuple[int, int]:
    ev = await ws.recv_json()
    if ev.get("type") != "session.created":
        raise RuntimeError(f"Expected session.created, got: {ev}")
    sess = ev["session"]
    audio_cfg = sess["audio"]
    in_rate = int(audio_cfg["input"]["format"]["rate"])
    out_rate = int((audio_cfg.get("output") or {}).get("format", {}).get("rate", 24000))
    turn_det = audio_cfg["input"].get("turn_detection")
    if turn_det is not None:
        raise RuntimeError(f"Expected turn_detection=null, got: {turn_det}")

    await ws.send_json(
        {
            "type": "session.update",
            "session": {
                "type": "realtime",
                "instructions": instructions,
                "output_modalities": ["text", "audio"],
            },
        }
    )
    ev2 = await ws.recv_json()
    if ev2.get("type") != "session.updated":
        raise RuntimeError(f"Expected session.updated, got: {ev2}")
    return in_rate, out_rate


async def run_synthetic_suite(args: argparse.Namespace) -> int:
    url = ws_url_from_base(args.url, args.model)
    instructions = load_instructions(args.instructions)

    if args.verify_auth_reject and args.api_key:
        bad_ws = StdlibWsClient(url, api_key="wrong-key-check")
        try:
            await bad_ws.connect()
            await bad_ws.close()
            raise RuntimeError("Expected 401 rejection on wrong API key, but connection succeeded!")
        except PermissionError as exc:
            print(f"[PASS] Auth rejection verified: {exc}")

    ws = StdlibWsClient(url, api_key=args.api_key)
    await ws.connect()
    try:
        in_rate, out_rate = await handshake(ws, instructions)
        print(f"[PASS] Handshake complete: in_rate={in_rate} Hz, out_rate={out_rate} Hz")

        # --- Turn 1: Normal complete turn ---
        t = np.arange(int(in_rate * 0.6), dtype=np.float32) / in_rate
        pcm1 = (np.sin(2.0 * np.pi * 440.0 * t) * 9000.0).astype(np.int16)
        step = in_rate * args.chunk_ms // 1000
        for i in range(0, len(pcm1), step):
            await ws.send_json(
                {"type": "input_audio_buffer.append", "audio": base64.b64encode(pcm1[i : i + step].tobytes()).decode("ascii")}
            )
        t_commit = time.perf_counter()
        await ws.send_json({"type": "input_audio_buffer.commit"})
        await ws.send_json({"type": "response.create"})

        active_resp_id: Optional[str] = None
        t_first_audio: Optional[float] = None
        text_chunks: list[str] = []
        audio_chunks: list[np.ndarray] = []

        while True:
            ev = await ws.recv_json()
            et = ev.get("type", "")
            if et == "error":
                raise RuntimeError(f"Server error in Turn 1: {ev}")
            if et == "response.created":
                active_resp_id = ev["response"]["id"]
            elif et == "response.output_text.delta":
                if ev.get("response_id") == active_resp_id:
                    text_chunks.append(ev.get("delta", ""))
            elif et == "response.output_audio.delta":
                if ev.get("response_id") == active_resp_id:
                    t_first_audio = t_first_audio or time.perf_counter()
                    raw = base64.b64decode(ev["delta"])
                    audio_chunks.append(np.frombuffer(raw, dtype=np.int16))
            elif et == "response.done":
                resp = ev["response"]
                if resp.get("id") == active_resp_id:
                    if resp.get("status") != "completed":
                        raise RuntimeError(f"Expected completed status in Turn 1, got {resp.get('status')}")
                    break

        total_dur = time.perf_counter() - t_commit
        pcm_out = np.concatenate(audio_chunks) if audio_chunks else np.zeros(0, dtype=np.int16)
        audio_sec = len(pcm_out) / out_rate
        if len(pcm_out) == 0 or not "".join(text_chunks).strip():
            raise RuntimeError("Turn 1 missing audio or text output")
        ttfa_ms = (t_first_audio - t_commit) * 1000.0 if t_first_audio else -1.0
        rtf = audio_sec / max(total_dur, 1e-6)
        print(
            f"[PASS] Turn 1 completed: TTFA={ttfa_ms:.1f} ms, audio={audio_sec:.2f} s, "
            f"RTF={rtf:.2f}x, text={''.join(text_chunks)!r}"
        )

        # --- Turn 2 -> Barge-In -> Turn 3 ---
        for i in range(0, len(pcm1) // 2, step):
            await ws.send_json(
                {"type": "input_audio_buffer.append", "audio": base64.b64encode(pcm1[i : i + step].tobytes()).decode("ascii")}
            )
        await ws.send_json({"type": "input_audio_buffer.commit"})
        await ws.send_json({"type": "response.create"})

        cancelled_resp_id: Optional[str] = None
        awaiting_cancel = False
        pending_response = False
        stale_filtered = 0
        turn3_audio: list[np.ndarray] = []
        turn3_text: list[str] = []
        turn3_resp_id: Optional[str] = None

        while True:
            ev = await ws.recv_json()
            et = ev.get("type", "")
            rid = ev.get("response_id") or ev.get("response", {}).get("id")
            if et == "error":
                raise RuntimeError(f"Server error in Barge-In test: {ev}")
            if et == "response.created":
                if cancelled_resp_id is None and not awaiting_cancel:
                    active_resp_id = rid
                else:
                    turn3_resp_id = rid
                    active_resp_id = rid
            elif et in ("response.output_audio.delta", "response.output_text.delta"):
                if active_resp_id is None or rid != active_resp_id:
                    stale_filtered += 1
                    continue
                if cancelled_resp_id is None and et == "response.output_audio.delta":
                    # Trigger barge-in on first audio chunk of Turn 2
                    cancelled_resp_id = active_resp_id
                    active_resp_id = None
                    awaiting_cancel = True
                    await ws.send_json({"type": "response.cancel"})
                    # Immediately stream Turn 3 speech + commit while cancel settles
                    for i in range(0, len(pcm1) // 2, step):
                        await ws.send_json(
                            {
                                "type": "input_audio_buffer.append",
                                "audio": base64.b64encode(pcm1[i : i + step].tobytes()).decode("ascii"),
                            }
                        )
                    await ws.send_json({"type": "input_audio_buffer.commit"})
                    pending_response = True
                elif turn3_resp_id is not None and rid == turn3_resp_id:
                    if et == "response.output_audio.delta":
                        turn3_audio.append(np.frombuffer(base64.b64decode(ev["delta"]), dtype=np.int16))
                    else:
                        turn3_text.append(ev.get("delta", ""))
            elif et == "response.done":
                status = ev["response"].get("status")
                if awaiting_cancel and rid == cancelled_resp_id:
                    if status != "cancelled":
                        raise RuntimeError(f"Expected cancelled status for {cancelled_resp_id}, got {status}")
                    awaiting_cancel = False
                    if pending_response:
                        pending_response = False
                        await ws.send_json({"type": "response.create"})
                elif turn3_resp_id is not None and rid == turn3_resp_id:
                    if status != "completed":
                        raise RuntimeError(f"Expected Turn 3 completed, got {status}")
                    break

        if not turn3_audio or not "".join(turn3_text).strip():
            raise RuntimeError("Turn 3 produced empty audio or text after barge-in")
        print(
            f"[PASS] Barge-in & Turn 3 completed: stale_tail_deltas_filtered={stale_filtered}, "
            f"turn3_audio={sum(len(c) for c in turn3_audio) / out_rate:.2f} s"
        )
        return 0
    finally:
        await ws.close()


async def run_wav_mode(args: argparse.Namespace) -> int:
    url = ws_url_from_base(args.url, args.model)
    ws = StdlibWsClient(url, api_key=args.api_key)
    await ws.connect()
    try:
        in_rate, out_rate = await handshake(ws, load_instructions(args.instructions))
        for idx, path in enumerate(args.wav):
            pcm = read_wav_mono(path, in_rate)
            step = in_rate * args.chunk_ms // 1000
            for j in range(0, len(pcm), step):
                chunk = pcm[j : j + step]
                await ws.send_json(
                    {"type": "input_audio_buffer.append", "audio": base64.b64encode(chunk.tobytes()).decode("ascii")}
                )
                await asyncio.sleep(len(chunk) / in_rate)
            t0 = time.perf_counter()
            await ws.send_json({"type": "input_audio_buffer.commit"})
            await ws.send_json({"type": "response.create"})
            t_first = None
            audio_parts: list[np.ndarray] = []
            while True:
                ev = await ws.recv_json()
                et = ev.get("type", "")
                if et == "response.output_text.delta":
                    t_first = t_first or time.perf_counter()
                    print(ev.get("delta", ""), end="", flush=True)
                elif et == "response.output_audio.delta":
                    t_first = t_first or time.perf_counter()
                    audio_parts.append(np.frombuffer(base64.b64decode(ev["delta"]), dtype=np.int16))
                elif et == "response.done":
                    break
                elif et == "error":
                    print(f"\nerror: {ev}", file=sys.stderr)
                    return 1
            print()
            if audio_parts:
                out_pcm = np.concatenate(audio_parts)
                out_path = f"{args.out.removesuffix('.wav')}_{idx}.wav" if len(args.wav) > 1 else args.out
                write_wav_mono(out_path, out_pcm, out_rate)
                dur = time.perf_counter() - t0
                print(
                    f"turn {idx}: ttfa={(t_first - t0)*1000:.0f} ms, "
                    f"audio={len(out_pcm)/out_rate:.2f} s, rtf={(len(out_pcm)/out_rate)/max(dur,1e-6):.2f}x -> {out_path}"
                )
        return 0
    finally:
        await ws.close()


async def run_mic_mode(args: argparse.Namespace) -> int:
    try:
        import pyaudio
    except ImportError:
        print("PyAudio not installed; use --synthetic or --wav instead.", file=sys.stderr)
        return 2

    url = ws_url_from_base(args.url, args.model)
    ws = StdlibWsClient(url, api_key=args.api_key)
    await ws.connect()
    pa = pyaudio.PyAudio()
    try:
        in_rate, out_rate = await handshake(ws, load_instructions(args.instructions))
        chunk_samples = in_rate * args.chunk_ms // 1000
        in_stream = pa.open(format=pyaudio.paInt16, channels=1, rate=in_rate, input=True, frames_per_buffer=chunk_samples)
        out_stream = pa.open(format=pyaudio.paInt16, channels=1, rate=out_rate, output=True)
        print("Live mic mode active (Ctrl+C to quit)...")

        active_resp_id: Optional[str] = None
        awaiting_cancel = False
        pending_response = False
        speaking_playback = False
        noise_db = -55.0
        speech_ms = 0.0
        silence_ms = 0.0
        in_utter = False
        confirmed = False
        hold_buf: list[bytes] = []

        async def rx_loop() -> None:
            nonlocal active_resp_id, awaiting_cancel, pending_response, speaking_playback
            while True:
                ev = await ws.recv_json(timeout=120.0)
                et = ev.get("type", "")
                rid = ev.get("response_id") or ev.get("response", {}).get("id")
                if et == "response.created":
                    active_resp_id = rid
                    speaking_playback = True
                elif et == "response.output_text.delta" and rid == active_resp_id:
                    print(ev.get("delta", ""), end="", flush=True)
                elif et == "response.output_audio.delta" and rid == active_resp_id:
                    out_stream.write(base64.b64decode(ev["delta"]))
                elif et == "response.done":
                    if awaiting_cancel and rid == active_resp_id or active_resp_id is None:
                        awaiting_cancel = False
                        speaking_playback = False
                        if pending_response:
                            pending_response = False
                            await ws.send_json({"type": "response.create"})
                    elif rid == active_resp_id:
                        print()
                        active_resp_id = None
                        speaking_playback = False

        rx_task = asyncio.create_task(rx_loop())
        try:
            while True:
                raw = await asyncio.to_thread(in_stream.read, chunk_samples, exception_on_overflow=False)
                samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                rms = float(np.sqrt(np.mean(samples * samples)) + 1e-9)
                db = 20.0 * float(np.log10(rms))
                thresh = noise_db + (14.0 if speaking_playback else 8.0)
                if not in_utter and not speaking_playback and db < noise_db + 6.0:
                    noise_db = max(-70.0, min(-30.0, noise_db * 0.9 + db * 0.1))

                if db > thresh:
                    speech_ms += args.chunk_ms
                    silence_ms = 0.0
                    if not in_utter and speech_ms >= (220.0 if speaking_playback else 120.0):
                        if speaking_playback and active_resp_id:
                            active_resp_id = None
                            awaiting_cancel = True
                            speaking_playback = False
                            await ws.send_json({"type": "response.cancel"})
                        in_utter = True
                        confirmed = False
                        hold_buf = [raw]
                    elif in_utter:
                        if confirmed:
                            await ws.send_json({"type": "input_audio_buffer.append", "audio": base64.b64encode(raw).decode("ascii")})
                        else:
                            hold_buf.append(raw)
                            if len(hold_buf) * args.chunk_ms >= 300:
                                confirmed = True
                                for h in hold_buf:
                                    await ws.send_json({"type": "input_audio_buffer.append", "audio": base64.b64encode(h).decode("ascii")})
                                hold_buf.clear()
                else:
                    speech_ms = 0.0
                    if in_utter:
                        silence_ms += args.chunk_ms
                        if confirmed:
                            await ws.send_json({"type": "input_audio_buffer.append", "audio": base64.b64encode(raw).decode("ascii")})
                        if silence_ms >= 650.0:
                            if confirmed:
                                await ws.send_json({"type": "input_audio_buffer.commit"})
                                if awaiting_cancel or active_resp_id:
                                    pending_response = True
                                else:
                                    await ws.send_json({"type": "response.create"})
                            in_utter = False
                            confirmed = False
                            hold_buf.clear()
                            silence_ms = 0.0
        finally:
            rx_task.cancel()
            in_stream.close()
            out_stream.close()
    finally:
        pa.terminate()
        await ws.close()
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("wav", nargs="*", help="WAV file(s) for --wav mode")
    p.add_argument("--synthetic", action="store_true", help="Run non-interactive synthetic protocol & barge-in check")
    p.add_argument("--mic", action="store_true", help="Run live PyAudio full-duplex microphone client")
    p.add_argument("--verify-auth-reject", action="store_true", help="Check that an invalid --api-key gets 401")
    p.add_argument("--url", default="http://127.0.0.1:8082", help="Server base URL")
    p.add_argument("--api-key", default="", help="Optional Bearer token")
    p.add_argument("--model", default="lfm3-audio-2b")
    p.add_argument("--instructions", default=None)
    p.add_argument("--chunk-ms", type=int, default=40)
    p.add_argument("--out", default="response.wav")
    args = p.parse_args()

    if args.synthetic or (not args.wav and not args.mic):
        return asyncio.run(run_synthetic_suite(args))
    if args.mic:
        return asyncio.run(run_mic_mode(args))
    return asyncio.run(run_wav_mode(args))


if __name__ == "__main__":
    raise SystemExit(main())
