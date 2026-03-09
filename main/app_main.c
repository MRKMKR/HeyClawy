/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * HeyClawy — OpenClaw ESP32 Interface Device
 *
 * This file handles initialization and main event loop only.
 * Logic is split into: voice_chat.c, serial_cmd.c, app_tasks.c, app_state.c
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"

#include "board.h"
#include "app_config.h"
#include "app_state.h"
#include "voice_chat.h"
#include "serial_cmd.h"
#include "app_tasks.h"
#include "settings.h"
#include "error_log.h"
#include "secrets.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "wifi_manager.h"
#include "openclaw_client.h"
#include "tts_client.h"
#include "stt_client.h"
#include "ui.h"
#include "ui_tasks.h"
#include "webserver.h"
#include "camera.h"
#include "wake_word.h"

static const char *TAG = "heyclawy";

/* ─── WiFi callback ──────────────────────────────────────────────────── */
static void on_wifi_state(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        xEventGroupSetBits(g_app_events, WIFI_CONNECTED_BIT);
        ui_set_wifi_status(true, wifi_manager_get_rssi());
        break;
    case WIFI_STATE_CONNECTING:
        ui_set_wifi_status(false, 0);
        app_set_state(UI_STATE_CONNECTING);
        ui_set_status_message("Connecting WiFi...");
        break;
    case WIFI_STATE_FAILED:
        ui_set_wifi_status(false, 0);
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("WiFi failed!");
        error_log_add(ERR_SRC_WIFI, ERR_SEV_ERROR, "WiFi connection failed");
        break;
    default:
        ui_set_wifi_status(false, 0);
        break;
    }
}

/* ─── OpenClaw state callback ────────────────────────────────────────── */

/* Notification callback: incoming message not initiated by this device */
static void on_openclaw_notify(const char *text, const char *source)
{
    const settings_t *cfg = settings_get();
    if (!cfg->auto_notify) {
        ESP_LOGI(TAG, "Notification suppressed (auto_notify=off): %.40s", text);
        return;
    }

    /* Only notify for cron-originated sessions (scheduled tasks/reminders).
     * Skip notifications from other channels (WhatsApp, Telegram, CLI). */
    if (source && strcmp(source, "cron") != 0) {
        ESP_LOGI(TAG, "Notification skipped (source=%s, not cron): %.40s", source, text);
        return;
    }
    ESP_LOGI(TAG, "Notification from %s: %.80s", source ? source : "?", text);

    /* Wake device if sleeping */
    app_reset_activity_timer();

    /* Show on display if available */
#if BOARD_HAS_DISPLAY
    app_set_state(UI_STATE_RESPONSE);
    ui_set_response("Notification", text);
#endif

    /* Always TTS the notification */
    xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
    /* Store text for TTS playback — reuse g_tts_text buffer */
    extern char g_tts_text[1024];
    strncpy(g_tts_text, text, sizeof(g_tts_text) - 1);
    g_tts_text[sizeof(g_tts_text) - 1] = '\0';

    /* RGB notification pattern (amber pulse) */
    board_rgb_animate(RGB_MODE_BREATHE, 20, 16, 0);  /* amber */
}

