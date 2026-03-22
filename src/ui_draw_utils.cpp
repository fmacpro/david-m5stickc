#include "ui_draw_utils.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int parseIntToken(const String& tok, int fallback = 0) {
  if (tok.length() == 0) return fallback;
  char* endptr = nullptr;
  long v = strtol(tok.c_str(), &endptr, 10);
  if (endptr == tok.c_str()) return fallback;
  return static_cast<int>(v);
}

static int scalePos(int v, int off, float scale) {
  return off + static_cast<int>(lroundf(v * scale));
}

static int scaleLen(int v, float scale) {
  if (v <= 0) return 0;
  int out = static_cast<int>(lroundf(v * scale));
  if (out < 1) out = 1;
  return out;
}

static int estimateTextWidthPx(const String& text, int sz) {
  int size = sz;
  if (size < 1) size = 1;
  if (size > 4) size = 4;
  return static_cast<int>(text.length()) * 6 * size;
}

static bool computeDrawBounds(
    const String& script,
    int& minx,
    int& miny,
    int& maxx,
    int& maxy) {
  minx = 10000;
  miny = 10000;
  maxx = -10000;
  maxy = -10000;
  bool any = false;

  String s = script;
  s.replace("\r", "");
  char* save_cmd = nullptr;
  char* cmd = strtok_r(const_cast<char*>(s.c_str()), ";", &save_cmd);
  while (cmd) {
    String c = String(cmd);
    c.trim();
    if (c.length() == 0 || c == "CLS") {
      cmd = strtok_r(nullptr, ";", &save_cmd);
      continue;
    }

    char* save_tok = nullptr;
    char* t0 = strtok_r(cmd, " ", &save_tok);
    if (!t0) {
      cmd = strtok_r(nullptr, ";", &save_cmd);
      continue;
    }
    String op = String(t0);
    op.toUpperCase();
    auto nextTok = [&save_tok]() -> String {
      char* t = strtok_r(nullptr, " ", &save_tok);
      return t ? String(t) : String("");
    };
    auto includePt = [&](int x, int y) {
      if (x < minx) minx = x;
      if (y < miny) miny = y;
      if (x > maxx) maxx = x;
      if (y > maxy) maxy = y;
      any = true;
    };

    if (op == "P") {
      includePt(parseIntToken(nextTok(), 0), parseIntToken(nextTok(), 0));
    } else if (op == "L") {
      int x1 = parseIntToken(nextTok(), 0);
      int y1 = parseIntToken(nextTok(), 0);
      int x2 = parseIntToken(nextTok(), 0);
      int y2 = parseIntToken(nextTok(), 0);
      includePt(x1, y1);
      includePt(x2, y2);
    } else if (op == "R" || op == "F") {
      int x = parseIntToken(nextTok(), 0);
      int y = parseIntToken(nextTok(), 0);
      int w = parseIntToken(nextTok(), 0);
      int h = parseIntToken(nextTok(), 0);
      includePt(x, y);
      includePt(x + w - 1, y + h - 1);
    } else if (op == "C" || op == "D") {
      int x = parseIntToken(nextTok(), 0);
      int y = parseIntToken(nextTok(), 0);
      int r = parseIntToken(nextTok(), 0);
      includePt(x - r, y - r);
      includePt(x + r, y + r);
    } else if (op == "T") {
      int x = parseIntToken(nextTok(), 0);
      int y = parseIntToken(nextTok(), 0);
      int sz = parseIntToken(nextTok(), 1);
      String text = String(save_tok ? save_tok : "");
      text.trim();
      text.replace("_", " ");
      includePt(x, y);
      includePt(x + estimateTextWidthPx(text, sz) - 1, y + 8 * sz - 1);
    }
    cmd = strtok_r(nullptr, ";", &save_cmd);
  }
  return any;
}

static void executeDrawScript(const String& script, uint16_t color, int offx, int offy, float scale) {
  if (script.length() == 0) return;
  String s = script;
  s.replace("\r", "");
  char* save_cmd = nullptr;
  char* cmd = strtok_r(const_cast<char*>(s.c_str()), ";", &save_cmd);
  while (cmd) {
    String c = String(cmd);
    c.trim();
    if (c.length() == 0) {
      cmd = strtok_r(nullptr, ";", &save_cmd);
      continue;
    }
    if (c == "CLS") {
      M5.Display.fillScreen(BLACK);
      cmd = strtok_r(nullptr, ";", &save_cmd);
      continue;
    }

    char* save_tok = nullptr;
    char* t0 = strtok_r(cmd, " ", &save_tok);
    if (!t0) {
      cmd = strtok_r(nullptr, ";", &save_cmd);
      continue;
    }
    String op = String(t0);
    op.toUpperCase();

    auto nextTok = [&save_tok]() -> String {
      char* t = strtok_r(nullptr, " ", &save_tok);
      return t ? String(t) : String("");
    };

    if (op == "P") {
      int x = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      M5.Display.drawPixel(x, y, color);
    } else if (op == "L") {
      int x1 = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y1 = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      int x2 = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y2 = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      M5.Display.drawLine(x1, y1, x2, y2, color);
    } else if (op == "R") {
      int x = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      int w = scaleLen(parseIntToken(nextTok(), 0), scale);
      int h = scaleLen(parseIntToken(nextTok(), 0), scale);
      M5.Display.drawRect(x, y, w, h, color);
    } else if (op == "F") {
      int x = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      int w = scaleLen(parseIntToken(nextTok(), 0), scale);
      int h = scaleLen(parseIntToken(nextTok(), 0), scale);
      M5.Display.fillRect(x, y, w, h, color);
    } else if (op == "C") {
      int x = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      int r = scaleLen(parseIntToken(nextTok(), 0), scale);
      M5.Display.drawCircle(x, y, r, color);
    } else if (op == "D") {
      int x = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      int r = scaleLen(parseIntToken(nextTok(), 0), scale);
      M5.Display.fillCircle(x, y, r, color);
    } else if (op == "T") {
      int x = scalePos(parseIntToken(nextTok(), 0), offx, scale);
      int y = scalePos(parseIntToken(nextTok(), 0), offy, scale);
      int sz = parseIntToken(nextTok(), 1);
      sz = scaleLen(sz, scale);
      if (sz > 4) sz = 4;
      String text = String(save_tok ? save_tok : "");
      text.trim();
      text.replace("_", " ");
      M5.Display.setTextSize(sz);
      M5.Display.setTextColor(color, BLACK);
      M5.Display.setCursor(x, y);
      M5.Display.print(text);
    }

    cmd = strtok_r(nullptr, ";", &save_cmd);
  }
}

