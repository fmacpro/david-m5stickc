#include "audio_pipeline.h"

#include <math.h>
#include <string.h>

namespace {

constexpr float kPlaybackNoiseGateNorm = 0.006f;

void writeLE16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

void writeLE32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

uint16_t readLE16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int16_t readI16LE(const uint8_t* p) {
  const uint16_t u = readLE16(p);
  return static_cast<int16_t>(u);
}

int16_t clampI16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return static_cast<int16_t>(v);
}

float softCompressor(float x) {
  const float ax = (x < 0.0f) ? -x : x;
  const float knee = 0.55f;
  if (ax <= knee) return x;
  const float sign = (x < 0.0f) ? -1.0f : 1.0f;
  const float y = knee + (ax - knee) * 0.35f;
  return sign * y;
}

}  // namespace

void writeWavHeader(uint8_t* out, uint32_t data_bytes, uint32_t sample_rate) {
  memcpy(out + 0, "RIFF", 4);
  writeLE32(out + 4, 36 + data_bytes);
  memcpy(out + 8, "WAVE", 4);
  memcpy(out + 12, "fmt ", 4);
  writeLE32(out + 16, 16);
  writeLE16(out + 20, 1);
  writeLE16(out + 22, 1);
  writeLE32(out + 24, sample_rate);
  writeLE32(out + 28, sample_rate * 2);
  writeLE16(out + 32, 2);
  writeLE16(out + 34, 16);
  memcpy(out + 36, "data", 4);
  writeLE32(out + 40, data_bytes);
}

void processMicPcmInPlace(int16_t* pcm, size_t n, int& avg_abs_out, int& peak_out) {
  if (!pcm || n == 0) {
    avg_abs_out = 0;
    peak_out = 0;
    return;
  }

  int64_t sum = 0;
  int64_t sum_abs = 0;
  int peak = 0;
  for (size_t i = 0; i < n; ++i) {
    const int v = pcm[i];
    sum += v;
    const int a = (v < 0) ? -v : v;
    sum_abs += a;
    if (a > peak) peak = a;
  }
  const int dc = static_cast<int>(sum / static_cast<int64_t>(n));
  const int avg_abs = static_cast<int>(sum_abs / static_cast<int64_t>(n));

  // Keep mic cleanup gentle for STT: aggressive gates can erase quieter
  // consonants/syllables in the middle of phrases.
  const int gate = constrain(avg_abs / 8, 40, 280);
  const int fade_start = gate / 2;

  int64_t out_sum_abs = 0;
  int out_peak = 0;
  for (size_t i = 0; i < n; ++i) {
    int v = static_cast<int>(pcm[i]) - dc;
    int a = (v < 0) ? -v : v;
    if (a < fade_start) {
      v = (v * 3) / 4;
    } else if (a < gate) {
      const int scaled = (a * 85) / 100;
      v = (v < 0) ? -scaled : scaled;
    }
    pcm[i] = clampI16(v);
    a = (v < 0) ? -v : v;
    out_sum_abs += a;
    if (a > out_peak) out_peak = a;
  }

  float gain = 1.0f;
  if (out_peak > 0) {
    const float target_peak = 12000.0f;
    gain = target_peak / static_cast<float>(out_peak);
  }
  if (gain < 0.8f) gain = 0.8f;
  if (gain > 2.4f) gain = 2.4f;

  out_sum_abs = 0;
  out_peak = 0;
  for (size_t i = 0; i < n; ++i) {
    float x = static_cast<float>(pcm[i]) / 32768.0f;
    x *= gain;
    x = softCompressor(x);
    x *= 0.92f;
    int v = static_cast<int>(x * 32767.0f);
    pcm[i] = clampI16(v);
    const int a = (v < 0) ? -v : v;
    out_sum_abs += a;
    if (a > out_peak) out_peak = a;
  }

  avg_abs_out = static_cast<int>(out_sum_abs / static_cast<int64_t>(n));
  peak_out = out_peak;
}

void processPlaybackWavInPlace(uint8_t* wav, size_t wav_len) {
  if (!wav || wav_len < 44) return;
  if (memcmp(wav + 0, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return;
  if (memcmp(wav + 12, "fmt ", 4) != 0 || memcmp(wav + 36, "data", 4) != 0) return;
  // Keep cloud TTS audio untouched. On-device compression/gating can introduce
  // audible distortion on the tiny M5 speaker path.
}

uint8_t estimateSpeakingLevel(const uint8_t* wav, size_t wav_len, unsigned long elapsed_ms) {
  if (!wav || wav_len < 44) return 0;
  if (memcmp(wav + 0, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return 0;
  if (memcmp(wav + 12, "fmt ", 4) != 0 || memcmp(wav + 36, "data", 4) != 0) return 0;
  const uint16_t channels = readLE16(wav + 22);
  const uint32_t sample_rate = readLE32(wav + 24);
  const uint16_t bits = readLE16(wav + 34);
  const size_t data_len = static_cast<size_t>(readLE32(wav + 40));
  if (channels != 1 || sample_rate == 0) return 0;
  if (data_len > (wav_len - 44)) return 0;
  const uint8_t* data = wav + 44;

  uint32_t total_samples = 0;
  if (bits == 8) total_samples = static_cast<uint32_t>(data_len);
  else if (bits == 16) {
    if ((data_len & 1u) != 0) return 0;
    total_samples = static_cast<uint32_t>(data_len / 2u);
  }
  else return 0;
  if (total_samples == 0) return 0;

  uint32_t center = static_cast<uint32_t>((static_cast<uint64_t>(elapsed_ms) * sample_rate) / 1000ULL);
  if (center >= total_samples) center = total_samples - 1;
  const uint32_t window = sample_rate / 80 + 1;
  const uint32_t start = (center > window) ? (center - window) : 0;
  const uint32_t end = ((center + window) < total_samples) ? (center + window) : (total_samples - 1);
  if (end <= start) return 0;

  uint64_t sum_abs = 0;
  uint32_t count = 0;
  if (bits == 8) {
    for (uint32_t i = start; i <= end; ++i) {
      int v = static_cast<int>(data[i]) - 128;
      if (v < 0) v = -v;
      sum_abs += static_cast<uint32_t>(v);
      ++count;
    }
    if (count == 0) return 0;
    const int avg = static_cast<int>(sum_abs / count);
    int level = (avg * 100) / 70;
    level = constrain(level, 0, 100);
    return static_cast<uint8_t>(level);
  }

  for (uint32_t i = start; i <= end; ++i) {
    int v = static_cast<int>(readI16LE(data + static_cast<size_t>(i) * 2u));
    if (v < 0) v = -v;
    sum_abs += static_cast<uint32_t>(v);
    ++count;
  }
  if (count == 0) return 0;
  const int avg = static_cast<int>(sum_abs / count);
  int level = (avg * 100) / 11000;
  level = constrain(level, 0, 100);
  return static_cast<uint8_t>(level);
}
