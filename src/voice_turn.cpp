#include "voice_turn.h"

namespace {

void applyConversationPost(VoiceTurnContext& ctx, const String& heard, const String& reply) {
  if (ctx.global_reply && reply.length() > 0) *ctx.global_reply = reply;
  if (ctx.mood && ctx.inferMoodFromConversationRaw && ctx.global_reply) {
    *ctx.mood = ctx.inferMoodFromConversationRaw(heard, *ctx.global_reply);
  }
  if (ctx.clampNeedRaw) {
    if (ctx.boredom) *ctx.boredom = ctx.clampNeedRaw(*ctx.boredom - 24);
    if (ctx.energy) *ctx.energy = ctx.clampNeedRaw(*ctx.energy - 5);
    if (ctx.hunger) *ctx.hunger = ctx.clampNeedRaw(*ctx.hunger + 2);
  }
  if (ctx.last_interaction_ms && ctx.nowMs) *ctx.last_interaction_ms = ctx.nowMs();
}

void logTurnError(VoiceTurnContext& ctx, const char* stage, const String& err) {
  if (!ctx.logLine) return;
  ctx.logLine(String("[") + stage + "] " + err);
}

bool playTurnOutput(
    VoiceTurnContext& ctx,
    uint8_t* wav,
    size_t wav_len,
    const String& screen_action,
    String& err) {
  bool played = false;
  const String reply = ctx.global_reply ? *ctx.global_reply : String("");
  if (wav && wav_len >= 44) {
    if (ctx.setUiRaw) ctx.setUiRaw(ctx.ui_speaking_state, "", "");
    played = ctx.playWavBuffer ? ctx.playWavBuffer(wav, wav_len, err) : false;
    free(wav);
  } else if (reply.length() > 0) {
    played = ctx.speakReplyInChunks ? ctx.speakReplyInChunks(reply, err) : false;
  } else {
    err = "No voice turn audio";
    return false;
  }
  if (played && screen_action.length() > 0 && ctx.applyScreenAction) {
    ctx.applyScreenAction(screen_action);
  }
  return played;
}

bool runTurnCore(VoiceTurnContext& ctx, String& heard, bool injected, String& err) {
  uint8_t* wav = nullptr;
  size_t wav_len = 0;
  String transcript_resp;
  String reply;
  String screen_action;

  if (ctx.voice_turn_seq && ctx.voice_turn_active) {
    *ctx.voice_turn_active = ++(*ctx.voice_turn_seq);
  }

  if (!injected) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (ctx.setUiRaw) {
        ctx.setUiRaw(
            ctx.ui_recording_state,
            "Listening...",
            (attempt == 0) ? String("Speak now") : String("Try again"));
      }
      if (!ctx.captureTranscriptChunked || !ctx.captureTranscriptChunked(heard, err)) {
        logTurnError(ctx, "TURN CAPTURE", err);
        return false;
      }
      if (!ctx.ensureWifiConnected || !ctx.ensureWifiConnected(err)) {
        logTurnError(ctx, "TURN WIFI", err);
        return false;
      }
      if (ctx.setUiRaw) ctx.setUiRaw(ctx.ui_thinking_state, "Cloud turn...", "STT+Chat+TTS");
      if (ctx.requestVoiceTurnText &&
          ctx.requestVoiceTurnText(heard, wav, wav_len, heard, reply, screen_action, err)) {
        break;
      }
      if (!(attempt == 0 && ctx.isNoSpeechError && ctx.isNoSpeechError(err))) {
        logTurnError(ctx, "TURN CLOUD", err);
        return false;
      }
      if (ctx.logLine) ctx.logLine("[TURN] No speech detected, retrying once");
      err = "";
    }
  } else {
    if (!ctx.ensureWifiConnected || !ctx.ensureWifiConnected(err)) {
      logTurnError(ctx, "TURN WIFI", err);
      return false;
    }
    if (!ctx.requestVoiceTurnText ||
        !ctx.requestVoiceTurnText(heard, wav, wav_len, transcript_resp, reply, screen_action, err)) {
      logTurnError(ctx, "TURN CLOUD", err);
      return false;
    }
  }

  if (transcript_resp.length() > 0) heard = transcript_resp;

  applyConversationPost(ctx, heard, reply);
  const String final_reply = ctx.global_reply ? *ctx.global_reply : String("");
  if (heard.length() > 0 && ctx.logLine) ctx.logLine(String("[STT] ") + heard);
  if (final_reply.length() > 0 && ctx.logLine) ctx.logLine(String("[CHAT] ") + final_reply);

  return playTurnOutput(ctx, wav, wav_len, screen_action, err);
}

}  // namespace

bool runVoiceTurnFlow(VoiceTurnContext& ctx, String& err) {
  String heard;
  return runTurnCore(ctx, heard, false, err);
}

bool runInjectedTranscriptTurnFlow(VoiceTurnContext& ctx, const String& transcript_in, String& err) {
  String heard = transcript_in;
  heard.trim();
  if (heard.length() == 0) {
    err = "Empty injected transcript";
    return false;
  }
  return runTurnCore(ctx, heard, true, err);
}
