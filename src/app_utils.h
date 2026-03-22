#pragma once

#include <Arduino.h>

String truncateForScreen(const String& in, size_t max_len);
String jsonEscape(const String& in);
String urlDecode(const String& in);
String sha256Hex(const uint8_t* data, size_t len);
String hmacSha256Hex(const String& key, const String& input);
String nonceHex();