static void on_openclaw_state(openclaw_state_t state)
{
    switch (state) {
    case OPENCLAW_STATE_CONNECTED: {
        ui_state_t cur = ui_get_state();
        ESP_LOGI(TAG, "OpenClaw connected");
        xEventGroupSetBits(g_app_events, OC_CONNECTED_BIT);
        ui_set_openclaw_connected(true);
        openclaw_request_health();
        openclaw_request_usage();
        if (cur <= UI_STATE_CONNECTING || cur == UI_STATE_ERROR) {
            app_set_state(UI_STATE_IDLE);
        }
        break;
    }
    case OPENCLAW_STATE_CONNECTING:
    case OPENCLAW_STATE_AUTHENTICATING:
        ui_set_openclaw_connected(false);
        app_set_state(UI_STATE_CONNECTING);
        ui_set_status_message("Connecting OpenClaw...");
        break;
    case OPENCLAW_STATE_CHAT_THINKING:
        app_set_state(UI_STATE_THINKING);
        break;
    case OPENCLAW_STATE_CHAT_STREAMING:
        app_set_state(UI_STATE_STREAMING);
        break;
    case OPENCLAW_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "OpenClaw disconnected — will auto-reconnect");
        xEventGroupClearBits(g_app_events, OC_CONNECTED_BIT);
        ui_set_openclaw_connected(false);
        {
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_IDLE || cur == UI_STATE_BOOT || cur == UI_STATE_CONNECTING) {
                app_set_state(UI_STATE_CONNECTING);
                ui_set_status_message("Reconnecting...");
            }
        }
        error_log_add(ERR_SRC_OPENCLAW, ERR_SEV_WARNING, "WebSocket disconnected");
        break;
    case OPENCLAW_STATE_ERROR:
        ui_set_openclaw_connected(false);
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("OpenClaw error");
        error_log_add(ERR_SRC_OPENCLAW, ERR_SEV_ERROR, "OpenClaw connection error");
        break;
    default:
        break;
    }
}

/* Forward-declare webserver toggle handler */
static void handle_webserver_toggle(void);

/* ── One-shot TTS announcement task (PSRAM stack) ────────────────────── */
static volatile bool s_announce_running = false;
static char s_announce_msg[128];
static StaticTask_t s_announce_tcb;
static StackType_t *s_announce_stack = NULL;

static void announce_tts_task(void *arg)
{
    const char *text = (const char *)arg;
    ESP_LOGI(TAG, "TTS announce start: %s", text);
    wake_word_pause();
    esp_err_t err = tts_speak(text);
    wake_word_resume();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS announce failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "TTS announce complete");
    }
    s_announce_running = false;
    vTaskDelete(NULL);
}

static void speak_announcement(const char *text)
{
    if (s_announce_running) {
        ESP_LOGW(TAG, "TTS announce busy, skipping: %s", text);
        return;
    }
    s_announce_running = true;
    snprintf(s_announce_msg, sizeof(s_announce_msg), "%s", text);

#if CONFIG_IDF_TARGET_ESP32S3
    /* Allocate PSRAM stack once (reused across calls) */
    if (!s_announce_stack) {
        s_announce_stack = heap_caps_calloc(1, 16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_announce_stack) {
        ESP_LOGE(TAG, "Failed to alloc announce stack");
        s_announce_running = false;
        return;
    }
    memset(s_announce_stack, 0, 16384);  /* Clear stack for reuse */
    xTaskCreateStaticPinnedToCore(announce_tts_task, "tts_ann", 16384,
                                   s_announce_msg, 3, s_announce_stack,
                                   &s_announce_tcb, 1);
#else
    xTaskCreatePinnedToCore(announce_tts_task, "tts_ann", 12288,
                            s_announce_msg, 3, NULL, 0);
#endif
}

/* ─── Main ───────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  HeyClawy v%s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "  OpenClaw ESP32 Interface Device");
    ESP_LOGI(TAG, "========================================");

    /* Log wake cause (useful for deep sleep debugging) */
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        const char *cause = "unknown";
        switch (wakeup) {
            case ESP_SLEEP_WAKEUP_EXT0:      cause = "EXT0 (button)"; break;
            case ESP_SLEEP_WAKEUP_EXT1:      cause = "EXT1"; break;
            case ESP_SLEEP_WAKEUP_TIMER:     cause = "TIMER"; break;
            case ESP_SLEEP_WAKEUP_GPIO:      cause = "GPIO"; break;
            case ESP_SLEEP_WAKEUP_UART:      cause = "UART"; break;
            default: break;
        }
        ESP_LOGW(TAG, "Woke from deep sleep — cause: %s (%d)", cause, wakeup);
    }

    /* Init settings from NVS (must be first) */
    settings_t defaults = {0};
    strncpy(defaults.wifi_ssid,     SECRETS_WIFI_SSID,     sizeof(defaults.wifi_ssid) - 1);
    strncpy(defaults.wifi_password, SECRETS_WIFI_PASSWORD,  sizeof(defaults.wifi_password) - 1);
    strncpy(defaults.oc_host,       SECRETS_OPENCLAW_HOST,  sizeof(defaults.oc_host) - 1);
    defaults.oc_port = SECRETS_OPENCLAW_PORT;
    strncpy(defaults.oc_token,      SECRETS_OPENCLAW_TOKEN, sizeof(defaults.oc_token) - 1);
    strncpy(defaults.oc_device_key, SECRETS_DEVICE_KEY_HEX, sizeof(defaults.oc_device_key) - 1);
    strncpy(defaults.tts_host,      SECRETS_TTS_HOST,       sizeof(defaults.tts_host) - 1);
    defaults.tts_port = SECRETS_TTS_PORT;
    strncpy(defaults.tts_api_key,   SECRETS_TTS_API_KEY,    sizeof(defaults.tts_api_key) - 1);
    strncpy(defaults.tts_voice,     SECRETS_TTS_VOICE,      sizeof(defaults.tts_voice) - 1);
    strncpy(defaults.stt_host,      SECRETS_STT_HOST,       sizeof(defaults.stt_host) - 1);
    defaults.stt_port = SECRETS_STT_PORT;
    defaults.volume = APP_SPEAKER_VOLUME;
    settings_init(&defaults);

    /* Error log */
    error_log_init();

    const settings_t *cfg = settings_get();

    g_app_events = xEventGroupCreate();

    /* Pass event group to UI for button callbacks */
    ui_set_event_group(g_app_events);

    /* Init hardware */
    esp_err_t ret = board_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "Board: %s (%s)", board_get_name(), board_get_mcu());

    /* Configure power management — CPU frequency scaling */
