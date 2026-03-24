# Worker Setup & Deployment

Worker code is in `cloudflare-worker/` and runs on Cloudflare Workers.

## Prerequisites

- Cloudflare account
- Node.js 20+
- Wrangler CLI (via `npx wrangler`)
- OpenAI API key

## 1) Install Dependencies

```bash
cd cloudflare-worker
npm install
cp .env.example .env
cp wrangler.toml.example wrangler.toml
```

## 2) Create KV Namespace

```bash
npx wrangler kv namespace create DEVICE_STATE
```

Copy the returned namespace ID into `wrangler.toml` (local file, ignored by git).

## 3) Configure Runtime Secrets

```bash
npx wrangler secret put OPENAI_API_KEY
npx wrangler secret put DEVICE_SHARED_SECRET
```

Optional stronger model:
- Store per-device secrets in KV (`device:<deviceId>`)

## 4) Configure Variables (`wrangler.toml`)

Key vars include:
- `CHAT_MODEL`
- `TRANSCRIBE_MODEL`
- `TTS_MODEL`
- `TTS_VOICE`
- `RATE_LIMIT_PER_MIN`
- `MAX_TEXT_CHARS`
- `MAX_REPLY_CHARS`
- `REPLY_MAX_TOKENS`

## 5) OpenAI API Key Permissions

At minimum, ensure your key has access to:
- Responses API (`/v1/responses`)
- Audio transcription (`/v1/audio/transcriptions`)
- Audio speech (`/v1/audio/speech`)

Recommended:
- Restrict key to only required model capabilities for this project
- Set project budget and usage alerts
- Keep key server-side only (Worker secrets), never in firmware

## 6) Deploy

```bash
npm run deploy
```

## 7) Verify

```bash
npx wrangler deployments list
```

## 8) Security and Cost Controls

- Signed request auth and nonce replay protection are enforced by worker
- Per-device minute rate limits backed by KV
- Keep `MAX_*` limits conservative on tiny-device workflows
- Never commit `.env`, `wrangler.toml`, or firmware secrets

## API Endpoints Used by Firmware

- `POST /v1/voice-turn`
- `POST /v1/voice-turn-text` (text-only fallback path)
- `POST /v1/tts`

All protected routes require signed auth headers.

For full variable/secret definitions, see [Configuration Reference](config-reference.md).
