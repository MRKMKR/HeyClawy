/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * App state helpers — LED mapping, state transitions, shared globals
 */

#include "app_state.h"
#include "board.h"
#include "ui.h"
#include "openclaw_client.h"
#include "settings.h"
#include "webserver.h"
#include "tts_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_state";

/* Globals */
EventGroupHandle_t g_app_events = NULL;
bool g_recording = false;
int64_t g_response_shown_at = 0;
uint8_t *g_pending_jpeg = NULL;
size_t   g_pending_jpeg_size = 0;
bool g_continue_listening = false;
volatile bool g_tts_pending = false;

static struct {
    int64_t turn_start_us;
    int64_t capture_done_us;
    int64_t stt_start_us;
    int64_t stt_done_us;
    int64_t openclaw_sent_us;
    int64_t openclaw_first_delta_us;
    int64_t openclaw_final_us;
    int64_t tts_start_us;
    app_latency_stats_t stats;
} s_turn = {0};

static struct {
    bool active;
    char raw[2048];
    size_t raw_len;
    size_t spoken_len;
} s_stream_tts = {0};

static size_t trim_trailing_ws(char *text)
{
    size_t len = strlen(text);
    while (len > 0) {
        char ch = text[len - 1];
        if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') break;
        text[--len] = '\0';
    }
    return len;
}

static void queue_tts_chunk(const char *text)
{
    if (!text || !text[0]) return;

    char chunk[512];
    strncpy(chunk, text, sizeof(chunk) - 1);
    chunk[sizeof(chunk) - 1] = '\0';
    if (trim_trailing_ws(chunk) == 0) return;

    size_t cur_len = strlen(g_tts_text);
    size_t add_len = strlen(chunk);
    if (cur_len >= sizeof(g_tts_text) - 2) return;

    if (cur_len > 0 && g_tts_text[cur_len - 1] != ' ' && g_tts_text[cur_len - 1] != '\n') {
        g_tts_text[cur_len++] = ' ';
        g_tts_text[cur_len] = '\0';
    }

    size_t space = sizeof(g_tts_text) - cur_len - 1;
    if (add_len > space) add_len = space;
    memcpy(g_tts_text + cur_len, chunk, add_len);
    g_tts_text[cur_len + add_len] = '\0';
    g_tts_pending = true;
    xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
}

static size_t find_speak_boundary(const char *text, size_t start_idx)
{
    if (!text) return 0;
    size_t len = strlen(text);
    size_t boundary = 0;

    for (size_t i = start_idx; i < len; i++) {
        char ch = text[i];
        if (ch == '[') break;  /* Wait for final response before speaking tags. */
        if (ch == '\n') {
            boundary = i + 1;
            continue;
        }
        if (ch == '.' || ch == '!' || ch == '?' || ch == ':' || ch == ';') {
            char next = (i + 1 < len) ? text[i + 1] : '\0';
            if (next == '\0' || next == ' ' || next == '\n' || next == '\r') {
                boundary = i + 1;
            }
        }
    }

    if (boundary <= start_idx) return 0;
    while (boundary > start_idx &&
           (text[boundary - 1] == '\n' || text[boundary - 1] == '\r' ||
            text[boundary - 1] == ' ' || text[boundary - 1] == '\t')) {
        boundary--;
    }
    return (boundary > start_idx) ? boundary : 0;
}

static void maybe_queue_streaming_sentence(void)
{
    const settings_t *cfg = settings_get();
#if BOARD_HAS_DISPLAY
    if (!cfg->auto_read_response) return;
#endif

    const char *nl = strchr(s_stream_tts.raw, '\n');
    if (!nl) return;  /* Wait until the short label is complete. */

    const char *long_text = nl + 1;
    while (*long_text == '\n' || *long_text == '\r') long_text++;
    size_t offset = (size_t)(long_text - s_stream_tts.raw);
    size_t boundary = find_speak_boundary(long_text, s_stream_tts.spoken_len);
    if (boundary == 0) return;

    size_t chunk_len = boundary - s_stream_tts.spoken_len;
    if (chunk_len == 0) return;

    char chunk[512];
    if (chunk_len >= sizeof(chunk)) chunk_len = sizeof(chunk) - 1;
    memcpy(chunk, long_text + s_stream_tts.spoken_len, chunk_len);
    chunk[chunk_len] = '\0';
    queue_tts_chunk(chunk);
    s_stream_tts.spoken_len = boundary;

    if (offset > 0) {
        ESP_LOGI(TAG, "Streaming speech queued: %u chars", (unsigned)chunk_len);
    }
}

