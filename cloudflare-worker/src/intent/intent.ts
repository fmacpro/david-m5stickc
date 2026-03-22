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

export function isLikelyTimeMisheardAsBattery(lower: string): boolean {
  if (!lower.includes("battery")) return false;
  if (lower.includes("battery level")) return false;
  if (lower.includes("charge") || lower.includes("power level") || lower.includes("%")) return false;
  return (
    lower.includes("what battery is it") ||
    lower.includes("tell me the battery") ||
    lower.includes("current battery is it") ||
    lower.includes("battery is it")
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
