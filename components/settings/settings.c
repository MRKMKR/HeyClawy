/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "settings";
#define NVS_NAMESPACE "heyclawy"

static settings_t s_settings;

/* ── Bare defaults (when no compile-time secrets provided) ───────────── */

static void apply_bare_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->oc_port             = 18789;
    s->tts_port            = 5050;
    s->stt_port            = 5051;
    s->volume              = 100;
    s->silence_timeout_ms  = 900;
    s->silence_threshold   = 500;
    s->max_record_seconds  = 15;
    s->no_speech_timeout_ms = 3000;
    s->auto_read_response  = true;
    s->short_response      = true;
    s->brightness          = 100;
    s->rgb_enabled         = true;
    s->startup_pattern     = 0;  /* rainbow */
    s->sleep_timeout_ms    = 180000;
    s->wake_word_in_sleep  = false;  /* save battery by default */
    s->webserver_enabled   = false;
    s->activity_carousel   = true;
    s->auto_notify         = true;
    s->log_verbosity       = 2;  /* INFO */
    strncpy(s->tts_voice, "alloy", sizeof(s->tts_voice) - 1);
}

/* ── NVS helpers ─────────────────────────────────────────────────────── */

#define NVS_STR(h, key, field) \
    do { \
        size_t _len = sizeof(s->field); \
        nvs_get_str(h, key, s->field, &_len); \
    } while(0)

#define NVS_U8(h, key, field)  nvs_get_u8(h, key, &s->field)
#define NVS_U16(h, key, field) nvs_get_u16(h, key, &s->field)
#define NVS_U32(h, key, field) nvs_get_u32(h, key, &s->field)

static void load_from_nvs(settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        return;
    }

    NVS_STR(h, "wifi_ssid",   wifi_ssid);
    NVS_STR(h, "wifi_pw",     wifi_password);
    NVS_STR(h, "oc_host",     oc_host);
    NVS_U16(h, "oc_port",     oc_port);
    NVS_STR(h, "oc_token",    oc_token);
    NVS_STR(h, "oc_devkey",   oc_device_key);
    NVS_STR(h, "tts_host",    tts_host);
    NVS_U16(h, "tts_port",    tts_port);
    NVS_STR(h, "tts_apikey",  tts_api_key);
    NVS_STR(h, "tts_voice",   tts_voice);
    NVS_STR(h, "stt_host",    stt_host);
    NVS_U16(h, "stt_port",    stt_port);
    NVS_U8(h,  "volume",      volume);
    NVS_U16(h, "sil_timeout", silence_timeout_ms);
    NVS_U16(h, "sil_thresh",  silence_threshold);
    NVS_U8(h,  "max_rec_s",   max_record_seconds);
    NVS_U16(h, "nospeech_ms", no_speech_timeout_ms);
    NVS_U8(h,  "brightness",  brightness);
    NVS_U32(h, "sleep_ms",    sleep_timeout_ms);
    NVS_U8(h,  "log_verb",    log_verbosity);

    uint8_t tmp = 0;
    if (nvs_get_u8(h, "ww_sleep", &tmp) == ESP_OK) s->wake_word_in_sleep = tmp;
    if (nvs_get_u8(h, "rgb_on", &tmp) == ESP_OK) s->rgb_enabled = tmp;
    if (nvs_get_u8(h, "start_pat", &tmp) == ESP_OK) s->startup_pattern = tmp;
    if (nvs_get_u8(h, "web_on", &tmp) == ESP_OK) s->webserver_enabled = tmp;
    if (nvs_get_u8(h, "auto_read", &tmp) == ESP_OK) s->auto_read_response = tmp;
    if (nvs_get_u8(h, "short_resp", &tmp) == ESP_OK) s->short_response = tmp;
    if (nvs_get_u8(h, "carousel", &tmp) == ESP_OK) s->activity_carousel = tmp;
    if (nvs_get_u8(h, "auto_notify", &tmp) == ESP_OK) s->auto_notify = tmp;

    nvs_close(h);
    ESP_LOGI(TAG, "Settings loaded from NVS");
}

