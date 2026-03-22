# Firmware Development

Firmware lives in `src/` and is built with PlatformIO.

## Prerequisites

- Python + PlatformIO CLI
- USB serial access to the device (`/dev/ttyUSB0` in most setups)
- `src/secrets.h` populated

## Local Setup

```bash
cp src/secrets.h.example src/secrets.h
```

Set values in `src/secrets.h`:
- `WIFI_SSID`
- `WIFI_PASSWORD`
- `API_BASE_URL`
- `DEVICE_ID`
- `DEVICE_SHARED_SECRET`

## Build

```bash
pio run -e m5stick-c
```

## Flash

```bash
pio run -e m5stick-c -t upload
```

## Serial Monitor

```bash
pio device monitor -p /dev/ttyUSB0 -b 115200
```

## Typical Workflow

1. Edit firmware in `src/`
2. Build with `pio run`
3. Flash with `pio run -t upload`
4. Validate behavior with serial monitor

## Common Useful Commands

```bash
# clean build artifacts
pio run -e m5stick-c -t clean

# rebuild + upload
pio run -e m5stick-c -t upload

# watch serial logs
pio device monitor -p /dev/ttyUSB0 -b 115200
```

## Notes

- The configured board is `m5stick-c` in `platformio.ini`.
- Audio and memory constraints are tight on-device; keep changes incremental and test often.
- If upload fails with "port busy", close any running serial monitor first.
