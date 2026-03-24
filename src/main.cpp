#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
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

#if defined(ARDUINO_ARCH_ESP32)
// TLS handshakes plus multipart upload path can exceed default loopTask stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

enum class FaceState { Idle, Connecting, Recording, Thinking, Speaking, Happy, Error };
enum class FaceMood { Neutral, Cheerful, Curious, Concerned, Sleepy, Hungry };
enum class ReactionKind { None, Patted, Fed };

static constexpr uint32_t kSampleRate = 16000;
static constexpr size_t kSttChunkMinSamples = (kSampleRate * 420) / 1000;
static constexpr size_t kMinTurnCaptureMs = 1400;
static constexpr size_t kMinTurnCaptureSamples = (kSampleRate * kMinTurnCaptureMs) / 1000;
static constexpr size_t kRecordChunk = 512;
static constexpr size_t kMicWarmupMs = 5;
static constexpr size_t kPreRollMs = 150;
static constexpr size_t kPreRollSamples = (kSampleRate * kPreRollMs) / 1000;
static constexpr size_t kReleaseTailMs = 50;
static constexpr size_t kTurnCaptureMaxMs = 58000;
static constexpr size_t kCaptureFileReserveBytes = 12288;
static constexpr size_t kTurnUploadSoftMaxBytes = 1850000;
// Mic front-end tuning for better STT clarity on natural speech/accents.
static constexpr int kMicMagnification = 24;
static constexpr int kMicNoiseFilterLevel = 2;
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
static uint8_t g_mic_magnification = kMicMagnification;
static uint8_t g_mic_noise_filter_level = kMicNoiseFilterLevel;
static int16_t g_preroll_buf[kPreRollSamples];
static constexpr const char* kOverlayImagePath = "/overlay_img.jpg";
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
static bool requestVoiceTurnText(
    const String& transcript_in,
    uint8_t*& wav_out,
    size_t& wav_len,
    String& transcript,
    String& reply,
    String& screen_action,
    String& err);
static bool requestPictureJpeg(
    const String& query,
    uint8_t*& jpg_out,
    size_t& jpg_len,
    String& image_title,
    String& image_source,
    String& err);
static bool requestVoiceTurnAudio(
    uint8_t*& wav_out,
    size_t& wav_len,
    String& transcript,
    String& reply,
    String& screen_action,
    String& err);
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