static void log_latency_summary(void)
{
    ESP_LOGI(TAG,
             "Latency summary: capture=%ums stt=%ums oc_first=%ums oc_final=%ums tts_headers=%ums tts_first=%ums tts_total=%ums samples=%u->%u trim=%u/%u",
             (unsigned)s_turn.stats.capture_ms,
             (unsigned)s_turn.stats.stt_ms,
             (unsigned)s_turn.stats.openclaw_first_delta_ms,
             (unsigned)s_turn.stats.openclaw_final_ms,
             (unsigned)s_turn.stats.tts_headers_ms,
             (unsigned)s_turn.stats.tts_first_audio_ms,
             (unsigned)s_turn.stats.tts_total_ms,
             (unsigned)s_turn.stats.recorded_samples,
             (unsigned)s_turn.stats.sent_samples,
             (unsigned)s_turn.stats.trimmed_leading_samples,
             (unsigned)s_turn.stats.trimmed_trailing_samples);
}

/* ── LED helper ──────────────────────────────────────────────────────── */

/* Map startup_pattern setting (0-4) to RGB mode enum */
static board_rgb_mode_t startup_mode(void)
{
    uint8_t pat = settings_get()->startup_pattern;
    switch (pat) {
    case 1:  return RGB_MODE_AURORA;
    case 2:  return RGB_MODE_STARFIELD;
    case 3:  return RGB_MODE_FIRE;
    case 4:  return RGB_MODE_OCEAN;
    default: return RGB_MODE_RAINBOW_SPIN;
    }
}

void app_led_for_state(ui_state_t st)
{
#if BOARD_RGB_LED_COUNT > 1
    /* Multi-LED ring: rich animations */
    switch (st) {
    case UI_STATE_BOOT:
    case UI_STATE_CONNECTING:
        board_rgb_animate(startup_mode(), 0, 0, 0);
        break;
    case UI_STATE_IDLE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 4, 0);
        break;
    case UI_STATE_LISTENING:
        board_rgb_animate(RGB_MODE_PULSE_WAVE, 8, 40, 16);
        break;
    case UI_STATE_SENDING:
        board_rgb_animate(RGB_MODE_CHASE, 0, 16, 40);
        break;
    case UI_STATE_THINKING:
        board_rgb_animate(RGB_MODE_CHASE, 40, 24, 0);
        break;
    case UI_STATE_STREAMING:
        board_rgb_animate(RGB_MODE_SPARKLE, 32, 8, 48);
        break;
    case UI_STATE_RESPONSE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 8, 0);
        break;
    case UI_STATE_TTS_LOADING:
        board_rgb_animate(RGB_MODE_CHASE, 0, 24, 24);
        break;
    case UI_STATE_TTS_PLAYING:
        board_rgb_animate(RGB_MODE_BREATHE, 0, 32, 16);
        break;
    case UI_STATE_ERROR:
        board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
        break;
    default:
        board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
        break;
    }
#else
    /* Single LED: simple modes */
    switch (st) {
    case UI_STATE_IDLE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 4, 0);
        break;
    case UI_STATE_LISTENING:
        board_rgb_animate(RGB_MODE_SOLID, 32, 0, 0);
        break;
    case UI_STATE_SENDING:
        board_rgb_animate(RGB_MODE_SOLID, 0, 0, 32);
        break;
    case UI_STATE_THINKING:
        board_rgb_animate(RGB_MODE_BREATHE, 40, 24, 0);
        break;
    case UI_STATE_STREAMING:
        board_rgb_animate(RGB_MODE_BREATHE, 24, 0, 40);
        break;
    case UI_STATE_RESPONSE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 6, 0);
        break;
    case UI_STATE_TTS_LOADING:
        board_rgb_animate(RGB_MODE_BLINK, 0, 24, 24);
        break;
    case UI_STATE_TTS_PLAYING:
        board_rgb_animate(RGB_MODE_BREATHE, 0, 32, 16);
        break;
    case UI_STATE_ERROR:
        board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
        break;
    default:
        board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
        break;
    }
