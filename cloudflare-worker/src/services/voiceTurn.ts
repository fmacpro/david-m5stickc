import { downsampleWav16MonoTo8bitRate, uint8ToArrayBuffer } from "../audio/wav";
import { corsHeaders, json, proxyJson } from "../core/http";
import {
  extractPictureQuery,
  isBatteryIntent,
  isDrawIntent,
  isTimeIntent,
  stripSttPromptLeak,
} from "../intent/intent";
import {
  fallbackScreenAction,
  finalizeScreenAction,
  generateScreenAction,
  iconScreenActionFromTranscript,
  sanitizeContextForPrompt,
} from "../intent/screen";
import {
  generateReply,
  transcribeFile,
} from "./openai";
import {
  appendHistory,
  clearHistory,
  getHistory,
  getMemoryFacts,
  parseMemoryCommand,
  putMemoryFacts,
  recallMemoryReply,
} from "./state";
import type { Env, ScreenAction } from "../types";

export async function handleVoiceTurn(
  request: Request,
  env: Env,
  deviceId: string
): Promise<Response> {
  const chunkedTts = request.headers.get("x-tts-chunked") === "1";
  const contentType = request.headers.get("content-type") || "";
  if (!contentType.includes("multipart/form-data")) {
    return json({ error: "Use multipart/form-data with field 'audio'" }, 400);
  }

  const form = await request.formData();
  const audio = form.get("audio");
  if (!(audio instanceof File)) {
    return json({ error: "Missing file field: audio" }, 400);
  }
  const contextRaw = form.get("context");
  const sensorContext =
    typeof contextRaw === "string" ? sanitizeContextForPrompt(contextRaw) : "";

  const sttResp = await transcribeFile(audio, env);
  if (!sttResp.ok) return sttResp.error;
  const transcript = sttResp.text.trim();
  if (!transcript) {
    return json({ error: "No speech recognized" }, 400);
  }
  return completeVoiceTurn(
    env,
    deviceId,
    transcript,
    sensorContext,
    chunkedTts,
    sttResp.meta
  );
}

export async function handleVoiceTurnText(
  request: Request,
  env: Env,
  deviceId: string
): Promise<Response> {
  const chunkedTts = request.headers.get("x-tts-chunked") === "1";
  let payload: { transcript?: string; context?: string };
  try {
    payload = (await request.json()) as { transcript?: string; context?: string };
  } catch {
    return json({ error: "Invalid JSON body" }, 400);
  }
  const transcript = (payload.transcript || "").trim();
  if (!transcript) return json({ error: "Missing field: transcript" }, 400);
  const sensorContext =
    typeof payload.context === "string"
      ? sanitizeContextForPrompt(payload.context)
      : "";
  return completeVoiceTurn(env, deviceId, transcript, sensorContext, chunkedTts);
}