static esp_err_t save_to_nvs(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "wifi_ssid",   s->wifi_ssid);
    nvs_set_str(h, "wifi_pw",     s->wifi_password);
    nvs_set_str(h, "oc_host",     s->oc_host);
    nvs_set_u16(h, "oc_port",     s->oc_port);
    nvs_set_str(h, "oc_token",    s->oc_token);
    nvs_set_str(h, "oc_devkey",   s->oc_device_key);
    nvs_set_str(h, "tts_host",    s->tts_host);
    nvs_set_u16(h, "tts_port",    s->tts_port);
    nvs_set_str(h, "tts_apikey",  s->tts_api_key);
    nvs_set_str(h, "tts_voice",   s->tts_voice);
    nvs_set_str(h, "stt_host",    s->stt_host);
    nvs_set_u16(h, "stt_port",    s->stt_port);
    nvs_set_u8(h,  "volume",      s->volume);
    nvs_set_u16(h, "sil_timeout", s->silence_timeout_ms);
    nvs_set_u16(h, "sil_thresh",  s->silence_threshold);
    nvs_set_u8(h,  "max_rec_s",   s->max_record_seconds);
    nvs_set_u16(h, "nospeech_ms", s->no_speech_timeout_ms);
    nvs_set_u8(h,  "brightness",  s->brightness);
    nvs_set_u32(h, "sleep_ms",    s->sleep_timeout_ms);
    nvs_set_u8(h,  "ww_sleep",   s->wake_word_in_sleep ? 1 : 0);
    nvs_set_u8(h,  "log_verb",    s->log_verbosity);
    nvs_set_u8(h,  "rgb_on",      s->rgb_enabled ? 1 : 0);
    nvs_set_u8(h,  "start_pat",   s->startup_pattern);
    nvs_set_u8(h,  "web_on",      s->webserver_enabled ? 1 : 0);
    nvs_set_u8(h,  "auto_read",   s->auto_read_response ? 1 : 0);
    nvs_set_u8(h,  "short_resp",  s->short_response ? 1 : 0);
    nvs_set_u8(h,  "carousel",    s->activity_carousel ? 1 : 0);
    nvs_set_u8(h,  "auto_notify", s->auto_notify ? 1 : 0);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved to NVS");
    return err;
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t settings_init(const settings_t *defaults)
{
    /* Initialize NVS flash (safe to call multiple times) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    /* Start with bare defaults, then overlay caller-provided defaults */
    apply_bare_defaults(&s_settings);
    if (defaults) {
        /* Copy non-zero strings from caller defaults */
#define COPY_IF_SET(field) \
        if (defaults->field[0]) strncpy(s_settings.field, defaults->field, sizeof(s_settings.field) - 1)
        COPY_IF_SET(wifi_ssid);
        COPY_IF_SET(wifi_password);
        COPY_IF_SET(oc_host);
        if (defaults->oc_port) s_settings.oc_port = defaults->oc_port;
        COPY_IF_SET(oc_token);
        COPY_IF_SET(oc_device_key);
        COPY_IF_SET(tts_host);
        if (defaults->tts_port) s_settings.tts_port = defaults->tts_port;
        COPY_IF_SET(tts_api_key);
        COPY_IF_SET(tts_voice);
        COPY_IF_SET(stt_host);
        if (defaults->stt_port) s_settings.stt_port = defaults->stt_port;
        if (defaults->volume) s_settings.volume = defaults->volume;
#undef COPY_IF_SET
    }

    /* NVS values override defaults */
    load_from_nvs(&s_settings);

    ESP_LOGI(TAG, "Settings init: vol=%d bright=%d rgb=%d web=%d log=%d",
             s_settings.volume, s_settings.brightness,
             s_settings.rgb_enabled, s_settings.webserver_enabled,
             s_settings.log_verbosity);

    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_settings;
}

settings_t *settings_get_mutable(void)
{
    return &s_settings;
}

esp_err_t settings_save(void)
{
    return save_to_nvs(&s_settings);
}

esp_err_t settings_reset(void)
{
    apply_bare_defaults(&s_settings);
    return save_to_nvs(&s_settings);
}

/* ── JSON export ─────────────────────────────────────────────────────── */

