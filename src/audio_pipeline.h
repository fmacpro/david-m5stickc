#pragma once

#include <Arduino.h>

void writeWavHeader(uint8_t* out, uint32_t data_bytes, uint32_t sample_rate);
void processMicPcmInPlace(int16_t* pcm, size_t n, int& avg_abs_out, int& peak_out);
void processPlaybackWavInPlace(uint8_t* wav, size_t wav_len);
uint8_t estimateSpeakingLevel(const uint8_t* wav, size_t wav_len, unsigned long elapsed_ms);
