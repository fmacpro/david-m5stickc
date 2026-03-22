#include "cloud_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "app_utils.h"

namespace {
constexpr time_t kMinValidUnix = 1700000000;

String canonicalPathOnly(const String& path) {
  const int q = path.indexOf('?');
  if (q < 0) return path;
  return path.substring(0, q);
}
}

bool cloudSyncTime(CloudLogFn log_fn) {
  if (log_fn) log_fn("[NTP] Starting time sync");
  // UK timezone with daylight saving (GMT/BST).
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  const unsigned long start = millis();
  time_t now = time(nullptr);
  while (now < kMinValidUnix && millis() - start < 10000) {
    delay(200);
    now = time(nullptr);
  }
  const bool ok = now >= kMinValidUnix;
  if (log_fn) log_fn(String("[NTP] ") + (ok ? "OK" : "FAILED") + String(" unix=") + String(static_cast<long>(now)));
  return ok;
}

bool cloudSignedPost(
    const CloudClientConfig& cfg,
    const String& path,
    const char* content_type,
    uint8_t* body,
    size_t body_len,
    String& payload,
    int& code,
    String& err,
    CloudLogFn log_fn) {
  if (WiFi.status() != WL_CONNECTED) {
    err = "WiFi disconnected";
    return false;
  }

  time_t now = time(nullptr);
  if (now < kMinValidUnix && !cloudSyncTime(log_fn)) {
    err = "NTP sync failed";
    return false;
  }
  now = time(nullptr);

  const String body_hash = sha256Hex(body, body_len);
  const String ts = String(static_cast<long>(now));
  const String nonce = nonceHex();
  const String canonical = ts + "." + nonce + ".POST." + canonicalPathOnly(path) + "." + body_hash;
  const String signature = hmacSha256Hex(cfg.device_shared_secret, canonical);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  const String url = String(cfg.api_base_url) + path;
  if (!http.begin(client, url)) {
    err = "http begin failed";
    return false;
  }
  http.addHeader("Content-Type", content_type);
  http.addHeader("x-device-id", cfg.device_id);
  http.addHeader("x-timestamp", ts);
  http.addHeader("x-nonce", nonce);
  http.addHeader("x-signature", signature);

  code = http.POST(body, body_len);
  payload = http.getString();
  http.end();
  if (log_fn) log_fn(String("[HTTP] ") + path + " -> " + String(code));
  return true;
}
