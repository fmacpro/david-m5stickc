#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "secrets.h"
#include "audio_pipeline.h"
#include "app_utils.h"
#include "cloud_client.h"
#include "face_renderer.h"
#include "ui_draw_utils.h"
#include "voice_turn.h"

enum class FaceState { Idle, Connecting, Recording, Thinking, Speaking, Happy, Error };
enum class FaceMood { Neutral, Cheerful, Curious, Concerned, Sleepy, Hungry };
enum class ReactionKind { None, Patted, Fed };

static constexpr uint32_t kSampleRate = 16000;
static constexpr size_t kRecordMsMin = 550;
static constexpr size_t kRecordMsMax = 3200;
static constexpr size_t kRecordSamplesMin = (kSampleRate * kRecordMsMin) / 1000;
static constexpr size_t kRecordSamplesMax = (kSampleRate * kRecordMsMax) / 1000;
static constexpr size_t kSttChunkMs = 1400;
static constexpr size_t kSttChunkSamples = (kSampleRate * kSttChunkMs) / 1000;
static constexpr size_t kSttChunkMinSamples = (kSampleRate * 420) / 1000;
static constexpr int kSttChunkMaxCount = 16; // ~32s max hold-to-talk capture
static constexpr size_t kRecordChunk = 512;
static constexpr size_t kMicWarmupMs = 120;
static constexpr size_t kReleaseTailMs = 300;
// Mic front-end tuning for better STT clarity on natural speech/accents.
static constexpr int kMicMagnification = 30;
static constexpr int kMicNoiseFilterLevel = 1;
// Speaker tuning to reduce hiss/static on tiny speaker hats.
static constexpr int kSpeakerVolume = 185;
static constexpr bool kVoiceDebug = false;

static FaceState g_state = FaceState::Idle;
static bool g_eyes_closed = false;
static unsigned long g_blink_toggle_at = 0;
static unsigned long g_next_blink_at = 0;
static unsigned long g_last_wifi_log_ms = 0;
static String g_status = "Booting...";
static String g_detail = "";
static String g_reply = "";
static int16_t* g_pcm = nullptr;
static uint8_t* g_stt_multipart_buf = nullptr;
static size_t g_stt_multipart_buf_cap = 0;
static size_t g_last_capture_samples = 0;
static int g_last_peak = 0;
static int g_last_avg_abs = 0;
static FaceMood g_mood = FaceMood::Neutral;
static uint8_t g_speaking_level = 0;  // 0..100
static unsigned long g_last_face_redraw_ms = 0;
static unsigned long g_last_sensor_mood_ms = 0;
static unsigned long g_last_interaction_ms = 0;
static unsigned long g_state_enter_ms = 0;
static unsigned long g_last_hunger_tick_ms = 0;
static bool g_overlay_active = false;
static unsigned long g_overlay_until_ms = 0;
static String g_overlay_mode = "none";
static String g_overlay_title = "";
static String g_overlay_value = "";
static String g_overlay_draw = "";
static int g_overlay_percent = -1;
static bool g_overlay_charging = false;
static bool g_screen_dimmed = false;
static bool g_low_power_idle = false;
static bool g_just_woke = false;
static unsigned long g_wake_guard_until_ms = 0;
static bool g_require_fresh_press = false;
static bool g_mic_started = false;
static bool g_btnb_down = false;
static unsigned long g_btnb_down_ms = 0;
static bool g_btnb_long_handled = false;
static String g_serial_cmd_buf = "";
static bool g_fs_ready = false;
static uint32_t g_voice_turn_seq = 0;
static uint32_t g_voice_turn_active = 0;
static ReactionKind g_reaction = ReactionKind::None;
static unsigned long g_reaction_until_ms = 0;
static int g_hunger = 22;  // 0..100
static int g_energy = 82;  // 0..100
static int g_boredom = 20; // 0..100
static constexpr uint8_t kBrightnessActive = 96;
static constexpr uint8_t kBrightnessIdleDim = 18;
static constexpr unsigned long kIdleDimAfterMs = 45000;
static constexpr unsigned long kLowPowerAfterMs = 120000;
static constexpr unsigned long kHungerTickMs = 60000;
static constexpr unsigned long kNeedsTickMs = 60000;
static constexpr int kHungryThreshold = 65;
static constexpr int kVeryHungryThreshold = 85;
static constexpr int kLowEnergyThreshold = 28;
static constexpr int kVeryLowEnergyThreshold = 14;
static constexpr int kBoredThreshold = 60;
static constexpr unsigned long kBtnBLongMs = 700;
static const CloudClientConfig kCloudConfig = {
    API_BASE_URL,
    DEVICE_ID,
    DEVICE_SHARED_SECRET,
};

static void drawFace();
static void setUi(FaceState state, const String& status, const String& detail);
static int clampNeed(int value);
static void startReaction(ReactionKind kind, unsigned long ttl_ms);
static bool speakActionResponse(const String& action);
static bool sendChatRequest(const String& user_text, String& reply, String& err);
static bool requestTtsWav(const String& text, uint8_t*& wav_out, size_t& wav_len, String& err);
static bool playWavBuffer(uint8_t* wav, size_t wav_len, String& err);
static bool ensureWifiConnected(String& err);
static bool speakReplyInChunks(const String& text, String& err);
static bool transcribeAudioSamples(
    size_t src_offset_samples,
    size_t sample_count,
    const String& stt_prompt,
    String& text,
    String& err,
    size_t& used_samples_out);
static bool captureTranscriptChunked(String& transcript, String& err);
static bool requestVoiceTurnText(
    const String& transcript_in,
    uint8_t*& wav_out,
    size_t& wav_len,
    String& transcript,
    String& reply,
    String& screen_action,
    String& err);
static void mergeTranscriptPart(String& transcript, const String& part);
static bool runInjectedTranscriptTurn(const String& transcript_in, String& err);
static void handleSerialDebugInput();

static void logLine(const String& msg) { Serial.println(msg); }
static void logVoice(const String& msg) {
  if (!kVoiceDebug) return;
  Serial.println(String("[VOICE DBG] ") + msg);
}

static void setScreenDimmed(bool dimmed) {
  if (g_screen_dimmed == dimmed) return;
  g_screen_dimmed = dimmed;
  M5.Display.setBrightness(dimmed ? kBrightnessIdleDim : kBrightnessActive);
  logLine(String("[LCD] brightness=") + String(dimmed ? kBrightnessIdleDim : kBrightnessActive));
}

static void startMicIfNeeded() {
  if (g_mic_started) return;
  M5.Mic.begin();
  g_mic_started = true;
}

static void stopMicIfNeeded() {
  if (!g_mic_started) return;
  while (M5.Mic.isRecording()) M5.delay(1);
  M5.Mic.end();
  g_mic_started = false;
}

static void setLowPowerIdle(bool enable) {
  if (g_low_power_idle == enable) return;
  g_low_power_idle = enable;
  if (enable) {
    // Keep only essentials alive while idle.
    stopMicIfNeeded();
    // Avoid repeated I2S driver uninstall warnings when speaker is already ended.
    if (M5.Speaker.isPlaying()) {
      M5.Speaker.stop();
    }
    WiFi.setSleep(true);   // modem sleep
    setScreenDimmed(true);
    logLine("[PWR] low_power_idle=on");
    return;
  }
  // Restore interactive subsystems on wake.
  WiFi.setSleep(false);
  startMicIfNeeded();
  setScreenDimmed(false);
  logLine("[PWR] low_power_idle=off");
}

static void logVoiceJson(const String& evt, const String& fields_json = "") {
  if (!kVoiceDebug) return;
  String line = "{\"tag\":\"voice\",\"turn\":";
  line += String(g_voice_turn_active);
  line += ",\"ms\":";
  line += String(millis());
  line += ",\"evt\":\"";
  line += jsonEscape(evt);
  line += "\"";
  if (fields_json.length() > 0) {
    line += ",\"fields\":{";
    line += fields_json;
    line += "}";
  }
  line += "}";
  Serial.println(String("[VOICE JSON] ") + line);
}

static void mergeTranscriptPart(String& transcript, const String& part_in) {
  String part = part_in;
  part.trim();
  if (part.length() == 0) return;
  if (transcript.length() == 0) {
    transcript = part;
    return;
  }

  // Avoid appending obvious repeats.
  if (transcript.endsWith(part)) return;
  if (part.length() >= 14 && transcript.indexOf(part) >= 0) return;

  int overlap = 0;
  const int max_check = (transcript.length() < part.length()) ? transcript.length() : part.length();
  const int cap = (max_check > 48) ? 48 : max_check;
  for (int n = cap; n >= 6; --n) {
    if (transcript.substring(transcript.length() - n) == part.substring(0, n)) {
      overlap = n;
      break;
    }
  }
  if (overlap > 0) {
    transcript += part.substring(overlap);
  } else {
    transcript += " ";
    transcript += part;
  }
  transcript.trim();
}

