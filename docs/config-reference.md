# Configuration Reference

This document lists the key configuration needed to run David safely and reliably.

## Firmware Secrets (`src/secrets.h`)

Create from `src/secrets.h.example` and keep it local only.

- `WIFI_SSID`: Wi-Fi network name used by the M5StickC
- `WIFI_PASSWORD`: Wi-Fi password
- `API_BASE_URL`: deployed worker URL (for example `https://m5stick-api-gate.<subdomain>.workers.dev`)
- `DEVICE_ID`: stable identifier for this device (for example `david-01`)
- `DEVICE_SHARED_SECRET`: shared secret used for HMAC request signing

Security note:
- `src/secrets.h` is gitignored and must never be committed.

Where values come from:
- `WIFI_SSID` / `WIFI_PASSWORD`: your local Wi-Fi or hotspot
- `API_BASE_URL`: your deployed worker URL (`https://<name>.<subdomain>.workers.dev`)
- `DEVICE_ID`: your chosen stable ID (for example `david-01`)
- `DEVICE_SHARED_SECRET`: generate locally (`openssl rand -hex 32`)

## Worker Secrets (Cloudflare `wrangler secret`)

Set these in production with Wrangler:

- `OPENAI_API_KEY`: project API key used server-side by the worker
- `DEVICE_SHARED_SECRET`: shared secret matching firmware for signed request validation

Optional hardened mode:
- Store per-device shared secrets in KV (`device:<deviceId>`) and rotate centrally.

Where values come from:
- `OPENAI_API_KEY`: OpenAI dashboard -> project API keys
- `DEVICE_SHARED_SECRET`: must match firmware `DEVICE_SHARED_SECRET`

## Worker Variables (`cloudflare-worker/wrangler.toml`)

`wrangler.toml` should stay local/ignored.
Use the committed template `cloudflare-worker/wrangler.toml.example` and copy it locally before deploy.

```bash
cp cloudflare-worker/wrangler.toml.example cloudflare-worker/wrangler.toml
```

Core runtime controls:

- `CHAT_MODEL`: LLM for response generation
- `TRANSCRIBE_MODEL`: speech-to-text model
- `TRANSCRIBE_LANGUAGE`: language hint for transcription (default `en`)
- `TRANSCRIBE_PROMPT`: optional custom transcription prompt for long-utterance behavior
- `TTS_MODEL`: text-to-speech model
- `TTS_VOICE`: selected voice
- `TTS_INSTRUCTIONS`: voice style guidance
- `RATE_LIMIT_PER_MIN`: per-device request cap
- `MAX_TEXT_CHARS`: incoming text guardrail
- `MAX_REPLY_CHARS`: response length cap
- `REPLY_MAX_TOKENS`: token cap for generated replies
- `MAX_BODY_BYTES`: max signed request payload accepted by worker (default 2,000,000)

Observability:

- `[observability] enabled = true`
- `head_sampling_rate` controls sampled invocation logs

## OpenAI API Key Capabilities

For this project, allow only what the worker uses:

- Responses (`/v1/responses`)
- Text-to-speech (`/v1/audio/speech`)
- Speech-to-text (`/v1/audio/transcriptions`) or equivalent transcription capability shown in your project settings
- List models (optional but useful for diagnostics)

Recommended hardening:

- Restrict model access to only your selected chat/transcribe/tts models
- Set project budget caps and usage alerts
- Keep the API key only in worker secrets, never firmware

## Git Safety Checklist

Before commit/push:

- `git status --short` does not show `src/secrets.h`
- `git status --short` does not show `cloudflare-worker/.env`
- `git status --short` does not show `cloudflare-worker/wrangler.toml`
- No plaintext API keys or shared secrets in tracked files

Verify ignore rules:

```bash
git check-ignore -v src/secrets.h cloudflare-worker/.env cloudflare-worker/wrangler.toml
```

Quick scan:

```bash
rg -n "sk-|OPENAI_API_KEY|DEVICE_SHARED_SECRET|WIFI_PASSWORD" .
```

Review hits before pushing.
