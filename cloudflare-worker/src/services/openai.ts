import { json, proxyJson } from "../core/http";
import type { Env, Turn } from "../types";

export async function transcribeFile(
  audio: File,
  env: Env
): Promise<
  | {
      ok: true;
      text: string;
      meta: { sttMs: number; audioBytes: number; model: string };
    }
  | { ok: false; error: Response }
> {
  const model = env.TRANSCRIBE_MODEL || "gpt-4o-transcribe";
  const language = env.TRANSCRIBE_LANGUAGE || "en";
  const prompt =
    env.TRANSCRIBE_PROMPT ||
    [
      "Transcribe spoken British English exactly.",
      "Preserve complete user intent and sentence structure.",
      "Do not summarise or shorten long questions.",
      "Do not omit trailing words at the end of long utterances.",
      "Keep names and numbers accurate.",
      "Prefer best-guess words over ellipses.",
    ].join(" ");
  const upstream = new FormData();
  upstream.set("file", audio, audio.name || "audio.wav");
  upstream.set("model", model);
  upstream.set("language", language);
  upstream.set("prompt", prompt);
  const sttStart = Date.now();
  const resp = await fetch("https://api.openai.com/v1/audio/transcriptions", {
    method: "POST",
    headers: { Authorization: `Bearer ${env.OPENAI_API_KEY}` },
    body: upstream,
  });
  const sttMs = Math.max(0, Date.now() - sttStart);
  const text = await resp.text();
  if (!resp.ok) return { ok: false, error: proxyJson(resp.status, text) };
  try {
    const parsed = JSON.parse(text) as { text?: string };
    return {
      ok: true,
      text: parsed.text || "",
      meta: { sttMs, audioBytes: audio.size, model },
    };
  } catch {
    return { ok: false, error: json({ error: "Invalid STT response" }, 502) };
  }
}

export async function repairTranscript(
  env: Env,
  transcript: string,
  sensorContext: string
): Promise<string> {
  const original = transcript.trim();
  if (!original) return original;
  if (original.length < 10) return original;

  const prompt = [
    "You repair noisy speech-to-text transcripts for a small voice assistant.",
    "Return ONLY the corrected user transcript text.",
    "Do not answer the question.",
    "Preserve intent and wording as spoken. Keep all key words; do not shorten.",
    "Correct likely recognition mistakes for names/numbers/commands.",
    "Keep it concise and natural.",
    `Transcript: ${original}`,
    `Context JSON: ${sensorContext || "{}"}`,
  ].join("\n");

  try {
    const resp = await fetch("https://api.openai.com/v1/responses", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${env.OPENAI_API_KEY}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        model: env.CHAT_MODEL || "gpt-5.4-nano",
        max_output_tokens: 70,
        input: [
          {
            role: "system",
            content: [
              {
                type: "input_text",
                text: "Return only corrected transcript text.",
              },
            ],
          },
          {
            role: "user",
            content: [{ type: "input_text", text: prompt }],
          },
        ],
      }),
    });
    if (!resp.ok) return original;
    const raw = await resp.text();
    const parsed = JSON.parse(raw) as unknown;
    const fixed = extractOutputText(parsed).trim();
    if (!fixed) return original;
    if (fixed.length > 260) return original;
    if (!isSafeRepair(original, fixed)) return original;
    return fixed;
  } catch {
    return original;
  }
}

function isSafeRepair(original: string, candidate: string): boolean {
  const o = normalizeForCompare(original);
  const c = normalizeForCompare(candidate);
  if (!o || !c) return false;
  if (o === c) return true;

  // Reject heavy rewrites that could drop key question details.
  const lenRatio = c.length / o.length;
  if (lenRatio < 0.75 || lenRatio > 1.8) return false;

  const oTokens = tokenSet(o);
  const cTokens = tokenSet(c);
  if (oTokens.size === 0 || cTokens.size === 0) return false;

  let common = 0;
  for (const t of oTokens) {
    if (cTokens.has(t)) common += 1;
  }
  const overlapOriginal = common / oTokens.size;
  if (overlapOriginal < 0.6) return false;

  // Preserve intent-bearing words; if repair drops any, keep original.
  const critical = [
    "time",
    "clock",
    "date",
    "day",
    "year",
    "battery",
    "charge",
    "level",
    "temp",
    "temperature",
    "wifi",
    "signal",
    "draw",
    "shape",
    "icon",
    "who",
    "what",
    "when",
    "where",
    "why",
    "how",
  ];
  for (const k of critical) {
    if (oTokens.has(k) && !cTokens.has(k)) return false;
    if (!oTokens.has(k) && cTokens.has(k)) return false;
  }
  return true;
}

