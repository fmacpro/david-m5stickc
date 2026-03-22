import { json, proxyJson } from "../core/http";
import { extractOutputText } from "../services/openai";
import type { Env } from "../types";

export async function handleChat(request: Request, env: Env): Promise<Response> {
  const payload = (await request.json()) as { text?: string; system?: string };
  const text = (payload.text || "").trim();
  const maxChars = parseInt(env.MAX_TEXT_CHARS || "1000", 10);
  if (!text) {
    return json({ error: "Missing field: text" }, 400);
  }
  if (text.length > maxChars) {
    return json({ error: `text too long (max ${maxChars})` }, 400);
  }

  const system =
    payload.system?.trim() ||
    "You are a friendly concise voice assistant in a tiny robot body.";

  const resp = await fetch("https://api.openai.com/v1/responses", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: env.CHAT_MODEL || "gpt-5.4-nano",
      max_output_tokens: 140,
      input: [
        {
          role: "system",
          content: [{ type: "input_text", text: system }],
        },
        {
          role: "user",
          content: [{ type: "input_text", text }],
        },
      ],
    }),
  });

  const raw = await resp.text();
  if (!resp.ok) return proxyJson(resp.status, raw);

  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return json({ error: "Invalid JSON from upstream" }, 502);
  }

  const reply = extractOutputText(parsed);
  return json({ reply, raw: parsed });
}