async function completeVoiceTurn(
  env: Env,
  deviceId: string,
  transcript: string,
  sensorContext: string,
  chunkedTts: boolean,
  sttMeta?: { sttMs: number; audioBytes: number; model: string }
): Promise<Response> {
  const normalizedTranscript = sanitizeTranscriptIntentNoise(
    stripSttPromptLeak(transcript)
  );
  const transcriptLower = normalizedTranscript.toLowerCase();
  const timeIntent = isTimeIntent(transcriptLower);
  const batteryIntent = isBatteryIntent(transcriptLower);
  const drawIntent = isDrawIntent(transcriptLower);
  const pictureQuery = extractPictureQuery(normalizedTranscript);

  const history = await getHistory(env, deviceId);
  const memory = await getMemoryFacts(env, deviceId);
  const memCmd = parseMemoryCommand(normalizedTranscript);
  let shouldAppendHistory = true;
  const forcedDrawAction = pictureQuery
    ? null
    : iconScreenActionFromTranscript(normalizedTranscript, sensorContext);
  let deterministicAction: ScreenAction | null = null;

  let finalReply = "";
  const wantsDate = isDateIntent(transcriptLower);
  const wantsWifi = isWifiIntent(transcriptLower);
  const wantsTemp = isTempIntent(transcriptLower);
  const wantsTime = timeIntent;
  const wantsBattery = batteryIntent;
  const statusIntentCount =
    (wantsDate ? 1 : 0) +
    (wantsWifi ? 1 : 0) +
    (wantsTemp ? 1 : 0) +
    (wantsTime ? 1 : 0) +
    (wantsBattery ? 1 : 0);

  if (!pictureQuery && statusIntentCount >= 2 && !drawIntent) {
    const parts: string[] = [];
    if (wantsDate) {
      const localDate = extractLocalDate(sensorContext);
      if (localDate.length > 0) parts.push(`Date is ${localDate}`);
    }
    if (wantsTime) {
      const hhmm = extractLocalTimeHHMM(sensorContext);
      if (hhmm.length > 0) parts.push(`time is ${hhmm}`);
    }
    if (wantsBattery) {
      const pct = extractBatteryPercent(sensorContext);
      const charging = extractCharging(sensorContext);
      if (pct >= 0) {
        parts.push(charging ? `battery is ${pct}% and charging` : `battery is ${pct}%`);
      }
    }
    if (wantsWifi) {
      const rssi = extractWifiRssi(sensorContext);
      if (Number.isFinite(rssi)) parts.push(`Wi-Fi RSSI is ${rssi} dBm`);
    }
    if (wantsTemp) {
      const tempC = extractInternalTempC(sensorContext);
      if (Number.isFinite(tempC)) parts.push(`internal temperature is ${tempC.toFixed(1)} C`);
    }
    if (parts.length > 0) {
      finalReply = `${parts.join(", ")}.`;
    }
  }

  if (!finalReply && pictureQuery) {
    finalReply = `Showing a picture of ${pictureQuery}.`;
    deterministicAction = { mode: "image", value: pictureQuery, ttl_ms: 15000 };
  } else if (!finalReply && batteryIntent && !timeIntent && !drawIntent) {
    const pct = extractBatteryPercent(sensorContext);
    const charging = extractCharging(sensorContext);
    if (pct >= 0) {
      finalReply = charging
        ? `Battery is ${pct}% and charging.`
        : `Battery is ${pct}%.`;
      deterministicAction = {
        mode: "big_battery",
        value: `${pct}%`,
        percent: pct,
        charging,
        ttl_ms: 8000,
      };
    } else {
      finalReply = "I can't read battery level right now.";
    }
  } else if (!finalReply && timeIntent && !drawIntent) {
    const hhmm = extractLocalTimeHHMM(sensorContext);
    if (hhmm.length > 0) {
      finalReply = `It's ${hhmm} in your local time.`;
      deterministicAction = { mode: "big_time", value: hhmm, ttl_ms: 8000 };
    } else {
      finalReply = "I can't read the local time right now.";
    }
  } else if (!finalReply && memCmd.action === "remember") {
    const next = memory.includes(memCmd.fact)
      ? memory
      : [...memory, memCmd.fact].slice(-8);
    await putMemoryFacts(env, deviceId, next);
    finalReply = "Got it, I will remember that.";
  } else if (!finalReply && memCmd.action === "forget_all") {
    await putMemoryFacts(env, deviceId, []);
    await clearHistory(env, deviceId);
    history.length = 0;
    shouldAppendHistory = false;
    finalReply = "Done, I cleared what I remembered.";
  } else if (!finalReply && memCmd.action === "recall") {
    finalReply = recallMemoryReply(memory);
  } else if (!finalReply && forcedDrawAction) {
    finalReply = sanitizeDrawReplySpeech(normalizedTranscript);
  } else {
    const reply = await generateReply(
      env,
      normalizedTranscript,
      history,
      sensorContext,
      memory
    );
    if (!reply.ok) return reply.error;
    const maxReplyCharsRaw = parseInt(env.MAX_REPLY_CHARS || "420", 10);
    const maxReplyChars = Number.isFinite(maxReplyCharsRaw)
      ? Math.max(120, Math.min(900, maxReplyCharsRaw))
      : 420;
    const maxReplyWords = Math.max(20, Math.min(90, Math.floor(maxReplyChars / 6)));
    finalReply = normalizeReply(reply.text, maxReplyWords, maxReplyChars);
  }

  finalReply = stripSpeechMarkdown(finalReply);
  if (shouldAppendHistory) {
    await appendHistory(env, deviceId, { user: normalizedTranscript, assistant: finalReply }, history);
  }
  const screenAction = deterministicAction
    ? deterministicAction
    : forcedDrawAction
    ? forcedDrawAction
    : await generateScreenAction(
        env,
        normalizedTranscript,
        finalReply,
        sensorContext
      );

  const safeScreenAction =
    screenAction.mode === "none"
      ? fallbackScreenAction(normalizedTranscript, finalReply, sensorContext)
      : screenAction;
  const finalizedScreenAction = finalizeScreenAction(
    safeScreenAction,
    normalizedTranscript,
    finalReply,
    sensorContext
  );
  if (finalizedScreenAction.mode === "draw") {
    finalReply = sanitizeDrawReplySpeech(normalizedTranscript);
  }

  if (chunkedTts) {
    const headers = new Headers();
    headers.set("Cache-Control", "no-store");
    headers.set("x-transcript", encodeHeaderValue(normalizedTranscript));
    headers.set("x-reply", encodeHeaderValue(finalReply, 1200));
    headers.set(
      "x-screen",
      encodeHeaderValue(JSON.stringify(finalizedScreenAction), 700)
    );
    if (sttMeta) {
      headers.set("x-stt-ms", String(sttMeta.sttMs));
      headers.set("x-audio-bytes", String(sttMeta.audioBytes));
      headers.set("x-stt-model", encodeHeaderValue(sttMeta.model, 120));
    }
    for (const [k, v] of Object.entries(corsHeaders())) headers.set(k, v);
    return new Response(null, { status: 204, headers });
  }

  const ttsResp = await fetch("https://api.openai.com/v1/audio/speech", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: env.TTS_MODEL || "gpt-4o-mini-tts",
      voice: env.TTS_VOICE || "alloy",
      response_format: "wav",
      speed: 1.0,
      instructions:
        env.TTS_INSTRUCTIONS ||
        "Speak with a warm British English accent, lower pitch, calm pacing, and clear articulation.",
      input: finalReply,
    }),
  });

  const audioBytes = await ttsResp.arrayBuffer();
  if (!ttsResp.ok) {
    const errText = new TextDecoder().decode(audioBytes);
    return proxyJson(ttsResp.status, errText);
  }

  const rawBytes = new Uint8Array(audioBytes);
  const looksLikeWav =
    rawBytes.byteLength >= 12 &&
    rawBytes[0] === 0x52 &&
    rawBytes[1] === 0x49 &&
    rawBytes[2] === 0x46 &&
    rawBytes[3] === 0x46 &&
    rawBytes[8] === 0x57 &&
    rawBytes[9] === 0x41 &&
    rawBytes[10] === 0x56 &&
    rawBytes[11] === 0x45;
  const compactAudio = looksLikeWav
    ? downsampleWav16MonoTo8bitRate(rawBytes, 8000)
    : rawBytes;

  const headers = new Headers();
  headers.set("Content-Type", ttsResp.headers.get("content-type") || "audio/wav");
  headers.set("Cache-Control", "no-store");
  headers.set("x-transcript", encodeHeaderValue(normalizedTranscript));
  headers.set("x-reply", encodeHeaderValue(finalReply));
  headers.set(
    "x-screen",
    encodeHeaderValue(JSON.stringify(finalizedScreenAction), 700)
  );
  if (sttMeta) {
    headers.set("x-stt-ms", String(sttMeta.sttMs));
    headers.set("x-audio-bytes", String(sttMeta.audioBytes));
    headers.set("x-stt-model", encodeHeaderValue(sttMeta.model, 120));
  }
  for (const [k, v] of Object.entries(corsHeaders())) headers.set(k, v);
  return new Response(uint8ToArrayBuffer(compactAudio), { status: 200, headers });
}

