#include "cloud_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include <time.h>

#include "app_utils.h"

namespace {
constexpr time_t kMinValidUnix = 1700000000;
constexpr const char* kTzRuleUk = "GMT0BST,M3.5.0/1,M10.5.0/2";

String canonicalPathOnly(const String& path) {
  const int q = path.indexOf('?');
  if (q < 0) return path;
  return path.substring(0, q);
}

int monthFromAbbrev(const char* mon) {
  if (!mon) return -1;
  if (strcmp(mon, "Jan") == 0) return 1;
  if (strcmp(mon, "Feb") == 0) return 2;
  if (strcmp(mon, "Mar") == 0) return 3;
  if (strcmp(mon, "Apr") == 0) return 4;
  if (strcmp(mon, "May") == 0) return 5;
  if (strcmp(mon, "Jun") == 0) return 6;
  if (strcmp(mon, "Jul") == 0) return 7;
  if (strcmp(mon, "Aug") == 0) return 8;
  if (strcmp(mon, "Sep") == 0) return 9;
  if (strcmp(mon, "Oct") == 0) return 10;
  if (strcmp(mon, "Nov") == 0) return 11;
  if (strcmp(mon, "Dec") == 0) return 12;
  return -1;
}

// Howard Hinnant's civil date algorithm: days since 1970-01-01.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parseHttpDateToUnix(const String& date_hdr, time_t& unix_out) {
  // RFC7231: "Sun, 06 Nov 1994 08:49:37 GMT"
  char wk[8] = {0};
  char mon[8] = {0};
  int day = 0;
  int year = 0;
  int hh = 0, mm = 0, ss = 0;
  const int fields = sscanf(
      date_hdr.c_str(),
      "%7[^,], %d %7s %d %d:%d:%d GMT",
      wk,
      &day,
      mon,
      &year,
      &hh,
      &mm,
      &ss);
  if (fields != 7) return false;
  const int month = monthFromAbbrev(mon);
  if (month < 1 || day < 1 || day > 31) return false;
  if (year < 2000 || year > 2100) return false;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return false;

  const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const int64_t secs = days * 86400LL + static_cast<int64_t>(hh) * 3600LL +
                       static_cast<int64_t>(mm) * 60LL + static_cast<int64_t>(ss);
  if (secs < 0) return false;
  unix_out = static_cast<time_t>(secs);
  return true;
}

bool setUnixClock(time_t unix_ts) {
  if (unix_ts < kMinValidUnix) return false;
  struct timeval tv;
  tv.tv_sec = unix_ts;
  tv.tv_usec = 0;
  return settimeofday(&tv, nullptr) == 0;
}

bool syncTimeFromHttpDate(const char* base_url, CloudLogFn log_fn) {
  if (!base_url || !base_url[0]) return false;
  String url = String(base_url);
  if (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/v1/health";

  if (log_fn) log_fn(String("[NTP] fallback via HTTP Date: ") + url);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  const char* header_keys[] = {"Date"};
  http.collectHeaders(header_keys, 1);
  const int code = http.GET();
  const String date_hdr = http.header("Date");
  http.end();
  if (code <= 0 || date_hdr.length() == 0) return false;

  time_t unix_ts = 0;
  if (!parseHttpDateToUnix(date_hdr, unix_ts)) return false;
  if (!setUnixClock(unix_ts)) return false;

  setenv("TZ", kTzRuleUk, 1);
  tzset();
  if (log_fn) log_fn(String("[NTP] HTTP Date OK unix=") + String(static_cast<long>(unix_ts)));
  return true;
}
}

bool cloudSyncTime(CloudLogFn log_fn, const char* http_time_url) {
  if (log_fn) log_fn("[NTP] Starting time sync");
  // UK timezone with daylight saving (GMT/BST).
  setenv("TZ", kTzRuleUk, 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  const unsigned long start = millis();
  time_t now = time(nullptr);
  while (now < kMinValidUnix && millis() - start < 15000) {
    delay(200);
    now = time(nullptr);
  }
  if (now >= kMinValidUnix) {
    if (log_fn) log_fn(String("[NTP] OK unix=") + String(static_cast<long>(now)));
    return true;
  }
  if (log_fn) log_fn(String("[NTP] FAILED unix=") + String(static_cast<long>(now)));

  if (syncTimeFromHttpDate(http_time_url, log_fn)) {
    now = time(nullptr);
    if (now >= kMinValidUnix) return true;
  }
  return false;
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
  if (now < kMinValidUnix && !cloudSyncTime(log_fn, cfg.api_base_url)) {
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
