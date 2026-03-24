#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
OUT_DIR="${2:-./logs}"
mkdir -p "$OUT_DIR"

STAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="$OUT_DIR/picture_smoke_${STAMP}.log"

PROMPTS=(
  "show me a picture of a cat"
  "show me a picture of a dog"
  "show me a picture of the moon"
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

sleep 4
cleanup

turn_ok_count="$(grep -c '\[HTTP\] /v1/voice-turn-text -> 200' "$LOG_FILE" || true)"
picture_ok_count="$(grep -c '\[HTTP\] /v1/picture -> 200' "$LOG_FILE" || true)"
screen_image_count="$(grep -c '\[SCREEN\] mode=image' "$LOG_FILE" || true)"
error_count="$(grep -Ec 'image fetch failed|PICTURE HTTP|Voice turn failed|TURN_TEXT HTTP|\[E\]' "$LOG_FILE" || true)"

echo
echo "Picture smoke summary"
echo "  turn_text_200: $turn_ok_count"
echo "  picture_200:   $picture_ok_count"
echo "  screen_image:  $screen_image_count"
echo "  error_lines:   $error_count"
echo "  log: $LOG_FILE"

if [[ "$turn_ok_count" -lt "${#PROMPTS[@]}" || "$picture_ok_count" -lt 1 || "$screen_image_count" -lt 1 || "$error_count" -gt 0 ]]; then
  exit 1
fi

exit 0
