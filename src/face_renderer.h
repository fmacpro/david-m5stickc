#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace face_renderer {

struct FaceRenderModel {
  bool overlay_active = false;
  unsigned long now_ms = 0;
  unsigned long overlay_until_ms = 0;
  String overlay_mode;
  String overlay_title;
  String overlay_value;
  String overlay_draw;
  int overlay_percent = -1;
  bool overlay_charging = false;
  int state = 0;    // FaceState ordinal from main.cpp
  int mood = 0;     // FaceMood ordinal from main.cpp
  int reaction = 0; // ReactionKind ordinal from main.cpp
  unsigned long reaction_until_ms = 0;
  bool eyes_closed = false;
  uint8_t speaking_level = 0;
};

void drawFace(const FaceRenderModel& model);
void drawSpeakingFeaturesFrame(bool eyes_closed, uint8_t speaking_level, unsigned long now_ms);

}  // namespace face_renderer
