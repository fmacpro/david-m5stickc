export function isTimeIntent(lower: string): boolean {
  const t = lower.trim();
  if (t === "time" || t === "clock") return true;
  return (
    lower.includes("what time") ||
    lower.includes("tell me the time") ||
    lower.includes("current time") ||
    lower.includes("time now") ||
    lower.includes("the time") ||
    lower.includes("time please") ||
    lower.includes("time is it") ||
    lower.trim() === "time"
  );
}

export function isBatteryIntent(lower: string): boolean {
  return (
    lower.includes("battery") ||
    lower.includes("charge level") ||
    lower.includes("power level")
  );
}

export function isDrawIntent(lower: string): boolean {
  const t = lower.trim();
  return (
    t.includes("draw") ||
    t.includes("sketch") ||
    t.includes("doodle") ||
    t.includes("icon") ||
    t.includes("shape") ||
    t.includes("show me a") ||
    t.includes("show a") ||
    t.includes("display a")
  );
}

function normalizeForPicture(text: string): string {
  return text
    .toLowerCase()
    .replace(/[?!.,]/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

function trimPictureLead(subject: string): string {
  let out = subject.trim();
  out = out.replace(/^(of|a|an|the)\s+/i, "");
  out = out.replace(/\s+(please|thanks|thank you)$/i, "");
  return out.trim();
}

export function extractPictureQuery(input: string): string {
  const t = normalizeForPicture(input);
  if (!t) return "";
  if (t.includes("picture in picture")) return "";
  const patterns = [
    /\b(?:show|display|give|find)\s+(?:me\s+)?(?:a|an)?\s*(?:picture|photo|image)\s+of\s+(.+)$/,
    /\b(?:show|display|give|find)\s+(?:me\s+)?(?:a|an)?\s*(?:picture|photo|image)\s+(.+)$/,
    /\bshow me (?:a|an)?\s*(?:picture|photo|image)\s+of\s+(.+)$/,
    /\bshow me (?:a|an)?\s*(?:picture|photo|image)\s+(.+)$/,
    /\bdisplay (?:a|an)?\s*(?:picture|photo|image)\s+of\s+(.+)$/,
    /\bdisplay (?:a|an)?\s*(?:picture|photo|image)\s+(.+)$/,
    /\b(?:a|an|the)\s+(?:picture|photo|image)\s+of\s+(.+)$/,
    /\b(?:picture|photo|image)\s+of\s+(.+)$/,
  ];
  for (const re of patterns) {
    const m = t.match(re);
    if (!m || m.length < 2) continue;
    const q = trimPictureLead(m[1] || "");
    if (q.length >= 2) return q.slice(0, 80);
  }
  return "";
}

export function stripSttPromptLeak(text: string): string {
  let out = (text || "").trim();
  if (!out) return out;
  out = out.replace(
    /([?.!])\s*display,\s*battery,\s*level,\s*time,\s*date,\s*draw,\s*temperature,\s*wi-?fi,\s*david\.?/gi,
    "$1"
  );
  out = out.replace(
    /\s*display,\s*battery,\s*level,\s*time,\s*date,\s*draw,\s*temperature,\s*wi-?fi,\s*david\.?/gi,
    " "
  );
  out = out.replace(/\s{2,}/g, " ").trim();
  return out;
}
