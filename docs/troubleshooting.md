# Troubleshooting

## USB / Serial

### Device not found in WSL

- On Windows host, check `usbipd list`
- Bind and attach bus ID to WSL
- In WSL, confirm `/dev/ttyUSB0` exists

### Upload fails: `port is busy`

- Close serial monitors (`pio device monitor`, IDE monitor)
- Retry upload

## Worker / Cloud

### `TURN_TEXT HTTP 400` with content-type error

Cause:
- Bad message content types in Responses API payload history.

Fix:
- Ensure assistant history entries use `output_text`.

### `Bad signature` / auth failures

- Confirm `DEVICE_ID` and shared secret match between firmware and worker
- Ensure timestamp is valid (NTP sync works)
- Check canonical string format exactly matches firmware + worker

### Rate limit exceeded

- Increase `RATE_LIMIT_PER_MIN` carefully
- Add backoff/retry behavior on device if needed

## Audio Issues

### No sound but screen updates

- Check serial logs for `/v1/tts` status and TTS errors
- Confirm worker returns manageable WAV sizes (downsample path active)
- Verify speaker hardware connection and hat seating

### `TTS audio too large`

- Ensure worker downsampling is active
- Reduce per-chunk spoken text size in firmware
- Keep reply limits conservative (`MAX_REPLY_CHARS`, `REPLY_MAX_TOKENS`)

### Crackling / static

- Lower speaker volume
- Keep mic stopped during playback
- Avoid excessive DSP/gating that can distort 8-bit audio

## Fast Health Checklist

1. `WiFi connected` on device logs
2. `/v1/voice-turn` returns 200/204
3. Serial shows `[STT DBG] model=... stt_ms=... audio_bytes=...`
4. `/v1/tts` returns 200
5. No device OOM / panic during playback

## Long Question STT Tuning

- Prefer worker-side transcription via `/v1/voice-turn` (single WAV upload).
- Raise `MAX_BODY_BYTES` in worker config if uploads are rejected with `413`.
- Tune worker vars:
  - `TRANSCRIBE_MODEL` (quality/cost tradeoff)
  - `TRANSCRIBE_LANGUAGE` (for example `en`)
  - `TRANSCRIBE_PROMPT` (emphasize complete long-utterance capture)
- On device serial, inspect `[STT DBG]` and `[HTTP] /v1/voice-turn` lines to verify:
  - audio size is within limits
  - transcription latency remains stable
  - model/prompt changes are taking effect
