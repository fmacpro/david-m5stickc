import { extractOutputText } from "../services/openai";
import type { Env, ScreenAction } from "../types";
import { isLikelyTimeMisheardAsBattery } from "./intent";

export function sanitizeContextForPrompt(raw: string): string {
  const trimmed = raw.trim();
  if (!trimmed) return "";
  try {
    const parsed = JSON.parse(trimmed);
    const compact = JSON.stringify(parsed);
    return compact.slice(0, 900);
  } catch {
    return trimmed.replace(/\s+/g, " ").slice(0, 900);
  }
}

export function sanitizeDrawScript(raw: string): string {
  if (!raw) return "";
  const commands = raw
    .split(";")
    .map((c) => c.trim())
    .filter(Boolean);
  const out: string[] = [];
  const allowedOps = new Set(["CLS", "P", "L", "R", "F", "C", "D"]);
  for (const cmd of commands) {
    const op = cmd.split(/\s+/, 1)[0]?.toUpperCase() ?? "";
    if (allowedOps.has(op)) out.push(cmd);
  }
  return out.join(";");
}

export async function generateScreenAction(
  env: Env,
  transcript: string,
  reply: string,
  sensorContext: string
): Promise<ScreenAction> {
  const contextHint = sensorContext || "{}";
  const prompt = [
    "Return ONLY minified JSON with this shape:",
    '{"mode":"none"|"big_time"|"big_battery"|"big_rssi"|"big_temp"|"big_date"|"big_value"|"draw","title":"optional","value":"optional","draw":"optional compact script","percent":0..100 optional,"charging":bool optional,"ttl_ms":1000..15000}',
    "Use large/iconic display styles. Prefer big_time, big_battery, big_rssi, or big_temp when possible.",
    'For mode=draw use script commands separated by ";" from this safe set only: CLS, P x y, L x1 y1 x2 y2, R x y w h, F x y w h, C x y r, D x y r.',
    "Never include text commands or labels inside draw scripts.",
    "No markdown. No extra keys.",
    `User request: ${transcript}`,
    `Assistant speech reply: ${reply}`,
    `Device context JSON: ${contextHint}`,
  ].join("\n");

  const resp = await fetch("https://api.openai.com/v1/responses", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: env.CHAT_MODEL || "gpt-5.4-nano",
      max_output_tokens: 120,
      input: [
        {
          role: "system",
          content: [
            {
              type: "input_text",
              text: "You output strict JSON only.",
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
  if (!resp.ok) return { mode: "none" };
  const raw = await resp.text();
  try {
    const parsed = JSON.parse(raw);
    const text = extractOutputText(parsed);
    return normalizeScreenAction(text);
  } catch {
    return { mode: "none" };
  }
}

function wantsVisualIntent(text: string): boolean {
  const t = text.toLowerCase();
  return (
    t.includes("show") ||
    t.includes("display") ||
    t.includes("screen") ||
    t.includes("visual") ||
    t.includes("draw") ||
    t.includes("write this")
  );
}

function wantsDrawIntent(text: string): boolean {
  const t = text.toLowerCase();
  return (
    t.includes("draw") ||
    t.includes("sketch") ||
    t.includes("icon") ||
    t.includes("show me a") ||
    t.includes("show a") ||
    t.includes("display a") ||
    t.includes("make a")
  );
}

function buildBatteryIconScript(percent: number, charging: boolean): string {
  const p = Math.max(0, Math.min(100, Math.trunc(percent)));
  const bars = Math.max(0, Math.min(5, Math.floor((p + 19) / 20)));
  const parts: string[] = ["CLS", "R 35 26 86 28", "F 121 34 7 12"];
  for (let i = 0; i < 5; i += 1) {
    const x = 41 + i * 15;
    if (i < bars) parts.push(`F ${x} 31 10 18`);
    else parts.push(`R ${x} 31 10 18`);
  }
  if (charging) {
    parts.push("F 76 28 4 10");
    parts.push("F 73 36 10 4");
  }
  return parts.join(";");
}

function iconScriptByName(name: string, sensorContextRaw = ""): string {
  const n = name.toLowerCase();
  if (n === "circle") return "CLS;C 80 40 22";
  if (n === "square") return "CLS;R 56 16 48 48";
  if (n === "rectangle") return "CLS;R 46 24 68 34";
  if (n === "triangle") return "CLS;L 80 16 48 58;L 48 58 112 58;L 112 58 80 16";
  if (n === "diamond") return "CLS;L 80 14 48 40;L 48 40 80 66;L 80 66 112 40;L 112 40 80 14";
  if (n === "line") return "CLS;L 44 40 116 40";
  if (n === "pentagon")
    return "CLS;L 80 14 50 32;L 50 32 60 62;L 60 62 100 62;L 100 62 110 32;L 110 32 80 14";
  if (n === "hexagon")
    return "CLS;L 62 20 98 20;L 98 20 114 40;L 114 40 98 60;L 98 60 62 60;L 62 60 46 40;L 46 40 62 20";
  if (n === "smile" || n === "smiley" || n === "face")
    return "CLS;C 80 38 24;D 71 33 2;D 89 33 2;L 68 46 72 50;L 72 50 88 50;L 88 50 92 46";
  if (n === "heart")
    return "CLS;D 71 35 8;D 89 35 8;L 63 39 80 58;L 97 39 80 58;F 72 33 16 8";
  if (n === "clock") return "CLS;C 80 40 24;L 80 40 80 27;L 80 40 91 45;D 80 40 2";
  if (n === "wifi")
    return "CLS;D 80 54 3;L 72 46 80 42;L 80 42 88 46;L 64 40 72 34;L 72 34 88 34;L 88 34 96 40;L 56 32 66 24;L 66 24 94 24;L 94 24 104 32";
  if (n === "temp" || n === "temperature") return "CLS;R 72 20 16 28;D 80 53 7;F 77 30 6 20";
  if (n === "home" || n === "house")
    return "CLS;L 54 38 80 20;L 80 20 106 38;R 58 38 44 26;R 74 46 12 18";
  if (n === "sun")
    return "CLS;D 80 40 12;L 80 18 80 10;L 80 62 80 70;L 58 40 50 40;L 102 40 110 40;L 64 24 58 18;L 96 24 102 18;L 64 56 58 62;L 96 56 102 62";
  if (n === "moon") return "CLS;D 78 38 18;D 86 32 16";
  if (n === "star")
    return "CLS;L 80 18 86 34;L 86 34 104 34;L 104 34 90 45;L 90 45 95 62;L 95 62 80 52;L 80 52 65 62;L 65 62 70 45;L 70 45 56 34;L 56 34 74 34;L 74 34 80 18";
  if (n === "star4") return "CLS;L 80 14 80 66;L 54 40 106 40;L 62 22 98 58;L 98 22 62 58";
  if (n === "star8")
    return "CLS;L 80 14 80 66;L 54 40 106 40;L 62 22 98 58;L 98 22 62 58;L 70 14 90 66;L 90 14 70 66";
  if (n === "arrowup") return "CLS;L 80 18 80 62;L 80 18 62 36;L 80 18 98 36";
  if (n === "arrowdown") return "CLS;L 80 18 80 62;L 80 62 62 44;L 80 62 98 44";
  if (n === "cloud") return "CLS;D 68 42 10;D 82 36 12;D 96 42 10;F 58 42 50 14";
  if (n === "rain")
    return "CLS;D 68 34 10;D 82 28 12;D 96 34 10;F 58 34 50 12;L 68 50 64 60;L 80 50 76 60;L 92 50 88 60";
  if (n === "calendar")
    return "CLS;R 52 20 56 40;F 52 28 56 8;F 62 16 4 8;F 94 16 4 8;R 62 40 10 8;R 78 40 10 8;R 94 40 10 8";
  if (n === "thermo") return "CLS;R 72 20 16 28;D 80 53 7;F 77 30 6 20";
  if (n === "check") return "CLS;L 56 42 74 58;L 74 58 106 24";
  if (n === "x" || n === "cross") return "CLS;L 56 24 104 56;L 104 24 56 56";
  if (n === "battery") {
    const pct = extractBatteryPercent(sensorContextRaw);
    const ch = extractCharging(sensorContextRaw);
    return buildBatteryIconScript(pct >= 0 ? pct : 50, ch);
  }
  return "";
}

function pickIconNameFromTranscript(transcript: string): string {
  const t = transcript.toLowerCase();
  if (t.includes("shape")) return "circle";
  if (t.includes("triangle")) return "triangle";
  if (t.includes("square")) return "square";
  if (t.includes("rectangle")) return "rectangle";
  if (t.includes("diamond")) return "diamond";
  if (t.includes("pentagon")) return "pentagon";
  if (t.includes("hexagon")) return "hexagon";
  if (t.includes("line")) return "line";
  if (t.includes("4 point star") || t.includes("four point star")) return "star4";
  if (t.includes("8 point star") || t.includes("eight point star")) return "star8";
  if (t.includes("calendar")) return "calendar";
  if (t.includes("battery")) return "battery";
  if (t.includes("time") || t.includes("clock")) return "clock";
  if (t.includes("wifi") || t.includes("signal")) return "wifi";
  if (t.includes("temp")) return "thermo";
  if (t.includes("weather") || t.includes("cloud")) return "cloud";
  if (t.includes("rain")) return "rain";
  if (t.includes("star")) return "star";
  if (t.includes("up arrow") || t.includes("arrow up")) return "arrowup";
  if (t.includes("down arrow") || t.includes("arrow down")) return "arrowdown";
  if (t.includes("home") || t.includes("house")) return "house";
  if (t.includes("sun")) return "sun";
  if (t.includes("moon")) return "moon";
  if (t.includes("heart")) return "heart";
  if (t.includes("check")) return "check";
  if (t.includes("cross") || t.includes(" x ")) return "x";
  if (t.includes("circle")) return "circle";
  if (t.includes("smile") || t.includes("smiley") || t.includes("face")) return "smiley";
  return "";
}

export function iconScreenActionFromTranscript(
  transcript: string,
  sensorContextRaw: string
): ScreenAction | null {
  if (!wantsDrawIntent(transcript)) return null;
  const name = pickIconNameFromTranscript(transcript);
  if (!name) return null;
  const script = iconScriptByName(name, sensorContextRaw);
  if (!script) return null;
  return {
    mode: "draw",
    draw: script,
    ttl_ms: 9000,
  };
}

export function fallbackScreenAction(
  transcript: string,
  reply: string,
  sensorContextRaw = ""
): ScreenAction {
  const t = transcript.toLowerCase();
  const r = reply.toLowerCase();
  const explicitVisual = wantsVisualIntent(transcript);
  const batteryPct = extractBatteryPercent(sensorContextRaw);
  const charging = extractCharging(sensorContextRaw);
  const rssi = extractRssi(sensorContextRaw);
  const localTime = extractLocalTimeHHMM(sensorContextRaw);
  const localMonthDay = extractLocalMonthDay(sensorContextRaw);
  const internalTempC = extractInternalTempC(sensorContextRaw);
  const iconAction = iconScreenActionFromTranscript(transcript, sensorContextRaw);
  if (iconAction) return iconAction;

  if (explicitVisual && (t.includes("battery") || r.includes("battery"))) {
    const pct = reply.match(/(\d{1,3})\s*%/);
    const p = pct ? Number(pct[1]) : batteryPct;
    return {
      mode: "big_battery",
      title: "Battery",
      value: p >= 0 ? `${p}%` : "Battery",
      percent: p,
      charging,
      ttl_ms: 8000,
    };
  }

  if (explicitVisual && (t.includes("time") || r.includes(":"))) {
    const hhmm = reply.match(/\b(\d{1,2}:\d{2})\b/);
    if (hhmm) {
      return {
        mode: "big_time",
        value: hhmm[1],
        ttl_ms: 8000,
      };
    }
    if (localTime.length > 0) {
      return {
        mode: "big_time",
        value: localTime,
        ttl_ms: 8000,
      };
    }
  }

  if (explicitVisual && (t.includes("signal") || t.includes("wifi") || t.includes("rssi"))) {
    if (rssi > -200) {
      return {
        mode: "big_rssi",
        value: `${rssi} dBm`,
        percent: rssiToPercent(rssi),
        ttl_ms: 8000,
      };
    }
  }

  if (
    explicitVisual &&
    (t.includes("temp") || t.includes("temperature") || t.includes("hot") || t.includes("cold"))
  ) {
    if (internalTempC > -200) {
      return {
        mode: "big_temp",
        value: `${formatOneDecimal(internalTempC)}C`,
        ttl_ms: 8000,
      };
    }
  }

  if (explicitVisual && (t.includes("date") || t.includes("day"))) {
    if (localMonthDay.length > 0) {
      return {
        mode: "big_date",
        title: "Date",
        value: localMonthDay,
        ttl_ms: 8000,
      };
    }
  }

  if (explicitVisual) {
    const token = pickDefaultWord(transcript);
    if (token) {
      return {
        mode: "big_value",
        value: token,
        ttl_ms: 5000,
      };
    }
    return { mode: "none" };
  }
  return { mode: "none" };
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

function extractLocalMonthDay(sensorContextRaw: string): string {
  const d = extractLocalDate(sensorContextRaw);
  const m = d.match(/^(\d{4})-(\d{2})-(\d{2})$/);
  if (!m) return "";
  const month = Number(m[2]);
  const day = Number(m[3]);
  if (!Number.isFinite(month) || !Number.isFinite(day)) return "";
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  const months = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"];
  return `${months[month - 1]} ${day}`;
}

function extractLocalYear(sensorContextRaw: string): string {
  const d = extractLocalDate(sensorContextRaw);
  const m = d.match(/^(\d{4})-/);
  return m ? m[1] : "";
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

function extractRssi(sensorContextRaw: string): number {
  if (!sensorContextRaw) return -999;
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const v = Number(parsed.wifi_rssi);
    return Number.isFinite(v) ? Math.trunc(v) : -999;
  } catch {
    return -999;
  }
}

function extractInternalTempC(sensorContextRaw: string): number {
  if (!sensorContextRaw) return -999;
  try {
    const parsed = JSON.parse(sensorContextRaw) as Record<string, unknown>;
    const imu = parsed.imu;
    if (!imu || typeof imu !== "object") return -999;
    const temp = Number((imu as Record<string, unknown>).internal_temp_c);
    return Number.isFinite(temp) ? temp : -999;
  } catch {
    return -999;
  }
}

function formatOneDecimal(v: number): string {
  return (Math.round(v * 10) / 10).toFixed(1);
}

function rssiToPercent(rssi: number): number {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return Math.round((rssi + 100) * 2);
}

function normalizeScreenAction(raw: string): ScreenAction {
  const parsed = parseLooseJsonObject(raw);
  if (!parsed || typeof parsed !== "object") return { mode: "none" };
  const obj = parsed as Record<string, unknown>;
  const modeRaw = typeof obj.mode === "string" ? obj.mode : "none";
  const allowed = new Set(["none", "big_time", "big_battery", "big_rssi", "big_temp", "big_date", "big_value", "draw"]);
  const mode = allowed.has(modeRaw) ? (modeRaw as ScreenAction["mode"]) : "none";
  if (mode === "none") return { mode: "none" };
  let title = clipText(obj.title, 20);
  let value = clipText(obj.value, 28);
  let draw = clipText(obj.draw, 420);
  // Model prompt uses "optional" as schema hint; treat literal "optional" as empty.
  if (title.toLowerCase() === "optional") title = "";
  if (value.toLowerCase() === "optional") value = "";
  if (draw.toLowerCase() === "optional") draw = "";
  if (mode === "draw" && draw.length === 0) {
    draw = clipText(obj.value, 420);
  }
  if (mode === "draw") {
    draw = sanitizeDrawScript(draw);
  }
  const percentRaw = Number(obj.percent);
  const percent = Number.isFinite(percentRaw)
    ? Math.max(0, Math.min(100, Math.trunc(percentRaw)))
    : undefined;
  const charging = typeof obj.charging === "boolean" ? obj.charging : undefined;
  const ttlRaw = Number(obj.ttl_ms);
  const ttlMs = Number.isFinite(ttlRaw)
    ? Math.max(1000, Math.min(15000, Math.trunc(ttlRaw)))
    : 7000;
  let normalizedMode = mode;
  if (mode === "big_time" && !looksLikeTimeValue(value)) {
    normalizedMode = "big_value";
  }
  if (normalizedMode === "big_value") {
    value = sanitizeBigValueToken(value);
    if (value.length > 10) title = "";
  }
  return { mode: normalizedMode, title, value, draw, percent, charging, ttl_ms: ttlMs };
}

function looksLikeTimeValue(value: string): boolean {
  return /^\d{1,2}:\d{2}$/.test(value.trim());
}

export function finalizeScreenAction(
  action: ScreenAction,
  transcript: string,
  reply: string,
  sensorContextRaw: string
): ScreenAction {
  const t = transcript.toLowerCase();
  const asksTime = /\b(time|clock)\b/.test(t) || isLikelyTimeMisheardAsBattery(t);
  const asksBattery = t.includes("battery");
  const asksTemp = t.includes("temp") || t.includes("temperature");
  const asksYear = t.includes("year");
  const asksDateOrDay = t.includes("date") || t.includes("day");
  const visualIntent = wantsVisualIntent(transcript);
  const topicIntent = wantsGlanceTopic(transcript);
  const greetingIntent = wantsGreeting(transcript);
  const allowDisplay = visualIntent || topicIntent;
  const iconAction = iconScreenActionFromTranscript(transcript, sensorContextRaw);
  if (iconAction) return iconAction;

  if (action.mode === "big_time" && action.value && action.value.length > 0) {
    return action;
  }
  if (action.mode === "big_battery" && (action.percent ?? -1) >= 0) {
    return action;
  }

  if (!allowDisplay) {
    if (greetingIntent) {
      return {
        mode: "big_value",
        value: "HI!",
        ttl_ms: 2500,
      };
    }
    const compact = compactReplyValue(reply);
    if (compact.length > 0 && /\b(who|what)\b/i.test(transcript)) {
      return {
        mode: "big_value",
        value: compact,
        ttl_ms: 6000,
      };
    }
    return { mode: "none" };
  }

  if (asksTime) {
    const localTime = extractLocalTimeHHMM(sensorContextRaw);
    if (localTime.length > 0) {
      return {
        mode: "big_time",
        value: localTime,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
    const hhmm = (reply.match(/\b(\d{1,2}:\d{2})\b/) || ["", ""])[1];
    if (hhmm.length > 0) {
      return {
        mode: "big_time",
        value: hhmm,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
  }

  if (asksBattery && !asksTime) {
    const pct = extractBatteryPercent(sensorContextRaw);
    const charging = extractCharging(sensorContextRaw);
    if (pct >= 0) {
      return {
        mode: "big_battery",
        title: "Battery",
        value: `${pct}%`,
        percent: pct,
        charging,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
  }

  if (asksTemp) {
    const temp = extractInternalTempC(sensorContextRaw);
    if (temp > -200) {
      return {
        mode: "big_temp",
        value: `${formatOneDecimal(temp)}C`,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
  }

  if (asksYear) {
    const localYear = extractLocalYear(sensorContextRaw);
    if (localYear.length === 4) {
      return {
        mode: "big_value",
        value: localYear,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
    const replyYear = (reply.match(/\b(19|20)\d{2}\b/) || [""])[0];
    if (replyYear.length === 4) {
      return {
        mode: "big_value",
        value: replyYear,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
  }

  if (asksDateOrDay && !asksYear) {
    const md = extractLocalMonthDay(sensorContextRaw);
    if (md.length > 0) {
      return {
        mode: "big_date",
        title: "Date",
        value: md,
        ttl_ms: action.ttl_ms ?? 8000,
      };
    }
  }

  if (hasRenderableContent(action)) return action;

  if (allowDisplay) {
    const fallback = fallbackScreenAction(transcript, reply, sensorContextRaw);
    if (hasRenderableContent(fallback)) return fallback;
    const word = pickDefaultWord(transcript);
    if (!word) return { mode: "none" };
    return {
      mode: "big_value",
      value: word,
      ttl_ms: 5000,
    };
  }

  return { mode: "none" };
}

function hasRenderableContent(action: ScreenAction): boolean {
  if (action.mode === "none") return false;
  if (action.mode === "draw") return Boolean(action.draw && action.draw.length > 0);
  if (action.mode === "big_battery") return (action.percent ?? -1) >= 0;
  if (action.mode === "big_time") return Boolean(action.value && action.value.length > 0);
  if (action.mode === "big_temp") return Boolean(action.value && action.value.length > 0);
  if (action.mode === "big_rssi")
    return (action.percent ?? -1) >= 0 || Boolean(action.value && action.value.length > 0);
  if (action.mode === "big_date")
    return Boolean((action.value && action.value.length > 0) || (action.title && action.title.length > 0));
  if (action.mode === "big_value") return Boolean(action.value && action.value.length > 0);
  return false;
}

function sanitizeBigValueToken(input: string): string {
  const cleaned = input.trim().replace(/\s+/g, " ");
  if (!cleaned) return "";
  const upper = cleaned.toUpperCase();
  if (
    upper === "HI" ||
    upper === "HI!" ||
    upper === "HELLO" ||
    upper === "HELLO!" ||
    upper === "HEY" ||
    upper === "HEY!"
  ) {
    return upper.endsWith("!") ? upper : `${upper}!`;
  }
  // Preserve short multi-word values (names, compact facts) for centered two-line rendering.
  if (cleaned.includes(" ")) return cleaned.slice(0, 24);
  const single = (cleaned.match(/[A-Za-z0-9]+/) || [""])[0].toUpperCase();
  return single.slice(0, 12);
}

function pickDefaultWord(transcript: string): string {
  const t = transcript.toLowerCase();
  if (wantsGreeting(transcript)) return "HI!";
  if (t.includes("time") || t.includes("clock")) return "TIME";
  if (t.includes("battery") || t.includes("charge")) return "POWER";
  if (t.includes("signal") || t.includes("wifi") || t.includes("network")) return "SIGNAL";
  if (t.includes("temp") || t.includes("temperature")) return "TEMP";
  if (t.includes("date") || t.includes("day") || t.includes("year")) return "DATE";
  return "";
}

function wantsGlanceTopic(text: string): boolean {
  const t = text.toLowerCase();
  return (
    t.includes("time") ||
    t.includes("clock") ||
    t.includes("date") ||
    t.includes("day") ||
    t.includes("year") ||
    t.includes("battery") ||
    t.includes("charge") ||
    t.includes("signal") ||
    t.includes("wifi") ||
    t.includes("network") ||
    t.includes("temp") ||
    t.includes("temperature")
  );
}

function wantsGreeting(text: string): boolean {
  const t = text.toLowerCase().trim();
  const normalized = t.replace(/[^a-z0-9\s]/g, " ").replace(/\s+/g, " ").trim();
  return (
    normalized === "hi" ||
    normalized === "hello" ||
    normalized === "hey" ||
    normalized.startsWith("hi ") ||
    normalized.startsWith("hello ") ||
    normalized.startsWith("hey ") ||
    normalized.includes("good morning") ||
    normalized.includes("good afternoon") ||
    normalized.includes("good evening")
  );
}

function parseLooseJsonObject(raw: string): unknown {
  const trimmed = raw.trim();
  try {
    return JSON.parse(trimmed);
  } catch {
    const start = trimmed.indexOf("{");
    const end = trimmed.lastIndexOf("}");
    if (start < 0 || end <= start) return null;
    const maybe = trimmed.slice(start, end + 1);
    try {
      return JSON.parse(maybe);
    } catch {
      return null;
    }
  }
}

function clipText(v: unknown, maxLen: number): string {
  if (typeof v !== "string") return "";
  return v.replace(/\s+/g, " ").trim().slice(0, maxLen);
}

function compactReplyValue(reply: string): string {
  const cleaned = reply
    .replace(/\*+/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/[.!?]+$/, "");
  if (!cleaned) return "";
  if (cleaned.length > 24) return "";
  if (/^it('?s| is)\b/i.test(cleaned)) return "";
  return cleaned;
}