static void applyMicConfig(uint8_t magnification, uint8_t noise_filter_level) {
  const uint8_t mag = (magnification < 1) ? 1 : ((magnification > 64) ? 64 : magnification);
  const uint8_t nf = (noise_filter_level > 4) ? 4 : noise_filter_level;
  g_mic_magnification = mag;
  g_mic_noise_filter_level = nf;
  stopMicIfNeeded();
  auto mic_cfg = M5.Mic.config();
  mic_cfg.magnification = g_mic_magnification;
  mic_cfg.noise_filter_level = g_mic_noise_filter_level;
  M5.Mic.config(mic_cfg);
  startMicIfNeeded();
  logLine(String("[MIC] magnification=") + String(g_mic_magnification) +
          " noise_filter=" + String(g_mic_noise_filter_level));
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
  } else if (mode == "image") {
    if (!g_fs_ready) {
      clearOverlay();
      drawFace();
      return;
    }
    String query = value;
    query.trim();
    if (query.length() == 0) {
      clearOverlay();
      drawFace();
      return;
    }
    uint8_t* jpg = nullptr;
    size_t jpg_len = 0;
    String img_title;
    String img_source;
    String img_err;
    if (!requestPictureJpeg(query, jpg, jpg_len, img_title, img_source, img_err)) {
      logLine(String("[SCREEN] image fetch failed: ") + img_err);
      clearOverlay();
      drawFace();
      return;
    }
    if (SPIFFS.exists(kOverlayImagePath)) SPIFFS.remove(kOverlayImagePath);
    File f = SPIFFS.open(kOverlayImagePath, FILE_WRITE);
    if (!f) {
      if (jpg) free(jpg);
      logLine("[SCREEN] image file open failed");
      clearOverlay();
      drawFace();
      return;
    }
    const size_t wrote = f.write(jpg, jpg_len);
    f.close();
    if (jpg) free(jpg);
    if (wrote != jpg_len) {
      if (SPIFFS.exists(kOverlayImagePath)) SPIFFS.remove(kOverlayImagePath);
      logLine("[SCREEN] image file write failed");
      clearOverlay();
      drawFace();
      return;
    }
    bool drawn_now = false;
    File imgf = SPIFFS.open(kOverlayImagePath, FILE_READ);
    if (!imgf) {
      logLine("[SCREEN] image file reopen failed");
      clearOverlay();
      drawFace();
      return;
    }
    M5.Display.fillScreen(BLACK);
    drawn_now = M5.Display.drawJpg(&imgf, 0, 0, 160, 80, 0, 0, 0.0f, 0.0f, middle_center);
    if (!drawn_now) {
      imgf.seek(0, SeekSet);
      drawn_now = M5.Display.drawPng(&imgf, 0, 0, 160, 80, 0, 0, 0.0f, 0.0f, middle_center);
    }
    if (!drawn_now) {
      imgf.seek(0, SeekSet);
      drawn_now = M5.Display.drawBmp(&imgf, 0, 0, 160, 80, 0, 0, 0.0f, 0.0f, middle_center);
    }
    imgf.close();
    if (!drawn_now) {
      logLine("[SCREEN] image decode failed");
      clearOverlay();
      drawFace();
      return;
    }
    value = kOverlayImagePath;
    if (img_title.length() > 0) {
      logLine(String("[SCREEN] image title=") + truncateForScreen(img_title, 72));
    }
    if (img_source.length() > 0) {
      logLine(String("[SCREEN] image src=") + truncateForScreen(img_source, 96));
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

static const char* wifiStatusLabel(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "WL_UNKNOWN";
  }
}

static bool waitForWifiConnection(unsigned long timeout_ms) {
  const unsigned long start = millis();
  wl_status_t last_status = WL_IDLE_STATUS;
  while (millis() - start < timeout_ms) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) return true;
    setUi(FaceState::Connecting, "Connecting WiFi...", String(".") + String((millis() / 400) % 4));
    if (status != last_status || (millis() - g_last_wifi_log_ms > 1000)) {
      g_last_wifi_log_ms = millis();
      logLine(
          String("[WiFi] status=") + String(static_cast<int>(status)) +
          " (" + wifiStatusLabel(status) + ")");
      last_status = status;
    }
    delay(200);
  }
  return false;
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  // Force a clean station state so reconnecting after SSID/password changes
  // does not get stuck on stale AP metadata.
  WiFi.disconnect(true, true);
  delay(250);
  WiFi.mode(WIFI_STA);

  logLine(String("[WiFi] Connecting to SSID: ") + WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (!waitForWifiConnection(20000)) {
    logLine(String("[WiFi] first attempt failed, retrying clean begin; status=") +
            wifiStatusLabel(WiFi.status()));
    WiFi.disconnect(true, true);
    delay(350);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (waitForWifiConnection(20000)) {
    logLine(String("[WiFi] Connected. IP=") + WiFi.localIP().toString());
    logLine(String("[WiFi] RSSI=") + String(WiFi.RSSI()) +
            " dBm channel=" + String(WiFi.channel()));
    setUi(FaceState::Idle, "WiFi connected", WiFi.localIP().toString());
    return true;
  }

  logLine(String("[WiFi] Connection failed. final_status=") +
          String(static_cast<int>(WiFi.status())) + " (" + wifiStatusLabel(WiFi.status()) + ")");
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

static bool syncTime() { return cloudSyncTime(logLine, API_BASE_URL); }

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

class MultipartUploadStream : public Stream {
 public:
  MultipartUploadStream(const uint8_t* prefix, size_t prefix_len, File* file, const uint8_t* suffix, size_t suffix_len)
      : prefix_(prefix), prefix_len_(prefix_len), file_(file), suffix_(suffix), suffix_len_(suffix_len) {}

  int available() override {
    const size_t rem = remaining();
    return rem > static_cast<size_t>(0x7fffffff) ? 0x7fffffff : static_cast<int>(rem);
  }

  int read() override {
    uint8_t c = 0;
    const size_t n = readBytes(&c, 1);
    if (n == 1) return static_cast<int>(c);
    return -1;
  }

  int peek() override { return -1; }

  void flush() override {}

  size_t write(uint8_t) override { return 0; }

  size_t write(const uint8_t*, size_t) override { return 0; }

  size_t readBytes(uint8_t* buffer, size_t length) override {
    if (!buffer || length == 0) return 0;
    size_t out = 0;
    while (out < length) {
      if (prefix_pos_ < prefix_len_) {
        const size_t n = min(length - out, prefix_len_ - prefix_pos_);
        memcpy(buffer + out, prefix_ + prefix_pos_, n);
        prefix_pos_ += n;
        out += n;
        continue;
      }
      if (file_ && file_pos_ < file_len_) {
        if (file_->position() != static_cast<uint32_t>(file_pos_)) {
          file_->seek(file_pos_, SeekSet);
        }
        const size_t n = min(length - out, file_len_ - file_pos_);
        const size_t got = file_->read(buffer + out, n);
        if (got == 0) break;
        file_pos_ += got;
        out += got;
        continue;
      }
      if (suffix_pos_ < suffix_len_) {
        const size_t n = min(length - out, suffix_len_ - suffix_pos_);
        memcpy(buffer + out, suffix_ + suffix_pos_, n);
        suffix_pos_ += n;
        out += n;
        continue;
      }
      break;
    }
    return out;
  }

  void begin() {
    prefix_pos_ = 0;
    suffix_pos_ = 0;
    file_pos_ = 0;
    file_len_ = file_ ? file_->size() : 0;
    if (file_) file_->seek(0, SeekSet);
  }

  size_t totalLength() const { return prefix_len_ + file_len_ + suffix_len_; }

 private:
  size_t remaining() const {
    return (prefix_len_ - prefix_pos_) + (file_len_ - file_pos_) + (suffix_len_ - suffix_pos_);
  }

  const uint8_t* prefix_ = nullptr;
  size_t prefix_len_ = 0;
  size_t prefix_pos_ = 0;
  File* file_ = nullptr;
  size_t file_len_ = 0;
  size_t file_pos_ = 0;
  const uint8_t* suffix_ = nullptr;
  size_t suffix_len_ = 0;
  size_t suffix_pos_ = 0;
};

static bool rewriteWavHeader(File& wav_file, size_t wav_data_bytes) {
  uint8_t hdr[44];
  writeWavHeader(hdr, wav_data_bytes, kSampleRate);
  if (!wav_file.seek(0, SeekSet)) return false;
  return wav_file.write(hdr, sizeof(hdr)) == sizeof(hdr);
}

static bool captureTurnWavToFile(const String& wav_path, size_t& total_samples, String& err) {
  total_samples = 0;
  g_last_peak = 0;
  g_last_avg_abs = 0;

  if (!M5.Mic.isEnabled()) startMicIfNeeded();
  if (!M5.Mic.isEnabled()) {
    err = "Mic not enabled";
    return false;
  }
  if (!g_fs_ready) {
    err = "FS unavailable";
    return false;
  }

  const size_t fs_total = SPIFFS.totalBytes();
  const size_t fs_used = SPIFFS.usedBytes();
  const size_t fs_free = (fs_total > fs_used) ? (fs_total - fs_used) : 0;
  size_t free_for_audio = (fs_free > (44 + kCaptureFileReserveBytes)) ? (fs_free - 44 - kCaptureFileReserveBytes) : 0;
  size_t max_samples_by_fs = free_for_audio / sizeof(int16_t);
  const size_t max_samples_by_time = (kSampleRate * kTurnCaptureMaxMs) / 1000;
  // Keep multipart upload under worker request-body limits with room for form fields/boundaries.
  const size_t max_samples_by_upload = ((kTurnUploadSoftMaxBytes > 4096) ? (kTurnUploadSoftMaxBytes - 4096) : 0) / sizeof(int16_t);
  size_t max_samples = min(max_samples_by_fs, min(max_samples_by_time, max_samples_by_upload));
  if (max_samples < kMinTurnCaptureSamples) max_samples = kMinTurnCaptureSamples;

  logVoice(String("capture_full fs_total=") + String(fs_total) +
           " fs_used=" + String(fs_used) +
           " fs_free=" + String(fs_free) +
           " max_samples=" + String(max_samples));
  logVoiceJson(
      "capture_full_start",
      String("\"fs_total\":") + String(fs_total) +
          ",\"fs_used\":" + String(fs_used) +
          ",\"fs_free\":" + String(fs_free) +
          ",\"max_samples\":" + String(max_samples) +
          ",\"upload_soft_max\":" + String(kTurnUploadSoftMaxBytes));

  if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
  File wav_file = SPIFFS.open(wav_path, FILE_WRITE);
  if (!wav_file) {
    err = "WAV open failed";
    return false;
  }
  uint8_t empty_hdr[44] = {0};
  if (wav_file.write(empty_hdr, sizeof(empty_hdr)) != sizeof(empty_hdr)) {
    wav_file.close();
    if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
    err = "WAV header write failed";
    return false;
  }

  uint64_t sum_abs = 0;
  int peak = 0;
  {
    // Capture a short pre-roll and prepend it so first words are less likely
    // to be clipped when users begin speaking right on button press.
    const size_t warmup_samples = (kSampleRate * kMicWarmupMs) / 1000;
    int16_t trash[160];
    size_t warm_left = warmup_samples;
    while (warm_left > 0) {
      const size_t chunk = (warm_left > 160) ? 160 : warm_left;
      M5.Mic.record(trash, chunk, kSampleRate);
      warm_left -= chunk;
    }

    size_t got_preroll = 0;
    while (got_preroll < kPreRollSamples) {
      size_t chunk = kPreRollSamples - got_preroll;
      if (chunk > kRecordChunk) chunk = kRecordChunk;
      if (M5.Mic.record(g_preroll_buf + got_preroll, chunk, kSampleRate)) {
        got_preroll += chunk;
      }
      M5.update();
      if (M5.BtnB.wasPressed()) {
        wav_file.close();
        if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
        err = "Cancelled";
        return false;
      }
    }
    if (got_preroll > 0) {
      const size_t wrote = wav_file.write(
          reinterpret_cast<const uint8_t*>(g_preroll_buf),
          got_preroll * sizeof(int16_t));
      if (wrote != got_preroll * sizeof(int16_t)) {
        wav_file.close();
        if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
        err = "WAV pre-roll write failed";
        return false;
      }
      for (size_t i = 0; i < got_preroll; ++i) {
        int v = g_preroll_buf[i];
        if (v < 0) v = -v;
        sum_abs += static_cast<uint32_t>(v);
        if (v > peak) peak = v;
      }
      total_samples += got_preroll;
    }
  }

  int16_t buf[kRecordChunk];
  bool still_holding = true;
  bool release_started = false;
  unsigned long release_start_ms = 0;
  int last_tick_ui = -1;
  while (still_holding && total_samples < max_samples) {
    size_t want = kRecordChunk;
    if (want > (max_samples - total_samples)) want = (max_samples - total_samples);
    if (want == 0) break;
    if (M5.Mic.record(buf, want, kSampleRate)) {
      const size_t wrote = wav_file.write(reinterpret_cast<const uint8_t*>(buf), want * sizeof(int16_t));
      if (wrote != want * sizeof(int16_t)) {
        logLine("[STT] wav capture write short, stopping early");
        still_holding = false;
      }
      for (size_t i = 0; i < want; ++i) {
        int v = buf[i];
        if (v < 0) v = -v;
        sum_abs += static_cast<uint32_t>(v);
        if (v > peak) peak = v;
      }
      total_samples += want;
    }

    M5.update();
    if (M5.BtnB.wasPressed()) {
      wav_file.close();
      if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
      err = "Cancelled";
      return false;
    }
    const bool a_pressed = M5.BtnA.isPressed();
    if (!a_pressed && total_samples >= kSttChunkMinSamples) {
      if (!release_started) {
        release_started = true;
        release_start_ms = millis();
      } else if (millis() - release_start_ms >= kReleaseTailMs &&
                 total_samples >= kMinTurnCaptureSamples) {
        still_holding = false;
      }
    } else if (a_pressed) {
      release_started = false;
    }
    const int ticks = static_cast<int>(total_samples / (kSampleRate / 10));
    if (last_tick_ui < 0 || ticks >= last_tick_ui + 5) {
      setUi(FaceState::Recording, "Listening...", String(ticks) + " ticks");
      last_tick_ui = ticks;
    }
  }

  g_last_capture_samples = total_samples;
  g_last_peak = peak;
  g_last_avg_abs = (total_samples > 0) ? static_cast<int>(sum_abs / total_samples) : 0;

  const size_t wav_data_bytes = total_samples * sizeof(int16_t);
  const bool hdr_ok = rewriteWavHeader(wav_file, wav_data_bytes);
  wav_file.close();
  if (!hdr_ok) {
    if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
    err = "WAV header finalize failed";
    return false;
  }

  if (total_samples < kSttChunkMinSamples) {
    if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
    err = "Too short";
    return false;
  }
  return true;
}

static bool sha256Multipart(const String& prefix, File& wav_file, const String& suffix, String& out_hex, String& err) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    err = "SHA256 init failed";
    return false;
  }

  if (prefix.length() > 0 &&
      mbedtls_sha256_update_ret(&ctx, reinterpret_cast<const uint8_t*>(prefix.c_str()), prefix.length()) != 0) {
    mbedtls_sha256_free(&ctx);
    err = "SHA256 prefix failed";
    return false;
  }

  if (!wav_file.seek(0, SeekSet)) {
    mbedtls_sha256_free(&ctx);
    err = "WAV seek failed";
    return false;
  }
  uint8_t buf[2048];
  while (true) {
    const size_t got = wav_file.read(buf, sizeof(buf));
    if (got == 0) break;
    if (mbedtls_sha256_update_ret(&ctx, buf, got) != 0) {
      mbedtls_sha256_free(&ctx);
      err = "SHA256 wav failed";
      return false;
    }
  }

  if (suffix.length() > 0 &&
      mbedtls_sha256_update_ret(&ctx, reinterpret_cast<const uint8_t*>(suffix.c_str()), suffix.length()) != 0) {
    mbedtls_sha256_free(&ctx);
    err = "SHA256 suffix failed";
    return false;
  }

  uint8_t digest[32];
  if (mbedtls_sha256_finish_ret(&ctx, digest) != 0) {
    mbedtls_sha256_free(&ctx);
    err = "SHA256 finish failed";
    return false;
  }
  mbedtls_sha256_free(&ctx);

  static const char* hex = "0123456789abcdef";
  out_hex = "";
  out_hex.reserve(64);
  for (size_t i = 0; i < sizeof(digest); ++i) {
    out_hex += hex[(digest[i] >> 4) & 0x0F];
    out_hex += hex[digest[i] & 0x0F];
  }
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

static bool requestPictureJpeg(
    const String& query,
    uint8_t*& jpg_out,
    size_t& jpg_len,
    String& image_title,
    String& image_source,
    String& err) {
  jpg_out = nullptr;
  jpg_len = 0;
  image_title = "";
  image_source = "";

  String q = query;
  q.trim();
  if (q.length() == 0) {
    err = "Empty picture query";
    return false;
  }
  if (q.length() > 80) q = q.substring(0, 80);
  const String body_str = String("{\"query\":\"") + jsonEscape(q) + "\"}";
  uint8_t* body = reinterpret_cast<uint8_t*>(const_cast<char*>(body_str.c_str()));

  time_t now = time(nullptr);
  if (now < 1700000000 && !syncTime()) {
    err = "NTP sync failed";
    return false;
  }
  now = time(nullptr);
  const String path = "/v1/picture";
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
  http.setConnectTimeout(9000);
  http.setTimeout(30000);
  const char* header_keys[] = {"Content-Type", "x-image-title", "x-image-source"};
  http.collectHeaders(header_keys, 3);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-timestamp", ts);
  http.addHeader("x-nonce", nonce);
  http.addHeader("x-signature", signature);

  const int code = http.POST(body, body_str.length());
  logLine(String("[HTTP] /v1/picture -> ") + String(code) + " q=" + q);
  if (code != 200) {
    const String payload = http.getString();
    logLine(String("[PICTURE ERROR PAYLOAD] ") + truncateForScreen(payload, 220));
    http.end();
    err = "PICTURE HTTP " + String(code) + " " + truncateForScreen(payload, 60);
    return false;
  }

  const String resp_type = http.header("Content-Type");
  if (resp_type.indexOf("image/") < 0) {
    const String payload = http.getString();
    http.end();
    err = "Unexpected image type: " + resp_type + " " + truncateForScreen(payload, 50);
    return false;
  }

  image_title = urlDecode(http.header("x-image-title"));
  image_source = urlDecode(http.header("x-image-source"));

  const int len = http.getSize();
  const size_t kImageCap = 140000;
  if (len <= 0 || len > static_cast<int>(kImageCap)) {
    http.end();
    err = "Picture too large";
    return false;
  }
  jpg_out = static_cast<uint8_t*>(malloc(static_cast<size_t>(len)));
  if (!jpg_out) {
    http.end();
    err = "OOM picture";
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  const bool full = readFully(*stream, jpg_out, static_cast<size_t>(len), 6000, got);
  http.end();
  jpg_len = got;
  if (jpg_len >= 4) {
    char sig[16];
    snprintf(sig, sizeof(sig), "%02X%02X%02X%02X", jpg_out[0], jpg_out[1], jpg_out[2], jpg_out[3]);
    logLine(String("[PICTURE] type=") + resp_type + " len=" + String(jpg_len) + " sig=" + String(sig));
  } else {
    logLine(String("[PICTURE] type=") + resp_type + " len=" + String(jpg_len));
  }
  if (!full || jpg_len < 256) {
    free(jpg_out);
    jpg_out = nullptr;
    jpg_len = 0;
    err = "Picture read failed";
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

  const int max_chunk_chars = 140;
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

static bool requestVoiceTurnAudio(
    uint8_t*& wav_out,
    size_t& wav_len,
    String& transcript,
    String& reply,
    String& screen_action,
    String& err) {
  wav_out = nullptr;
  wav_len = 0;
  transcript = "";
  reply = "";
  screen_action = "";

  const String wav_path = "/turn_capture.wav";
  size_t captured_samples = 0;
  if (!captureTurnWavToFile(wav_path, captured_samples, err)) return false;

  setUi(FaceState::Thinking, "Cloud turn...", "STT+Chat+TTS");
  const String sensor_context = buildSensorContextJson();

  File wav_file = SPIFFS.open(wav_path, FILE_READ);
  if (!wav_file) {
    if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
    err = "WAV open failed";
    return false;
  }

  auto cleanup = [&]() {
    wav_file.close();
    if (SPIFFS.exists(wav_path)) SPIFFS.remove(wav_path);
  };

  const String boundary = "----m5turn" + nonceHex();
  const String prefix =
      String("--") + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"context\"\r\n\r\n" +
      sensor_context + "\r\n" +
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\n" +
      "Content-Type: audio/wav\r\n\r\n";
  const String suffix = String("\r\n--") + boundary + "--\r\n";
  String body_hash;
  String hash_err;
  if (!sha256Multipart(prefix, wav_file, suffix, body_hash, hash_err)) {
    cleanup();
    err = hash_err;
    return false;
  }

  time_t now = time(nullptr);
  if (now < 1700000000 && !syncTime()) {
    cleanup();
    err = "NTP sync failed";
    return false;
  }
  now = time(nullptr);

  const String path = "/v1/voice-turn";
  const String ts = String(static_cast<long>(now));
  const String nonce = nonceHex();
  const String canonical = ts + "." + nonce + ".POST." + path + "." + body_hash;
  const String signature = hmacSha256Hex(DEVICE_SHARED_SECRET, canonical);
  const String content_type = String("multipart/form-data; boundary=") + boundary;

  String last_err = "";
  for (int attempt = 0; attempt < 2; ++attempt) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String(API_BASE_URL) + path)) {
      last_err = "http begin failed";
      continue;
    }
    http.setConnectTimeout(9000);
    http.setTimeout(55000);
    const char* header_keys[] = {
        "Content-Type",
        "x-transcript",
        "x-reply",
        "x-screen",
        "x-stt-ms",
        "x-audio-bytes",
        "x-stt-model"};
    http.collectHeaders(header_keys, 7);
    http.addHeader("Content-Type", content_type);
    http.addHeader("x-device-id", DEVICE_ID);
    http.addHeader("x-timestamp", ts);
    http.addHeader("x-nonce", nonce);
    http.addHeader("x-signature", signature);
    http.addHeader("x-tts-chunked", "0");

    MultipartUploadStream body_stream(
        reinterpret_cast<const uint8_t*>(prefix.c_str()),
        prefix.length(),
        &wav_file,
        reinterpret_cast<const uint8_t*>(suffix.c_str()),
        suffix.length());
    body_stream.begin();
    const int code = http.sendRequest("POST", &body_stream, body_stream.totalLength());
    logLine(String("[HTTP] /v1/voice-turn -> ") + String(code) +
            " samples=" + String(captured_samples));

    if (code < 0) {
      last_err = "TURN HTTP " + String(code);
      http.end();
      if (attempt == 0) {
        logLine("[TURN] transient transport error, retrying once...");
        delay(120);
        continue;
      }
      cleanup();
      err = last_err;
      return false;
    }
    if (code != 200 && code != 204) {
      const String payload = http.getString();
      http.end();
      cleanup();
      err = "TURN HTTP " + String(code) + " " + truncateForScreen(payload, 60);
      return false;
    }

    transcript = urlDecode(http.header("x-transcript"));
    reply = urlDecode(http.header("x-reply"));
    screen_action = http.header("x-screen");
    const String stt_ms = http.header("x-stt-ms");
    const String audio_bytes = http.header("x-audio-bytes");
    const String stt_model = urlDecode(http.header("x-stt-model"));
    if (stt_ms.length() > 0 || audio_bytes.length() > 0 || stt_model.length() > 0) {
      logLine(String("[STT DBG] model=") + stt_model +
              " stt_ms=" + stt_ms +
              " audio_bytes=" + audio_bytes +
              " captured_samples=" + String(captured_samples));
    }
    if (code == 204) {
      http.end();
      cleanup();
      return true;
    }

    const String resp_type = http.header("Content-Type");
    if (resp_type.indexOf("audio/wav") < 0) {
      const String payload = http.getString();
      http.end();
      cleanup();
      err = "Unexpected type: " + resp_type + " " + truncateForScreen(payload, 40);
      return false;
    }

    const int len = http.getSize();
    if (len <= 0 || len > 180000) {
      http.end();
      cleanup();
      err = "Turn audio too large";
      return false;
    }

    wav_out = static_cast<uint8_t*>(malloc(len));
    if (!wav_out) {
      http.end();
      cleanup();
      wav_len = 0;
      return true;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    const bool full = readFully(*stream, wav_out, static_cast<size_t>(len), 6000, got);
    http.end();
    cleanup();
    wav_len = got;
    if (!full || wav_len < 44) {
      free(wav_out);
      wav_out = nullptr;
      wav_len = 0;
      return true;
    }
    return true;
  }

  cleanup();
  err = last_err.length() ? last_err : "TURN failed";
  return false;
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
  String last_err = "";
  for (int attempt = 0; attempt < 2; ++attempt) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String(API_BASE_URL) + path)) {
      last_err = "http begin failed";
      continue;
    }
    http.setTimeout(30000);
    const char* header_keys[] = {"Content-Type", "x-transcript", "x-reply", "x-screen"};
    http.collectHeaders(header_keys, 4);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-device-id", DEVICE_ID);
    http.addHeader("x-timestamp", ts);
    http.addHeader("x-nonce", nonce);
    http.addHeader("x-signature", signature);
    // Prefer single cloud-generated audio to reduce on-device chunk orchestration and
    // improve reliability on constrained memory devices.
    http.addHeader("x-tts-chunked", "0");

    const int code = http.POST(body, body_str.length());
    logLine(String("[HTTP] /v1/voice-turn-text -> ") + String(code));
    if (code < 0) {
      last_err = "TURN_TEXT HTTP " + String(code);
      http.end();
      if (attempt == 0) {
        logLine("[TURN TEXT] transient transport error, retrying once...");
        delay(120);
        continue;
      }
      err = last_err;
      return false;
    }
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
      logLine(String("[TURN AUDIO] malloc fallback len=") + String(len) +
              " free=" + String(ESP.getFreeHeap()) +
              " max=" + String(ESP.getMaxAllocHeap()));
      http.end();
      wav_out = nullptr;
      wav_len = 0;
      return true;
    }
    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    const bool full = readFully(*stream, wav_out, static_cast<size_t>(len), 4000, got);
    http.end();
    wav_len = got;
    if (!full || wav_len < 44) {
      free(wav_out);
      wav_out = nullptr;
      logLine(String("[TURN AUDIO] stream fallback got=") + String(got) + "/" + String(len));
      wav_len = 0;
      return true;
    }
    return true;
  }
  err = last_err.length() ? last_err : "TURN_TEXT failed";
  return false;
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
  ctx.ensureWifiConnected = ensureWifiConnected;
  ctx.requestVoiceTurnAudio = requestVoiceTurnAudio;
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
      logLine("[SERIAL CMD] use: ASK <question> | MIC? | MICCFG <mag 1..64> <nf 0..4>");
      continue;
    }
    if (line == "MIC?") {
      logLine(String("[MIC] magnification=") + String(g_mic_magnification) +
              " noise_filter=" + String(g_mic_noise_filter_level));
      continue;
    }
    if (line.startsWith("MICCFG ")) {
      String rest = line.substring(7);
      rest.trim();
      const int sep = rest.indexOf(' ');
      if (sep <= 0) {
        logLine("[SERIAL CMD] use: MICCFG <mag 1..64> <nf 0..4>");
        continue;
      }
      String mag_s = rest.substring(0, sep);
      String nf_s = rest.substring(sep + 1);
      mag_s.trim();
      nf_s.trim();
      const int mag = mag_s.toInt();
      const int nf = nf_s.toInt();
      if (mag < 1 || mag > 64 || nf < 0 || nf > 4) {
        logLine("[SERIAL CMD] invalid MICCFG range");
        continue;
      }
      applyMicConfig(static_cast<uint8_t>(mag), static_cast<uint8_t>(nf));
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
  if (g_overlay_active && g_overlay_mode == "image") return;
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

  g_next_blink_at = millis() + 1200;
  g_last_hunger_tick_ms = millis();
  setUi(FaceState::Connecting, "Booting...", "Init");

  if (!connectWifi()) return;
  if (!syncTime()) {
    // Allow operation even if boot-time NTP fails; signed cloud calls will
    // retry time sync on demand before request auth.
    logLine("[NTP] boot sync failed; continuing and will retry on demand");
  }

  // Favor intelligibility over aggressive denoise; helps longer natural questions.
  applyMicConfig(g_mic_magnification, g_mic_noise_filter_level);
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
