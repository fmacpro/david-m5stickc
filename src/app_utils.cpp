#include "app_utils.h"

#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

static String hexFromBytes(const uint8_t* bytes, size_t len) {
  static const char* hex = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[(bytes[i] >> 4) & 0x0F];
    out += hex[bytes[i] & 0x0F];
  }
  return out;
}

String truncateForScreen(const String& in, size_t max_len) {
  if (in.length() <= max_len) return in;
  return in.substring(0, max_len - 3) + "...";
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String urlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    if (c == '%' && i + 2 < in.length()) {
      char h1 = in[i + 1];
      char h2 = in[i + 2];
      auto hexVal = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
      };
      int v1 = hexVal(h1);
      int v2 = hexVal(h2);
      if (v1 >= 0 && v2 >= 0) {
        out += static_cast<char>((v1 << 4) | v2);
        i += 2;
        continue;
      }
    } else if (c == '+') {
      out += ' ';
      continue;
    }
    out += c;
  }
  return out;
}

String sha256Hex(const uint8_t* data, size_t len) {
  uint8_t hash[32];
  mbedtls_sha256_ret(data, len, hash, 0);
  return hexFromBytes(hash, sizeof(hash));
}

String hmacSha256Hex(const String& key, const String& input) {
  uint8_t hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const uint8_t*>(key.c_str()), key.length());
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t*>(input.c_str()), input.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  return hexFromBytes(hmac, sizeof(hmac));
}

String nonceHex() {
  const uint32_t a = esp_random();
  const uint32_t b = esp_random();
  char buf[17];
  snprintf(buf, sizeof(buf), "%08lx%08lx", static_cast<unsigned long>(a), static_cast<unsigned long>(b));
  return String(buf);
}