char *settings_to_json(bool include_secrets)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;

    const settings_t *s = &s_settings;

    /* WiFi */
    cJSON_AddStringToObject(j, "wifi_ssid", s->wifi_ssid);
    cJSON_AddStringToObject(j, "wifi_password", include_secrets ? s->wifi_password : "****");

    /* OpenClaw */
    cJSON_AddStringToObject(j, "oc_host", s->oc_host);
    cJSON_AddNumberToObject(j, "oc_port", s->oc_port);
    cJSON_AddStringToObject(j, "oc_token", include_secrets ? s->oc_token : "****");
    cJSON_AddStringToObject(j, "oc_device_key", include_secrets ? s->oc_device_key : "****");

    /* TTS */
    cJSON_AddStringToObject(j, "tts_host", s->tts_host);
    cJSON_AddNumberToObject(j, "tts_port", s->tts_port);
    cJSON_AddStringToObject(j, "tts_api_key", include_secrets ? s->tts_api_key : "****");
    cJSON_AddStringToObject(j, "tts_voice", s->tts_voice);

    /* STT */
    cJSON_AddStringToObject(j, "stt_host", s->stt_host);
    cJSON_AddNumberToObject(j, "stt_port", s->stt_port);

    /* Audio */
    cJSON_AddNumberToObject(j, "volume", s->volume);
    cJSON_AddNumberToObject(j, "silence_timeout_ms", s->silence_timeout_ms);
    cJSON_AddNumberToObject(j, "silence_threshold", s->silence_threshold);
    cJSON_AddNumberToObject(j, "max_record_seconds", s->max_record_seconds);
    cJSON_AddNumberToObject(j, "no_speech_timeout_ms", s->no_speech_timeout_ms);

    /* Auto-read & short response */
    cJSON_AddBoolToObject(j, "auto_read_response", s->auto_read_response);
    cJSON_AddBoolToObject(j, "short_response", s->short_response);

    /* Display */
    cJSON_AddNumberToObject(j, "brightness", s->brightness);

    /* RGB */
    cJSON_AddBoolToObject(j, "rgb_enabled", s->rgb_enabled);
    cJSON_AddNumberToObject(j, "startup_pattern", s->startup_pattern);

    /* Power */
    cJSON_AddNumberToObject(j, "sleep_timeout_ms", s->sleep_timeout_ms);
    cJSON_AddBoolToObject(j, "wake_word_in_sleep", s->wake_word_in_sleep);

    /* Web server */
    cJSON_AddBoolToObject(j, "webserver_enabled", s->webserver_enabled);

    /* Activity & Notifications */
    cJSON_AddBoolToObject(j, "activity_carousel", s->activity_carousel);
    cJSON_AddBoolToObject(j, "auto_notify", s->auto_notify);

    /* Logging */
    cJSON_AddNumberToObject(j, "log_verbosity", s->log_verbosity);

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return str;
}

/* ── JSON import ─────────────────────────────────────────────────────── */

esp_err_t settings_from_json(const char *json, size_t len)
{
    cJSON *j = cJSON_ParseWithLength(json, len);
    if (!j) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    settings_t *s = &s_settings;
    cJSON *item;

#define JSON_STR(key, field) \
    if ((item = cJSON_GetObjectItem(j, key)) && cJSON_IsString(item) && \
        strcmp(item->valuestring, "****") != 0) { \
        strncpy(s->field, item->valuestring, sizeof(s->field) - 1); \
        s->field[sizeof(s->field) - 1] = '\0'; \
    }
#define JSON_U8(key, field) \
    if ((item = cJSON_GetObjectItem(j, key)) && cJSON_IsNumber(item)) \
        s->field = (uint8_t)item->valuedouble;
#define JSON_U16(key, field) \
    if ((item = cJSON_GetObjectItem(j, key)) && cJSON_IsNumber(item)) \
        s->field = (uint16_t)item->valuedouble;
#define JSON_U32(key, field) \
    if ((item = cJSON_GetObjectItem(j, key)) && cJSON_IsNumber(item)) \
        s->field = (uint32_t)item->valuedouble;
#define JSON_BOOL(key, field) \
    if ((item = cJSON_GetObjectItem(j, key)) && cJSON_IsBool(item)) \
        s->field = cJSON_IsTrue(item);

    JSON_STR("wifi_ssid",        wifi_ssid);
    JSON_STR("wifi_password",    wifi_password);
    JSON_STR("oc_host",          oc_host);
    JSON_U16("oc_port",          oc_port);
    JSON_STR("oc_token",         oc_token);
    JSON_STR("oc_device_key",    oc_device_key);
    JSON_STR("tts_host",         tts_host);
    JSON_U16("tts_port",         tts_port);
    JSON_STR("tts_api_key",      tts_api_key);
    JSON_STR("tts_voice",        tts_voice);
    JSON_STR("stt_host",         stt_host);
    JSON_U16("stt_port",         stt_port);
    JSON_U8("volume",            volume);
    JSON_U16("silence_timeout_ms", silence_timeout_ms);
    JSON_U16("silence_threshold", silence_threshold);
    JSON_U8("max_record_seconds", max_record_seconds);
    JSON_U16("no_speech_timeout_ms", no_speech_timeout_ms);
    JSON_BOOL("auto_read_response", auto_read_response);
    JSON_BOOL("short_response",  short_response);
    JSON_U8("brightness",        brightness);
    JSON_BOOL("rgb_enabled",     rgb_enabled);
    JSON_U8("startup_pattern",   startup_pattern);
    JSON_U32("sleep_timeout_ms", sleep_timeout_ms);
    JSON_BOOL("wake_word_in_sleep", wake_word_in_sleep);
    JSON_BOOL("webserver_enabled", webserver_enabled);
    JSON_BOOL("activity_carousel", activity_carousel);
    JSON_BOOL("auto_notify",     auto_notify);
    JSON_U8("log_verbosity",     log_verbosity);

    cJSON_Delete(j);
    ESP_LOGI(TAG, "Settings updated from JSON");
    return ESP_OK;
}
