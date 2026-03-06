/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * UI module — LVGL main screen for 412x412 round display
 *
 * Layout zones (top to bottom, respecting circular shape):
 *   Status bar: thin bar with WiFi, OC dot, cost  (y≈35, narrow)
 *   Center:     large status word or response      (y≈100-300, wide)
 *   Bottom:     interaction hint                   (y≈360, narrow)
 */

#pragma once

#include "esp_err.h"
#include "board.h"
#include "openclaw_client.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Application-level states (drives entire screen layout)
typedef enum {
    UI_STATE_BOOT = 0,      // Initializing hardware
    UI_STATE_CONNECTING,    // WiFi / OpenClaw connecting
    UI_STATE_IDLE,          // Ready — big "IDLE" in green
    UI_STATE_LISTENING,     // Recording — big "LISTENING" in red
    UI_STATE_SENDING,       // Uploading — big "SENDING" in blue
    UI_STATE_THINKING,      // Waiting for OC — big "THINKING" + timer in orange
    UI_STATE_STREAMING,     // Receiving response — big "..." in purple
    UI_STATE_RESPONSE,      // Short response displayed — wheel = TTS play
    UI_STATE_TTS_LOADING,   // Fetching TTS audio
    UI_STATE_TTS_PLAYING,   // Playing TTS audio
    UI_STATE_ERROR,         // Error state
} ui_state_t;

// Initialize the main UI screen
esp_err_t ui_init(void);

// Set event group for UI button callbacks (must be called before buttons work)
void ui_set_event_group(void *event_group);

// Set the main application state (updates entire screen)
void ui_set_state(ui_state_t state);
ui_state_t ui_get_state(void);
bool ui_cycle_idle_page(int delta);

// Status bar updates
void ui_set_wifi_status(bool connected, int rssi);
void ui_set_battery_status(int percent, bool charging);
void ui_set_openclaw_connected(bool connected);

// Set the short response text (displayed big in center)
void ui_set_response(const char *short_text, const char *full_text);

// Update thinking timer (called periodically while THINKING)
void ui_set_thinking_time(uint32_t elapsed_ms);

/* Update big label during THINKING with activity detail (tool name, etc) */
void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms);

// Set LLM cost display in status bar (e.g., "$0.42")
void ui_set_cost(const char *cost_str);

// Update server info display (from openclaw_info_t snapshot)
void ui_set_server_info(const openclaw_info_t *info);

// Status message for boot/connecting phases
void ui_set_status_message(const char *msg);

// Strip non-ASCII (emoji) characters from text for display
void ui_sanitize_text(char *dst, const char *src, size_t dst_size);

// Get the stored full response text (for TTS playback)
const char *ui_get_full_response(void);

// Get the main screen LVGL object (for tasks screen navigation)
lv_obj_t *ui_get_main_screen(void);

// Web server status indicator in status bar
void ui_set_webserver_status(bool running);

// Set task/progress info in upper area (between status bar and center)
void ui_set_task_info(const char *task_text);

// Set detailed task info from structured cron data
void ui_set_task_info_detailed(const openclaw_info_t *info);

// Show/hide external activity indicator with detail and elapsed timer
void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms);

// Show camera preview image (JPEG → decode → display)
void ui_show_camera_preview(const uint8_t *jpeg, size_t jpeg_size);

#ifdef __cplusplus
}
#endif
