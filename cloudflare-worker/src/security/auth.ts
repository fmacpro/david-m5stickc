import type { Env } from "../types";
import { hmacSha256Hex, timingSafeEqual } from "./crypto";

const MAX_SKEW_SECONDS = 120;
const NONCE_TTL_SECONDS = 300;

export async function authenticate(
  request: Request,
  bodyHashHex: string,
  env: Env
): Promise<{ ok: true; deviceId: string } | { ok: false; error: string }> {
  const deviceId = request.headers.get("x-device-id") || "";
  const tsRaw = request.headers.get("x-timestamp") || "";
  const nonce = request.headers.get("x-nonce") || "";
  const sig = request.headers.get("x-signature") || "";
  if (!deviceId || !tsRaw || !nonce || !sig) {
    return { ok: false, error: "Missing auth headers" };
  }

  const ts = parseInt(tsRaw, 10);
  if (!Number.isFinite(ts)) return { ok: false, error: "Invalid timestamp" };
  const now = Math.floor(Date.now() / 1000);
  if (Math.abs(now - ts) > MAX_SKEW_SECONDS) {
    return { ok: false, error: "Timestamp skew too large" };
  }

  const nonceKey = `nonce:${deviceId}:${nonce}`;
  const seen = await env.DEVICE_STATE.get(nonceKey);
  if (seen) return { ok: false, error: "Replay detected" };

  const url = new URL(request.url);
  const canonical = `${ts}.${nonce}.${request.method}.${url.pathname}.${bodyHashHex}`;
  const deviceSecret =
    (await env.DEVICE_STATE.get(`device:${deviceId}`)) || env.DEVICE_SHARED_SECRET;
  if (!deviceSecret) return { ok: false, error: "Device not enrolled" };
  const expected = await hmacSha256Hex(deviceSecret, canonical);
  if (!timingSafeEqual(expected, sig)) {
    return { ok: false, error: "Bad signature" };
  }

  await env.DEVICE_STATE.put(nonceKey, "1", { expirationTtl: NONCE_TTL_SECONDS });
  return { ok: true, deviceId };
}

export async function enforceRateLimit(
  env: Env,
  deviceId: string,
  limitPerMinute: number
): Promise<boolean> {
  const minute = Math.floor(Date.now() / 60000);
  const key = `rate:${deviceId}:${minute}`;
  const curr = parseInt((await env.DEVICE_STATE.get(key)) || "0", 10);
  if (curr >= limitPerMinute) return false;
  await env.DEVICE_STATE.put(key, String(curr + 1), { expirationTtl: 120 });
  return true;
}