static const char* stateLabel(FaceState state) {
  switch (state) {
    case FaceState::Idle: return "Idle";
    case FaceState::Connecting: return "Connecting";
    case FaceState::Recording: return "Recording";
    case FaceState::Thinking: return "Thinking";
    case FaceState::Speaking: return "Speaking";
    case FaceState::Happy: return "Happy";
    case FaceState::Error: return "Error";
    default: return "State";
  }
}

static const char* moodLabel(FaceMood mood) {
  switch (mood) {
    case FaceMood::Neutral: return "Neutral";
    case FaceMood::Cheerful: return "Cheerful";
    case FaceMood::Curious: return "Curious";
    case FaceMood::Concerned: return "Concerned";
    case FaceMood::Sleepy: return "Sleepy";
    case FaceMood::Hungry: return "Hungry";
    default: return "Mood";
  }
}

static bool containsAny(const String& text_lower, const char* const* tokens, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (text_lower.indexOf(tokens[i]) >= 0) return true;
  }
  return false;
}

static FaceMood inferMoodFromConversation(const String& heard, const String& reply) {
  String h = heard;
  String r = reply;
  h.toLowerCase();
  r.toLowerCase();

  static const char* kCheerful[] = {"great", "awesome", "nice", "good", "hello", "hi", "glad", "happy", "thanks"};
  static const char* kConcerned[] = {"error", "failed", "can't", "cannot", "sorry", "issue", "problem", "low"};
  static const char* kCurious[] = {"maybe", "perhaps", "think", "could", "might", "question"};

  if (containsAny(h, kConcerned, sizeof(kConcerned) / sizeof(kConcerned[0])) ||
      containsAny(r, kConcerned, sizeof(kConcerned) / sizeof(kConcerned[0]))) {
    return FaceMood::Concerned;
  }
  if (containsAny(h, kCheerful, sizeof(kCheerful) / sizeof(kCheerful[0])) ||
      containsAny(r, kCheerful, sizeof(kCheerful) / sizeof(kCheerful[0]))) {
    return FaceMood::Cheerful;
  }
  if (containsAny(r, kCurious, sizeof(kCurious) / sizeof(kCurious[0]))) {
    return FaceMood::Curious;
  }
  return FaceMood::Neutral;
}

static void refreshSensorMood() {
  const unsigned long now = millis();
  if (now - g_last_sensor_mood_ms < 6000) return;
  g_last_sensor_mood_ms = now;

  if (g_energy <= kVeryLowEnergyThreshold) {
    g_mood = FaceMood::Sleepy;
    return;
  }
  if (g_hunger >= kVeryHungryThreshold) {
    g_mood = FaceMood::Hungry;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    g_mood = FaceMood::Concerned;
    return;
  }
  const int battery = M5.Power.getBatteryLevel();
  const int rssi = WiFi.RSSI();
  if (battery >= 0 && battery <= 15) {
    g_mood = FaceMood::Concerned;
    return;
  }
  if (rssi < -82) {
    g_mood = FaceMood::Curious;
    return;
  }
  if (g_energy <= kLowEnergyThreshold) {
    g_mood = FaceMood::Sleepy;
    return;
  }
  if (g_hunger >= kHungryThreshold) {
    g_mood = FaceMood::Hungry;
    return;
  }
  if (g_boredom >= kBoredThreshold) {
    g_mood = FaceMood::Curious;
    return;
  }
  if (battery > 85 && M5.Power.isCharging()) {
    g_mood = FaceMood::Cheerful;
    return;
  }
  g_mood = FaceMood::Neutral;
}

