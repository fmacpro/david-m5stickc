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