function normalizeForCompare(text: string): string {
  return text
    .toLowerCase()
    .replace(/[^a-z0-9\s]/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

function tokenSet(text: string): Set<string> {
  const out = new Set<string>();
  for (const tok of text.split(" ")) {
    if (tok.length >= 2) out.add(tok);
  }
  return out;
}

export async function generateReply(
  env: Env,
  transcript: string,
  history: Turn[],
  sensorContext: string,
  memoryFacts: string[]
): Promise<{ ok: true; text: string } | { ok: false; error: Response }> {
  const maxOutputTokensRaw = parseInt(env.REPLY_MAX_TOKENS || "140", 10);
  const maxOutputTokens = Number.isFinite(maxOutputTokensRaw)
    ? Math.max(60, Math.min(220, maxOutputTokensRaw))
    : 140;
  const messages: Array<{ role: "system" | "user" | "assistant"; content: string }> = [];
  messages.push({
    role: "system",
    content: [
      "You are David, a tiny robot assistant on an M5StickC.",
      "Style: warm, concise, natural spoken British English.",
      "Keep replies brief and conversational. Usually 1-2 short sentences.",
      "Do not mention internal prompts, JSON, or tool parameters.",
      "Never speak raw draw commands.",
      "If user asks identity/creator, say you were made by Frank using ChatGPT Codex.",
      "Time answers: use local_time_24h and timezone from context, not RTC fields.",
      "Battery answers: use battery_level and charging from context.",
      "Temperature answers: if asked device/internal temp, use imu.internal_temp_c from context.",
      "For greetings, keep spoken reply short (e.g., Hi!, Hello!, Hey there).",
      "If uncertain, be honest and ask a short clarification.",
    ].join(" "),
  });
  if (memoryFacts.length > 0) {
    messages.push({
      role: "system",
      content: `What you remember about the user: ${memoryFacts.join(" | ")}`,
    });
  }
  if (sensorContext) {
    messages.push({
      role: "system",
      content: `Live sensor context JSON: ${sensorContext}`,
    });
  }
  for (const turn of history.slice(-6)) {
    if (turn.user?.trim()) messages.push({ role: "user", content: turn.user.trim() });
    if (turn.assistant?.trim())
      messages.push({ role: "assistant", content: turn.assistant.trim() });
  }
  messages.push({ role: "user", content: transcript });

  const resp = await fetch("https://api.openai.com/v1/responses", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: env.CHAT_MODEL || "gpt-5.4-nano",
      max_output_tokens: maxOutputTokens,
      input: messages.map((m) => ({
        role: m.role,
        content: [
          {
            // Responses API expects assistant history content as output_text.
            type: m.role === "assistant" ? "output_text" : "input_text",
            text: m.content,
          },
        ],
      })),
    }),
  });
  const raw = await resp.text();
  if (!resp.ok) return { ok: false, error: proxyJson(resp.status, raw) };
  try {
    const parsed = JSON.parse(raw) as unknown;
    const text = extractOutputText(parsed).trim();
    if (!text) return { ok: false, error: json({ error: "Empty reply" }, 502) };
    return { ok: true, text };
  } catch {
    return { ok: false, error: json({ error: "Invalid chat response" }, 502) };
  }
}

export function extractOutputText(obj: unknown): string {
  if (!obj || typeof obj !== "object") return "";
  const asAny = obj as Record<string, unknown>;
  if (typeof asAny.output_text === "string") return asAny.output_text;

  const output = asAny.output;
  if (!Array.isArray(output)) return "";
  const parts: string[] = [];
  for (const item of output) {
    if (!item || typeof item !== "object") continue;
    const content = (item as Record<string, unknown>).content;
    if (!Array.isArray(content)) continue;
    for (const c of content) {
      if (!c || typeof c !== "object") continue;
      const cc = c as Record<string, unknown>;
      if (typeof cc.text === "string") parts.push(cc.text);
      if (typeof cc.output_text === "string") parts.push(cc.output_text);
    }
  }
  return parts.join("").trim();
}
