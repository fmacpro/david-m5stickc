# David: M5StickC Voice Assistant

This repository powers **David**, a conversational robot assistant running on an **M5StickC**.

## What David Is

David is a tiny voice-first robot companion:
- Runs directly on an M5StickC with an expressive face UI
- Uses a Cloudflare Worker as a secure cloud gateway
- Combines local interactivity with cloud AI for conversation

## What David Does

- Push-to-talk conversation (STT -> LLM -> TTS)
- Answers spoken questions naturally
- Speaks responses and shows glance-friendly visual overlays
- Supports tamagotchi-style interactions like feeding and patting
- Can draw simple shapes/icons on screen when asked
- Reports internal device status (for example battery, time/date, and sensor context)
- Uses device context (time, battery, sensors) in replies
- Protects requests with signed auth, replay protection, and rate limits

David runs with:
- Firmware on M5StickC (`src/`)
- Cloudflare Worker backend (`cloudflare-worker/`)

## Start Here

- [Quick Setup](docs/getting-started.md)
- [Architecture](docs/architecture.md)
- [Configuration Reference](docs/config-reference.md)
- [Firmware Development](docs/firmware.md)
- [Worker Setup & Deployment](docs/worker.md)
- [Troubleshooting](docs/troubleshooting.md)

## Repository Layout

- `src/`: M5StickC firmware (PlatformIO / Arduino)
- `cloudflare-worker/`: API gateway worker (TypeScript / Wrangler)
- `docs/`: project documentation

## Recommended Read Order

1. [Quick Setup](docs/getting-started.md)
2. [Configuration Reference](docs/config-reference.md)
3. [Worker Setup & Deployment](docs/worker.md)
4. [Firmware Development](docs/firmware.md)
5. [Architecture](docs/architecture.md)
6. [Troubleshooting](docs/troubleshooting.md)

## Security

- Secrets/config are local-only and gitignored (`src/secrets.h`, `cloudflare-worker/.env`, `cloudflare-worker/wrangler.toml`)
- Worker enforces signed auth, replay protection, and rate limits
