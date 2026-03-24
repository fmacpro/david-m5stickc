import type { Env, MemoryCommand, Turn } from "../types";

export async function getHistory(env: Env, deviceId: string): Promise<Turn[]> {
  const raw = await env.DEVICE_STATE.get(`history:${deviceId}`);
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw) as Turn[];
    return Array.isArray(parsed) ? parsed.slice(-6) : [];
  } catch {
    return [];
  }
}

export async function appendHistory(
  env: Env,
  deviceId: string,
  turn: Turn,
  baseHistory?: Turn[]
): Promise<void> {
  const history = Array.isArray(baseHistory)
    ? baseHistory.slice(-6)
    : await getHistory(env, deviceId);
  history.push(turn);
  const compact = history.slice(-6);
  await env.DEVICE_STATE.put(`history:${deviceId}`, JSON.stringify(compact), {
    expirationTtl: 60 * 60 * 24,
  });
}

export async function clearHistory(env: Env, deviceId: string): Promise<void> {
  await env.DEVICE_STATE.delete(`history:${deviceId}`);
}

export async function getMemoryFacts(env: Env, deviceId: string): Promise<string[]> {
  const raw = await env.DEVICE_STATE.get(`memory:${deviceId}`);
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw) as string[];
    if (!Array.isArray(parsed)) return [];
    return parsed
      .filter((v) => typeof v === "string")
      .map((v) => v.trim())
      .filter((v) => v.length > 0)
      .slice(-8);
  } catch {
    return [];
  }
}

export async function putMemoryFacts(
  env: Env,
  deviceId: string,
  facts: string[]
): Promise<void> {
  const compact = facts
    .map((v) => v.replace(/\s+/g, " ").trim())
    .filter((v) => v.length > 0)
    .slice(-8);
  await env.DEVICE_STATE.put(`memory:${deviceId}`, JSON.stringify(compact), {
    expirationTtl: 60 * 60 * 24 * 30,
  });
}

export function parseMemoryCommand(transcript: string): MemoryCommand {
  const t = transcript.trim();
  const lower = t.toLowerCase();
  if (lower.startsWith("remember ")) {
    const fact = t.slice("remember ".length).trim().replace(/\s+/g, " ");
    if (fact.length >= 3) return { action: "remember", fact: fact.slice(0, 120) };
  }
  if (
    lower.includes("forget everything") ||
    lower.includes("clear memory") ||
    lower.includes("forget what you remember")
  ) {
    return { action: "forget_all" };
  }
  if (
    lower.includes("what do you remember") ||
    lower.includes("what do you know about me") ||
    lower.includes("recall what you remember")
  ) {
    return { action: "recall" };
  }
  return { action: "none" };
}

export function recallMemoryReply(facts: string[]): string {
  if (facts.length === 0) return "I don't remember anything yet.";
  if (facts.length === 1) return `You told me: ${facts[0]}.`;
  return `I remember ${facts.length} things, like: ${facts.slice(0, 2).join("; ")}.`;
}
