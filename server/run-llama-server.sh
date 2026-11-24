#!/usr/bin/env bash
set -euo pipefail

# Reproducible llama-server launcher for LFM3-Audio-2B /v1/realtime voice sessions.
# Matches the verified hardware flags in /home/nick/Documents/Models/lfm3.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODELS_ROOT="${MODELS_ROOT:-/home/nick/Documents/Models}"
MODEL_DIR="${MODEL_DIR:-$MODELS_ROOT/lfm3}"
MODEL_FILE="${MODEL_FILE:-$MODEL_DIR/LFM3-Audio-2B-Q4_K_M.gguf}"
MMPROJ_FILE="${MMPROJ_FILE:-$MODEL_DIR/mmproj-LFM3-Audio-2B-Q8_0.gguf}"
ALIAS="${ALIAS:-lfm3-audio-2b}"
HOST="${HOST:-0.0.0.0}"
PORT="${PORT:-8082}"
CTX_SIZE="${CTX_SIZE:-128000}"
API_KEY="${API_KEY:-}"

if [ -n "${LLAMA_BIN:-}" ]; then
  BIN="$LLAMA_BIN"
elif [ -x "$MODELS_ROOT/llama.cpp/build/bin/llama-server" ]; then
  BIN="$MODELS_ROOT/llama.cpp/build/bin"
elif [ -x "$MODELS_ROOT/llama.cpp/build-vulkan/bin/llama-server" ]; then
  BIN="$MODELS_ROOT/llama.cpp/build-vulkan/bin"
elif command -v llama-server >/dev/null 2>&1; then
  BIN="$(dirname "$(command -v llama-server)")"
else
  echo "Error: llama-server binary not found. Set LLAMA_BIN or build $MODELS_ROOT/llama.cpp/build." >&2
  exit 1
fi

[ -f "$MODEL_FILE" ]  || { echo "Missing model file: $MODEL_FILE" >&2; exit 1; }
[ -f "$MMPROJ_FILE" ] || { echo "Missing mmproj file: $MMPROJ_FILE" >&2; exit 1; }

if [ -z "${LLAMA_DEVICE:-}" ]; then
  DEVICE="$("$BIN/llama-server" --list-devices 2>/dev/null | awk '
    /Device/ { next }
    /^[A-Za-z]+[0-9]+:/ {
      if ($0 ~ /Ryzen|CPU|llvmpipe|Software/) next
      print $1
      exit
    }')"
  DEVICE="${DEVICE:-ROCm0}"
else
  DEVICE="$LLAMA_DEVICE"
fi

EXTRA_ARGS=()
if [ -n "$API_KEY" ]; then
  EXTRA_ARGS+=(--api-key "$API_KEY")
fi

echo "Starting llama-server ($ALIAS) on $HOST:$PORT using $BIN ($DEVICE)" >&2
exec "$BIN/llama-server" \
  -m "$MODEL_FILE" \
  --mmproj "$MMPROJ_FILE" \
  --alias "$ALIAS" \
  -c "$CTX_SIZE" \
  -t 8 -tb 16 \
  --device "$DEVICE" \
  --gpu-layers auto \
  --fit on \
  -fa on \
  -b 2048 -ub 512 \
  -np 1 \
  --cache-type-k q8_0 --cache-type-v q8_0 \
  --jinja \
  --cache-prompt \
  --metrics --slots \
  --reasoning on \
  --reasoning-preserve \
  --temp 0 \
  --host "$HOST" \
  --port "$PORT" \
  "${EXTRA_ARGS[@]}"