static int clampNeed(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

static void startReaction(ReactionKind kind, unsigned long ttl_ms) {
  g_reaction = kind;
  g_reaction_until_ms = millis() + ttl_ms;
}

static void updateNeeds() {
  const unsigned long now = millis();
  if (g_last_hunger_tick_ms == 0) {
    g_last_hunger_tick_ms = now;
    return;
  }
  if (now <= g_last_hunger_tick_ms) return;
  const unsigned long elapsed = now - g_last_hunger_tick_ms;
  if (elapsed < kNeedsTickMs) return;
  const unsigned long steps = elapsed / kNeedsTickMs;
  g_last_hunger_tick_ms += steps * kNeedsTickMs;

  int hunger_step = 2;
  int boredom_step = 2;
  int energy_step = 1;
  if (g_state == FaceState::Idle && !g_overlay_active) {
    boredom_step = 3;
  }
  if (g_state == FaceState::Speaking || g_state == FaceState::Recording || g_state == FaceState::Thinking) {
    energy_step = 2;
    hunger_step = 3;
  }
  if (g_low_power_idle) {
    energy_step = 0;
  }

  g_hunger = clampNeed(g_hunger + static_cast<int>(steps) * hunger_step);
  g_boredom = clampNeed(g_boredom + static_cast<int>(steps) * boredom_step);
  g_energy = clampNeed(g_energy - static_cast<int>(steps) * energy_step);
}

static void showActionDraw(const String& script, unsigned long ttl_ms = 1800) {
  g_overlay_mode = "draw";
  g_overlay_title = "";
  g_overlay_value = "";
  g_overlay_draw = script;
  g_overlay_percent = -1;
  g_overlay_charging = false;
  g_overlay_active = true;
  g_overlay_until_ms = millis() + ttl_ms;
  drawFace();
}

static void onPetAction() {
  g_boredom = clampNeed(g_boredom - 35);
  g_energy = clampNeed(g_energy + 6);
  g_hunger = clampNeed(g_hunger + 1);
  startReaction(ReactionKind::Patted, 2600);
  if (g_hunger < kHungryThreshold) {
    g_mood = FaceMood::Cheerful;
  } else {
    g_mood = FaceMood::Curious;
  }
  g_last_interaction_ms = millis();
  setUi(FaceState::Happy, "", "");
  showActionDraw("CLS;D 71 35 8;D 89 35 8;L 63 39 80 58;L 97 39 80 58;F 72 33 16 8", 1800);
  (void)speakActionResponse("pet");
}

static void onFeedAction() {
  g_hunger = clampNeed(g_hunger - 45);
  g_energy = clampNeed(g_energy + 12);
  g_boredom = clampNeed(g_boredom - 8);
  startReaction(ReactionKind::Fed, 3000);
  g_mood = FaceMood::Cheerful;
  g_last_interaction_ms = millis();
  setUi(FaceState::Happy, "", "");
  // Bowl + kibble.
  showActionDraw("CLS;R 44 46 72 16;L 44 46 54 56;L 116 46 106 56;D 66 38 3;D 80 34 3;D 94 38 3", 1800);
  (void)speakActionResponse("feed");
}

static bool speakActionResponse(const String& action) {
  String prompt;
  if (action == "feed") {
    prompt = String("ACTION: FEED. The user just fed you. ")
             + "Reply with thanks about being fed, 2 to 4 words only. "
             + "Do not mention any name. No markdown. "
             + "Need context: hunger=" + String(g_hunger) + ", energy=" + String(g_energy) + ", boredom=" + String(g_boredom) + ".";
  } else {
    prompt = String("ACTION: PET. The user just patted you. ")
             + "Reply with affection about being patted, 2 to 4 words only. "
             + "Do not mention food. Do not mention any name. No markdown. "
             + "Need context: hunger=" + String(g_hunger) + ", energy=" + String(g_energy) + ", boredom=" + String(g_boredom) + ".";
  }

  String reply;
  String err;
  if (!ensureWifiConnected(err)) {
    logLine(String("[ACTION] wifi failed: ") + err);
    setUi(FaceState::Error, "WiFi", "Reconnect failed");
    return false;
  }
  setUi(FaceState::Thinking, "", "");
  if (!sendChatRequest(prompt, reply, err)) {
    logLine(String("[ACTION] chat failed: ") + err);
    setUi(FaceState::Idle, "", "");
    return false;
  }
  if (reply.length() > 0) {
    reply.replace("*", "");
    reply.replace("`", "");
    reply.replace("_", "");
    g_reply = reply;
    logLine(String("[ACTION CHAT] ") + reply);
  }

  uint8_t* wav = nullptr;
  size_t wav_len = 0;
  if (!requestTtsWav(reply, wav, wav_len, err)) {
    logLine(String("[ACTION] tts failed: ") + err);
    setUi(FaceState::Idle, "", "");
    return false;
  }

  setUi(FaceState::Speaking, "", "");
  const bool played = playWavBuffer(wav, wav_len, err);
  free(wav);
  if (!played) {
    logLine(String("[ACTION] play failed: ") + err);
    setUi(FaceState::Idle, "", "");
    return false;
  }
  setUi(FaceState::Happy, "", "");
  return true;
}

static void clearOverlay() {
  g_overlay_active = false;
  g_overlay_until_ms = 0;
  g_overlay_mode = "none";
  g_overlay_title = "";
  g_overlay_value = "";
  g_overlay_draw = "";
  g_overlay_percent = -1;
  g_overlay_charging = false;
}

static face_renderer::FaceRenderModel buildFaceRenderModel() {
  face_renderer::FaceRenderModel model;
  model.overlay_active = g_overlay_active;
  model.now_ms = millis();
  model.overlay_until_ms = g_overlay_until_ms;
  model.overlay_mode = g_overlay_mode;
  model.overlay_title = g_overlay_title;
  model.overlay_value = g_overlay_value;
  model.overlay_draw = g_overlay_draw;
  model.overlay_percent = g_overlay_percent;
  model.overlay_charging = g_overlay_charging;
  model.state = static_cast<int>(g_state);
  model.mood = static_cast<int>(g_mood);
  model.reaction = static_cast<int>(g_reaction);
  model.reaction_until_ms = g_reaction_until_ms;
  model.eyes_closed = g_eyes_closed;
  model.speaking_level = g_speaking_level;
  return model;
}

static void drawSpeakingFeaturesFrame() {
  face_renderer::drawSpeakingFeaturesFrame(g_eyes_closed, g_speaking_level, millis());
}

static void drawFace() {
  face_renderer::drawFace(buildFaceRenderModel());
}

static void setUi(FaceState state, const String& status, const String& detail) {
  if (state != FaceState::Idle) setLowPowerIdle(false);
  g_state = state;
  g_state_enter_ms = millis();
  g_status = status;
  g_detail = detail;
  logLine(String("[UI] ") + stateLabel(state) + " mood=" + moodLabel(g_mood) + " | " + status + " | " + detail);
  drawFace();
}

static void applyScreenAction(const String& encoded_json) {
  if (encoded_json.length() == 0) return;
  const String decoded = urlDecode(encoded_json);
  JsonDocument doc;
  const auto jerr = deserializeJson(doc, decoded);
  if (jerr) {
    logLine(String("[SCREEN] json error: ") + jerr.c_str());
    return;
  }
  const String mode = doc["mode"] | "none";
  if (mode == "none") {
    clearOverlay();
    drawFace();
    return;
  }
  const String title = doc["title"] | "";
  String value = doc["value"] | "";
  String draw = doc["draw"] | "";
  if (value.length() == 0) value = doc["line1"] | "";
  int percent = doc["percent"] | -1;
  bool charging = doc["charging"] | false;

  if (mode == "big_battery") {
    if (percent < 0 || percent > 100) {
      // Try to recover from legacy value formats like "87%".
      const int pctPos = value.indexOf('%');
      if (pctPos > 0) {
        percent = value.substring(0, pctPos).toInt();
      }
    }
    if (percent < 0 || percent > 100) {
      clearOverlay();
      drawFace();
      return;
    }
    value = String(percent) + "%";
  } else if (
      mode == "big_time" || mode == "big_temp" || mode == "big_date" || mode == "big_value" || mode == "big_rssi") {
    if (value.length() == 0 && title.length() == 0 && percent < 0) {
      clearOverlay();
      drawFace();
      return;
    }
  } else if (mode == "draw") {
    if (draw.length() == 0) draw = value;
    if (draw.length() == 0) {
      clearOverlay();
      drawFace();
      return;
    }
  } else {
    clearOverlay();
    drawFace();
    return;
  }

  g_overlay_mode = mode;
  g_overlay_title = doc["title"] | "";
  g_overlay_value = value;
  g_overlay_draw = draw;
  g_overlay_percent = percent;
  g_overlay_charging = charging;
  int ttl = doc["ttl_ms"] | 7000;
  if (ttl < 1000) ttl = 1000;
  if (ttl > 15000) ttl = 15000;
  g_overlay_active = true;
  g_overlay_until_ms = millis() + static_cast<unsigned long>(ttl);
  logLine(String("[SCREEN] mode=") + g_overlay_mode + " title=" + g_overlay_title + " value=" + g_overlay_value);
  drawFace();
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  logLine(String("[WiFi] Connecting to SSID: ") + WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    setUi(FaceState::Connecting, "Connecting WiFi...", String(".") + String((millis() / 400) % 4));
    if (millis() - g_last_wifi_log_ms > 1000) {
      g_last_wifi_log_ms = millis();
      logLine(String("[WiFi] status=") + String(WiFi.status()));
    }
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logLine(String("[WiFi] Connected. IP=") + WiFi.localIP().toString());
    setUi(FaceState::Idle, "WiFi connected", WiFi.localIP().toString());
    return true;
  }

  logLine("[WiFi] Connection failed");
  setUi(FaceState::Error, "WiFi failed", "Check ssid/password");
  return false;
}

static bool ensureWifiConnected(String& err) {
  if (WiFi.status() == WL_CONNECTED) return true;
  logLine("[WiFi] reconnect requested");
  if (!connectWifi()) {
    err = "WiFi reconnect failed";
    return false;
  }
  return true;
}

static bool syncTime() { return cloudSyncTime(logLine); }

static bool readFully(
    WiFiClient& stream,
    uint8_t* out,
    size_t expected,
    unsigned long timeout_ms,
    size_t& out_read) {
  out_read = 0;
  const unsigned long start = millis();
  unsigned long last_data_at = start;
  while (out_read < expected) {
    const int avail = stream.available();
    if (avail > 0) {
      const size_t want = expected - out_read;
      const size_t chunk = (static_cast<size_t>(avail) < want) ? static_cast<size_t>(avail) : want;
      const int got = stream.readBytes(out + out_read, chunk);
      if (got > 0) {
        out_read += static_cast<size_t>(got);
        last_data_at = millis();
        continue;
      }
    }
    if (millis() - last_data_at > timeout_ms) break;
    delay(2);
    M5.update();
  }
  return out_read == expected;
}

static bool readUnknownLength(
    WiFiClient& stream,
    uint8_t* out,
    size_t cap,
    unsigned long timeout_ms,
    size_t& out_read) {
  out_read = 0;
  const unsigned long start = millis();
  unsigned long last_data_at = start;
  while (millis() - start < timeout_ms) {
    int avail = stream.available();
    while (avail > 0) {
      if (out_read >= cap) return false;
      const size_t want = cap - out_read;
      const size_t chunk = (static_cast<size_t>(avail) < want) ? static_cast<size_t>(avail) : want;
      const int got = stream.readBytes(out + out_read, chunk);
      if (got <= 0) break;
      out_read += static_cast<size_t>(got);
      last_data_at = millis();
      avail -= got;
    }
    if (!stream.connected() && stream.available() == 0) break;
    if (millis() - last_data_at > timeout_ms) break;
    delay(2);
    M5.update();
  }
  return out_read > 0;
}

static bool readFileFully(File& f, uint8_t* out, size_t expected, size_t& out_read) {
  out_read = 0;
  while (out_read < expected) {
    const size_t got = f.read(out + out_read, expected - out_read);
    if (got == 0) break;
    out_read += got;
  }
  return out_read == expected;
}

static bool writeFileFully(File& f, const uint8_t* data, size_t expected, size_t& out_written) {
  out_written = 0;
  while (out_written < expected) {
    size_t step = expected - out_written;
    if (step > 2048) step = 2048;
    const size_t put = f.write(data + out_written, step);
    if (put == 0) break;
    out_written += put;
  }
  return out_written == expected;
}

static bool ensureSttMultipartCapacity(size_t need_bytes) {
  if (g_stt_multipart_buf && g_stt_multipart_buf_cap >= need_bytes) return true;
  if (g_stt_multipart_buf) {
    free(g_stt_multipart_buf);
    g_stt_multipart_buf = nullptr;
    g_stt_multipart_buf_cap = 0;
  }
  g_stt_multipart_buf = static_cast<uint8_t*>(malloc(need_bytes));
  if (!g_stt_multipart_buf) return false;
  g_stt_multipart_buf_cap = need_bytes;
  return true;
}

static String buildSensorContextJson() {
  float ax = 0, ay = 0, az = 0;
  float gx = 0, gy = 0, gz = 0;
  float imu_temp_c = 0;
  const bool has_accel = M5.Imu.getAccel(&ax, &ay, &az);
  const bool has_gyro = M5.Imu.getGyro(&gx, &gy, &gz);
  const bool has_temp = M5.Imu.getTemp(&imu_temp_c);

  const int battery_level = M5.Power.getBatteryLevel();
  const int battery_mv = M5.Power.getBatteryVoltage();
  const int battery_ma = M5.Power.getBatteryCurrent();
  const int vbus_mv = M5.Power.getVBUSVoltage();
  const int charge_state = static_cast<int>(M5.Power.isCharging());
  const bool charging = (charge_state == 1);

  const time_t now = time(nullptr);
  struct tm local_tm = {};
  localtime_r(&now, &local_tm);
  struct tm utc_tm = {};
  gmtime_r(&now, &utc_tm);
  const long tz_offset_sec = static_cast<long>(mktime(&local_tm) - mktime(&utc_tm));

  String ctx;
  ctx.reserve(700);
  ctx += "{";
  ctx += "\"ts_unix\":";
  ctx += String(static_cast<long>(now));
  ctx += ",\"local_time_24h\":\"";
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             local_tm.tm_year + 1900,
             local_tm.tm_mon + 1,
             local_tm.tm_mday,
             local_tm.tm_hour,
             local_tm.tm_min,
             local_tm.tm_sec);
    ctx += buf;
  }
  ctx += "\"";
  ctx += ",\"utc_time_24h\":\"";
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             utc_tm.tm_year + 1900,
             utc_tm.tm_mon + 1,
             utc_tm.tm_mday,
             utc_tm.tm_hour,
             utc_tm.tm_min,
             utc_tm.tm_sec);
    ctx += buf;
  }
  ctx += "\"";
  ctx += ",\"timezone\":\"Europe/London\"";
  ctx += ",\"tz_offset_sec\":";
  ctx += String(tz_offset_sec);
  ctx += ",\"uptime_ms\":";
  ctx += String(millis());
  ctx += ",\"wifi_rssi\":";
  ctx += String(WiFi.RSSI());
  ctx += ",\"heap_free\":";
  ctx += String(ESP.getFreeHeap());
  ctx += ",\"heap_max_alloc\":";
  ctx += String(ESP.getMaxAllocHeap());
  ctx += ",\"mic_avg_abs\":";
  ctx += String(g_last_avg_abs);
  ctx += ",\"mic_peak\":";
  ctx += String(g_last_peak);
  ctx += ",\"battery_level\":";
  ctx += String(battery_level);
  ctx += ",\"battery_mv\":";
  ctx += String(battery_mv);
  ctx += ",\"battery_ma\":";
  ctx += String(battery_ma);
  ctx += ",\"vbus_mv\":";
  ctx += String(vbus_mv);
  ctx += ",\"charging\":";
  ctx += (charging ? "true" : "false");
  ctx += ",\"needs\":{\"hunger\":";
  ctx += String(g_hunger);
  ctx += ",\"energy\":";
  ctx += String(g_energy);
  ctx += ",\"boredom\":";
  ctx += String(g_boredom);
  ctx += ",\"hungry\":";
  ctx += (g_hunger >= kHungryThreshold ? "true" : "false");
  ctx += ",\"sleepy\":";
  ctx += (g_energy <= kLowEnergyThreshold ? "true" : "false");
  ctx += ",\"bored\":";
  ctx += (g_boredom >= kBoredThreshold ? "true" : "false");
  ctx += "}";
  ctx += ",\"charge_state\":";
  ctx += String(charge_state);
  ctx += ",\"buttons\":{\"a\":";
  ctx += (M5.BtnA.isPressed() ? "true" : "false");
  ctx += ",\"b\":";
  ctx += (M5.BtnB.isPressed() ? "true" : "false");
  ctx += "}";
  ctx += ",\"imu\":{\"enabled\":";
  ctx += (M5.Imu.isEnabled() ? "true" : "false");
  ctx += ",\"type\":";
  ctx += String(static_cast<int>(M5.Imu.getType()));
  ctx += ",\"has_accel\":";
  ctx += (has_accel ? "true" : "false");
  ctx += ",\"has_gyro\":";
  ctx += (has_gyro ? "true" : "false");
  ctx += ",\"has_temp\":";
  ctx += (has_temp ? "true" : "false");
  if (has_accel) {
    ctx += ",\"accel\":{\"x\":";
    ctx += String(ax, 3);
    ctx += ",\"y\":";
    ctx += String(ay, 3);
    ctx += ",\"z\":";
    ctx += String(az, 3);
    ctx += "}";
  }
  if (has_gyro) {
    ctx += ",\"gyro\":{\"x\":";
    ctx += String(gx, 3);
    ctx += ",\"y\":";
    ctx += String(gy, 3);
    ctx += ",\"z\":";
    ctx += String(gz, 3);
    ctx += "}";
  }
  if (has_temp) {
    ctx += ",\"internal_temp_c\":";
    ctx += String(imu_temp_c, 2);
  }
  ctx += "}";
  ctx += "}";
  return ctx;
}

