/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * Settings — NVS-backed runtime configuration
 *
 * All device settings are stored in NVS and loaded on boot.
 * Compile-time defaults from secrets.h are used as initial values.
 * Settings can be changed at runtime via the web interface.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Settings struct ─────────────────────────────────────────────────── */

typedef struct {
    /* WiFi */
    char     wifi_ssid[64];
    char     wifi_password[64];

    /* OpenClaw Gateway */
    char     oc_host[128];
    uint16_t oc_port;
    char     oc_token[128];
    char     oc_device_key[128];    /* ED25519 seed hex */

    /* TTS (EdgeTTS / OpenAI-compatible) */
    char     tts_host[128];
    uint16_t tts_port;
    char     tts_api_key[128];
    char     tts_voice[32];

    /* STT (faster-whisper) */
    char     stt_host[128];
    uint16_t stt_port;

    /* Audio */
    uint8_t  volume;               /* 0-100 */
    uint16_t silence_timeout_ms;   /* silence detection */
    uint16_t silence_threshold;    /* RMS threshold */
    uint8_t  max_record_seconds;   /* max recording duration */
    uint16_t no_speech_timeout_ms; /* abort if no speech detected */
    bool     auto_read_response;   /* auto TTS every response */
    bool     short_response;       /* instruct OC for 1-3 word replies */

    /* Display */
    uint8_t  brightness;           /* 0-100 */
    bool     eyes_default_view;    /* idle opens on eyes screen instead of action screen */

    /* RGB LED */
    bool     rgb_enabled;
    uint8_t  startup_pattern;      /* 0=rainbow,1=aurora,2=stars,3=fire,4=ocean */

    /* Power */
    uint32_t sleep_timeout_ms;     /* 0 = disabled */
    bool     wake_word_in_sleep;   /* keep wake word active during light sleep */

    /* Web server */
    bool     webserver_enabled;    /* persisted on/off preference */

    /* Activity carousel + notifications */
    bool     activity_carousel;    /* carousel between active runs on display */
    bool     auto_notify;          /* auto-TTS non-device-initiated messages */

    /* Logging */
    uint8_t  log_verbosity;        /* 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=VERBOSE */
} settings_t;

/**
 * Initialize settings subsystem: load from NVS (or use defaults).
 * Must be called early in app_main, before other components.
 * @param defaults  If non-NULL, used as initial values when NVS is empty.
 *                  Typically populated from compile-time secrets.h values.
 *                  If NULL, empty/zero defaults are used.
 */
esp_err_t settings_init(const settings_t *defaults);

/**
 * Get pointer to current settings (read-only access).
 * Thread-safe to read fields; use settings_set_*() to modify.
 */
const settings_t *settings_get(void);

/**
 * Get mutable pointer (for bulk updates from web UI).
 * Caller must call settings_save() after modifications.
 */
settings_t *settings_get_mutable(void);

/**
 * Save current settings to NVS. Call after modifying via get_mutable().
 */
esp_err_t settings_save(void);

/**
 * Reset all settings to compile-time defaults and save.
 */
esp_err_t settings_reset(void);

/**
 * Export settings as JSON string. Caller must free() returned buffer.
 * Secrets (passwords, keys) are masked with "****" unless include_secrets is true.
 */
char *settings_to_json(bool include_secrets);

/**
 * Import settings from JSON string. Only updates fields present in JSON.
 * Does NOT auto-save — call settings_save() after.
 */
esp_err_t settings_from_json(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
