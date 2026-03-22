import { json, proxyJson } from "../core/http";
import type { Env } from "../types";

export async function handleStt(request: Request, env: Env): Promise<Response> {
  const contentType = request.headers.get("content-type") || "";
  if (!contentType.includes("multipart/form-data")) {
    return json({ error: "Use multipart/form-data with field 'audio'" }, 400);
  }

  const form = await request.formData();
  const audio = form.get("audio");
  if (!(audio instanceof File)) {
    return json({ error: "Missing file field: audio" }, 400);
  }

  const upstream = new FormData();
  upstream.set("file", audio, audio.name || "audio.wav");
  upstream.set("model", env.TRANSCRIBE_MODEL || "gpt-4o-mini-transcribe");

  const language = form.get("language");
  if (typeof language === "string" && language.length > 0) {
    upstream.set("language", language);
  }

  const prompt = form.get("prompt");
  if (typeof prompt === "string" && prompt.length > 0) {
    upstream.set("prompt", prompt);
  }

  const resp = await fetch("https://api.openai.com/v1/audio/transcriptions", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
    },
    body: upstream,
  });

  const text = await resp.text();
  return proxyJson(resp.status, text);
}

export async function handleSttRaw(request: Request, env: Env): Promise<Response> {
  const contentType = request.headers.get("content-type") || "";
  if (!contentType.includes("audio/wav") && !contentType.includes("application/octet-stream")) {
    return json({ error: "Use audio/wav body" }, 400);
  }

  const audioBytes = await request.arrayBuffer();
  if (audioBytes.byteLength < 64) {
    return json({ error: "Audio body too small" }, 400);
  }

  const url = new URL(request.url);
  const promptHintRaw = (url.searchParams.get("prompt") || "").trim();
  const promptHint = promptHintRaw.slice(0, 180);

  const audioFile = new File([audioBytes], "audio.wav", { type: "audio/wav" });
  const upstream = new FormData();
  upstream.set("file", audioFile, "audio.wav");
  upstream.set("model", env.TRANSCRIBE_MODEL || "gpt-4o-mini-transcribe");
  upstream.set("language", "en");
  const promptParts = [
    "Transcribe spoken British English exactly.",
    "Preserve full user intent and sentence structure.",
    "Keep names and numbers accurate (for example: David, Star Trek, Voyager, Kathryn Janeway).",
    "Do not add words that were not spoken.",
    "Prefer best-guess words over ellipses.",
  ];
  if (promptHint.length > 0) {
    promptParts.push(`Prior transcript context: ${promptHint}`);
    promptParts.push("Continue naturally from context without dropping words.");
  }
  upstream.set(
    "prompt",
    promptParts.join(" ")
  );

  const resp = await fetch("https://api.openai.com/v1/audio/transcriptions", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
    },
    body: upstream,
  });

  const text = await resp.text();
  return proxyJson(resp.status, text);
}