static bool signedPost(
    const String& path,
    const char* content_type,
    uint8_t* body,
    size_t body_len,
    String& payload,
    int& code,
    String& err) {
  return cloudSignedPost(kCloudConfig, path, content_type, body, body_len, payload, code, err, logLine);
}

static bool isNoSpeechError(const String& err) {
  return err.indexOf("No speech recognized") >= 0;
}

static bool transcribeAudioSamples(
    size_t src_offset_samples,
    size_t sample_count,
    const String& stt_prompt,
    String& text,
    String& err,
    size_t& used_samples_out) {
  (void)stt_prompt;
  used_samples_out = 0;
  const size_t base = (sample_count > 0) ? sample_count : kRecordSamplesMin;
  const size_t sample_opts[] = {
      base,
      (base * 90) / 100,
      (base * 80) / 100,
      (base * 70) / 100,
      (base * 60) / 100,
      (base * 50) / 100,
      (base * 40) / 100,
      (base * 30) / 100,
      (base * 25) / 100,
      (base * 20) / 100,
      (base * 15) / 100,
  };
  uint8_t* body = nullptr;
  size_t body_len = 0;
  size_t used_samples = 0;
  size_t built_len = 0;
  for (size_t i = 0; i < (sizeof(sample_opts) / sizeof(sample_opts[0])); ++i) {
    const size_t s = sample_opts[i];
    if (s < (kSttChunkMinSamples / 3)) continue;
    const size_t wav_data_bytes = s * sizeof(int16_t);
    const size_t try_body_len = 44 + wav_data_bytes;
    if (!ensureSttMultipartCapacity(try_body_len)) continue;

    size_t idx = 0;
    writeWavHeader(g_stt_multipart_buf + idx, wav_data_bytes, kSampleRate);
    idx += 44;
    // Preserve current segment start when fallback reduces samples.
    memcpy(g_stt_multipart_buf + idx, reinterpret_cast<uint8_t*>(g_pcm + src_offset_samples), wav_data_bytes);
    idx += wav_data_bytes;
    body = g_stt_multipart_buf;
    body_len = try_body_len;
    used_samples = s;
    built_len = idx;
    break;
  }
  if (!body) {
    err = "OOM multipart";
    return false;
  }
  used_samples_out = used_samples;
  if (built_len != body_len) {
    err = "Multipart build mismatch";
    return false;
  }
  if (used_samples < base) {
    logLine(String("[STT] multipart fallback samples=") + String(used_samples) + "/" + String(base));
  }

  String payload;
  int code = 0;
  String http_err;
  const bool ok = signedPost("/v1/stt-raw", "audio/wav", body, body_len, payload, code, http_err);
  if (!ok) {
    err = http_err;
    return false;
  }
  if (code != 200) {
    err = "STT HTTP " + String(code) + " " + truncateForScreen(payload, 60);
    return false;
  }

  JsonDocument doc;
  const auto jerr = deserializeJson(doc, payload);
  if (jerr) {
    err = String("STT json ") + jerr.c_str();
    return false;
  }
  text = doc["text"] | "";
  if (text.length() == 0) return true;
  return true;
}

