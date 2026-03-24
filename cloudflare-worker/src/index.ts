import { json, withCors } from "./core/http";
import { handleChat } from "./routes/chat";
import { handlePicture } from "./routes/picture";
import { handleTts } from "./routes/tts";
import { authenticate, enforceRateLimit } from "./security/auth";
import { sha256Hex } from "./security/crypto";
import { handleVoiceTurn, handleVoiceTurnText } from "./services/voiceTurn";
import type { Env } from "./types";

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const url = new URL(request.url);
      const maxBodyBytes = clampInt(env.MAX_BODY_BYTES, 2048, 10_000_000, 2_000_000);

      if (request.method === "OPTIONS") {
        return withCors(new Response(null, { status: 204 }));
      }

      if (url.pathname === "/v1/health") {
        return json({ ok: true, service: "m5stick-api-gate" });
      }

      if (request.method !== "POST") {
        return json({ error: "Method not allowed" }, 405);
      }

      const contentLengthRaw = request.headers.get("content-length");
      if (contentLengthRaw) {
        const contentLength = parseInt(contentLengthRaw, 10);
        if (Number.isFinite(contentLength) && contentLength > maxBodyBytes) {
          return json({ error: "Request body too large" }, 413);
        }
      }

      const bodyBytes = await request.arrayBuffer();
      if (bodyBytes.byteLength > maxBodyBytes) {
        return json({ error: "Request body too large" }, 413);
      }
      const bodyHashHex = await sha256Hex(new Uint8Array(bodyBytes));

      const authResult = await authenticate(request, bodyHashHex, env);
      if (!authResult.ok) {
        return json({ error: authResult.error }, 401);
      }

      const limit = parseInt(env.RATE_LIMIT_PER_MIN || "20", 10);
      const allowed = await enforceRateLimit(env, authResult.deviceId, limit);
      if (!allowed) {
        return json({ error: "Rate limit exceeded" }, 429);
      }

      const reqForParsing = new Request(request.url, {
        method: request.method,
        headers: request.headers,
        body: bodyBytes,
      });

      if (url.pathname === "/v1/chat") {
        return withCors(await handleChat(reqForParsing, env));
      }
      if (url.pathname === "/v1/tts") {
        return withCors(await handleTts(reqForParsing, env));
      }
      if (url.pathname === "/v1/voice-turn") {
        return withCors(await handleVoiceTurn(reqForParsing, env, authResult.deviceId));
      }
      if (url.pathname === "/v1/voice-turn-text") {
        return withCors(
          await handleVoiceTurnText(reqForParsing, env, authResult.deviceId)
        );
      }
      if (url.pathname === "/v1/picture") {
        return withCors(await handlePicture(reqForParsing, env));
      }

      return json({ error: "Not found" }, 404);
    } catch (err) {
      if (env.DEBUG_ERRORS === "1") {
        const message = err instanceof Error ? err.message : "unknown_error";
        return json({ error: "Internal error", detail: message }, 500);
      }
      return json({ error: "Internal error" }, 500);
    }
  },
};

function clampInt(raw: string | undefined, min: number, max: number, fallback: number): number {
  const n = parseInt(raw || String(fallback), 10);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.min(max, n));
}
