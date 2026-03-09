/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * OpenClaw Gateway WebSocket client
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENCLAW_STATE_DISCONNECTED = 0,
    OPENCLAW_STATE_CONNECTING,
    OPENCLAW_STATE_AUTHENTICATING,
    OPENCLAW_STATE_CONNECTED,
    OPENCLAW_STATE_CHAT_SENDING,
    OPENCLAW_STATE_CHAT_THINKING,
    OPENCLAW_STATE_CHAT_STREAMING,
    OPENCLAW_STATE_ERROR,
} openclaw_state_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *token;
    const char *device_key_hex; // 64-char hex string: ED25519 seed (32 bytes)
    const char *session_key;
} openclaw_config_t;

/* Individual cron job info */
#define OC_MAX_TASKS 16
typedef struct {
    char id[40];             /* cron job UUID (36 chars + null) */
    char name[48];           /* job name */
    bool enabled;            /* is job enabled */
    bool running;            /* state.runningAtMs > 0 */
    int64_t running_at_ms;   /* when job started running (epoch ms), 0 if not */
    int64_t next_run_at_ms;  /* next scheduled run (epoch ms), 0 if unknown */
    int64_t last_run_at_ms;  /* last run time (epoch ms) */
    int last_duration_ms;    /* last run duration ms */
    char last_status[12];    /* "ok" / "error" / "skipped" */
    char last_error[80];     /* last error message if any */
    char schedule_kind[8];   /* "every" / "cron" / "at" */
    char schedule_expr[32];  /* cron expr or "every Nms" */
    int consecutive_errors;  /* consecutive error count */
} openclaw_task_t;

/* Individual active run tracking (for carousel display) */
#define OC_MAX_ACTIVE_RUNS 8
typedef struct {
    char run_id[48];         /* runId from agent events */
    char session_key[48];    /* sessionKey */
    char detail[64];         /* "Memory..." / "Exec..." / "Thinking" */
    char source[24];         /* "cron" / "tui" / "whatsapp" / "device" / "hooks" */
    int64_t started_ms;      /* epoch ms when this run started */
    bool active;             /* slot in use */
} openclaw_active_run_t;

/* Snapshot data extracted from connect response and health events */
typedef struct {
    bool ok;                 /* health.ok */
    uint32_t uptime_min;     /* server uptime in minutes */
    char version[24];        /* server version string */
    char wa_status[16];      /* "on" / "off" / "linked" */
    char agent_id[16];       /* default agent id */
    int session_count;       /* number of sessions */
    int last_activity_min;   /* minutes since last session activity */
    int last_activity_sec;   /* seconds since last session activity (finer granularity) */
    /* Usage/cost data from usage.status */
    char provider_name[24];  /* e.g. "anthropic", "openai-codex" */
    int usage_percent;       /* 0-100, first provider's first window */
    char usage_label[32];    /* e.g. "Monthly usage" */
    bool has_usage;          /* true after first usage.status response with data */
    bool usage_empty;        /* true if no providers configured */
    /* Token totals from sessions data */
    int total_tokens_k;      /* total tokens across sessions, in thousands */
    /* Cron/tasks data from cron.list */
    char task_summary[64];   /* e.g. "3 tasks (2 active)" */
    bool has_tasks;          /* true after first cron.list response */
    openclaw_task_t tasks[OC_MAX_TASKS];
    int task_count;          /* number of tasks in array */
    int tasks_running;       /* number currently running */
    int tasks_active;        /* number enabled */
    /* Real-time activity tracking from WebSocket events */
    bool is_active;              /* ANY run in progress (device or external) */
    bool is_external;            /* current activity is from external source */
    char active_source[24];      /* "whatsapp" / "tui" / "hooks" / "cron" / "device" */
    char active_run_id[48];      /* current runId (for abort tracking) */
    char active_session_key[48]; /* session key (for chat.abort) */
    char active_detail[64];      /* "Using: web_search" / "Thinking..." */
    int64_t active_started_ms;   /* when activity started (epoch ms, for timer) */
    int active_count;            /* number of concurrent active runs */
    /* Multi-run carousel tracking */
    openclaw_active_run_t active_runs[OC_MAX_ACTIVE_RUNS];
    int active_runs_count;       /* number of active runs in array */
    int carousel_index;          /* current display index for carousel rotation */
} openclaw_info_t;

// Callback for chat response chunks
typedef void (*openclaw_chat_cb_t)(const char *text, bool is_final);

// Callback for state changes
typedef void (*openclaw_state_cb_t)(openclaw_state_t state);

// Callback for incoming notifications (non-device-initiated messages)
typedef void (*openclaw_notify_cb_t)(const char *text, const char *source);

esp_err_t openclaw_init(const openclaw_config_t *config, openclaw_state_cb_t state_cb);
void openclaw_set_notify_cb(openclaw_notify_cb_t cb);
esp_err_t openclaw_connect(void);
esp_err_t openclaw_disconnect(void);
openclaw_state_t openclaw_get_state(void);

// Send a text chat message and receive streaming response via callback
esp_err_t openclaw_chat_send(const char *message, openclaw_chat_cb_t response_cb);

// Send text + JPEG image to OpenClaw
esp_err_t openclaw_chat_send_with_image(const char *message,
                                         const uint8_t *jpeg, size_t jpeg_size,
                                         openclaw_chat_cb_t response_cb);

// Send audio (WAV) with optional text to OpenClaw for transcription + response
esp_err_t openclaw_chat_send_audio(const int16_t *pcm, size_t num_samples,
                                    int sample_rate, const char *text,
                                    openclaw_chat_cb_t response_cb);

// Send audio + optional JPEG image to OpenClaw
esp_err_t openclaw_chat_send_audio_with_image(const int16_t *pcm, size_t num_samples,
                                               int sample_rate, const char *text,
                                               const uint8_t *jpeg, size_t jpeg_size,
                                               openclaw_chat_cb_t response_cb);

// Request full details about the last response (follow-up)
esp_err_t openclaw_chat_send_details(openclaw_chat_cb_t response_cb);

// Request usage/cost data from OpenClaw
esp_err_t openclaw_request_usage(void);

// Request health data from OpenClaw
esp_err_t openclaw_request_health(void);

/** Poll OpenClaw cron jobs (background tasks) */
esp_err_t openclaw_request_tasks(void);

/** Add a cron job */
esp_err_t openclaw_cron_add(const char *name, const char *schedule_expr,
                             const char *payload_text);

/** Remove a cron job by ID */
esp_err_t openclaw_cron_remove(const char *job_id);

/** Manually run a cron job */
esp_err_t openclaw_cron_run(const char *job_id);

/** Abort a running chat operation (device-initiated) */
esp_err_t openclaw_chat_abort(void);

/** Abort a running chat by session key (any source) */
esp_err_t openclaw_chat_abort_session(const char *session_key);

/** Toggle a cron job enabled/disabled */
esp_err_t openclaw_cron_toggle(const char *job_id, bool enabled);

// Get the accumulated full response text (valid after final callback)
const char *openclaw_get_last_response(void);

// Get elapsed time since chat was sent (ms)
uint32_t openclaw_get_thinking_time_ms(void);

// Get server info snapshot (uptime, channels, agent)
const openclaw_info_t *openclaw_get_info(void);

// Check and clear external activity flag (for wake-from-sleep)
bool openclaw_consume_external_activity(void);

#ifdef __cplusplus
}
#endif