static bool captureTranscriptChunked(String& transcript, String& err) {
  transcript = "";
  if (!g_pcm) {
    g_pcm = static_cast<int16_t*>(malloc(kSttChunkSamples * sizeof(int16_t)));
    if (!g_pcm) {
      err = "OOM pcm buffer";
      return false;
    }
  }
  if (!M5.Mic.isEnabled()) startMicIfNeeded();
  if (!M5.Mic.isEnabled()) {
    err = "Mic not enabled";
    return false;
  }
  if (!g_fs_ready) {
    err = "FS unavailable";
    return false;
  }

  auto blockPath = [](int idx) -> String {
    char p[20];
    snprintf(p, sizeof(p), "/stt_%02d.bin", idx);
    return String(p);
  };
  // Store captured audio as 16-bit PCM for STT fidelity (better long-question accuracy).
  const size_t bytes_per_block = kSttChunkSamples * sizeof(int16_t);
  const size_t fs_total = SPIFFS.totalBytes();
  const size_t fs_used = SPIFFS.usedBytes();
  const size_t fs_free = (fs_total > fs_used) ? (fs_total - fs_used) : 0;
  int fs_chunk_limit = static_cast<int>(fs_free / bytes_per_block);
  if (fs_chunk_limit < 1) fs_chunk_limit = 1;
  if (fs_chunk_limit > kSttChunkMaxCount) fs_chunk_limit = kSttChunkMaxCount;
  logVoice(String("capture fs_total=") + String(fs_total) +
           " fs_used=" + String(fs_used) +
           " fs_free=" + String(fs_free) +
           " chunk_samples=" + String(kSttChunkSamples) +
           " chunk_limit=" + String(fs_chunk_limit));
  logVoiceJson(
      "capture_start",
      String("\"fs_total\":") + String(fs_total) +
          ",\"fs_used\":" + String(fs_used) +
          ",\"fs_free\":" + String(fs_free) +
          ",\"chunk_samples\":" + String(kSttChunkSamples) +
          ",\"chunk_limit\":" + String(fs_chunk_limit));

  // Warmup once at the beginning of hold-to-talk capture.
  {
    const size_t warmup_samples = (kSampleRate * kMicWarmupMs) / 1000;
    int16_t trash[160];
    size_t left = warmup_samples;
    while (left > 0) {
      const size_t chunk = (left > 160) ? 160 : left;
      M5.Mic.record(trash, chunk, kSampleRate);
      left -= chunk;
    }
  }

  size_t total_samples = 0;
  uint16_t block_samples_list[kSttChunkMaxCount];
  uint8_t block_file_idx[kSttChunkMaxCount];
  int block_count = 0;
  int chunk_index = 0;
  bool still_holding = true;
  while (still_holding && chunk_index < fs_chunk_limit) {
    if (!g_pcm) {
      g_pcm = static_cast<int16_t*>(malloc(kSttChunkSamples * sizeof(int16_t)));
      if (!g_pcm) {
        err = "OOM pcm buffer";
        return false;
      }
    }
    size_t offset = 0;
    uint64_t sum_abs = 0;
    int peak = 0;
    const unsigned long chunk_start = millis();

    while (offset < kSttChunkSamples) {
      const size_t chunk =
          (kSttChunkSamples - offset > kRecordChunk) ? kRecordChunk : (kSttChunkSamples - offset);
      if (M5.Mic.record(&g_pcm[offset], chunk, kSampleRate)) {
        for (size_t i = 0; i < chunk; ++i) {
          int v = g_pcm[offset + i];
          if (v < 0) v = -v;
          sum_abs += static_cast<uint32_t>(v);
          if (v > peak) peak = v;
        }
        offset += chunk;
      }
      M5.update();
      if (M5.BtnB.wasPressed()) {
        err = "Cancelled";
        return false;
      }
      if (!M5.BtnA.isPressed() && offset >= kSttChunkMinSamples) {
        still_holding = false;
        break;
      }
      if (millis() - chunk_start > 3000) break;
    }

    if (offset < kSttChunkMinSamples) {
      if (transcript.length() > 0) break;
      err = "Too short";
      return false;
    }

    int avg_after = static_cast<int>(sum_abs / offset);
    int peak_after = peak;
    processMicPcmInPlace(g_pcm, offset, avg_after, peak_after);
    g_last_peak = peak_after;
    g_last_avg_abs = avg_after;
    total_samples += offset;
    const uint16_t block_samples = static_cast<uint16_t>(offset);
    size_t wrote = 0;
    const String p = blockPath(chunk_index);
    File block_file = SPIFFS.open(p, FILE_WRITE);
    if (block_file) {
      const size_t want_bytes = static_cast<size_t>(block_samples) * sizeof(int16_t);
      writeFileFully(
          block_file,
          reinterpret_cast<const uint8_t*>(g_pcm),
          want_bytes,
          wrote);
      if (wrote != want_bytes) {
        // Retry once after reopening; helps recover from transient SPIFFS write stalls.
        block_file.close();
        delay(2);
        block_file = SPIFFS.open(p, FILE_WRITE);
        if (block_file) {
          wrote = 0;
          writeFileFully(
              block_file,
              reinterpret_cast<const uint8_t*>(g_pcm),
              want_bytes,
              wrote);
        }
      }
      block_file.close();
    } else {
      logLine(String("[STT] cache open failed: ") + p);
    }
    if (wrote >= (static_cast<size_t>(kSttChunkMinSamples / 3) * sizeof(int16_t))) {
      if (block_count < kSttChunkMaxCount) {
        block_samples_list[block_count++] = block_samples;
        block_file_idx[block_count - 1] = static_cast<uint8_t>(chunk_index);
      }
    }
    logVoice(String("capture block#") + String(chunk_index + 1) +
             " samples=" + String(block_samples) +
             " wrote_bytes=" + String(wrote) +
             " total_samples=" + String(total_samples));
    logVoiceJson(
        "capture_block",
        String("\"idx\":") + String(chunk_index + 1) +
            ",\"samples\":" + String(block_samples) +
            ",\"wrote_bytes\":" + String(wrote) +
            ",\"total_samples\":" + String(total_samples));
    if (wrote != static_cast<size_t>(block_samples) * sizeof(int16_t)) {
      logLine("[STT] cache full, stopping capture early");
      still_holding = false;
      break;
    }
    setUi(FaceState::Recording, "Listening...", String(total_samples / (kSampleRate / 10)) + " ticks");
    ++chunk_index;
  }
  g_last_capture_samples = total_samples;
  if (total_samples < kSttChunkMinSamples) {
    for (int i = 0; i < block_count; ++i) {
      const String p = blockPath(block_file_idx[i]);
      if (SPIFFS.exists(p)) SPIFFS.remove(p);
    }
    err = "Too short";
    return false;
  }
  setUi(FaceState::Thinking, "Transcribing...", "");
  logVoice(String("transcribe blocks=") + String(block_count) +
           " total_samples=" + String(total_samples));
  logVoiceJson(
      "transcribe_start",
      String("\"blocks\":") + String(block_count) +
          ",\"total_samples\":" + String(total_samples));

  for (int bi = 0; bi < block_count; ++bi) {
    uint16_t block_samples = block_samples_list[bi];
    if (block_samples == 0 || block_samples > kSttChunkSamples) continue;
    const String p = blockPath(block_file_idx[bi]);
    File in = SPIFFS.open(p, FILE_READ);
    if (!in) {
      err = "STT block open failed";
      return false;
    }
    logVoice(String("stt block#") + String(bi + 1) + "/" + String(block_count) +
             " block_samples=" + String(block_samples));
    logVoiceJson(
        "stt_block_start",
        String("\"idx\":") + String(bi + 1) +
            ",\"count\":" + String(block_count) +
            ",\"samples\":" + String(block_samples));
    if (!g_pcm) {
      g_pcm = static_cast<int16_t*>(malloc(kSttChunkSamples * sizeof(int16_t)));
      if (!g_pcm) {
        in.close();
        if (SPIFFS.exists(p)) SPIFFS.remove(p);
        err = "OOM pcm buffer";
        return false;
      }
    }
    const size_t want_bytes = static_cast<size_t>(block_samples) * sizeof(int16_t);
    size_t got_bytes = 0;
    if (!readFileFully(in, reinterpret_cast<uint8_t*>(g_pcm), want_bytes, got_bytes)) {
      const uint16_t got_samples = static_cast<uint16_t>(got_bytes / sizeof(int16_t));
      if (got_samples >= (kSttChunkMinSamples / 2)) {
        logLine(String("[STT] cache short read using partial ") + String(got_bytes) + "/" + String(want_bytes));
        block_samples = got_samples;
      } else if (transcript.length() > 0) {
        logLine(String("[STT] cache short read tail ") + String(got_bytes) + "/" + String(want_bytes));
        break;
      } else {
        in.close();
        if (SPIFFS.exists(p)) SPIFFS.remove(p);
        err = String("STT cache short read ") + String(got_bytes) + "/" + String(want_bytes);
        return false;
      }
    }
    in.close();
    if (SPIFFS.exists(p)) SPIFFS.remove(p);

    size_t consumed = 0;
    int stt_seg = 0;
    while (consumed < block_samples) {
      size_t request_samples = block_samples - consumed;
      String part;
      String stt_prompt = "Transcribe British English speech exactly, including names and numbers.";
      String stt_err;
      size_t used_samples = 0;
      bool ok_seg = false;
      while (request_samples >= (kSttChunkMinSamples / 3)) {
        if (transcribeAudioSamples(consumed, request_samples, stt_prompt, part, stt_err, used_samples)) {
          ok_seg = true;
          break;
        }
        if (stt_err.indexOf("OOM multipart") < 0) break;
        request_samples /= 2;
        if (request_samples < (kSttChunkMinSamples / 3)) break;
        logLine(String("[STT] oom retry with samples=") + String(request_samples));
      }
      if (!ok_seg) {
        if (!isNoSpeechError(stt_err)) {
          if (SPIFFS.exists(p)) SPIFFS.remove(p);
          err = stt_err;
          return false;
        }
        logVoice(String("stt block#") + String(bi + 1) + " seg#" + String(stt_seg + 1) + " no-speech");
        logVoiceJson(
            "stt_block_no_speech",
            String("\"idx\":") + String(bi + 1) +
                ",\"seg\":" + String(stt_seg + 1));
      } else {
        part.trim();
        logVoice(String("stt block#") + String(bi + 1) + " seg#" + String(stt_seg + 1) +
                 " used=" + String(used_samples) + " text=\"" + part + "\"");
        logVoiceJson(
            "stt_block_text",
            String("\"idx\":") + String(bi + 1) +
                ",\"seg\":" + String(stt_seg + 1) +
                ",\"used\":" + String(used_samples) +
                ",\"text\":\"" + jsonEscape(part) + "\"");
        if (part.length() > 0) {
          mergeTranscriptPart(transcript, part);
          logVoice(String("stt merged len=") + String(transcript.length()) +
                   " transcript=\"" + transcript + "\"");
          logVoiceJson(
              "stt_merged",
              String("\"len\":") + String(transcript.length()) +
                  ",\"text\":\"" + jsonEscape(transcript) + "\"");
        }
      }
      if (used_samples == 0) {
        // Avoid infinite loop on unexpected upstream behavior.
        break;
      }
      consumed += used_samples;
      ++stt_seg;
    }
  }
  transcript.trim();
  if (transcript.length() == 0) {
    err = "No speech recognized";
    return false;
  }
  logLine(String("[STT CHUNKED] ") + transcript);
  logVoice(String("stt final len=") + String(transcript.length()));
  logVoiceJson(
      "stt_final",
      String("\"len\":") + String(transcript.length()) +
          ",\"text\":\"" + jsonEscape(transcript) + "\"");
  return true;
}

