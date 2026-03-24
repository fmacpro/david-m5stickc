#include "face_renderer.h"

#include <SPIFFS.h>

#include "ui_draw_utils.h"

namespace {

constexpr int kStateIdle = 0;
constexpr int kStateConnecting = 1;
constexpr int kStateRecording = 2;
constexpr int kStateThinking = 3;
constexpr int kStateSpeaking = 4;
constexpr int kStateHappy = 5;
constexpr int kStateError = 6;

constexpr int kMoodCheerful = 1;
constexpr int kMoodCurious = 2;
constexpr int kMoodConcerned = 3;
constexpr int kMoodSleepy = 4;
constexpr int kMoodHungry = 5;

constexpr int kReactionPatted = 1;
constexpr int kReactionFed = 2;

bool splitIntoTwoBalancedLines(const String& text, String& line1, String& line2) {
  line1 = "";
  line2 = "";
  const int n = text.length();
  if (n < 8) return false;

  int best = -1;
  int bestScore = 1 << 30;
  const int mid = n / 2;
  for (int i = 1; i < n - 1; ++i) {
    if (text[i] != ' ') continue;
    const int left = i;
    const int right = n - i - 1;
    if (left < 3 || right < 3) continue;
    const int score = abs(left - mid);
    if (score < bestScore) {
      bestScore = score;
      best = i;
    }
  }
  if (best < 0) return false;

  line1 = text.substring(0, best);
  line2 = text.substring(best + 1);
  line1.trim();
  line2.trim();
  return line1.length() > 0 && line2.length() > 0;
}

void drawBaseMouthOutline(uint16_t accent) {
  M5.Display.drawRoundRect(56, 49, 36, 8, 4, accent);
}

void drawPupils(int dx_left, int dy_left, int dx_right, int dy_right) {
  const int eye_y = 20;
  M5.Display.fillCircle(47 + dx_left, eye_y + 7 + dy_left, 2, BLACK);
  M5.Display.fillCircle(101 + dx_right, eye_y + 7 + dy_right, 2, BLACK);
}

}  // namespace

