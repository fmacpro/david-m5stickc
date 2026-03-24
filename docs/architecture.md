# Architecture

David is split into two primary parts:

- Firmware on M5StickC (`src/`)
- Cloud API gate on Cloudflare Worker (`cloudflare-worker/`)

## High-Level Flow

1. Button press starts mic capture on device
2. Firmware captures a full hold-to-talk WAV utterance
3. Firmware calls worker `/v1/voice-turn` with audio + sensor context
4. Worker:
   - validates request signature
   - transcribes request audio
   - generates LLM reply
   - selects screen action
5. Firmware requests TTS from worker `/v1/tts`
6. Firmware plays audio and renders face/overlay state

## Firmware Components

- `main.cpp`: orchestration (state machine, buttons, network calls, UI transitions)
- `voice_turn.cpp/.h`: voice-turn flow + interaction lifecycle
- `cloud_client.cpp/.h`: signed HTTP requests and NTP sync helpers
- `audio_pipeline.cpp/.h`: mic and playback audio processing utilities
- `face_renderer.cpp/.h`: face animation and expression rendering
- `ui_draw_utils.cpp/.h`: low-level display drawing helpers
- `app_utils.cpp/.h`: shared app utility functions

## Worker Components

- `src/index.ts`: request routing + auth + limits
- `src/routes/*`: endpoint handlers (`chat`, `tts`)
- `src/services/*`: OpenAI calls, state storage, voice-turn orchestration
- `src/security/*`: HMAC auth, replay protection, rate limiting
- `src/intent/*`: intent detection and screen action generation
- `src/audio/wav.ts`: WAV downsampling for device-friendly audio sizes

## Security Model

- HMAC signed requests with canonical body hashing
- Required headers:
  - `x-device-id`
  - `x-timestamp`
  - `x-nonce`
  - `x-signature`
- KV-backed nonce replay protection
- KV-backed per-device rate limiting

## State Storage

Cloudflare KV namespace `DEVICE_STATE` stores:
- per-device short chat history
- memory facts
- replay nonces
- rate-limit counters
- optional per-device secrets (`device:<id>`)

## Why This Split Works

- Device stays lightweight and battery-efficient
- Heavy AI work stays in cloud
- Secrets are kept server-side (worker secrets + KV)
- Low-latency user interaction on a tiny screen/speaker form factor