static bool sendChatRequest(const String& user_text, String& reply, String& err) {
  const String body_str =
      String("{\"text\":\"") + jsonEscape(user_text) +
      "\",\"system\":\"You are a tiny robot. Reply in max 6 words.\"}";
  uint8_t* body = reinterpret_cast<uint8_t*>(const_cast<char*>(body_str.c_str()));

  String payload;
  int code = 0;
  String http_err;
  const bool ok = signedPost("/v1/chat", "application/json", body, body_str.length(), payload, code, http_err);
  if (!ok) {
    err = http_err;
    return false;
  }
  if (code != 200) {
    err = "CHAT HTTP " + String(code) + " " + truncateForScreen(payload, 60);
    return false;
  }

  JsonDocument doc;
  const auto jerr = deserializeJson(doc, payload);
  if (jerr) {
    err = String("CHAT json ") + jerr.c_str();
    return false;
  }
  reply = doc["reply"] | "";
  if (reply.length() == 0) {
    err = doc["error"] | "No chat reply";
    return false;
  }
  return true;
}

static bool requestTtsWav(const String& text, uint8_t*& wav_out, size_t& wav_len, String& err) {
  String safe_text = text;
  if (safe_text.length() > 96) safe_text = safe_text.substring(0, 96);
  const String body_str =
      String("{\"text\":\"") + jsonEscape(safe_text) + "\",\"format\":\"wav\"}";
  uint8_t* body = reinterpret_cast<uint8_t*>(const_cast<char*>(body_str.c_str()));

  time_t now = time(nullptr);
  if (now < 1700000000 && !syncTime()) {
    err = "NTP sync failed";
    return false;
  }
  now = time(nullptr);
  const String path = "/v1/tts";
  const String body_hash = sha256Hex(body, body_str.length());
  const String ts = String(static_cast<long>(now));
  const String nonce = nonceHex();
  const String canonical = ts + "." + nonce + ".POST." + path + "." + body_hash;
  const String signature = hmacSha256Hex(DEVICE_SHARED_SECRET, canonical);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(API_BASE_URL) + path)) {
    err = "http begin failed";
    return false;
  }
  // TTS can occasionally take longer between chunk requests; avoid spurious -11 timeouts.
  http.setConnectTimeout(8000);
  http.setTimeout(25000);
  // Prefer non-reused HTTP/1.0 connections for stability on constrained TLS clients.
  http.useHTTP10(true);
  const char* header_keys[] = {"Content-Type"};
  http.collectHeaders(header_keys, 1);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-timestamp", ts);
  http.addHeader("x-nonce", nonce);
  http.addHeader("x-signature", signature);

  const int code = http.POST(body, body_str.length());
  logLine(String("[HTTP] /v1/tts -> ") + String(code));
  if (code != 200) {
    const String payload = http.getString();
    http.end();
    err = "TTS HTTP " + String(code) + " " + truncateForScreen(payload, 60);
    return false;
  }

  const String resp_type = http.header("Content-Type");
  logLine(String("[TTS] content-type=") + resp_type);

  if (resp_type.indexOf("application/json") >= 0) {
    const String payload = http.getString();
    logLine(String("[TTS] payload=") + truncateForScreen(payload, 220));

    const String key = "\"audio_base64\":\"";
    const int key_pos = payload.indexOf(key);
    if (key_pos < 0) {
      http.end();
      err = "Missing audio_base64";
      return false;
    }
    const int start = key_pos + key.length();
    const int end = payload.indexOf('"', start);
    if (end <= start) {
      http.end();
      err = "Bad audio_base64";
      return false;
    }
    const String b64_str = payload.substring(start, end);

    const size_t b64_len = b64_str.length();
    size_t out_cap = (b64_len * 3) / 4 + 8;
    wav_out = static_cast<uint8_t*>(malloc(out_cap));
    if (!wav_out) {
      http.end();
      err = "OOM tts";
      return false;
    }

    size_t out_len = 0;
    const int rc = mbedtls_base64_decode(wav_out, out_cap, &out_len,
                                         reinterpret_cast<const uint8_t*>(b64_str.c_str()), b64_len);
    http.end();
    if (rc != 0 || out_len < 44) {
      free(wav_out);
      wav_out = nullptr;
      err = "Bad base64";
      return false;
    }

    wav_len = out_len;
    return true;
  }

  int len = http.getSize();
  // Conservative but practical cap for short spoken replies.
  const size_t kTtsCap = 120000;
  if (len > static_cast<int>(kTtsCap)) {
    http.end();
    err = "TTS audio too large";
    return false;
  }
  const size_t alloc_len = (len > 0) ? static_cast<size_t>(len) : kTtsCap;
  wav_out = static_cast<uint8_t*>(malloc(alloc_len));
  if (!wav_out) {
    http.end();
    err = "OOM binary tts";
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  if (len > 0) {
    got = stream->readBytes(wav_out, static_cast<size_t>(len));
  } else {
    size_t dyn_read = 0;
    const bool ok_dyn = readUnknownLength(*stream, wav_out, alloc_len, 12000, dyn_read);
    got = dyn_read;
    if (!ok_dyn) {
      http.end();
      free(wav_out);
      wav_out = nullptr;
      err = "TTS stream read failed";
      return false;
    }
  }
  http.end();
  wav_len = got;
  if (wav_len < 44) {
    free(wav_out);
    wav_out = nullptr;
    err = "Binary audio too short";
    return false;
  }
  if (resp_type.indexOf("audio/wav") < 0) {
    free(wav_out);
    wav_out = nullptr;
    err = "Unsupported audio type: " + resp_type;
    return false;
  }
  return true;
}

static bool playWavBufferWithActiveSpeaker(uint8_t* wav, size_t wav_len, String& err) {
  processPlaybackWavInPlace(wav, wav_len);
  M5.Speaker.setVolume(kSpeakerVolume);
  if (!M5.Speaker.playWav(wav, wav_len, 1, 0, true)) {
    err = "Speaker playWav failed";
    return false;
  }
  const unsigned long speak_start_ms = millis();
  g_speaking_level = 0;
  g_last_face_redraw_ms = 0;
  while (M5.Speaker.isPlaying()) {
    M5.update();
    const unsigned long elapsed = millis() - speak_start_ms;
    const uint8_t level_raw = estimateSpeakingLevel(wav, wav_len, elapsed);
    const int smooth = (static_cast<int>(g_speaking_level) * 7 + static_cast<int>(level_raw) * 3) / 10;
    g_speaking_level = static_cast<uint8_t>(constrain(smooth, 0, 100));
    if (millis() - g_last_face_redraw_ms > 42) {
      g_last_face_redraw_ms = millis();
      drawSpeakingFeaturesFrame();
    }
    delay(1);
  }
  g_speaking_level = 0;
  return true;
}

static bool beginSpeakerWithRetry(int attempts, String& err) {
  for (int i = 0; i < attempts; ++i) {
    if (M5.Speaker.begin()) return true;
    delay(12);
  }
  err = "Speaker begin failed";
  return false;
}

static bool playWavBuffer(uint8_t* wav, size_t wav_len, String& err) {
  stopMicIfNeeded();

  if (!beginSpeakerWithRetry(3, err)) {
    startMicIfNeeded();
    return false;
  }
  M5.Speaker.setVolume(kSpeakerVolume);
  if (!playWavBufferWithActiveSpeaker(wav, wav_len, err)) {
    M5.Speaker.end();
    startMicIfNeeded();
    return false;
  }
  M5.Speaker.end();
  startMicIfNeeded();
  return true;
}