namespace face_renderer {

void drawSpeakingFeaturesFrame(bool eyes_closed, uint8_t speaking_level, unsigned long now_ms) {
  const uint16_t accent = ORANGE;
  const int eye_y = 20;

  // Clear only feature regions to avoid full-screen flicker.
  M5.Display.fillRect(30, eye_y - 1, 90, 18, BLACK);
  M5.Display.fillRect(52, 44, 44, 20, BLACK);

  if (eyes_closed) {
    M5.Display.drawFastHLine(38, eye_y + 6, 18, accent);
    M5.Display.drawFastHLine(92, eye_y + 6, 18, accent);
  } else {
    const int t = static_cast<int>((now_ms / 120) % 8);
    const int dx = (t < 4) ? t - 1 : 6 - t;  // -1..2..-1
    M5.Display.fillRoundRect(35, eye_y, 24, 14, 4, accent);
    M5.Display.fillRoundRect(89, eye_y, 24, 14, 4, accent);
    drawPupils(dx, 0, dx, 0);
  }

  const int mouth_y = 48;
  const int phase = static_cast<int>((now_ms / 70) % 8);
  const int pulse = (phase < 4) ? phase : (7 - phase);  // 0..3..0
  const int outer_x = 56;
  const int outer_y = mouth_y + 1;
  const int outer_w = 36;
  const int outer_h = 8;
  drawBaseMouthOutline(accent);

  const int open = 1 + pulse + static_cast<int>(speaking_level / 20);
  const int inner_h = constrain(open, 1, 5);
  const int inner_w = outer_w - 8;
  const int inner_x = outer_x + 4;
  const int inner_y = outer_y + (outer_h - inner_h) / 2;
  M5.Display.fillRect(inner_x, outer_y + 1, inner_w, outer_h - 2, BLACK);
  M5.Display.fillRect(inner_x, inner_y, inner_w, inner_h, accent);
}

void drawFace(const FaceRenderModel& m) {
  if (m.overlay_active && m.now_ms < m.overlay_until_ms && m.state != kStateSpeaking) {
    const uint16_t c = GREEN;
    if (m.overlay_mode == "draw") {
      M5.Display.fillScreen(BLACK);
      drawCenteredScript(m.overlay_draw, c);
    } else if (m.overlay_mode == "image") {
      // Keep image overlay stable if another UI path has repainted the display.
      if (SPIFFS.exists(m.overlay_value.c_str())) {
        File imgf = SPIFFS.open(m.overlay_value.c_str(), FILE_READ);
        if (imgf) {
          M5.Display.fillScreen(BLACK);
          bool ok = M5.Display.drawJpg(&imgf, 0, 0, 160, 80, 0, 0, 0.0f, 0.0f, middle_center);
          if (!ok) {
            imgf.seek(0, SeekSet);
            ok = M5.Display.drawPng(&imgf, 0, 0, 160, 80, 0, 0, 0.0f, 0.0f, middle_center);
          }
          if (!ok) {
            imgf.seek(0, SeekSet);
            (void)M5.Display.drawBmp(&imgf, 0, 0, 160, 80, 0, 0, 0.0f, 0.0f, middle_center);
          }
          imgf.close();
        }
      }
      return;
    } else if (m.overlay_mode == "big_time") {
      M5.Display.fillScreen(BLACK);
      drawCenteredFitText(m.overlay_value, 24, 150, 4, 2, c);
    } else if (m.overlay_mode == "big_battery") {
      M5.Display.fillScreen(BLACK);
      const int bw = 74;
      const int bh = 34;
      const int terminalW = 4;
      const int bx = (160 - (bw + terminalW)) / 2;
      const int by = 12;
      drawBatteryIcon(bx, by, bw, bh, m.overlay_percent, m.overlay_charging, c);
      String pct = String(m.overlay_percent) + "%";
      drawCenteredFitText(pct, 52, 150, 2, 1, c);
    } else if (m.overlay_mode == "big_temp") {
      M5.Display.fillScreen(BLACK);
      const int tw = 16;
      const int th = 28;
      const int tx = 24;
      const int ty = 12;
      M5.Display.drawRoundRect(tx, ty, tw, th, 5, c);
      M5.Display.fillCircle(tx + tw / 2, ty + th + 3, 6, c);
      M5.Display.drawCircle(tx + tw / 2, ty + th + 3, 7, c);

      const float tempC = m.overlay_value.toFloat();
      int fill = static_cast<int>((tempC - 10.0f) * (th - 8) / 30.0f);
      if (fill < 2) fill = 2;
      if (fill > th - 8) fill = th - 8;
      M5.Display.fillRect(tx + 5, ty + th - 4 - fill, 6, fill, c);

      const int textX = 64;
      const int textW = 92;
      M5.Display.fillRect(textX, 10, textW, 60, BLACK);
      drawFitTextInRect("TEMP", textX, 12, textW, 14, 1, 1, c);
      drawFitTextInRect(m.overlay_value, textX, 28, textW, 34, 3, 1, c);
    } else if (m.overlay_mode == "big_rssi") {
      M5.Display.fillScreen(BLACK);
      const int bx = 24;
      const int by = 30;
      const int bw = 8;
      for (int i = 0; i < 4; ++i) {
        int h = 6 + i * 5;
        int x = bx + i * (bw + 4);
        int y = by + (24 - h);
        bool on = (m.overlay_percent >= (i + 1) * 25);
        if (on) M5.Display.fillRect(x, y, bw, h, c);
        else M5.Display.drawRect(x, y, bw, h, c);
      }
      drawCenteredFitText(m.overlay_value, 52, 150, 2, 1, c);
    } else if (m.overlay_mode == "big_date") {
      M5.Display.fillScreen(BLACK);
      if (m.overlay_title.length() > 0) drawCenteredFitText(m.overlay_title, 18, 150, 1, 1, c);
      drawCenteredFitText(m.overlay_value, 36, 150, 2, 1, c);
    } else if (m.overlay_mode == "big_value") {
      M5.Display.fillScreen(BLACK);
      if (m.overlay_title.length() > 0) drawCenteredFitText(m.overlay_title, 16, 150, 1, 1, c);
      String line1, line2;
      if (splitIntoTwoBalancedLines(m.overlay_value, line1, line2)) {
        // Keep two-line values visually centered on the 80px display.
        const int top = (m.overlay_title.length() > 0) ? 24 : 26;
        drawCenteredFitText(line1, top, 156, 2, 1, c);
        drawCenteredFitText(line2, top + 16, 156, 2, 1, c);
      } else {
        int maxSize = 2;
        const size_t n = m.overlay_value.length();
        if (n <= 4) maxSize = 4;
        else if (n <= 8) maxSize = 3;
        int y = (maxSize >= 4) ? 22 : 30;
        if (m.overlay_title.length() == 0 && m.overlay_value.length() > 10) y = 26;
        drawCenteredFitText(m.overlay_value, y, 156, maxSize, 1, c);
      }
    } else {
      M5.Display.fillScreen(BLACK);
      if (m.overlay_title.length() > 0) drawCenteredFitText(m.overlay_title, 16, 150, 1, 1, c);
      drawCenteredFitText(m.overlay_value, 34, 150, 2, 1, c);
    }
    return;
  }

  M5.Display.fillScreen(BLACK);

  uint16_t accent = WHITE;
  if (m.state == kStateConnecting) accent = CYAN;
  if (m.state == kStateRecording) accent = MAGENTA;
  if (m.state == kStateThinking) accent = YELLOW;
  if (m.state == kStateSpeaking) accent = ORANGE;
  if (m.state == kStateHappy) accent = GREEN;
  if (m.state == kStateError) accent = RED;
  if (m.state == kStateIdle) {
    if (m.mood == kMoodCheerful) accent = GREEN;
    else if (m.mood == kMoodCurious) accent = CYAN;
    else if (m.mood == kMoodConcerned) accent = YELLOW;
    else if (m.mood == kMoodHungry) accent = ORANGE;
    else if (m.mood == kMoodSleepy) accent = DARKGREY;
    if (m.reaction == kReactionPatted && m.now_ms < m.reaction_until_ms) accent = GREEN;
    if (m.reaction == kReactionFed && m.now_ms < m.reaction_until_ms) accent = ORANGE;
  }

  const int eye_y = 20;
  if (m.eyes_closed) {
    M5.Display.drawFastHLine(38, eye_y + 6, 18, accent);
    M5.Display.drawFastHLine(92, eye_y + 6, 18, accent);
  } else {
    if (m.state == kStateIdle && m.reaction == kReactionPatted && m.now_ms < m.reaction_until_ms) {
      M5.Display.fillRoundRect(34, eye_y + 2, 26, 10, 4, accent);
      M5.Display.fillRoundRect(90, eye_y + 2, 26, 10, 4, accent);
      drawPupils(0, 1, 0, 1);
    } else if (m.state == kStateIdle && m.reaction == kReactionFed && m.now_ms < m.reaction_until_ms) {
      M5.Display.fillRoundRect(35, eye_y + 3, 24, 9, 4, accent);
      M5.Display.fillRoundRect(89, eye_y + 3, 24, 9, 4, accent);
      drawPupils(0, 2, 0, 2);
    } else if (m.state == kStateIdle && m.mood == kMoodCurious) {
      M5.Display.fillRoundRect(33, eye_y, 28, 14, 5, accent);
      M5.Display.fillRoundRect(91, eye_y + 2, 20, 10, 4, accent);
      drawPupils(1, 0, -1, 1);
    } else if (m.state == kStateIdle && m.mood == kMoodHungry) {
      M5.Display.fillRoundRect(35, eye_y + 2, 24, 10, 4, accent);
      M5.Display.fillRoundRect(89, eye_y + 2, 24, 10, 4, accent);
      drawPupils(0, 2, 0, 2);
    } else if (m.state == kStateIdle && m.mood == kMoodSleepy) {
      M5.Display.fillRoundRect(35, eye_y + 4, 24, 8, 3, accent);
      M5.Display.fillRoundRect(89, eye_y + 4, 24, 8, 3, accent);
      drawPupils(0, 2, 0, 2);
    } else {
      M5.Display.fillRoundRect(35, eye_y, 24, 14, 4, accent);
      M5.Display.fillRoundRect(89, eye_y, 24, 14, 4, accent);
      drawPupils(0, 0, 0, 0);
    }
  }

  const int mouth_y = 48;
  const int mouth_x = 56;
  const int mouth_w = 36;
  if (m.state == kStateRecording) {
    drawBaseMouthOutline(accent);
    M5.Display.fillRoundRect(mouth_x, mouth_y + 1, mouth_w, 10, 5, accent);
    M5.Display.fillRect(mouth_x + 6, mouth_y + 3, mouth_w - 12, 6, BLACK);
  } else if (m.state == kStateThinking) {
    drawBaseMouthOutline(accent);
    M5.Display.drawFastHLine(mouth_x + 6, mouth_y + 5, mouth_w - 12, accent);
  } else if (m.state == kStateSpeaking) {
    drawBaseMouthOutline(accent);
    M5.Display.fillRect(mouth_x + 4, mouth_y + 4, mouth_w - 8, 2, accent);
  } else if (m.state == kStateHappy) {
    drawBaseMouthOutline(accent);
    M5.Display.drawRoundRect(mouth_x, mouth_y - 1, mouth_w, 12, 6, accent);
    M5.Display.drawFastHLine(mouth_x + 4, mouth_y + 8, mouth_w - 8, accent);
  } else if (m.state == kStateError) {
    drawBaseMouthOutline(accent);
    M5.Display.drawRoundRect(mouth_x, mouth_y + 4, mouth_w, 6, 4, accent);
    M5.Display.drawFastHLine(mouth_x + 2, mouth_y + 5, mouth_w - 4, accent);
  } else {
    if (m.reaction == kReactionPatted && m.now_ms < m.reaction_until_ms) {
      drawBaseMouthOutline(accent);
      M5.Display.drawRoundRect(mouth_x, mouth_y - 1, mouth_w, 12, 5, accent);
      M5.Display.drawFastHLine(mouth_x + 4, mouth_y + 8, mouth_w - 8, accent);
    } else if (m.reaction == kReactionFed && m.now_ms < m.reaction_until_ms) {
      drawBaseMouthOutline(accent);
      M5.Display.fillRect(mouth_x + 8, mouth_y + 3, mouth_w - 16, 3, accent);
      M5.Display.drawFastHLine(mouth_x + 10, mouth_y + 8, mouth_w - 20, accent);
    } else if (m.mood == kMoodCheerful) {
      drawBaseMouthOutline(accent);
      M5.Display.drawRoundRect(mouth_x, mouth_y - 1, mouth_w, 12, 5, accent);
      M5.Display.drawFastHLine(mouth_x + 4, mouth_y + 8, mouth_w - 8, accent);
    } else if (m.mood == kMoodHungry) {
      drawBaseMouthOutline(accent);
      M5.Display.drawFastHLine(mouth_x + 6, mouth_y + 5, mouth_w - 12, accent);
      M5.Display.drawFastVLine(mouth_x + mouth_w / 2, mouth_y + 6, 4, accent);
    } else if (m.mood == kMoodConcerned) {
      drawBaseMouthOutline(accent);
      M5.Display.drawRoundRect(mouth_x, mouth_y + 4, mouth_w, 6, 4, accent);
      M5.Display.drawFastHLine(mouth_x + 2, mouth_y + 5, mouth_w - 4, accent);
    } else {
      drawBaseMouthOutline(accent);
    }
  }
}

}  // namespace face_renderer
