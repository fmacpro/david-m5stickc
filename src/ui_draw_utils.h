#pragma once

#include <Arduino.h>
#include <M5Unified.h>

void drawCenteredScript(const String& script, uint16_t color);
void drawCenteredText(const String& text, int y, int size, uint16_t color);
void drawCenteredFitText(
    const String& text,
    int y,
    int max_width,
    int max_size,
    int min_size,
    uint16_t color);
void drawFitTextInBox(
    const String& text,
    int box_x,
    int box_y,
    int box_w,
    int max_size,
    int min_size,
    uint16_t color);
void drawFitTextInRect(
    const String& text,
    int x,
    int y,
    int w,
    int h,
    int max_size,
    int min_size,
    uint16_t color);
void drawBatteryIcon(int x, int y, int w, int h, int percent, bool charging, uint16_t color);
