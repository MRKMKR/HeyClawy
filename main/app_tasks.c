/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * Background tasks — knob, status polling, TTS playback
 */

#include "app_tasks.h"
#include "app_state.h"
#include "settings.h"

#include <sys/time.h>

#include "board.h"
#include "camera.h"
#include "openclaw_client.h"
#include "wifi_manager.h"
#include "tts_client.h"
#include "ui.h"
#include "ui_tasks.h"
#include "wake_word.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#if BOARD_HAS_DISPLAY
#include "esp_lvgl_port.h"
#endif
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "app_tasks";

/* TTS text buffer for notifications (set by notify callback, read by TTS task) */
char g_tts_text[1024] = {0};

#define RESPONSE_TIMEOUT_MS 60000

/* Forward declaration */
static void enter_deep_sleep(void);  /* internal — called from knob_task */
static void presence_task(void *arg);

/* ── Knob monitoring — long press detection + cancel ──────────────────── */
#define LONG_PRESS_MS 4000
#define DEBOUNCE_MS   50

static void knob_task(void *arg)
{
    bool was_pressed = false;
    int64_t press_start = 0;
    bool long_press_fired = false;

    while (1) {
#if BOARD_HAS_KNOB
        bool pressed = board_knob_button_pressed();
#elif BOARD_HAS_USER_BUTTONS && !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
        /* M5Stick handles Button A in the M5Stick-specific section with double-click */
        bool pressed = board_boot_button_pressed();
#else
        bool pressed = false;
#endif

        if (pressed && !was_pressed) {
            /* Button just pressed — record timestamp */
            press_start = esp_timer_get_time() / 1000; /* ms */
            long_press_fired = false;
        } else if (pressed && was_pressed && !long_press_fired) {
            /* Button held — check for long press */
            int64_t held_ms = (esp_timer_get_time() / 1000) - press_start;
            if (held_ms >= LONG_PRESS_MS) {
                long_press_fired = true;
#if BOARD_HAS_USER_BUTTONS && !BOARD_HAS_KNOB
                /* M5Stick/Audio: Button A long press = cancel/abort, NOT deep sleep */
                ESP_LOGW(TAG, "Button A long press — cancel/abort");
                ui_state_t st = ui_get_state();
                if (st == UI_STATE_THINKING || st == UI_STATE_STREAMING) {
                    openclaw_chat_abort();
                    app_set_state(UI_STATE_IDLE);
                } else if (st == UI_STATE_TTS_LOADING || st == UI_STATE_TTS_PLAYING) {
                    tts_stop();
                }
#else
                /* SenseCAP: knob long press = deep sleep */
                ESP_LOGW(TAG, "Long press detected (%lld ms) — entering deep sleep", held_ms);
                board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                enter_deep_sleep();
#endif
            }
        } else if (!pressed && was_pressed && !long_press_fired) {
            /* Button released (short press) */
            if (g_recording) {
                /* During recording: cancel or stop */
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else {
                xEventGroupSetBits(g_app_events, KNOB_PRESSED_BIT);
            }
        }

        was_pressed = pressed;

#if BOARD_HAS_KNOB
        /* Check for wheel spin (rotary encoder) */
        int delta = board_knob_get_delta();
        if (delta != 0) {
            app_reset_activity_timer();
            if (g_recording) {
                /* Wheel spin during recording = cancel */
                ESP_LOGI(TAG, "Wheel spin during recording — cancelling");
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else if (ui_tasks_is_visible()) {
                /* Scroll tasks list */
                ui_tasks_scroll(delta);
            } else if (ui_get_state() == UI_STATE_SENDING) {
                /* Spin during SENDING — set CANCEL_BIT for voice_chat to check */
                ESP_LOGI(TAG, "Wheel spin during SENDING — setting cancel");
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else if (ui_get_state() == UI_STATE_THINKING ||
                       ui_get_state() == UI_STATE_STREAMING) {
                /* Spin during THINKING/STREAMING = abort chat */
                ESP_LOGI(TAG, "Wheel spin during %s — aborting chat",
                         ui_get_state() == UI_STATE_THINKING ? "THINKING" : "STREAMING");
                openclaw_chat_abort();
                app_set_state(UI_STATE_IDLE);
            } else if (ui_get_state() == UI_STATE_TTS_LOADING ||
                       ui_get_state() == UI_STATE_TTS_PLAYING) {
                /* Spin during TTS = stop playback */
                ESP_LOGI(TAG, "Wheel spin during TTS — stopping playback");
                tts_stop();
                g_continue_listening = false;
            } else if (ui_get_state() == UI_STATE_RESPONSE) {
                if (g_tts_pending) {
                    /* TTS is about to play — cancel it */
                    ESP_LOGI(TAG, "Wheel spin during RESPONSE — cancelling pending TTS");
                    g_tts_pending = false;
                    g_continue_listening = false;
                    g_response_shown_at = esp_timer_get_time();
                } else {
                    /* Spin during RESPONSE = dismiss response */
                    ESP_LOGI(TAG, "Wheel spin dismissed response");
                    g_response_shown_at = 0;
                    app_set_state(UI_STATE_IDLE);
                }
            } else if (ui_get_state() == UI_STATE_IDLE) {
                if (ui_cycle_idle_page(delta)) {
                    ESP_LOGI(TAG, "Wheel switched idle page");
                    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
                    continue;
                }
                /* If pending camera image, discard it (require 3+ ticks to avoid noise) */
                if (g_pending_jpeg && (delta > 2 || delta < -2)) {
                    ESP_LOGI(TAG, "Wheel spin — discarding pending camera image");
                    free(g_pending_jpeg);
                    g_pending_jpeg = NULL;
                    g_pending_jpeg_size = 0;
                    app_set_state(UI_STATE_IDLE);
                }
                /* Spin during IDLE + external activity = abort external */
                const openclaw_info_t *info = openclaw_get_info();
                if (info->is_active && info->is_external && info->active_session_key[0]) {
                    ESP_LOGI(TAG, "Wheel spin during external activity — aborting session=%s",
                             info->active_session_key);
                    openclaw_chat_abort_session(info->active_session_key);
                }
            }
            /* Future: volume control, menu scroll in other states */
        }
#endif /* BOARD_HAS_KNOB */

        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

#if BOARD_HAS_USER_BUTTONS && !BOARD_HAS_KNOB
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        /* ══════════════════════════════════════════════════════════════════
         * M5StickCPlus2 — 3-button system with short/long/double-click
         *
         * Button A (front, GPIO37) — Primary:
         *   Short:  Talk (IDLE/RESPONSE) / Cancel (LISTENING)
         *   Long:   Deep sleep
         *   DblClk: Toggle auto-read TTS
         *
         * Button B (side, GPIO39) — Secondary:
         *   Short:  Context: Web(IDLE) / Cancel(REC/SEND) / Abort(THINK) /
         *           Play(RESPONSE) / Stop(TTS)
         *   Long:   Tasks screen
         *   DblClk: Device status info
         *
         * Button C (power, GPIO35) — Utility:
         *   Short:  Details(RESPONSE) / Status(IDLE)
         *   Long:   Deep sleep
         *   DblClk: Toggle web server
         * ════════════════════════════════════════════════════════════════ */

        /* ── Double-click detection helper macros ── */
        #define DBLCLK_WINDOW_MS 350

        /* ── Button A ── */
        {
            static bool a_was = false;
            static int64_t a_press_ms = 0;
            static bool a_long_fired = false;
            static int64_t a_release_ms = 0;
            static bool a_pending_single = false;
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool a_now = board_boot_button_pressed();

            if (a_now && !a_was) {
                /* Press down */
                if (a_pending_single && (now_ms - a_release_ms) < DBLCLK_WINDOW_MS) {
                    /* Double click! */
                    a_pending_single = false;
                    app_reset_activity_timer();
                    bool ar = settings_get()->auto_read_response;
                    settings_get_mutable()->auto_read_response = !ar;
                    settings_save();
                    ESP_LOGI(TAG, "Btn A DblClk: auto-read TTS %s", ar ? "OFF" : "ON");
                    ui_set_status_message(ar ? "Auto-TTS: OFF" : "Auto-TTS: ON");
                }
                a_press_ms = now_ms;
                a_long_fired = false;
            } else if (a_now && a_was && !a_long_fired) {
                /* Held */
                if ((now_ms - a_press_ms) >= LONG_PRESS_MS) {
                    a_long_fired = true;
                    ESP_LOGW(TAG, "Btn A long press — deep sleep");
                    board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    enter_deep_sleep();
                }
            } else if (!a_now && a_was && !a_long_fired) {
                /* Released — wait for potential double click */
                a_release_ms = now_ms;
                a_pending_single = true;
            }

            /* Fire single press after double-click window expires */
            if (a_pending_single && !a_now && (now_ms - a_release_ms) >= DBLCLK_WINDOW_MS) {
                a_pending_single = false;
                app_reset_activity_timer();
                if (g_recording) {
                    ESP_LOGI(TAG, "Btn A: cancel recording");
                    xEventGroupSetBits(g_app_events, CANCEL_BIT);
                } else {
                    ESP_LOGI(TAG, "Btn A: talk");
                    xEventGroupSetBits(g_app_events, KNOB_PRESSED_BIT);
                }
            }
            a_was = a_now;
        }

        /* ── Button B ── */
        {
            static bool b_was = false;
            static int64_t b_press_ms = 0;
            static bool b_long_fired = false;
            static int64_t b_release_ms = 0;
            static bool b_pending_single = false;
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool b_now = board_user_button_pressed(2);

            if (b_now && !b_was) {
                if (b_pending_single && (now_ms - b_release_ms) < DBLCLK_WINDOW_MS) {
                    /* Double click — show status info */
                    b_pending_single = false;
                    app_reset_activity_timer();
                    ESP_LOGI(TAG, "Btn B DblClk: status info");
                    xEventGroupSetBits(g_app_events, DETAILS_BIT);
                }
                b_press_ms = now_ms;
                b_long_fired = false;
            } else if (b_now && b_was && !b_long_fired) {
                if ((now_ms - b_press_ms) >= (LONG_PRESS_MS / 2)) {
                    b_long_fired = true;
                    app_reset_activity_timer();
                    ESP_LOGI(TAG, "Btn B long press: tasks screen");
                    xEventGroupSetBits(g_app_events, TASKS_SCREEN_BIT);
                }
            } else if (!b_now && b_was && !b_long_fired) {
                b_release_ms = now_ms;
                b_pending_single = true;
            }

            if (b_pending_single && !b_now && (now_ms - b_release_ms) >= DBLCLK_WINDOW_MS) {
                b_pending_single = false;
                app_reset_activity_timer();
                ui_state_t st = ui_get_state();
                if (st == UI_STATE_LISTENING) {
                    ESP_LOGI(TAG, "Btn B: cancel recording");
                    xEventGroupSetBits(g_app_events, CANCEL_BIT);
                } else if (st == UI_STATE_THINKING || st == UI_STATE_STREAMING) {
                    ESP_LOGI(TAG, "Btn B: abort chat");
                    openclaw_chat_abort();
                    app_set_state(UI_STATE_IDLE);
                } else if (st == UI_STATE_TTS_LOADING || st == UI_STATE_TTS_PLAYING) {
                    ESP_LOGI(TAG, "Btn B: stop TTS");
                    tts_stop();
                    g_continue_listening = false;
                } else if (st == UI_STATE_RESPONSE) {
                    if (g_tts_pending) {
                        ESP_LOGI(TAG, "Btn B: play TTS");
                        xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
                    } else {
                        ESP_LOGI(TAG, "Btn B: dismiss response");
                        g_response_shown_at = 0;
                        app_set_state(UI_STATE_IDLE);
                    }
                } else if (st == UI_STATE_SENDING) {
                    ESP_LOGI(TAG, "Btn B: cancel send");
                    xEventGroupSetBits(g_app_events, CANCEL_BIT);
                } else {
                    ESP_LOGI(TAG, "Btn B: toggle web server");
                    xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
                }
            }
            b_was = b_now;
        }

        /* ── Button C ── */
        {
            static bool c_was = false;
            static int64_t c_press_ms = 0;
            static bool c_long_fired = false;
            static int64_t c_release_ms = 0;
            static bool c_pending_single = false;
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool c_now = board_user_button_pressed(3);

            if (c_now && !c_was) {
                if (c_pending_single && (now_ms - c_release_ms) < DBLCLK_WINDOW_MS) {
                    /* Double click — toggle web server */
                    c_pending_single = false;
                    app_reset_activity_timer();
                    ESP_LOGI(TAG, "Btn C DblClk: toggle webserver");
                    xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
                }
                c_press_ms = now_ms;
                c_long_fired = false;
            } else if (c_now && c_was && !c_long_fired) {
                if ((now_ms - c_press_ms) >= LONG_PRESS_MS) {
                    c_long_fired = true;
                    ESP_LOGW(TAG, "Btn C long press — deep sleep");
                    board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    enter_deep_sleep();
                }
            } else if (!c_now && c_was && !c_long_fired) {
                c_release_ms = now_ms;
                c_pending_single = true;
            }

            if (c_pending_single && !c_now && (now_ms - c_release_ms) >= DBLCLK_WINDOW_MS) {
                c_pending_single = false;
                app_reset_activity_timer();
                ui_state_t st = ui_get_state();
                if (st == UI_STATE_RESPONSE) {
                    ESP_LOGI(TAG, "Btn C: show details");
                    xEventGroupSetBits(g_app_events, DETAILS_BIT);
                } else {
                    ESP_LOGI(TAG, "Btn C: tasks screen");
                    xEventGroupSetBits(g_app_events, TASKS_SCREEN_BIT);
                }
            }
            c_was = c_now;
        }
#else
        /* Waveshare audio board: BTN1 = webserver toggle */
        static bool btn1_was = false;
        bool btn1 = board_user_button_pressed(1);
        if (btn1 && !btn1_was) {
            app_reset_activity_timer();
            xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
        }
        btn1_was = btn1;
#endif /* CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2 */
#endif /* BOARD_HAS_USER_BUTTONS && !BOARD_HAS_KNOB */
    }
}

/* ── TTS playback ────────────────────────────────────────────────────── */
static void tts_play_task(void *arg)
{
    while (1) {
        xEventGroupWaitBits(g_app_events, TTS_PLAY_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        g_tts_pending = false;  /* TTS task now owns the transition */

        if (tts_is_playing()) continue;

        /* Check for notification text first, then UI response */
        const char *text = NULL;
        bool is_notification = false;
        if (g_tts_text[0]) {
            text = g_tts_text;
            is_notification = true;
        } else {
            text = ui_get_full_response();
        }
        if (!text || !text[0]) {
            ESP_LOGW(TAG, "No response text for TTS");
            app_set_state(UI_STATE_IDLE);
            continue;
        }

        app_set_state(UI_STATE_TTS_LOADING);
        ESP_LOGI(TAG, "TTS: speaking %.60s%s", text, strlen(text) > 60 ? "..." : "");

        wake_word_pause();
        esp_err_t err = tts_speak(text);
        wake_word_resume();

        /* Clear notification text after speaking */
        if (is_notification) g_tts_text[0] = '\0';

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TTS failed: %s", esp_err_to_name(err));
            /* Show small error in sub_label without wiping the response */
            app_set_state(UI_STATE_RESPONSE);
            ui_set_response(NULL, text);  /* Restore response display */
            ui_set_status_message("TTS unavailable");
            vTaskDelay(pdMS_TO_TICKS(2000));
            g_continue_listening = false;
        }

        app_set_state(UI_STATE_IDLE);

        /* If OpenClaw expects a follow-up reply, auto-trigger recording */
        if (g_continue_listening) {
            g_continue_listening = false;
            ESP_LOGI(TAG, "Auto-listen: continuing conversation");
            vTaskDelay(pdMS_TO_TICKS(400));  /* brief pause before listening */
            xEventGroupSetBits(g_app_events, WAKE_WORD_BIT);
        }
    }
}

/* ── Sleep state (declared early so status_update_task can check) ───── */
static volatile int64_t s_last_activity_us = 0;
static volatile int64_t s_last_wake_us = 0;
static volatile bool s_sleeping = false;

/* ── Periodic status ─────────────────────────────────────────────────── */
static void status_update_task(void *arg)
{
    int usage_poll_counter = 0;
    int sleep_health_counter = 0;
    int carousel_tick = 0;
    int reconnect_ticks = 0;
    while (1) {
        /* ── Sleep mode: minimal processing ────────────────────────── */
        if (s_sleeping) {
            /* Only check for external activity + reconnect watchdog during sleep.
             * Skip all UI updates, RSSI, timers — saves CPU cycles. */
            if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
                if (openclaw_consume_external_activity()) {
                    ESP_LOGI(TAG, "External OC activity detected during sleep — waking!");
                    app_reset_activity_timer();
                }
                /* Slow poll during sleep — health comes from server push, only poll tasks */
                if (++sleep_health_counter >= 30) {  /* Every 60s = 30 * 2000ms */
                    sleep_health_counter = 0;
                    openclaw_request_tasks();
                }
                /* Check health data for activity during sleep */
                const openclaw_info_t *info = openclaw_get_info();
                if (info->last_activity_sec < 15) {
                    ESP_LOGI(TAG, "OC active (last=%ds) during sleep — waking!", info->last_activity_sec);
                    app_reset_activity_timer();
                }
            }

            /* Reconnect watchdog still runs during sleep */
            {
                openclaw_state_t oc_st = openclaw_get_state();
                if (oc_st == OPENCLAW_STATE_DISCONNECTED || oc_st == OPENCLAW_STATE_ERROR ||
                    oc_st == OPENCLAW_STATE_CONNECTING) {
                    reconnect_ticks++;
                    if (reconnect_ticks >= 15) {  /* 30s = 15 * 2000ms */
                        reconnect_ticks = 0;
                        ESP_LOGW(TAG, "Reconnect watchdog (sleep): forcing reconnect");
                        openclaw_disconnect();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        openclaw_connect();
                    }
                } else {
                    reconnect_ticks = 0;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(2000));  /* 2s poll during sleep (was 500ms) */
            continue;
        }

        /* ── Awake mode: full processing ───────────────────────────── */
        /* WiFi RSSI */
        if (wifi_manager_get_state() == WIFI_STATE_CONNECTED) {
            ui_set_wifi_status(true, wifi_manager_get_rssi());
        }

        /* Thinking timer + detail */
        if (ui_get_state() == UI_STATE_THINKING) {
            uint32_t think_ms = openclaw_get_thinking_time_ms();
            const openclaw_info_t *tinfo = openclaw_get_info();
            if (tinfo->active_detail[0]) {
                ui_set_thinking_detail(tinfo->active_detail, think_ms);
            } else {
                ui_set_thinking_time(think_ms);
            }
        }

        /* Response timeout */
        if (ui_get_state() == UI_STATE_RESPONSE && g_response_shown_at > 0) {
            int64_t elapsed = (esp_timer_get_time() - g_response_shown_at) / 1000;
            if (elapsed > RESPONSE_TIMEOUT_MS) {
                app_set_state(UI_STATE_IDLE);
                g_response_shown_at = 0;
            }
        }

        /* Server info — full processing when awake */
        if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
            /* Consume flag if any (was set between sleeps) */
            openclaw_consume_external_activity();

            const openclaw_info_t *info = openclaw_get_info();

            /* Carousel rotation: advance every 3s (6 ticks × 500ms) when multiple runs */
            if (info->active_runs_count > 1) {
                if (++carousel_tick >= 6) {
                    carousel_tick = 0;
                    /* Cast away const to update carousel index — it's only a display hint */
                    ((openclaw_info_t *)info)->carousel_index++;
                }
            } else {
                carousel_tick = 0;
            }

            ui_set_server_info(info);

            /* Display task info — use detailed version */
            if (info->has_tasks) {
                ui_set_task_info_detailed(info);
            }

            /* Show external activity elapsed timer */
            if (info->is_active && info->is_external && info->active_started_ms > 0) {
                struct timeval tv; gettimeofday(&tv, NULL);
                int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                uint32_t elapsed = (uint32_t)(now_ms - info->active_started_ms);
                ui_set_external_activity(true, info->active_detail, elapsed);
            } else if (!info->is_active) {
                ui_set_external_activity(false, NULL, 0);
            }

            /* RGB LED activity indicators when IDLE */
            static bool activity_led_active = false;
            bool oc_active = info->is_active || (info->last_activity_sec < 30);
            bool bg_running = (info->tasks_running > 0);

            if (ui_get_state() == UI_STATE_IDLE && (oc_active || bg_running)) {
                if (!activity_led_active) {
                    if (info->is_active) {
                        /* Blue pulse for active processing (external request) */
                        board_rgb_animate(RGB_MODE_BREATHE, 0, 8, 24);
                    } else if (bg_running) {
                        /* Teal pulse for bg tasks only */
                        board_rgb_animate(RGB_MODE_BREATHE, 0, 8, 12);
                    } else {
                        /* Dim blue for recent activity */
                        board_rgb_animate(RGB_MODE_BREATHE, 0, 4, 12);
                    }
                    activity_led_active = true;
                }
            } else if (activity_led_active && !oc_active && !bg_running) {
                activity_led_active = false;
                if (ui_get_state() == UI_STATE_IDLE) {
                    app_led_for_state(UI_STATE_IDLE);
                }
            }

            /* Poll interval: 5s when active/tasks visible, 60s otherwise.
             * Health is pushed by server (~30s), so we only request it during fast poll.
             * usage.status is cost data — only fetch when tasks screen is visible. */
            bool fast_poll = (bg_running || oc_active || ui_tasks_is_visible());
            int poll_threshold = fast_poll ? 10 : 120;  /* 5s active, 60s idle */
            if (++usage_poll_counter >= poll_threshold) {
                usage_poll_counter = 0;
                if (fast_poll) {
                    openclaw_request_health();
                    openclaw_request_usage();
                }
                openclaw_request_tasks();
                if (ui_tasks_is_visible()) {
                    ui_tasks_refresh();
                }
            }
        }

        /* ── Reconnect watchdog ── */
        {
            openclaw_state_t oc_st = openclaw_get_state();
            if (oc_st == OPENCLAW_STATE_DISCONNECTED || oc_st == OPENCLAW_STATE_ERROR ||
                oc_st == OPENCLAW_STATE_CONNECTING) {
                reconnect_ticks++;
                /* If stuck for 30s (60 × 500ms), force full reconnect */
                if (reconnect_ticks >= 60) {
                    reconnect_ticks = 0;
                    ESP_LOGW(TAG, "Reconnect watchdog: stuck in state %d for 30s, forcing reconnect", oc_st);
                    openclaw_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    openclaw_connect();
                }
            } else {
                reconnect_ticks = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── Sleep management ────────────────────────────────────────────────── */

void app_reset_activity_timer(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_sleeping) {
        s_sleeping = false;
        s_last_wake_us = esp_timer_get_time();
        /* Restore display and LED on wake */
        const settings_t *cfg = settings_get();
        board_display_set_brightness(cfg->brightness);
        if (cfg->rgb_enabled) app_led_for_state(ui_get_state());
#if BOARD_HAS_DISPLAY
        /* Resume LVGL timer task */
        lvgl_port_resume();
#endif
#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
        /* Restore faster WiFi PS when awake */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
        /* Resume wake word if it was paused for sleep */
        if (!cfg->wake_word_in_sleep) {
            wake_word_resume();
        }
        ESP_LOGI(TAG, "Wake from light sleep (activity)");
    }
}

bool app_just_woke(void)
{
    if (s_last_wake_us == 0) return false;
    return (esp_timer_get_time() - s_last_wake_us) < 800000; /* 800ms cooldown */
}

bool app_is_sleeping(void)
{
    return s_sleeping;
}

/* Deep sleep: board-specific wake source */
static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "Entering deep sleep — press button to wake");

    /* Disconnect OpenClaw + WiFi so no events can trigger anything */
    openclaw_disconnect();
    esp_wifi_disconnect();
    esp_wifi_stop();

    board_display_set_brightness(0);
    board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Power off peripherals + configure wake GPIO (board-specific) */
    board_prepare_deep_sleep();

    ESP_LOGW(TAG, "Calling esp_deep_sleep_start()");
    esp_deep_sleep_start();
    /* Never returns — device will reboot on wake */
}

void app_enter_deep_sleep(void)
{
    enter_deep_sleep();
}

static void sleep_task(void *arg)
{
    s_last_activity_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        const settings_t *cfg = settings_get();
        if (cfg->sleep_timeout_ms == 0) continue; /* sleep disabled */

        ui_state_t state = ui_get_state();
        /* Don't sleep during active operations */
        if (state == UI_STATE_LISTENING || state == UI_STATE_SENDING ||
            state == UI_STATE_THINKING || state == UI_STATE_STREAMING ||
            state == UI_STATE_TTS_LOADING || state == UI_STATE_TTS_PLAYING) {
            s_last_activity_us = esp_timer_get_time();
            continue;
        }

        /* Don't enter sleep if OpenClaw is actively processing */
        if (!s_sleeping && openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
            const openclaw_info_t *info = openclaw_get_info();
            if (info->last_activity_sec < 30) {
                s_last_activity_us = esp_timer_get_time();
                continue;
            }
        }

        int64_t idle_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;

        if (s_sleeping) {
            /* Light sleep mode: display/LED off, CPU running.
             * Deep sleep is ONLY triggered by long wheel press (knob_task),
             * NOT automatically — deep sleep = full cold reboot on ESP32-S3
             * which takes ~20s to reconnect WiFi+SNTP+OpenClaw. */

            /* Check for wake events (button press) */
#if BOARD_HAS_KNOB
            if (board_knob_button_pressed()) {
                while (board_knob_button_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
#elif BOARD_HAS_USER_BUTTONS
            if (board_boot_button_pressed()) {
                while (board_boot_button_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
#else
            if (false) {
#endif
                /* Wake from light sleep */
                ESP_LOGI(TAG, "Wake from light sleep (knob press)");
                s_sleeping = false;
                s_last_wake_us = esp_timer_get_time();
                board_display_set_brightness(cfg->brightness);
                if (cfg->rgb_enabled) app_led_for_state(ui_get_state());
#if BOARD_HAS_DISPLAY
                lvgl_port_resume();
#endif
#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
                if (!cfg->wake_word_in_sleep) {
                    wake_word_resume();
                }
                s_last_activity_us = esp_timer_get_time();
            }
            continue;
        }

        if (idle_ms > (int64_t)cfg->sleep_timeout_ms) {
            ESP_LOGI(TAG, "Idle for %lld ms, entering light sleep", idle_ms);
            s_sleeping = true;

            /* Turn off display and LED to save power */
            board_display_set_brightness(0);
            board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);

#if BOARD_HAS_DISPLAY
            /* Pause LVGL timer task — no display updates needed during sleep */
            lvgl_port_stop();
#endif

            /* Pause wake word during sleep to save ~15-25mA (mic + neural net) */
            if (!cfg->wake_word_in_sleep) {
                wake_word_pause();
                ESP_LOGI(TAG, "Wake word paused for sleep (use button to wake)");
            }

#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
            /* Switch to MAX_MODEM during sleep for better power savings */
            esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
#endif
        }
    }
}

static void presence_task(void *arg)
{
#if !BOARD_HAS_CAMERA
    (void)arg;
    vTaskDelete(NULL);
#else
    bool warned_unavailable = false;
    bool was_present = false;
    int consecutive_hits = 0;
    int consecutive_misses = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(800));

        ui_state_t state = ui_get_state();
        if (state == UI_STATE_LISTENING || state == UI_STATE_SENDING ||
            state == UI_STATE_THINKING || state == UI_STATE_STREAMING ||
            state == UI_STATE_TTS_LOADING || state == UI_STATE_TTS_PLAYING) {
            continue;
        }

        if (!camera_is_ready()) {
            if (camera_init() != ESP_OK) {
                if (!warned_unavailable) {
                    ESP_LOGW(TAG, "Presence monitor: camera model not ready");
                    warned_unavailable = true;
                }
                continue;
            }
        }

        bool present = false;
        int score = 0;
        esp_err_t ret = camera_check_presence(&present, &score);
        if (ret == ESP_OK) {
            warned_unavailable = false;
            if (present) {
                consecutive_hits++;
                consecutive_misses = 0;
            } else {
                consecutive_hits = 0;
                consecutive_misses++;
            }

            bool confirmed_present = consecutive_hits >= 2;
            bool confirmed_absent = consecutive_misses >= 2;

            if (confirmed_present) {
                if (!was_present) {
                    ESP_LOGI(TAG, "Presence confirmed (score=%d) — keeping display awake", score);
                }
                app_reset_activity_timer();
                was_present = true;
            } else if (confirmed_absent) {
                was_present = false;
            }
        } else {
            consecutive_hits = 0;
        }
    }
#endif
}

/* ── Start all tasks ─────────────────────────────────────────────────── */
void app_tasks_start(void)
{
    xTaskCreatePinnedToCore(knob_task, "knob", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(status_update_task, "status", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(sleep_task, "sleep", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(presence_task, "presence", 4096, NULL, 1, NULL, 0);

    /* TTS task — PSRAM stack on S3, internal RAM on ESP32 (16KB needed for minimp3) */
#if CONFIG_IDF_TARGET_ESP32S3
    StaticTask_t *tts_tcb = heap_caps_calloc(1, sizeof(StaticTask_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *tts_stack = heap_caps_calloc(1, 16384,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tts_tcb && tts_stack) {
        xTaskCreateStaticPinnedToCore(tts_play_task, "tts_play", 16384,
                                       NULL, 4, tts_stack, tts_tcb, 1);
    } else {
        ESP_LOGE(TAG, "Failed to allocate TTS task in PSRAM");
    }
#else
    xTaskCreatePinnedToCore(tts_play_task, "tts_play", 16384, NULL, 4, NULL, 0);
#endif

    ESP_LOGI(TAG, "Background tasks started");
}
