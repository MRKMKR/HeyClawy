/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * TTS client — fetches MP3 from EdgeTTS, decodes with minimp3,
 * resamples to board sample rate, and plays through I2S speaker.
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#include "tts_client.h"
#include "board.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "tts";

/* Rolling MP3 buffer for incremental download + decode. */
#define MP3_BUF_SIZE  (32 * 1024)

static struct {
    char host[64];
    uint16_t port;
    char api_key[64];
    char voice[32];
    char model[32];
    bool playing;
    bool stop_requested;
    tts_stats_t last_stats;
} s_tts;

static int play_mp3_frames(mp3dec_t *dec, uint8_t *mp3_buf, size_t *buf_len,
                           int16_t *pcm, int16_t *resamp,
                           int *src_rate, bool *first_frame,
                           size_t *total_pcm_samples)
{
    size_t offset = 0;
    int frames_played = 0;

    while (offset < *buf_len && !s_tts.stop_requested) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, mp3_buf + offset,
                                          *buf_len - offset, pcm, &info);
        if (info.frame_bytes == 0) break;
        offset += info.frame_bytes;

        if (samples <= 0) continue;

        if (*first_frame) {
            *src_rate = info.hz;
            ESP_LOGI(TAG, "MP3: %dHz %dch %dkbps", info.hz, info.channels, info.bitrate_kbps);
            *first_frame = false;
        }

        int16_t *mono = pcm;
        int mono_samples = samples;
        if (info.channels == 2) {
            for (int i = 0; i < samples; i++) {
                pcm[i] = (int16_t)(((int32_t)pcm[i * 2] + pcm[i * 2 + 1]) / 2);
            }
        }

        int out_samples;
        int16_t *play_buf;
        if (*src_rate != BOARD_AUDIO_SAMPLE_RATE) {
            out_samples = (int)((int64_t)mono_samples * BOARD_AUDIO_SAMPLE_RATE / *src_rate);
            for (int i = 0; i < out_samples; i++) {
                float pos = (float)i * (*src_rate) / BOARD_AUDIO_SAMPLE_RATE;
                int idx = (int)pos;
                float frac = pos - idx;
                if (idx + 1 < mono_samples) {
                    resamp[i] = (int16_t)(mono[idx] * (1.0f - frac) + mono[idx + 1] * frac);
                } else if (idx < mono_samples) {
                    resamp[i] = mono[idx];
                }
            }
            play_buf = resamp;
        } else {
            out_samples = mono_samples;
            play_buf = mono;
        }

        size_t written = 0;
        board_audio_play(play_buf, out_samples, &written);
        *total_pcm_samples += out_samples;
        frames_played++;
    }

    if (offset > 0) {
        size_t remaining = *buf_len - offset;
        if (remaining > 0) memmove(mp3_buf, mp3_buf + offset, remaining);
        *buf_len = remaining;
    }

    return frames_played;
}

esp_err_t tts_init(const tts_config_t *config)
{
    memset(&s_tts, 0, sizeof(s_tts));
    strncpy(s_tts.host, config->host, sizeof(s_tts.host) - 1);
    s_tts.port = config->port;
    if (config->api_key) strncpy(s_tts.api_key, config->api_key, sizeof(s_tts.api_key) - 1);
    strncpy(s_tts.voice, config->voice ? config->voice : "alloy", sizeof(s_tts.voice) - 1);
    strncpy(s_tts.model, config->model ? config->model : "tts-1", sizeof(s_tts.model) - 1);

    ESP_LOGI(TAG, "TTS init: %s:%d voice=%s", s_tts.host, s_tts.port, s_tts.voice);
    return ESP_OK;
}

