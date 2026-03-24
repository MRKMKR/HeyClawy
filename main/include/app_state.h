/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * Shared application state — event groups, helpers, state transitions
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ui.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event bits */
#define WIFI_CONNECTED_BIT  BIT0
#define OC_CONNECTED_BIT    BIT1
#define KNOB_PRESSED_BIT    BIT2
#define TTS_PLAY_BIT        BIT3
#define WEBSERVER_TOGGLE_BIT BIT4
#define DETAILS_BIT         BIT5
#define CANCEL_BIT          BIT6
#define TOUCH_BIT           BIT7
#define TASKS_SCREEN_BIT    BIT8
#define CAMERA_BIT          BIT9
#define WAKE_WORD_BIT       BIT10

/** Global event group — created in app_main */
extern EventGroupHandle_t g_app_events;

/** Recording flag */
extern bool g_recording;

/** Response shown timestamp (for auto-return to IDLE) */
extern int64_t g_response_shown_at;

/** Pending camera image for next voice chat (PSRAM-allocated, caller frees) */
extern uint8_t *g_pending_jpeg;
extern size_t   g_pending_jpeg_size;

/** Auto-listen flag: set true when OpenClaw expects a follow-up reply */
extern bool g_continue_listening;

/** TTS pending flag: set true when TTS_PLAY_BIT is queued, prevents premature dismissal */
extern volatile bool g_tts_pending;

/** Shared queued TTS text buffer (notifications + streaming speech chunks). */
extern char g_tts_text[1024];

typedef struct {
    uint32_t capture_ms;
    uint32_t stt_ms;
    uint32_t openclaw_first_delta_ms;
    uint32_t openclaw_final_ms;
    uint32_t tts_headers_ms;
    uint32_t tts_first_audio_ms;
    uint32_t tts_total_ms;
    size_t recorded_samples;
    size_t sent_samples;
    size_t trimmed_leading_samples;
    size_t trimmed_trailing_samples;
} app_latency_stats_t;

/** Transition to a new UI state + update RGB LED */
void app_set_state(ui_state_t st);

/** LED color for given UI state */
void app_led_for_state(ui_state_t st);

/** Chat response callback (used by voice_chat and serial_cmd) */
void app_on_chat_response(const char *text, bool is_final);

/** Reset turn timing and streaming-speech state before sending a new request. */
void app_turn_reset(void);

/** Mark recording complete and store trimming stats for later logging. */
void app_turn_mark_capture(size_t recorded_samples, size_t sent_samples,
                           size_t trimmed_leading_samples, size_t trimmed_trailing_samples);

/** Mark STT timing around the transcription call. */
void app_turn_mark_stt_start(void);
void app_turn_mark_stt_done(void);

/** Mark when the request was handed off to OpenClaw. */
void app_turn_mark_openclaw_sent(void);

/** Mark TTS lifecycle around each spoken chunk. */
void app_turn_mark_tts_start(void);
void app_turn_mark_tts_done(void);

/** Copy the latest latency stats for logging/debugging. */
void app_turn_get_latency_stats(app_latency_stats_t *out);

#ifdef __cplusplus
}
#endif