#endif
}

void app_set_state(ui_state_t st)
{
    ui_set_state(st);
    app_led_for_state(st);
}

void app_turn_reset(void)
{
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.turn_start_us = esp_timer_get_time();
    memset(&s_stream_tts, 0, sizeof(s_stream_tts));
    g_tts_text[0] = '\0';
}

void app_turn_mark_capture(size_t recorded_samples, size_t sent_samples,
                           size_t trimmed_leading_samples, size_t trimmed_trailing_samples)
{
    s_turn.capture_done_us = esp_timer_get_time();
    s_turn.stats.capture_ms = (uint32_t)((s_turn.capture_done_us - s_turn.turn_start_us) / 1000);
    s_turn.stats.recorded_samples = recorded_samples;
    s_turn.stats.sent_samples = sent_samples;
    s_turn.stats.trimmed_leading_samples = trimmed_leading_samples;
    s_turn.stats.trimmed_trailing_samples = trimmed_trailing_samples;
}

void app_turn_mark_stt_start(void)
{
    s_turn.stt_start_us = esp_timer_get_time();
}

void app_turn_mark_stt_done(void)
{
    s_turn.stt_done_us = esp_timer_get_time();
    if (s_turn.stt_start_us > 0) {
        s_turn.stats.stt_ms = (uint32_t)((s_turn.stt_done_us - s_turn.stt_start_us) / 1000);
    }
}

void app_turn_mark_openclaw_sent(void)
{
    s_turn.openclaw_sent_us = esp_timer_get_time();
}

void app_turn_mark_tts_start(void)
{
    s_turn.tts_start_us = esp_timer_get_time();
}

void app_turn_mark_tts_done(void)
{
    tts_stats_t stats = {0};
    tts_get_last_stats(&stats);
    s_turn.stats.tts_headers_ms = stats.request_to_headers_ms;
    s_turn.stats.tts_first_audio_ms = stats.request_to_first_audio_ms;
    s_turn.stats.tts_total_ms = stats.total_ms;
    log_latency_summary();
}

void app_turn_get_latency_stats(app_latency_stats_t *out)
{
    if (!out) return;
    *out = s_turn.stats;
}

/* ── Device command parser ───────────────────────────────────────────── */
/* Parses and executes [DEVICE:key=value] commands from OpenClaw response.
 * Strips processed command lines from buf in-place.
 * Returns number of commands executed. */
