export interface Env {
  OPENAI_API_KEY: string;
  DEVICE_SHARED_SECRET: string;
  DEBUG_ERRORS?: string;
  CHAT_MODEL?: string;
  TRANSCRIBE_MODEL?: string;
  TRANSCRIBE_LANGUAGE?: string;
  TRANSCRIBE_PROMPT?: string;
  TTS_MODEL?: string;
  TTS_VOICE?: string;
  TTS_INSTRUCTIONS?: string;
  RATE_LIMIT_PER_MIN?: string;
  MAX_BODY_BYTES?: string;
  MAX_TEXT_CHARS?: string;
  MAX_REPLY_CHARS?: string;
  REPLY_MAX_TOKENS?: string;
  DEVICE_STATE: KVNamespace;
}

export type Turn = { user: string; assistant: string };

export type MemoryCommand =
  | { action: "none" }
  | { action: "remember"; fact: string }
  | { action: "recall" }
  | { action: "forget_all" };

export type ScreenAction = {
  mode:
    | "none"
    | "big_time"
    | "big_battery"
    | "big_rssi"
    | "big_temp"
    | "big_date"
    | "big_value"
    | "draw"
    | "image";
  title?: string;
  value?: string;
  draw?: string;
  percent?: number;
  charging?: boolean;
  ttl_ms?: number;
};