function normalizeReply(text: string, maxWords = 20, maxChars = 120): string {
  const cleaned = text.replace(/\s+/g, " ").trim();
  if (!cleaned) return "";
  const words = cleaned.split(" ").filter(Boolean);
  let capped = words.length > maxWords ? words.slice(0, maxWords).join(" ") : cleaned;
  if (capped.length > maxChars) capped = capped.slice(0, maxChars).trim();
  const endsWithPunct = /[.!?]$/.test(capped);
  return endsWithPunct ? capped : `${capped}.`;
}

function sanitizeDrawReplySpeech(transcript: string): string {
  const t = transcript.toLowerCase();
  if (
    t.includes("draw") ||
    t.includes("sketch") ||
    t.includes("show on screen") ||
    t.includes("display on screen")
  ) {
    return "Sure, drawing that now.";
  }
  return "Drawing now.";
}

function stripSpeechMarkdown(text: string): string {
  return text
    .replace(/\*\*(.*?)\*\*/g, "$1")
    .replace(/\*(.*?)\*/g, "$1")
    .replace(/`(.*?)`/g, "$1")
    .replace(/\[(.*?)\]\((.*?)\)/g, "$1")
    .replace(/\s{2,}/g, " ")
    .trim();
}

function encodeHeaderValue(input: string, maxLen = 220): string {
  return encodeURIComponent(input.slice(0, maxLen));
}

