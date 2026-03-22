import { downsampleWav16MonoTo8bitRate, uint8ToArrayBuffer } from "../audio/wav";
import { json, proxyJson } from "../core/http";
import type { Env } from "../types";

export async function handleTts(request: Request, env: Env): Promise<Response> {
  const payload = (await request.json()) as {
    text?: string;
    voice?: string;
    format?: "mp3" | "wav";
  };
  const text = (payload.text || "").trim();
  const maxChars = parseInt(env.MAX_TEXT_CHARS || "1000", 10);
  if (!text) return json({ error: "Missing field: text" }, 400);
  if (text.length > maxChars) {
    return json({ error: `text too long (max ${maxChars})` }, 400);
  }

  const upstreamResp = await fetch("https://api.openai.com/v1/audio/speech", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: env.TTS_MODEL || "gpt-4o-mini-tts",
      voice: payload.voice || env.TTS_VOICE || "alloy",
      response_format: payload.format || "wav",
      input: text,
    }),
  });

  const audio = await upstreamResp.arrayBuffer();
  if (!upstreamResp.ok) {
    const errText = new TextDecoder().decode(audio);
    return proxyJson(upstreamResp.status, errText);
  }

  const contentType = upstreamResp.headers.get("content-type") || "audio/wav";
  let outAudio: ArrayBuffer = audio;
  const rawBytes = new Uint8Array(audio);
  const looksLikeWav =
    rawBytes.byteLength >= 12 &&
    rawBytes[0] === 0x52 && // R
    rawBytes[1] === 0x49 && // I
    rawBytes[2] === 0x46 && // F
    rawBytes[3] === 0x46 && // F
    rawBytes[8] === 0x57 && // W
    rawBytes[9] === 0x41 && // A
    rawBytes[10] === 0x56 && // V
    rawBytes[11] === 0x45; // E
  if (contentType.includes("audio/wav") || looksLikeWav) {
    outAudio = uint8ToArrayBuffer(downsampleWav16MonoTo8bitRate(rawBytes, 8000));
  }

  return new Response(outAudio, {
    status: 200,
    headers: {
      "Content-Type": contentType,
      "Cache-Control": "no-store",
    },
  });
}