static bool speakReplyInChunks(const String& text, String& err) {
  String remaining = text;
  remaining.trim();
  if (remaining.length() == 0) {
    err = "Empty reply";
    return false;
  }

  const int max_chunk_chars = 64;
  const int min_break_chars = 20;
  int chunk_count = 0;
  const int max_chunks = 10;
  int played_chunks = 0;

  // Fast-path for short replies: one TTS request is more reliable and lower memory churn.
  if (remaining.length() <= max_chunk_chars) {
    uint8_t* wav = nullptr;
    size_t wav_len = 0;
    String one_err;
    if (requestTtsWav(remaining, wav, wav_len, one_err)) {
      const bool ok = playWavBuffer(wav, wav_len, one_err);
      free(wav);
      if (ok) return true;
      err = one_err;
      return false;
    }
    // Fall through to chunking if single-shot synthesis fails.
  }

  stopMicIfNeeded();

  setUi(FaceState::Speaking, "", "");
  while (remaining.length() > 0) {
    if (++chunk_count > max_chunks) break;
    int cut = remaining.length();
    if (cut > max_chunk_chars) {
      cut = max_chunk_chars;
      int best = -1;
      for (int i = cut; i >= min_break_chars; --i) {
        const char c = remaining[i - 1];
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':') {
          best = i;
          break;
        }
      }
      if (best < 0) {
        for (int i = cut; i >= min_break_chars; --i) {
          if (remaining[i - 1] == ' ') {
            best = i;
            break;
          }
        }
      }
      if (best > 0) cut = best;
    }

    String chunk = remaining.substring(0, cut);
    chunk.trim();
    remaining.remove(0, cut);
    remaining.trim();
    if (chunk.length() == 0) continue;

    bool chunk_ok = false;
    String chunk_err;
    String active_chunk = chunk;
    int shrink_tries = 0;
    while (!chunk_ok && shrink_tries < 8) {
      for (int attempt = 0; attempt < 2 && !chunk_ok; ++attempt) {
        uint8_t* wav = nullptr;
        size_t wav_len = 0;
        if (!requestTtsWav(active_chunk, wav, wav_len, chunk_err)) {
          delay(15);
          continue;
        }
        if (!beginSpeakerWithRetry(3, chunk_err)) {
          free(wav);
          delay(15);
          continue;
        }
        M5.Speaker.setVolume(kSpeakerVolume);
        chunk_ok = playWavBufferWithActiveSpeaker(wav, wav_len, chunk_err);
        M5.Speaker.setVolume(0);
        M5.Speaker.end();
        free(wav);
        if (!chunk_ok) delay(15);
      }
      if (chunk_ok) break;
      const bool retry_split =
          chunk_err.indexOf("OOM binary tts") >= 0 ||
          chunk_err.indexOf("OOM tts") >= 0 ||
          chunk_err.indexOf("TTS audio too large") >= 0;
      if (!retry_split) break;
      if (active_chunk.length() <= 20) break;
      // Split this chunk and defer the tail; keeps conversation flowing under low heap.
      const int split = active_chunk.length() / 2;
      String head = active_chunk.substring(0, split);
      String tail = active_chunk.substring(split);
      head.trim();
      tail.trim();
      if (tail.length() > 0) {
        if (remaining.length() > 0) remaining = tail + " " + remaining;
        else remaining = tail;
      }
      active_chunk = head;
      ++shrink_tries;
      delay(10);
    }
    if (!chunk_ok) {
      const bool low_mem_tts =
          chunk_err.indexOf("OOM binary tts") >= 0 ||
          chunk_err.indexOf("OOM tts") >= 0 ||
          chunk_err.indexOf("TTS audio too large") >= 0;
      if (low_mem_tts && played_chunks == 0) {
        // Last-resort short utterance to avoid hard-failing the turn on low heap.
        uint8_t* wav = nullptr;
        size_t wav_len = 0;
        String tiny_err;
        if (requestTtsWav("Okay.", wav, wav_len, tiny_err) &&
            beginSpeakerWithRetry(2, tiny_err) &&
            playWavBufferWithActiveSpeaker(wav, wav_len, tiny_err)) {
          M5.Speaker.setVolume(0);
          M5.Speaker.end();
          free(wav);
          startMicIfNeeded();
          return true;
        }
        if (wav) free(wav);
      }
      if (low_mem_tts) {
        logLine(String("[TTS CHUNK] low-memory graceful stop: ") + chunk_err);
        startMicIfNeeded();
        return true;
      }
      // If at least one chunk played, finish gracefully instead of surfacing a hard error.
      if (played_chunks > 0) {
        logLine(String("[TTS CHUNK] partial stop: ") + chunk_err);
        startMicIfNeeded();
        return true;
      }
      err = chunk_err.length() ? chunk_err : "Chunk TTS failed";
      startMicIfNeeded();
      return false;
    }
    ++played_chunks;
  }
  startMicIfNeeded();
  return true;
}

static bool requestVoiceTurnText(
    const String& transcript_in,
    uint8_t*& wav_out,
    size_t& wav_len,
    String& transcript,
    String& reply,
    String& screen_action,
    String& err) {
  const String sensor_context = buildSensorContextJson();
  const String body_str =
      String("{\"transcript\":\"") + jsonEscape(transcript_in) +
      "\",\"context\":\"" + jsonEscape(sensor_context) + "\"}";
  uint8_t* body = reinterpret_cast<uint8_t*>(const_cast<char*>(body_str.c_str()));

  time_t now = time(nullptr);
  if (now < 1700000000 && !syncTime()) {
    err = "NTP sync failed";
    return false;
  }
  now = time(nullptr);

  const String path = "/v1/voice-turn-text";
  const String body_hash = sha256Hex(body, body_str.length());
  const String ts = String(static_cast<long>(now));
  const String nonce = nonceHex();
  const String canonical = ts + "." + nonce + ".POST." + path + "." + body_hash;
  const String signature = hmacSha256Hex(DEVICE_SHARED_SECRET, canonical);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(API_BASE_URL) + path)) {
    err = "http begin failed";
    return false;
  }
  const char* header_keys[] = {"Content-Type", "x-transcript", "x-reply", "x-screen"};
  http.collectHeaders(header_keys, 4);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-timestamp", ts);
  http.addHeader("x-nonce", nonce);
  http.addHeader("x-signature", signature);
  http.addHeader("x-tts-chunked", "1");

  const int code = http.POST(body, body_str.length());
  logLine(String("[HTTP] /v1/voice-turn-text -> ") + String(code));
  if (code != 200 && code != 204) {
    const String payload = http.getString();
    logLine(String("[TURN TEXT ERROR] ") + payload);
    http.end();
    err = "TURN_TEXT HTTP " + String(code) + " " + truncateForScreen(payload, 60);
    return false;
  }

  transcript = urlDecode(http.header("x-transcript"));
  reply = urlDecode(http.header("x-reply"));
  screen_action = http.header("x-screen");

  if (code == 204) {
    http.end();
    wav_out = nullptr;
    wav_len = 0;
    return true;
  }

  const String resp_type = http.header("Content-Type");
  if (resp_type.indexOf("audio/wav") < 0) {
    const String payload = http.getString();
    http.end();
    err = "Unexpected type: " + resp_type + " " + truncateForScreen(payload, 40);
    return false;
  }

  const int len = http.getSize();
  if (len <= 0 || len > 140000) {
    http.end();
    err = "Turn audio too large";
    return false;
  }

  wav_out = static_cast<uint8_t*>(malloc(len));
  if (!wav_out) {
    http.end();
    err = String("OOM turn audio free=") + String(ESP.getFreeHeap()) + " max=" + String(ESP.getMaxAllocHeap());
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  const bool full = readFully(*stream, wav_out, static_cast<size_t>(len), 4000, got);
  http.end();
  wav_len = got;
  if (!full || wav_len < 44) {
    free(wav_out);
    wav_out = nullptr;
    err = String("Turn audio incomplete: ") + String(got) + "/" + String(len);
    return false;
  }
  return true;
}

static void setUiRaw(int state, const String& status, const String& detail) {
  setUi(static_cast<FaceState>(state), status, detail);
}

static int inferMoodFromConversationRaw(const String& heard, const String& reply) {
  return static_cast<int>(inferMoodFromConversation(heard, reply));
}

static int clampNeedRaw(int value) {
  return clampNeed(value);
}

static unsigned long nowMs() {
  return millis();
}

static VoiceTurnContext makeVoiceTurnContext() {
  VoiceTurnContext ctx;
  ctx.voice_turn_seq = &g_voice_turn_seq;
  ctx.voice_turn_active = &g_voice_turn_active;
  ctx.global_reply = &g_reply;
  ctx.boredom = &g_boredom;
  ctx.energy = &g_energy;
  ctx.hunger = &g_hunger;
  ctx.last_interaction_ms = &g_last_interaction_ms;
  ctx.ui_recording_state = static_cast<int>(FaceState::Recording);
  ctx.ui_thinking_state = static_cast<int>(FaceState::Thinking);
  ctx.ui_speaking_state = static_cast<int>(FaceState::Speaking);
  ctx.captureTranscriptChunked = captureTranscriptChunked;
  ctx.ensureWifiConnected = ensureWifiConnected;
  ctx.requestVoiceTurnText = requestVoiceTurnText;
  ctx.isNoSpeechError = isNoSpeechError;
  ctx.playWavBuffer = playWavBuffer;
  ctx.speakReplyInChunks = speakReplyInChunks;
  ctx.setUiRaw = setUiRaw;
  ctx.applyScreenAction = applyScreenAction;
  ctx.inferMoodFromConversationRaw = inferMoodFromConversationRaw;
  ctx.clampNeedRaw = clampNeedRaw;
  ctx.logLine = logLine;
  ctx.nowMs = nowMs;
  return ctx;
}