esp_err_t tts_speak(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (s_tts.playing) {
        ESP_LOGW(TAG, "Already playing, stopping first");
        tts_stop();
    }

    memset(&s_tts.last_stats, 0, sizeof(s_tts.last_stats));
    int64_t request_start_us = esp_timer_get_time();

    /* Detect Hebrew text and use appropriate voice */
    const char *voice = s_tts.voice;
    for (size_t i = 0; text[i]; ) {
        uint8_t c = (uint8_t)text[i];
        if (c >= 0xD6 && c <= 0xD7) {
            /* UTF-8 lead bytes for U+0590-U+05FF (Hebrew range) */
            voice = "he-IL-HilaNeural";
            break;
        }
        if (c < 0x80) i++;
        else if (c < 0xC0) i++;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else i += 4;
    }

    s_tts.playing = true;
    s_tts.stop_requested = false;

    /* Sanitize text: replace problematic Unicode with ASCII equivalents */
    size_t text_len = strlen(text);
    char *clean = malloc(text_len + 1);
    if (!clean) { s_tts.playing = false; return ESP_ERR_NO_MEM; }
    size_t ci = 0;
    for (size_t i = 0; i < text_len; ) {
        uint8_t c = (uint8_t)text[i];
        if (c == 0xE2 && i + 2 < text_len) {
            uint8_t b1 = (uint8_t)text[i+1], b2 = (uint8_t)text[i+2];
            if (b1 == 0x80 && b2 == 0x94) { clean[ci++] = '-'; i += 3; continue; }  /* — em-dash */
            if (b1 == 0x80 && b2 == 0x93) { clean[ci++] = '-'; i += 3; continue; }  /* – en-dash */
            if (b1 == 0x80 && b2 == 0xA6) { clean[ci++] = '.'; clean[ci++] = '.'; clean[ci++] = '.'; i += 3; continue; }  /* … */
            if (b1 == 0x80 && (b2 == 0x98 || b2 == 0x99)) { clean[ci++] = '\''; i += 3; continue; }  /* ' ' */
            if (b1 == 0x80 && (b2 == 0x9C || b2 == 0x9D)) { clean[ci++] = '"'; i += 3; continue; }   /* " " */
        }
        if (c == 0xC2 && i + 1 < text_len && (uint8_t)text[i+1] == 0xA0) {
            clean[ci++] = ' '; i += 2; continue;  /* non-breaking space */
        }
        clean[ci++] = text[i]; i++;
    }
    clean[ci] = '\0';
    text_len = ci;

    /* Build JSON body */
    size_t json_size = text_len + 256;
    char *json_body = malloc(json_size);
    if (!json_body) {
        s_tts.playing = false;
        return ESP_ERR_NO_MEM;
    }

    /* Escape special chars for JSON string */
    char *escaped = malloc(text_len * 2 + 1);
    if (!escaped) {
        free(json_body);
        free(clean);
        s_tts.playing = false;
        return ESP_ERR_NO_MEM;
    }
    size_t ei = 0;
    for (size_t i = 0; i < text_len && ei < text_len * 2 - 2; i++) {
        char ch = clean[i];
        if (ch == '"' || ch == '\\') {
            escaped[ei++] = '\\'; escaped[ei++] = ch;
        } else if (ch == '\n') {
            escaped[ei++] = '\\'; escaped[ei++] = 'n';
        } else if (ch == '\r') {
            escaped[ei++] = '\\'; escaped[ei++] = 'r';
        } else if (ch == '\t') {
            escaped[ei++] = '\\'; escaped[ei++] = 't';
        } else if ((uint8_t)ch < 0x20) {
            /* Skip other control chars */
        } else {
            escaped[ei++] = ch;
        }
    }
    escaped[ei] = '\0';
    free(clean);

    snprintf(json_body, json_size,
             "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\"}",
             s_tts.model, escaped, voice);

    /* Build URL */
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/v1/audio/speech", s_tts.host, s_tts.port);

    ESP_LOGI(TAG, "TTS request: voice=%s text=%.60s%s", voice, escaped, strlen(escaped) > 60 ? "..." : "");
    free(escaped);

    /* Configure HTTP client */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = json_size + 64,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(json_body);
        s_tts.playing = false;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_tts.api_key[0]) {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_tts.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    /* Open connection */
    esp_err_t err = esp_http_client_open(client, strlen(json_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Write request body */
    int wlen = esp_http_client_write(client, json_body, strlen(json_body));
    if (wlen < 0) {
        ESP_LOGE(TAG, "HTTP write failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    s_tts.last_stats.request_to_headers_ms = (uint32_t)((esp_timer_get_time() - request_start_us) / 1000);
    ESP_LOGI(TAG, "TTS response: status=%d content_length=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "TTS server error: HTTP %d", status);
        char err_buf[256];
        int rd = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (rd > 0) { err_buf[rd] = '\0'; ESP_LOGE(TAG, "Error: %s", err_buf); }
        err = ESP_FAIL;
        goto cleanup;
    }

    uint8_t *mp3_buf = heap_caps_malloc(MP3_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "Failed to alloc MP3 buffer (%u bytes)", (unsigned)MP3_BUF_SIZE);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Decode MP3 frames and play */
    mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec) {
        ESP_LOGE(TAG, "Failed to alloc MP3 decoder");
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    mp3dec_init(dec);

    /* PCM output buffer: minimp3 outputs max 1152 samples per frame × 2 channels */
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to alloc PCM buffer");
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Resample buffer: for 24kHz→16kHz, output is 2/3 of input size */
    int16_t *resamp = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resamp) {
        free(pcm);
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t mp3_len = 0;
    size_t total_mp3_bytes = 0;
    size_t total_pcm_samples = 0;
    int64_t first_audio_us = 0;
    bool first_frame = true;
    int src_rate = 24000;  /* default, updated from first frame */
    int idle_reads = 0;

    while (!s_tts.stop_requested) {
        if (mp3_len == MP3_BUF_SIZE) {
            int frames = play_mp3_frames(dec, mp3_buf, &mp3_len, pcm, resamp,
                                         &src_rate, &first_frame, &total_pcm_samples);
            if (frames == 0 && mp3_len == MP3_BUF_SIZE) {
                ESP_LOGE(TAG, "MP3 streaming buffer exhausted without a decodable frame");
                err = ESP_FAIL;
                break;
            }
            if (frames > 0 && first_audio_us == 0) {
                first_audio_us = esp_timer_get_time();
            }
        }

        int rd = esp_http_client_read(client, (char *)mp3_buf + mp3_len, MP3_BUF_SIZE - mp3_len);
        if (rd < 0) {
            ESP_LOGE(TAG, "HTTP read failed during TTS stream");
            err = ESP_FAIL;
            break;
        }
        if (rd == 0) {
            idle_reads++;
            int frames = play_mp3_frames(dec, mp3_buf, &mp3_len, pcm, resamp,
                                         &src_rate, &first_frame, &total_pcm_samples);
            if (frames > 0 && first_audio_us == 0) {
                first_audio_us = esp_timer_get_time();
            }
            if (idle_reads > 1) break;
            continue;
        }

        idle_reads = 0;
        mp3_len += (size_t)rd;
        total_mp3_bytes += (size_t)rd;

        int frames = play_mp3_frames(dec, mp3_buf, &mp3_len, pcm, resamp,
                                     &src_rate, &first_frame, &total_pcm_samples);
        if (frames > 0 && first_audio_us == 0) {
            first_audio_us = esp_timer_get_time();
        }
    }

    if (!s_tts.stop_requested && mp3_len > 0 && err == ESP_OK) {
        int frames = play_mp3_frames(dec, mp3_buf, &mp3_len, pcm, resamp,
                                     &src_rate, &first_frame, &total_pcm_samples);
        if (frames == 0 && mp3_len > 0) {
            ESP_LOGW(TAG, "TTS finished with %u undecoded MP3 bytes", (unsigned)mp3_len);
        }
    }

    s_tts.last_stats.total_mp3_bytes = total_mp3_bytes;
    s_tts.last_stats.total_pcm_samples = total_pcm_samples;
    s_tts.last_stats.total_ms = (uint32_t)((esp_timer_get_time() - request_start_us) / 1000);
    if (first_audio_us > 0) {
        s_tts.last_stats.request_to_first_audio_ms =
            (uint32_t)((first_audio_us - request_start_us) / 1000);
    }
    ESP_LOGI(TAG,
             "TTS playback complete: headers=%ums first_audio=%ums total=%ums mp3=%u pcm=%u",
             (unsigned)s_tts.last_stats.request_to_headers_ms,
             (unsigned)s_tts.last_stats.request_to_first_audio_ms,
             (unsigned)s_tts.last_stats.total_ms,
             (unsigned)total_mp3_bytes,
             (unsigned)total_pcm_samples);

    free(dec);
    free(resamp);
    free(pcm);
    free(mp3_buf);

cleanup:
    vTaskDelay(pdMS_TO_TICKS(50));  /* Let lwip finish before cleanup */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(json_body);
    s_tts.playing = false;
    return err;
}

void tts_stop(void)
{
    s_tts.stop_requested = true;
    /* Wait for playback to actually stop */
    int timeout = 50;
    while (s_tts.playing && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool tts_is_playing(void)
{
    return s_tts.playing;
}

void tts_get_last_stats(tts_stats_t *out)
{
    if (!out) return;
    *out = s_tts.last_stats;
}
