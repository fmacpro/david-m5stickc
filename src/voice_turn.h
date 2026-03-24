#pragma once

#include <Arduino.h>

struct VoiceTurnContext {
  uint32_t* voice_turn_seq = nullptr;
  uint32_t* voice_turn_active = nullptr;
  String* global_reply = nullptr;
  int* mood = nullptr;
  int* boredom = nullptr;
  int* energy = nullptr;
  int* hunger = nullptr;
  unsigned long* last_interaction_ms = nullptr;

  int ui_recording_state = 0;
  int ui_thinking_state = 0;
  int ui_speaking_state = 0;

  bool (*ensureWifiConnected)(String&) = nullptr;
  bool (*requestVoiceTurnAudio)(uint8_t*&, size_t&, String&, String&, String&, String&) = nullptr;
  bool (*requestVoiceTurnText)(const String&, uint8_t*&, size_t&, String&, String&, String&, String&) = nullptr;
  bool (*isNoSpeechError)(const String&) = nullptr;
  bool (*playWavBuffer)(uint8_t*, size_t, String&) = nullptr;
  bool (*speakReplyInChunks)(const String&, String&) = nullptr;
  void (*setUiRaw)(int, const String&, const String&) = nullptr;
  void (*applyScreenAction)(const String&) = nullptr;
  int (*inferMoodFromConversationRaw)(const String&, const String&) = nullptr;
  int (*clampNeedRaw)(int) = nullptr;
  void (*logLine)(const String&) = nullptr;
  unsigned long (*nowMs)() = nullptr;
};

bool runVoiceTurnFlow(VoiceTurnContext& ctx, String& err);
bool runInjectedTranscriptTurnFlow(VoiceTurnContext& ctx, const String& transcript_in, String& err);
