/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * Voice chat — record → STT → OpenClaw
 */

#include "voice_chat.h"
#include "app_state.h"
#include "recording_sounds.h"
#include "settings.h"

#include "board.h"
#include "openclaw_client.h"
#include "stt_client.h"
#include "ui.h"
#include "error_log.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "voice_chat";

/* Helper: play a short PCM sound through speaker */
static void play_feedback_sound(const int16_t *pcm, size_t len)
{
    size_t written = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t chunk = len - pos;
        if (chunk > 1024) chunk = 1024;
        board_audio_play(pcm + pos, chunk, &written);
        pos += written;
    }
}

void voice_chat_start(void)
{
    if (openclaw_get_state() != OPENCLAW_STATE_CONNECTED) {
        ESP_LOGW(TAG, "OpenClaw not connected");
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("Not connected");
        vTaskDelay(pdMS_TO_TICKS(2000));
        app_set_state(UI_STATE_IDLE);
        return;
    }

    const settings_t *cfg = settings_get();
    app_turn_reset();

    /* Play start sound before recording */
    play_feedback_sound(snd_rec_start, snd_rec_start_len);
    vTaskDelay(pdMS_TO_TICKS(100));

    g_recording = true;
    app_set_state(UI_STATE_LISTENING);

    /* Allocate recording buffer in PSRAM */
    const int max_samples = BOARD_AUDIO_SAMPLE_RATE * cfg->max_record_seconds;
    int16_t *audio_buf = heap_caps_malloc(max_samples * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Audio buffer alloc failed");
        error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "Audio buffer alloc failed");
        g_recording = false;
        app_set_state(UI_STATE_IDLE);
        return;
    }

    /* Record with silence detection + wheel-press-to-stop */
    const int chunk = 512;
    const int silence_chunks = (cfg->silence_timeout_ms * BOARD_AUDIO_SAMPLE_RATE) / (chunk * 1000);
    int silent_count = 0;
    bool had_speech = false;
    size_t total_read = 0;
    size_t speech_start_sample = 0;
    size_t speech_end_sample = 0;

    /* Adaptive noise floor: calibrate from audio after I2S stabilizes */
    const int skip_chunks = 4;      /* Skip first 4 chunks (~128ms) — I2S startup noise */
    const int calibration_chunks = 16;
    int noise_floor = 0;
    int chunk_index = 0;
    int calibration_count = 0;
    int64_t calibration_sum = 0;
    int64_t calibration_dc_sum = 0;  /* Sum of raw samples for DC offset */
    int calibration_dc_count = 0;
    int32_t dc_offset = 0;           /* PDM mic DC bias (subtracted from all samples) */
    /* Effective threshold: max(configured, noise_floor * 2.5) */
    int effective_threshold = cfg->silence_threshold;
    /* No-speech timeout: stop after configured timeout of silence (no speech detected) */
    const int no_speech_max_chunks = ((int)cfg->no_speech_timeout_ms * BOARD_AUDIO_SAMPLE_RATE) / (chunk * 1000);

    ESP_LOGI(TAG, "Recording (silence=%dms thresh=%d max=%ds)...",
             cfg->silence_timeout_ms, cfg->silence_threshold, cfg->max_record_seconds);

    while (total_read < (size_t)max_samples) {
        EventBits_t ev = xEventGroupGetBits(g_app_events);
        if (ev & CANCEL_BIT) {
            xEventGroupClearBits(g_app_events, CANCEL_BIT);
            ESP_LOGI(TAG, "Recording cancelled by user (had_speech=%d)", had_speech);
            g_recording = false;
            play_feedback_sound(snd_rec_stop, snd_rec_stop_len);
            free(audio_buf);
            app_set_state(UI_STATE_IDLE);
            return;
        }

        size_t to_read = (size_t)chunk;
        if (total_read + to_read > (size_t)max_samples) to_read = max_samples - total_read;
        size_t read_now = 0;
        if (board_audio_record(audio_buf + total_read, to_read, &read_now) != ESP_OK) break;

        int64_t sum_sq = 0;
        for (size_t i = 0; i < read_now; i++) {
            int32_t s = audio_buf[total_read + i] - dc_offset;
            sum_sq += s * s;
        }
        int rms = read_now > 0 ? (int)sqrtf((float)(sum_sq / read_now)) : 0;

        total_read += read_now;
        chunk_index++;

        /* Skip first few chunks — I2S startup noise */
        if (chunk_index <= skip_chunks) {
            continue;
        }

        /* Noise floor calibration phase */
        if (calibration_count < calibration_chunks) {
            /* Accumulate DC offset from raw samples */
            for (size_t i = total_read - read_now; i < total_read; i++) {
                calibration_dc_sum += audio_buf[i];
                calibration_dc_count++;
            }
            calibration_sum += rms;
            calibration_count++;
            if (calibration_count == calibration_chunks) {
                /* Compute DC offset = mean of all calibration samples */
                dc_offset = (int32_t)(calibration_dc_sum / calibration_dc_count);
                /* Recompute noise floor RMS with DC offset removed */
                int64_t recalc_sum = 0;
                int recalc_n = 0;
                for (int c = 0; c < calibration_chunks; c++) {
                    size_t start = (skip_chunks + c) * chunk;
                    size_t end = start + chunk;
                    if (end > total_read) end = total_read;
                    int64_t sq = 0;
                    for (size_t i = start; i < end; i++) {
                        int32_t v = audio_buf[i] - dc_offset;
                        sq += v * v;
                    }
                    int n = (int)(end - start);
                    if (n > 0) {
                        recalc_sum += (int)sqrtf((float)(sq / n));
                        recalc_n++;
                    }
                }
                noise_floor = recalc_n > 0 ? (int)(recalc_sum / recalc_n) : 0;
                /* Effective threshold: noise_floor * 2.5, but at least the configured value */
                int adaptive = (noise_floor * 5) / 2;
                effective_threshold = (adaptive > cfg->silence_threshold) ? adaptive : cfg->silence_threshold;
                ESP_LOGI(TAG, "DC offset: %d | Noise floor: %d → threshold: %d",
                         (int)dc_offset, noise_floor, effective_threshold);
            }
            continue;  /* Don't do speech detection during calibration */
        }

        if (rms > effective_threshold) {
            size_t chunk_start = total_read - read_now;
            had_speech = true;
            silent_count = 0;
            if (speech_end_sample == 0) {
                speech_start_sample = chunk_start;
            }
            speech_end_sample = total_read;
        } else {
            silent_count++;
            if (had_speech && silent_count >= silence_chunks) {
                ESP_LOGI(TAG, "Silence detected after speech (rms=%d < %d), stopping",
                         rms, effective_threshold);
                break;
            }
            /* No-speech timeout: abort if nobody spoke after calibration */
            if (!had_speech && silent_count >= no_speech_max_chunks) {
                ESP_LOGI(TAG, "No speech for %dms, aborting recording",
                         cfg->no_speech_timeout_ms);
                break;
            }
        }
    }
    g_recording = false;

    /* Play stop sound */
    play_feedback_sound(snd_rec_stop, snd_rec_stop_len);

    /* Remove DC offset from recorded audio for better STT accuracy */
    if (dc_offset != 0) {
        for (size_t i = 0; i < total_read; i++) {
            int32_t v = audio_buf[i] - dc_offset;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            audio_buf[i] = (int16_t)v;
        }
    }

    /* Log audio levels */
    int32_t min_val = 32767, max_val = -32768;
    int64_t sum_abs = 0;
    for (size_t i = 0; i < total_read; i++) {
        int16_t s = audio_buf[i];
        if (s < min_val) min_val = s;
        if (s > max_val) max_val = s;
        sum_abs += (s < 0) ? -s : s;
    }
    int avg_abs = total_read ? (int)(sum_abs / total_read) : 0;
    ESP_LOGI(TAG, "Recorded %d samples %.1fs (min=%d max=%d avg_abs=%d noise_floor=%d)",
             (int)total_read, (float)total_read / BOARD_AUDIO_SAMPLE_RATE,
             (int)min_val, (int)max_val, avg_abs, noise_floor);

    /* Don't send if too short (< 0.3s) */
    if (total_read < (size_t)(BOARD_AUDIO_SAMPLE_RATE * 0.3f)) {
        ESP_LOGW(TAG, "Recording too short, discarding");
        free(audio_buf);
        app_set_state(UI_STATE_IDLE);
        return;
    }

    /* Don't send if no speech was detected (just noise) */
    if (!had_speech) {
        ESP_LOGW(TAG, "No speech detected (avg_abs=%d, noise=%d), discarding", avg_abs, noise_floor);
        ui_set_status_message("No speech detected");
        free(audio_buf);
        vTaskDelay(pdMS_TO_TICKS(1500));
        app_set_state(UI_STATE_IDLE);
        return;
    }

    size_t trimmed_leading = 0;
    size_t trimmed_trailing = 0;
    size_t sent_samples = total_read;
    const int pre_roll_chunks = 4;
    const int post_roll_chunks = 6;
    size_t trim_start = 0;
    size_t trim_end = total_read;

    if (speech_end_sample > speech_start_sample) {
        size_t pre_roll = (size_t)(pre_roll_chunks * chunk);
        size_t post_roll = (size_t)(post_roll_chunks * chunk);
        trim_start = (speech_start_sample > pre_roll) ? (speech_start_sample - pre_roll) : 0;
        /* For short commands, keep the tail to avoid clipping quiet final words. */
        if (total_read <= (size_t)(BOARD_AUDIO_SAMPLE_RATE * 2.5f)) {
            trim_end = total_read;
        } else {
            trim_end = speech_end_sample + post_roll;
            if (trim_end > total_read) trim_end = total_read;
        }
        if (trim_end > trim_start) {
            trimmed_leading = trim_start;
            trimmed_trailing = total_read - trim_end;
            sent_samples = trim_end - trim_start;
        }
    }

    if (sent_samples < (size_t)(BOARD_AUDIO_SAMPLE_RATE * 0.3f)) {
        trim_start = 0;
        trim_end = total_read;
        trimmed_leading = 0;
        trimmed_trailing = 0;
        sent_samples = total_read;
    }

    ESP_LOGI(TAG, "STT trim: lead=%u tail=%u send=%u/%u samples",
             (unsigned)trimmed_leading, (unsigned)trimmed_trailing,
             (unsigned)sent_samples, (unsigned)total_read);
    app_turn_mark_capture(total_read, sent_samples, trimmed_leading, trimmed_trailing);

    app_set_state(UI_STATE_SENDING);
    ui_set_status_message("Transcribing...");

    /* STT */
    char transcribed[512] = {0};
    app_turn_mark_stt_start();
    esp_err_t stt_err = stt_transcribe(audio_buf + trim_start, sent_samples, BOARD_AUDIO_SAMPLE_RATE,
                                       transcribed, sizeof(transcribed));
    app_turn_mark_stt_done();
    free(audio_buf);

    if (stt_err != ESP_OK || transcribed[0] == '\0') {
        ESP_LOGW(TAG, "STT failed or empty");
        error_log_add(ERR_SRC_STT, ERR_SEV_WARNING, "STT failed or returned empty");
        strcpy(transcribed, "Voice message (could not transcribe)");
    }

    /* Check for cancel during STT */
    EventBits_t cancel_ev = xEventGroupGetBits(g_app_events);
    if (cancel_ev & CANCEL_BIT) {
        xEventGroupClearBits(g_app_events, CANCEL_BIT);
        ESP_LOGI(TAG, "Cancelled during STT/send");
        app_set_state(UI_STATE_IDLE);
        return;
    }

    ESP_LOGI(TAG, "STT result: '%s'", transcribed);
    ui_set_status_message("Sending...");

    /* Check for pending camera image */
    uint8_t *jpeg = g_pending_jpeg;
    size_t jpeg_sz = g_pending_jpeg_size;
    g_pending_jpeg = NULL;
    g_pending_jpeg_size = 0;

    if (jpeg && jpeg_sz > 0) {
        ESP_LOGI(TAG, "Including camera image (%d bytes) with message", (int)jpeg_sz);
    }

    /* Send the transcribed text (dual-response prompt is handled by openclaw_chat_send) */
    const char *msg = transcribed;

    /* Send with or without image */
    app_turn_mark_openclaw_sent();
    if (jpeg && jpeg_sz > 0) {
        // Use the audio+image function with the original audio for better quality
        // But since we already STT'd, just send text + image attachment
        openclaw_chat_send_with_image(msg, jpeg, jpeg_sz, app_on_chat_response);
        free(jpeg);
    } else {
        openclaw_chat_send(msg, app_on_chat_response);
    }
}
