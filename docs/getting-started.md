# Getting Started

This guide gets David running from zero on an M5StickC.
For deeper Cloudflare details, see [Worker Setup & Deployment](worker.md).

## 1) Prerequisites

- M5StickC device + speaker hat (if used)
- USB data cable
- OpenAI API key (project key recommended)
- Cloudflare account
- Node.js 20+
- Python 3.10+ and PlatformIO CLI
- WSL2 users: `usbipd-win` on Windows host

## 2) Install USB Drivers (Windows)

Depending on your USB bridge chip, install one of:
- Silicon Labs CP210x USB to UART driver: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
- FTDI VCP driver (for `0403:6001` devices): https://ftdichip.com/drivers/vcp-drivers/

After install, reconnect the device and confirm it appears as a serial converter.

## 3) WSL2 USB Attach (if using WSL2)

In PowerShell (Admin):

```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

In WSL:

```bash
ls -l /dev/ttyUSB* /dev/ttyACM*
```

You should see `/dev/ttyUSB0` (or similar).

## 4) Clone and Install Tools

```bash
git clone <your-repo-url>
cd m5stick

# Worker dependencies
cd cloudflare-worker
npm install
cd ..

# PlatformIO CLI (if needed)
pip install platformio
```

## 5) Configure Firmware (`src/secrets.h`)

First, create a shared secret value (you will reuse this in firmware, `.env`, and Worker secrets):

```bash
openssl rand -hex 32
```

Create `src/secrets.h` from the example:

```bash
cp src/secrets.h.example src/secrets.h
```

Set:
- Wi-Fi SSID/password
- Worker base URL
- Device ID
- `DEVICE_SHARED_SECRET` (paste the value you generated above)

## 6) Configure Worker Local Files

In `cloudflare-worker`:

```bash
cp .env.example .env
cp wrangler.toml.example wrangler.toml
```

Use `.env` and `wrangler.toml` for local runtime config only. Do not commit them.

Fill values:
- `.env`:
  - `OPENAI_API_KEY`: from OpenAI project API keys
  - `DEVICE_SHARED_SECRET`: same generated value used in `src/secrets.h`
- `wrangler.toml`:
  - `[[kv_namespaces]].id`: output from `wrangler kv namespace create DEVICE_STATE`
  - any runtime vars you want to tune (`CHAT_MODEL`, `TTS_VOICE`, limits, etc.)

Use the same generated secret in:
- `src/secrets.h` (`DEVICE_SHARED_SECRET`)
- `cloudflare-worker/.env` (`DEVICE_SHARED_SECRET`)
- Worker runtime secret `DEVICE_SHARED_SECRET`

Need more worker setup detail? Open [Worker Setup & Deployment](worker.md).

## 7) Create KV Namespace and Runtime Secrets

From `cloudflare-worker/`:

```bash
cd cloudflare-worker
npx wrangler kv namespace create DEVICE_STATE
npx wrangler secret put OPENAI_API_KEY
npx wrangler secret put DEVICE_SHARED_SECRET
```

## 8) Deploy Worker

```bash
cd cloudflare-worker
npm run deploy
```

## 9) Verify Ignore Rules (Safety Check)

```bash
git check-ignore -v src/secrets.h cloudflare-worker/.env cloudflare-worker/wrangler.toml
git status --short
```

`git status --short` should not show those files.

## 10) Build and Flash Firmware

From repo root:

```bash
pio run -e m5stick-c -t upload
pio device monitor -p /dev/ttyUSB0 -b 115200
```

## 11) First Voice Test

- Press talk button
- Ask: `What time is it?`
- Expected: spoken response + matching screen overlay

If anything fails, continue with [Troubleshooting](troubleshooting.md).