#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,  /* Don't auto-sleep — WiFi needs active CPU */
    };
    ret = esp_pm_configure(&pm_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PM: CPU scaling %d-%dMHz", 80, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    } else {
        ESP_LOGW(TAG, "PM configure failed: %s", esp_err_to_name(ret));
    }
#endif

    board_audio_set_volume(cfg->volume);
    board_display_set_brightness(cfg->brightness);

    /* Camera init deferred to first use (saves ~5-10mA from SSCMA background tasks) */
    ESP_LOGI(TAG, "Camera: deferred to first use (power saving)");
    (void)ret;  /* camera_init() will be called on first CAMERA_BIT */

    /* RGB */
    board_rgb_task_start();
    if (cfg->rgb_enabled) {
#if BOARD_RGB_LED_COUNT > 1
        board_rgb_animate(RGB_MODE_RAINBOW_SPIN, 20, 0, 0);
#else
        board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
#endif
    } else {
        board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);
    }

    /* Init UI */
    ret = ui_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "UI init failed (continuing)");
    /* Init tasks screen */
    ui_tasks_init();
    ui_tasks_set_event_group(g_app_events);
    app_set_state(UI_STATE_BOOT);
    ui_set_status_message("Starting...");

    /* Init TTS from settings */
    tts_config_t tts_cfg = {
        .host = cfg->tts_host,
        .port = cfg->tts_port,
        .api_key = cfg->tts_api_key,
        .voice = cfg->tts_voice,
        .model = "tts-1",
    };
    tts_init(&tts_cfg);

    /* Init STT from settings */
    stt_init(cfg->stt_host, cfg->stt_port);

    /* Connect WiFi */
    wifi_manager_init(cfg->wifi_ssid, cfg->wifi_password, on_wifi_state);

    EventBits_t bits = xEventGroupWaitBits(g_app_events, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi timeout — continuing anyway");
    } else {
        ESP_LOGI(TAG, "WiFi ready");
    }

    /* Wait for SNTP */
    ESP_LOGI(TAG, "Waiting for SNTP time sync...");
    bool time_ok = false;
    for (int i = 0; i < 20; i++) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > 1700000000) {
            ESP_LOGI(TAG, "Time synced (attempt %d)", i + 1);
            /* Print current date/time */
            time_t now = tv.tv_sec;
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            char strftime_buf[64];
            strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "Current time: %s (UTC)", strftime_buf);
            time_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!time_ok) {
        ESP_LOGW(TAG, "SNTP timeout — auth may fail");
        error_log_add(ERR_SRC_DEVICE, ERR_SEV_WARNING, "SNTP time sync failed");
    }

    /* Connect to OpenClaw */
    openclaw_config_t oc_config = {
        .host = cfg->oc_host,
        .port = cfg->oc_port,
        .token = cfg->oc_token,
        .device_key_hex = cfg->oc_device_key,
        .session_key = SECRETS_OPENCLAW_SESSION_KEY,
    };
    openclaw_init(&oc_config, on_openclaw_state);
    openclaw_set_notify_cb(on_openclaw_notify);
    openclaw_connect();

    /* Start background tasks */
    app_tasks_start();
    serial_cmd_task_start();

    /* Init wake word detection (non-critical — continue if fails) */
    ret = wake_word_init();
    if (ret == ESP_OK) {
        wake_word_start(g_app_events, WAKE_WORD_BIT);
        ESP_LOGI(TAG, "Wake word: \"%s\"", wake_word_get_phrase());
    } else {
        ESP_LOGW(TAG, "Wake word init failed — voice wake disabled");
    }

    ESP_LOGI(TAG, "All tasks started. Ready.");

    /* ── Main event loop ── */
    while (1) {
        EventBits_t ev = xEventGroupWaitBits(g_app_events,
                                              KNOB_PRESSED_BIT |
                                              WEBSERVER_TOGGLE_BIT | DETAILS_BIT |
                                              TOUCH_BIT | TASKS_SCREEN_BIT |
                                              CAMERA_BIT | WAKE_WORD_BIT,
                                              pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(100));

        if (ev & TOUCH_BIT) {
            app_reset_activity_timer();
            if (g_recording) {
                /* Touch during recording = cancel */
                ESP_LOGI(TAG, "Touch during recording — cancelling");
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else if (ui_get_state() == UI_STATE_RESPONSE) {
                if (g_tts_pending) {
                    ESP_LOGD(TAG, "Touch during RESPONSE ignored — TTS pending");
                } else {
                    /* Touch during response = dismiss */
                    ESP_LOGI(TAG, "Touch dismissed response");
                    g_response_shown_at = 0;
                    app_set_state(UI_STATE_IDLE);
                }
            } else if (!app_is_sleeping()) {
                board_play_tick();
            }
        }

        if (ev & TASKS_SCREEN_BIT) {
            app_reset_activity_timer();
            if (ui_tasks_is_visible()) {
                ui_tasks_hide();
            } else {
                ui_tasks_show();
            }
        }

        if (ev & KNOB_PRESSED_BIT) {
            app_reset_activity_timer();
            /* If on tasks screen, go back to main */
            if (ui_tasks_is_visible()) {
                ui_tasks_hide();
                continue;
            }
            /* If sleeping or just woke, don't start recording */
            if (app_is_sleeping() || app_just_woke()) {
                ESP_LOGI(TAG, "Woke from sleep via event — ignoring action");
                continue;
            }
            ui_state_t cur = ui_get_state();
            switch (cur) {
            case UI_STATE_IDLE:
            case UI_STATE_RESPONSE:
                /* Both idle and response → start new recording */
                wake_word_pause();
                voice_chat_start();
                wake_word_resume();
                break;
            case UI_STATE_TTS_PLAYING:
            case UI_STATE_TTS_LOADING:
                tts_stop();
                g_continue_listening = false;
                app_set_state(UI_STATE_IDLE);
                break;
            case UI_STATE_THINKING:
            case UI_STATE_STREAMING:
                /* Abort the current chat operation */
                ESP_LOGI(TAG, "Aborting chat (state=%d)...", cur);
                openclaw_chat_abort();
                ui_set_status_message("Aborting...");
                board_rgb_animate(RGB_MODE_BLINK, 32, 16, 0);
                vTaskDelay(pdMS_TO_TICKS(800));
                app_set_state(UI_STATE_IDLE);
                break;
            default:
                break;
            }
        }

        if (ev & WAKE_WORD_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Wake word detected!");
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE) {
                wake_word_pause();
                voice_chat_start();
                wake_word_resume();
            } else if (cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_TTS_LOADING) {
                /* Talk-mode style barge-in: user speech interrupts playback immediately. */
                ESP_LOGI(TAG, "Wake word during TTS — barging in");
                tts_stop();
                g_continue_listening = false;
                wake_word_pause();
                voice_chat_start();
                wake_word_resume();
            }
        }

        if (ev & DETAILS_BIT) {
            app_reset_activity_timer();
            if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
                ESP_LOGI(TAG, "Requesting details...");
                app_set_state(UI_STATE_SENDING);
                openclaw_chat_send_details(app_on_chat_response);
            }
        }

        if (ev & WEBSERVER_TOGGLE_BIT) {
            handle_webserver_toggle();
        }

        if (ev & CAMERA_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Camera capture requested");
            /* Lazy init: initialize camera on first use */
            if (!camera_is_ready()) {
                ui_set_status_message("Starting camera...");
                ret = camera_init();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
                    ui_set_status_message("Camera not available");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    if (ui_get_state() == UI_STATE_BOOT) app_set_state(UI_STATE_IDLE);
                    continue;
                }
            }
            if (!camera_is_ready()) {
                ui_set_status_message("Camera not ready");
                vTaskDelay(pdMS_TO_TICKS(1500));
            } else {
                ui_set_status_message("Capturing...");
                uint8_t *jpeg = NULL;
                size_t jpeg_sz = 0;
                esp_err_t ret = camera_capture_jpeg(&jpeg, &jpeg_sz);
                if (ret == ESP_OK && jpeg && jpeg_sz > 0) {
                    // Store for next voice chat
                    if (g_pending_jpeg) free(g_pending_jpeg);
                    g_pending_jpeg = jpeg;
                    g_pending_jpeg_size = jpeg_sz;
                    ESP_LOGI(TAG, "Image captured: %d bytes, pending for next voice chat", (int)jpeg_sz);

                    // Show preview on display
                    ui_show_camera_preview(jpeg, jpeg_sz);
                    ui_set_status_message("📷 Ready (talk to send)");
                } else {
                    ESP_LOGE(TAG, "Camera capture failed: %s", esp_err_to_name(ret));
                    ui_set_status_message("Capture failed");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
            }
            if (ui_get_state() == UI_STATE_BOOT) {
                app_set_state(UI_STATE_IDLE);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ─── Web server toggle ──────────────────────────────────────────────── */
static void handle_webserver_toggle(void)
{
    if (!webserver_is_running()) {
        const char *ip = wifi_manager_get_ip();
        if (webserver_start() == ESP_OK) {
            ESP_LOGI(TAG, "Web server started on http://%s", ip);
            ui_set_webserver_status(true);
#if BOARD_HAS_DISPLAY
            /* Show IP on screen for 5 seconds */
            char msg[80];
            snprintf(msg, sizeof(msg), "Go to\n%s", ip);
            ui_set_status_message(msg);
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (ui_get_state() == UI_STATE_IDLE || ui_get_state() == UI_STATE_BOOT) {
                app_set_state(UI_STATE_IDLE);
            }
#else
            /* Audio-only device: speak the IP address */
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "Web server started. Connect to %s", ip);
                speak_announcement(msg);
            }
#endif
        } else {
            error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "Web server failed to start");
        }
    } else {
        webserver_stop();
        ESP_LOGI(TAG, "Web server stopped");
        ui_set_webserver_status(false);
    }
}