static int parse_device_commands(char *buf)
{
    int count = 0;
    char *pos = buf;

    while ((pos = strstr(pos, "[DEVICE:")) != NULL) {
        char *start = pos;
        char *end = strchr(pos, ']');
        if (!end) break;

        /* Extract key=value from [DEVICE:key=value] */
        const char *kv = pos + 8;  /* skip "[DEVICE:" */
        size_t kv_len = end - kv;
        char cmd[64];
        if (kv_len >= sizeof(cmd)) { pos = end + 1; continue; }
        memcpy(cmd, kv, kv_len);
        cmd[kv_len] = '\0';

        /* Split key=value */
        char *eq = strchr(cmd, '=');
        const char *key = cmd;
        const char *val = "";
        if (eq) {
            *eq = '\0';
            val = eq + 1;
        }

        ESP_LOGI(TAG, "Device cmd: %s=%s", key, val);
        settings_t *cfg = settings_get_mutable();

        if (strcmp(key, "volume") == 0) {
            int v = atoi(val);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            cfg->volume = (uint8_t)v;
            board_audio_set_volume(v);
            settings_save();
            ESP_LOGI(TAG, "Volume set to %d%%", v);
            count++;
        } else if (strcmp(key, "brightness") == 0) {
            int v = atoi(val);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            cfg->brightness = (uint8_t)v;
            board_display_set_brightness(v);
            settings_save();
            ESP_LOGI(TAG, "Brightness set to %d%%", v);
            count++;
        } else if (strcmp(key, "rgb") == 0) {
            if (strcmp(val, "off") == 0) {
                cfg->rgb_enabled = false;
                board_rgb_set(0, 0, 0);
                settings_save();
            } else if (strcmp(val, "on") == 0) {
                cfg->rgb_enabled = true;
                settings_save();
                app_led_for_state(ui_get_state());
            } else if (strcmp(val, "rainbow") == 0) {
                board_rgb_animate(RGB_MODE_RAINBOW_SPIN, 0, 0, 0);
            } else if (strcmp(val, "aurora") == 0) {
                board_rgb_animate(RGB_MODE_AURORA, 0, 0, 0);
            } else if (strcmp(val, "starfield") == 0) {
                board_rgb_animate(RGB_MODE_STARFIELD, 0, 0, 0);
            } else if (strcmp(val, "fire") == 0) {
                board_rgb_animate(RGB_MODE_FIRE, 0, 0, 0);
            } else if (strcmp(val, "ocean") == 0) {
                board_rgb_animate(RGB_MODE_OCEAN, 0, 0, 0);
            } else {
                /* Try as R,G,B values e.g. "255,0,128" */
                int r = 0, g = 0, b = 0;
                if (sscanf(val, "%d,%d,%d", &r, &g, &b) == 3) {
                    board_rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
                }
            }
            ESP_LOGI(TAG, "RGB: %s", val);
            count++;
        } else if (strcmp(key, "sleep") == 0) {
            int mins = atoi(val);
            cfg->sleep_timeout_ms = (uint32_t)(mins * 60000);
            settings_save();
            ESP_LOGI(TAG, "Sleep timeout: %d min", mins);
            count++;
        } else if (strcmp(key, "reboot") == 0) {
            ESP_LOGW(TAG, "Reboot requested via voice");
            count++;
            /* Strip command, then reboot after short delay */
            memmove(start, end + 1, strlen(end + 1) + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else if (strcmp(key, "webserver") == 0) {
            if (strcmp(val, "on") == 0 && !webserver_is_running()) {
                xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
            } else if (strcmp(val, "off") == 0 && webserver_is_running()) {
                xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
            }
            count++;
        } else if (strcmp(key, "auto_read") == 0) {
            cfg->auto_read_response = (strcmp(val, "on") == 0 || strcmp(val, "true") == 0);
            settings_save();
            count++;
        } else {
            ESP_LOGW(TAG, "Unknown device cmd: %s", key);
        }

        /* Strip the [DEVICE:...] tag from buffer (including trailing newline) */
        char *after = end + 1;
        if (*after == '\n') after++;
        memmove(start, after, strlen(after) + 1);
        pos = start;  /* Continue scanning from same position */
    }

    return count;
}
void app_on_chat_response(const char *text, bool is_final)
{
    if (!text) return;

    if (!is_final) {
        if (!s_stream_tts.active) {
            s_stream_tts.active = true;
            s_stream_tts.raw[0] = '\0';
            s_stream_tts.raw_len = 0;
            s_stream_tts.spoken_len = 0;
        }

        size_t text_len = strlen(text);
        size_t avail = sizeof(s_stream_tts.raw) - s_stream_tts.raw_len - 1;
        if (text_len > avail) text_len = avail;
        if (text_len > 0) {
            memcpy(s_stream_tts.raw + s_stream_tts.raw_len, text, text_len);
            s_stream_tts.raw_len += text_len;
            s_stream_tts.raw[s_stream_tts.raw_len] = '\0';
        }

        if (s_turn.openclaw_sent_us > 0 && s_turn.openclaw_first_delta_us == 0) {
            s_turn.openclaw_first_delta_us = esp_timer_get_time();
            s_turn.stats.openclaw_first_delta_ms =
                (uint32_t)((s_turn.openclaw_first_delta_us - s_turn.openclaw_sent_us) / 1000);
        }
        maybe_queue_streaming_sentence();
        return;
    }

    if (s_turn.openclaw_sent_us > 0 && s_turn.openclaw_final_us == 0) {
        s_turn.openclaw_final_us = esp_timer_get_time();
        s_turn.stats.openclaw_final_ms =
            (uint32_t)((s_turn.openclaw_final_us - s_turn.openclaw_sent_us) / 1000);
        if (s_turn.stats.openclaw_first_delta_ms == 0) {
            s_turn.stats.openclaw_first_delta_ms = s_turn.stats.openclaw_final_ms;
        }
    }

    ESP_LOGI(TAG, "Response (%lu ms): %.100s%s",
             (unsigned long)openclaw_get_thinking_time_ms(),
             text, strlen(text) > 100 ? "..." : "");

    /* Check for [LISTEN] / [END] tag at end of response */
    g_continue_listening = false;
    const char *listen_tag = strstr(text, "[LISTEN]");
    if (listen_tag) {
        g_continue_listening = true;
        ESP_LOGI(TAG, "Conversational: will auto-listen after TTS");
    }

    /* Work on a mutable copy so we can strip the tag */
    static char resp_buf[2048];
    strncpy(resp_buf, text, sizeof(resp_buf) - 1);
    resp_buf[sizeof(resp_buf) - 1] = '\0';

    /* Strip [LISTEN] or [END] tag from response text */
    char *tag = strstr(resp_buf, "[LISTEN]");
    if (tag) {
        char *p = tag;
        while (p > resp_buf && (*(p-1) == '\n' || *(p-1) == '\r' || *(p-1) == ' ')) p--;
        *p = '\0';
    }
    tag = strstr(resp_buf, "[END]");
    if (tag) {
        char *p = tag;
        while (p > resp_buf && (*(p-1) == '\n' || *(p-1) == '\r' || *(p-1) == ' ')) p--;
        *p = '\0';
    }

    /* Parse and execute device commands ([DEVICE:key=value]) */
    int dev_cmds = parse_device_commands(resp_buf);
    if (dev_cmds > 0) {
        ESP_LOGI(TAG, "Executed %d device command(s)", dev_cmds);
    }

    /* Parse dual response: first line = short label, rest = spoken response */
    static char short_buf[128];
    const char *long_text = resp_buf;
    const char *nl = strchr(resp_buf, '\n');
    if (nl && (nl - resp_buf) < (int)sizeof(short_buf) - 1) {
        size_t short_len = nl - resp_buf;
        memcpy(short_buf, resp_buf, short_len);
        short_buf[short_len] = '\0';
        long_text = nl + 1;
        while (*long_text == '\n' || *long_text == '\r') long_text++;
        if (*long_text == '\0') long_text = short_buf;
        ESP_LOGI(TAG, "Short: '%s' | Long: '%.80s%s'", short_buf, long_text,
                 strlen(long_text) > 80 ? "..." : "");
    } else {
        strncpy(short_buf, resp_buf, sizeof(short_buf) - 1);
        short_buf[sizeof(short_buf) - 1] = '\0';
    }

    if (s_stream_tts.spoken_len > 0) {
        size_t long_len = strlen(long_text);
        if (s_stream_tts.spoken_len < long_len) {
            queue_tts_chunk(long_text + s_stream_tts.spoken_len);
        }
    }
    s_stream_tts.active = false;

    app_set_state(UI_STATE_RESPONSE);
    g_response_shown_at = esp_timer_get_time();
    ui_set_response(short_buf, long_text);

    /* Auto-TTS: always on screenless boards, otherwise respect setting */
#if BOARD_HAS_DISPLAY
    const settings_t *cfg = settings_get();
    if (cfg->auto_read_response && s_stream_tts.spoken_len == 0) {
        g_tts_pending = true;
        xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
    }
#else
    if (s_stream_tts.spoken_len == 0) {
        g_tts_pending = true;
        xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
    }
#endif
}