static bool runVoiceTurn(String& err) {
  VoiceTurnContext ctx = makeVoiceTurnContext();
  int mood_value = static_cast<int>(g_mood);
  ctx.mood = &mood_value;
  const bool ok = runVoiceTurnFlow(ctx, err);
  g_mood = static_cast<FaceMood>(mood_value);
  return ok;
}

static bool runInjectedTranscriptTurn(const String& transcript_in, String& err) {
  VoiceTurnContext ctx = makeVoiceTurnContext();
  int mood_value = static_cast<int>(g_mood);
  ctx.mood = &mood_value;
  const bool ok = runInjectedTranscriptTurnFlow(ctx, transcript_in, err);
  g_mood = static_cast<FaceMood>(mood_value);
  return ok;
}

static void handleSerialDebugInput() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (g_serial_cmd_buf.length() < 512) g_serial_cmd_buf += c;
      continue;
    }
    String line = g_serial_cmd_buf;
    g_serial_cmd_buf = "";
    line.trim();
    if (line.length() == 0) continue;

    if (line == "PING") {
      logLine("[SERIAL CMD] PONG");
      continue;
    }
    if (line == "HELP") {
      logLine("[SERIAL CMD] use: ASK <question>");
      continue;
    }
    if (line.startsWith("ASK ")) {
      const String injected = line.substring(4);
      setLowPowerIdle(false);
      setScreenDimmed(false);
      g_last_interaction_ms = millis();
      String err;
      if (runInjectedTranscriptTurn(injected, err)) {
        setUi(FaceState::Happy, "", "");
      } else {
        g_mood = FaceMood::Concerned;
        setUi(FaceState::Error, "Inject failed", truncateForScreen(err, 24));
      }
      continue;
    }
    logLine(String("[SERIAL CMD] unknown: ") + line);
  }
}

static void updateBlink() {
  if (g_state == FaceState::Speaking || g_low_power_idle) return;
  const unsigned long now = millis();
  if (!g_eyes_closed && now >= g_next_blink_at) {
    g_eyes_closed = true;
    g_blink_toggle_at = now + 120;
    drawFace();
  } else if (g_eyes_closed && now >= g_blink_toggle_at) {
    g_eyes_closed = false;
    g_next_blink_at = now + static_cast<unsigned long>(random(2200, 6200));
    drawFace();
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.external_speaker_value = 0;
  cfg.external_speaker.hat_spk = true;  // speaker HAT on StickC
  cfg.external_speaker.hat_spk2 = false;
  cfg.external_speaker.atomic_spk = false;
  cfg.external_speaker.atomic_echo = false;
  M5.begin(cfg);

  Serial.begin(115200);
  delay(50);
  logLine("[Boot] M5Stick starting");

  M5.Speaker.setVolume(0);
  M5.Speaker.stop();
  M5.Speaker.end();
  pinMode(0, OUTPUT);
  digitalWrite(0, LOW);
  pinMode(26, OUTPUT);
  digitalWrite(26, LOW);

  M5.Display.setRotation(3);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(kBrightnessActive);
  randomSeed(esp_random());
  g_last_interaction_ms = millis();
  g_fs_ready = SPIFFS.begin(true);
  logLine(String("[FS] SPIFFS ") + (g_fs_ready ? "ready" : "failed"));

  // Keep persistent PCM buffer sized for chunked STT path to preserve TLS headroom.
  g_pcm = static_cast<int16_t*>(malloc(kSttChunkSamples * sizeof(int16_t)));
  if (!g_pcm) {
    setUi(FaceState::Error, "OOM", "pcm buffer");
    return;
  }

  g_next_blink_at = millis() + 1200;
  g_last_hunger_tick_ms = millis();
  setUi(FaceState::Connecting, "Booting...", "Init");

  if (!connectWifi()) return;
  if (!syncTime()) {
    setUi(FaceState::Error, "NTP failed", "Clock not set");
    return;
  }

  auto mic_cfg = M5.Mic.config();
  // Favor intelligibility over aggressive denoise; helps longer natural questions.
  mic_cfg.magnification = kMicMagnification;
  mic_cfg.noise_filter_level = kMicNoiseFilterLevel;
  M5.Mic.config(mic_cfg);
  startMicIfNeeded();
  setUi(FaceState::Idle, "Ready", "BtnA: talk");
}

void loop() {
  M5.update();
  handleSerialDebugInput();
  const unsigned long now = millis();
  const bool btnA_pressed = M5.BtnA.wasPressed();
  const bool btnB_pressed = M5.BtnB.wasPressed();
  const bool any_btn_down = M5.BtnA.isPressed() || M5.BtnB.isPressed();
  if (g_overlay_active && millis() >= g_overlay_until_ms) {
    clearOverlay();
    drawFace();
  }
  if (g_reaction != ReactionKind::None && millis() >= g_reaction_until_ms) {
    g_reaction = ReactionKind::None;
    drawFace();
  }
  updateNeeds();
  refreshSensorMood();
  if (g_state == FaceState::Idle &&
      millis() - g_last_interaction_ms > 120000 &&
      g_energy <= kLowEnergyThreshold) {
    if (g_mood != FaceMood::Sleepy) {
      g_mood = FaceMood::Sleepy;
      drawFace();
    }
  }
  updateBlink();

  // Return to idle shortly after success/error expression so idle-dim can engage.
  if ((g_state == FaceState::Happy || g_state == FaceState::Error) &&
      (now - g_state_enter_ms > 1800) &&
      !g_overlay_active) {
    setUi(FaceState::Idle, "", "");
  }

  const bool activeVisual =
      (g_state != FaceState::Idle) ||
      g_overlay_active ||
      (now - g_last_interaction_ms < kIdleDimAfterMs);
  setScreenDimmed(!activeVisual);

  const bool can_low_power =
      (g_state == FaceState::Idle) &&
      !g_overlay_active &&
      (now - g_last_interaction_ms >= kLowPowerAfterMs);
  const bool was_low_power = g_low_power_idle;
  setLowPowerIdle(can_low_power);

  // First press after low-power only wakes the device.
  if (was_low_power && (btnA_pressed || btnB_pressed)) {
    setLowPowerIdle(false);
    g_just_woke = true;
    g_wake_guard_until_ms = now + 1200;
    g_require_fresh_press = true;
    g_last_interaction_ms = now;
    setUi(FaceState::Idle, "", "");
    return;
  }

  // If screen is only dimmed (not low-power), first press should just wake/undim.
  if (!was_low_power && g_screen_dimmed && (btnA_pressed || btnB_pressed)) {
    setScreenDimmed(false);
    g_just_woke = true;
    g_wake_guard_until_ms = now + 900;
    g_require_fresh_press = true;
    g_last_interaction_ms = now;
    setUi(FaceState::Idle, "", "");
    return;
  }

  if (g_just_woke && now >= g_wake_guard_until_ms) {
    g_just_woke = false;
  }

  // Require button release after wake so long-press/hold doesn't trigger immediate recording.
  if (g_require_fresh_press) {
    if (!any_btn_down) {
      g_require_fresh_press = false;
    }
    return;
  }

  if (btnA_pressed) {
    if (g_just_woke || now < g_wake_guard_until_ms) return;
    setLowPowerIdle(false);
    g_last_interaction_ms = millis();
    setScreenDimmed(false);
    if (g_mood == FaceMood::Sleepy) g_mood = FaceMood::Curious;
    String err;
    if (runVoiceTurn(err)) {
      setUi(FaceState::Happy, "", "");
    } else {
      g_mood = FaceMood::Concerned;
      setUi(FaceState::Error, "Voice turn failed", truncateForScreen(err, 24));
      startMicIfNeeded();
    }
  }

  if (btnB_pressed) {
    if (g_just_woke || now < g_wake_guard_until_ms) return;
    // Actual Button B action is handled on release or long-hold below.
  }

  const bool b_down = M5.BtnB.isPressed();
  if (b_down && !g_btnb_down) {
    g_btnb_down = true;
    g_btnb_down_ms = now;
    g_btnb_long_handled = false;
  }
  if (!b_down && g_btnb_down) {
    g_btnb_down = false;
    if (!g_btnb_long_handled && !(g_just_woke || now < g_wake_guard_until_ms)) {
      setLowPowerIdle(false);
      setScreenDimmed(false);
      onPetAction();
    }
  }
  if (b_down && g_btnb_down && !g_btnb_long_handled && (now - g_btnb_down_ms >= kBtnBLongMs)) {
    if (!(g_just_woke || now < g_wake_guard_until_ms)) {
      setLowPowerIdle(false);
      setScreenDimmed(false);
      onFeedAction();
    }
    g_btnb_long_handled = true;
  }

  delay(10);
}