void drawCenteredScript(const String& script, uint16_t color) {
  int minx = 0, miny = 0, maxx = 0, maxy = 0;
  int offx = 0, offy = 0;
  float scale = 1.0f;
  if (computeDrawBounds(script, minx, miny, maxx, maxy)) {
    const int pad_x = 12;
    const int pad_y = 8;
    const int fit_w = 160 - pad_x * 2;
    const int fit_h = 80 - pad_y * 2;
    if (fit_w <= 0 || fit_h <= 0) return;
    const int w = maxx - minx + 1;
    const int h = maxy - miny + 1;
    if (w > 0 && h > 0) {
      const float sx = static_cast<float>(fit_w) / static_cast<float>(w);
      const float sy = static_cast<float>(fit_h) / static_cast<float>(h);
      scale = fminf(1.0f, fminf(sx, sy));
      const float sw = w * scale;
      const float sh = h * scale;
      const float origin_x = pad_x + (fit_w - sw) * 0.5f;
      const float origin_y = pad_y + (fit_h - sh) * 0.5f;
      offx = static_cast<int>(lroundf(origin_x - minx * scale));
      offy = static_cast<int>(lroundf(origin_y - miny * scale));
    }
  }
  executeDrawScript(script, color, offx, offy, scale);
}

void drawCenteredText(const String& text, int y, int size, uint16_t color) {
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(color, BLACK);
  const int w = M5.Display.textWidth(text);
  int x = (160 - w) / 2;
  if (x < 0) x = 0;
  M5.Display.setCursor(x, y);
  M5.Display.print(text);
}

void drawCenteredFitText(
    const String& text,
    int y,
    int max_width,
    int max_size,
    int min_size,
    uint16_t color) {
  int size = max_size;
  while (size > min_size) {
    M5.Display.setTextSize(size);
    if (M5.Display.textWidth(text) <= max_width) break;
    --size;
  }
  drawCenteredText(text, y, size, color);
}

void drawFitTextInBox(
    const String& text,
    int box_x,
    int box_y,
    int box_w,
    int max_size,
    int min_size,
    uint16_t color) {
  int size = max_size;
  while (size > min_size) {
    M5.Display.setTextSize(size);
    if (M5.Display.textWidth(text) <= box_w) break;
    --size;
  }
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(color, BLACK);
  int x = box_x + (box_w - M5.Display.textWidth(text)) / 2;
  if (x < box_x) x = box_x;
  M5.Display.setCursor(x, box_y);
  M5.Display.print(text);
}

void drawFitTextInRect(
    const String& text,
    int x,
    int y,
    int w,
    int h,
    int max_size,
    int min_size,
    uint16_t color) {
  int size = max_size;
  while (size > min_size) {
    M5.Display.setTextSize(size);
    const int tw = M5.Display.textWidth(text);
    const int th = 8 * size;
    if (tw <= w && th <= h) break;
    --size;
  }
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(color, BLACK);
  const int tw = M5.Display.textWidth(text);
  const int th = 8 * size;
  int tx = x + (w - tw) / 2;
  int ty = y + (h - th) / 2;
  if (tx < x) tx = x;
  if (ty < y) ty = y;
  M5.Display.setCursor(tx, ty);
  M5.Display.print(text);
}

void drawBatteryIcon(int x, int y, int w, int h, int percent, bool charging, uint16_t color) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  const int terminalW = 4;
  const int terminalH = h / 3;
  M5.Display.drawRoundRect(x, y, w, h, 3, color);
  M5.Display.fillRect(x + w, y + (h - terminalH) / 2, terminalW, terminalH, color);

  const int innerPad = 3;
  const int innerW = w - innerPad * 2;
  const int innerH = h - innerPad * 2;
  M5.Display.drawRect(x + innerPad, y + innerPad, innerW, innerH, color);

  const int bars = (percent + 19) / 20;
  const int gap = 2;
  const int barW = (innerW - gap * 6) / 5;
  for (int i = 0; i < 5; ++i) {
    const int bx = x + innerPad + gap + i * (barW + gap);
    const int by = y + innerPad + gap;
    const int bh = innerH - gap * 2;
    if (i < bars) M5.Display.fillRect(bx, by, barW, bh, color);
    else M5.Display.drawRect(bx, by, barW, bh, color);
  }

  if (charging) {
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    M5.Display.fillTriangle(cx - 3, cy - 8, cx + 1, cy - 8, cx - 1, cy - 1, color);
    M5.Display.fillTriangle(cx + 1, cy - 1, cx - 2, cy + 8, cx + 4, cy + 1, color);
  }
}