function extractLocalTimeHHMM(sensorContextRaw: string): string {
  if (!sensorContextRaw) return "";
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const local = parsed.local_time_24h;
    if (typeof local !== "string") return "";
    const m = local.match(/\b(\d{2}:\d{2})/);
    return m ? m[1] : "";
  } catch {
    return "";
  }
}

function extractBatteryPercent(sensorContextRaw: string): number {
  if (!sensorContextRaw) return -1;
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const v = Number(parsed.battery_level);
    if (!Number.isFinite(v)) return -1;
    return Math.max(0, Math.min(100, Math.trunc(v)));
  } catch {
    return -1;
  }
}

function extractCharging(sensorContextRaw: string): boolean {
  if (!sensorContextRaw) return false;
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    return Boolean(parsed.charging);
  } catch {
    return false;
  }
}

function extractWifiRssi(sensorContextRaw: string): number {
  if (!sensorContextRaw) return Number.NaN;
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const v = Number(parsed.wifi_rssi);
    return Number.isFinite(v) ? Math.trunc(v) : Number.NaN;
  } catch {
    return Number.NaN;
  }
}

function extractInternalTempC(sensorContextRaw: string): number {
  if (!sensorContextRaw) return Number.NaN;
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const v = Number(parsed.internal_temp_c);
    return Number.isFinite(v) ? v : Number.NaN;
  } catch {
    return Number.NaN;
  }
}

function extractLocalDate(sensorContextRaw: string): string {
  if (!sensorContextRaw) return "";
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const local = parsed.local_time_24h;
    if (typeof local !== "string") return "";
    const m = local.match(/\b(\d{4}-\d{2}-\d{2})\b/);
    return m ? m[1] : "";
  } catch {
    return "";
  }
}

function isDateIntent(lower: string): boolean {
  return (
    lower.includes("what date") ||
    lower.includes("current date") ||
    lower.includes("today's date") ||
    lower.includes("todays date") ||
    lower.includes("date is it today")
  );
}

function isWifiIntent(lower: string): boolean {
  return (
    lower.includes("wifi") ||
    lower.includes("wi-fi") ||
    lower.includes("signal strength") ||
    lower.includes("rssi")
  );
}

function isTempIntent(lower: string): boolean {
  return (
    lower.includes("temperature") ||
    lower.includes("temp")
  );
}

function sanitizeTranscriptIntentNoise(input: string): string {
  const text = input.replace(/\s+/g, " ").trim();
  if (!text) return text;
  const q = text.indexOf("?");
  if (q < 20 || q >= text.length - 8) return text;

  const tail = text.slice(q + 1).toLowerCase();
  const hasImperative = /\b(display|show|draw)\b/.test(tail);
  if (!hasImperative) return text;

  const keyHints = [
    "battery",
    "time",
    "date",
    "draw",
    "temp",
    "temperature",
    "wifi",
    "wi-fi",
    "signal",
  ];
  let hits = 0;
  for (const k of keyHints) {
    if (tail.includes(k)) hits += 1;
  }
  if (hits < 4) return text;
  return text.slice(0, q + 1).trim();
}
