/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * TTS client — EdgeTTS (OpenAI-compatible) text-to-speech via HTTP
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;       // TTS server host (see secrets.h)
    uint16_t port;          // TTS server port (e.g., 5050)
    const char *api_key;    // API key (can be dummy for EdgeTTS)
    const char *voice;      // Voice name (e.g., "alloy", "echo", "nova")
    const char *model;      // Model name (e.g., "tts-1")
} tts_config_t;

typedef struct {
    uint32_t request_to_headers_ms;
    uint32_t request_to_first_audio_ms;
    uint32_t total_ms;
    size_t total_mp3_bytes;
    size_t total_pcm_samples;
} tts_stats_t;

// Initialize the TTS client
esp_err_t tts_init(const tts_config_t *config);

// Speak text: fetches audio from TTS server and plays through speaker.
// This is a blocking call — it streams and plays audio in chunks.
// Returns ESP_OK when playback is complete.
esp_err_t tts_speak(const char *text);

// Stop any current playback
void tts_stop(void);

// Check if TTS is currently playing
bool tts_is_playing(void);

// Copy the last completed TTS timing stats.
void tts_get_last_stats(tts_stats_t *out);

#ifdef __cplusplus
}
#endif
