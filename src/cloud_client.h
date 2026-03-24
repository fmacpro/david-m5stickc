#pragma once

#include <Arduino.h>

struct CloudClientConfig {
  const char* api_base_url;
  const char* device_id;
  const char* device_shared_secret;
};

using CloudLogFn = void (*)(const String&);

bool cloudSyncTime(CloudLogFn log_fn = nullptr, const char* http_time_url = nullptr);
bool cloudSignedPost(
    const CloudClientConfig& cfg,
    const String& path,
    const char* content_type,
    uint8_t* body,
    size_t body_len,
    String& payload,
    int& code,
    String& err,
    CloudLogFn log_fn = nullptr);
