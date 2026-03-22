#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
OUT_DIR="${2:-./logs}"
mkdir -p "$OUT_DIR"

STAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="$OUT_DIR/serial_smoke_${STAMP}.log"

PROMPTS=(
  "what time is it david?"
  "David, what is the battery level right now?"
  "David, can you draw a circle?"
  "David, what date is it today?"
  "David, what is your internal temperature?"
  "David, in star trek voyager, who is the captain of the ship?"
  "David, tell me a short joke."
  "David, what is 12 times 7?"
  "David, what is the capital of Japan?"
  "David, what year is it?"
  "David, who made you?"
  "David, can you draw a heart?"
  "David, can you draw a square?"
  "David, can you draw a triangle?"
  "David, can you draw a star?"
  "David, can you draw a smiley face?"
  "David, can you draw a battery icon?"
  "David, can you draw a wifi icon?"
)

cleanup() {
  if [[ -n "${CAT_PID:-}" ]] && kill -0 "$CAT_PID" 2>/dev/null; then
    kill "$CAT_PID" 2>/dev/null || true
    wait "$CAT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

echo "Using port: $PORT"
echo "Log file:   $LOG_FILE"

stty -F "$PORT" 115200 raw -echo -echoe -echok -echoctl -echoke
stdbuf -oL cat "$PORT" | tee "$LOG_FILE" >/dev/null &
CAT_PID=$!
sleep 2

for i in "${!PROMPTS[@]}"; do
  idx=$((i + 1))
  prompt="${PROMPTS[$i]}"
  echo "[$idx/${#PROMPTS[@]}] ASK: $prompt"
  printf 'ASK %s\n' "$prompt" >"$PORT"
  sleep 12
done

sleep 6
cleanup

turn_ok_count="$(grep -c '\[HTTP\] /v1/voice-turn-text -> 200' "$LOG_FILE" || true)"
fallback_count="$(grep -c 'TURN AUDIO .*fallback' "$LOG_FILE" || true)"
tts_chunk_count="$(grep -c '\[HTTP\] /v1/tts -> 200' "$LOG_FILE" || true)"
error_count="$(grep -Ec 'Voice turn failed|TURN_TEXT HTTP|STT HTTP|TTS HTTP|Inject failed|\[E\]' "$LOG_FILE" || true)"

echo
echo "Smoke test summary"
echo "  turn_text_200: $turn_ok_count"
echo "  turn_audio_fallbacks: $fallback_count"
echo "  tts_chunk_calls: $tts_chunk_count"
echo "  error_lines: $error_count"
echo "  log: $LOG_FILE"

if [[ "$turn_ok_count" -lt "${#PROMPTS[@]}" || "$error_count" -gt 0 ]]; then
  exit 1
fi

exit 0
