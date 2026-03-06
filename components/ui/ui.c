/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * UI implementation — LVGL-based main screen for 412x412 round display
 *
 * Layout zones (top to bottom in circle):
 *
 *   ╭─── Status Bar ───╮     y=32   WiFi OC-dot WA Batt WebSrv
 *   ╰──────────────────╯
 *
 *   ┌── Task/Progress ─┐     y=80   background tasks / progress
 *   └─────────────────-┘
 *
 *   ╔══════════════════╗
 *   ║     READY        ║     y≈center-20  48pt big status
 *   ║   OpenClaw Online║     y≈center+30  28pt sub text
 *   ╚══════════════════╝
 *
 *   ┌─── Info strip ───┐     y=center+70  server info
 *   └─────────────────-┘
 *
 *   ╭── Button Bar ────╮     y≈340   [▶Play] [📋Details] [🌐Web]
 *   ╰──────────────────╯
 */

#include "ui.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "ui";

#if !BOARD_HAS_DISPLAY
/* ══════════════════════════════════════════════════════════════════════
 * Stub implementations for screenless boards — state tracking only
 * ══════════════════════════════════════════════════════════════════════ */
static ui_state_t s_state = UI_STATE_BOOT;
static char s_full_response[2048] = {0};

esp_err_t ui_init(void) { ESP_LOGI(TAG, "UI: no display — stubs active"); return ESP_OK; }
void ui_set_event_group(void *event_group) { (void)event_group; }
void ui_set_state(ui_state_t state) { s_state = state; }
ui_state_t ui_get_state(void) { return s_state; }
void ui_set_wifi_status(bool connected, int rssi) { (void)connected; (void)rssi; }
void ui_set_battery_status(int percent, bool charging) { (void)percent; (void)charging; }
void ui_set_openclaw_connected(bool connected) { (void)connected; }
void ui_set_response(const char *short_text, const char *full_text) {
    if (full_text) { strncpy(s_full_response, full_text, sizeof(s_full_response)-1); }
    ESP_LOGI(TAG, "Response: %s", short_text ? short_text : "(null)");
}
void ui_set_thinking_time(uint32_t elapsed_ms) { (void)elapsed_ms; }
void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms) { (void)detail; (void)elapsed_ms; }
void ui_set_cost(const char *cost_str) { (void)cost_str; }
void ui_set_server_info(const openclaw_info_t *info) { (void)info; }
void ui_set_status_message(const char *msg) { if (msg) ESP_LOGI(TAG, "Status: %s", msg); }
void ui_sanitize_text(char *dst, const char *src, size_t dst_size) {
    if (!dst || !src) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}
const char *ui_get_full_response(void) { return s_full_response; }
lv_obj_t *ui_get_main_screen(void) { return NULL; }
void ui_set_webserver_status(bool running) { (void)running; }
void ui_set_task_info(const char *task_text) { (void)task_text; }
void ui_set_task_info_detailed(const openclaw_info_t *info) { (void)info; }
void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms) { (void)active; (void)detail; (void)elapsed_ms; }
void ui_show_camera_preview(const uint8_t *jpeg, size_t jpeg_size) { (void)jpeg; (void)jpeg_size; }

#else /* BOARD_HAS_DISPLAY */

#include "esp_lvgl_port.h"

/* ── Color palette ────────────────────────────────────────────────────── */
#define C_BG            lv_color_hex(0x0D1117)
#define C_BAR_BG        lv_color_hex(0x161B22)
#define C_TEXT          lv_color_hex(0xE6EDF3)
#define C_TEXT_DIM      lv_color_hex(0x7D8590)
#define C_BORDER        lv_color_hex(0x30363D)

#define C_GREEN         lv_color_hex(0x3FB950)
#define C_RED           lv_color_hex(0xF85149)
#define C_ORANGE        lv_color_hex(0xD29922)
#define C_BLUE          lv_color_hex(0x58A6FF)
#define C_PURPLE        lv_color_hex(0xBC8CFF)
#define C_TEAL          lv_color_hex(0x39D353)

/* Hebrew/RTL font declarations (generated via lv_font_conv) */
LV_FONT_DECLARE(lv_font_hebrew_28);
LV_FONT_DECLARE(lv_font_hebrew_36);

/* ── Font size abstraction (adapts to screen size) ───────────────────── */
#if defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
  /* 240×135 compact screen */
  #define FONT_BIG     lv_font_montserrat_20
  #define FONT_BIG_SM  lv_font_montserrat_16
  #define FONT_MED     lv_font_montserrat_14
  #define FONT_SUB     lv_font_montserrat_12
  #define FONT_INFO    lv_font_montserrat_12
  #define FONT_TASK    lv_font_montserrat_12
  #define FONT_HEBREW  lv_font_hebrew_28     /* smallest available */
  #define FONT_HEB_BIG lv_font_hebrew_28
  #define CONTENT_W    220
#else
  /* 412×412 round display (SenseCAP Watcher) */
  #define FONT_BIG     lv_font_montserrat_48
  #define FONT_BIG_SM  lv_font_montserrat_36
  #define FONT_MED     lv_font_montserrat_28
  #define FONT_SUB     lv_font_montserrat_20
  #define FONT_INFO    lv_font_montserrat_20
  #define FONT_TASK    lv_font_montserrat_18
  #define FONT_HEBREW  lv_font_hebrew_28
  #define FONT_HEB_BIG lv_font_hebrew_36
  #define CONTENT_W    340
#endif

/* ── UI elements ──────────────────────────────────────────────────────── */
static lv_obj_t *s_scr = NULL;

/* Status bar (top) */
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_status_chip_left = NULL;
static lv_obj_t *s_status_chip_center = NULL;
static lv_obj_t *s_status_chip_right = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_oc_dot = NULL;
static lv_obj_t *s_batt_label = NULL;
static lv_obj_t *s_web_label = NULL;

/* Task / progress area */
static lv_obj_t *s_task_label = NULL;

/* Center zone */
static lv_obj_t *s_big_label = NULL;     /* 48pt status / response */
static lv_obj_t *s_sub_label = NULL;     /* 28pt sub-text */

/* Info strip (server info) */
static lv_obj_t *s_info_line1 = NULL;
static lv_obj_t *s_info_line2 = NULL;

/* Bottom button bar */
static lv_obj_t *s_btn_bar = NULL;
static lv_obj_t *s_play_btn = NULL;
static lv_obj_t *s_details_btn = NULL;
static lv_obj_t *s_web_btn = NULL;
static lv_obj_t *s_tasks_btn = NULL;
static lv_obj_t *s_camera_btn = NULL;
static lv_obj_t *s_cancel_btn = NULL;

/* State */
static ui_state_t s_state = UI_STATE_BOOT;
static char s_full_response[4096];
static bool s_showing_activity = false;  /* true when big_label shows external activity */
static char s_activity_detail[128];      /* cached detail for tap-to-reveal */

/* Thinking animation: multi-arc spinner (concentric rotating arcs) */
static lv_obj_t *s_think_arc1  = NULL;  /* outer arc — orange */
static lv_obj_t *s_think_arc2  = NULL;  /* middle arc — cyan  */
static lv_obj_t *s_think_arc3  = NULL;  /* inner arc  — pink  */
static lv_obj_t *s_think_label = NULL;  /* center detail/time text */

/* Event group for button callbacks (set via ui_set_event_group) */
static EventGroupHandle_t s_events = NULL;

/* Event bits — must match app_state.h */
#define UI_TTS_PLAY_BIT        BIT3
#define UI_WEBSERVER_TOGGLE_BIT BIT4
#define UI_DETAILS_BIT         BIT5
#define UI_KNOB_PRESSED_BIT    BIT2
#define UI_TOUCH_BIT           BIT7
#define UI_TASKS_SCREEN_BIT    BIT8
#define UI_CAMERA_BIT          BIT9
#define UI_CANCEL_BIT          BIT6

/* ── Helpers ──────────────────────────────────────────────────────────── */

static lv_obj_t *create_status_chip(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_size(chip, w, h);
    lv_obj_set_style_bg_color(chip, C_BAR_BG, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_80, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, C_BORDER, 0);
    lv_obj_set_style_border_opa(chip, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_pad_ver(chip, 3, 0);
    lv_obj_set_style_shadow_width(chip, 12, 0);
    lv_obj_set_style_shadow_opa(chip, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(chip, lv_color_hex(0x000000), 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    return chip;
}

/* Check if a 2-byte UTF-8 character is in the Latin range renderable by Montserrat.
 * Montserrat covers: Basic Latin (ASCII), Latin-1 Supplement (U+0080-U+00FF),
 * Latin Extended-A (U+0100-U+017F), and some symbol glyphs.
 * Non-Latin scripts (Hebrew U+0590+, Arabic U+0600+, CJK, etc.) are NOT covered. */
static bool is_renderable_2byte(uint8_t b0, uint8_t b1)
{
    /* Decode UTF-8 to codepoint */
    uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
    /* Latin ranges: U+00A0-U+024F covers Latin-1 Supplement + Extended-A/B */
    if (cp >= 0x00A0 && cp <= 0x024F) return true;
    /* Special symbols that Montserrat includes */
    if (cp >= 0x2000 && cp <= 0x206F) return true; /* General Punctuation */
    return false;
}

/* Check if a 2-byte UTF-8 character is Hebrew (U+0590-U+05FF) */
static bool is_hebrew_2byte(uint8_t b0, uint8_t b1)
{
    uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
    return (cp >= 0x0590 && cp <= 0x05FF);
}

void ui_sanitize_text(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; ) {
        uint8_t c = (uint8_t)src[si];
        if (c < 0x80) {
            /* ASCII — keep printable chars */
            if (c >= 0x20) dst[di++] = src[si];
            si++;
        } else if (c < 0xC0) {
            si++; /* continuation byte orphan — skip */
        } else if (c < 0xE0) {
            /* 2-byte UTF-8: only keep if font can render it */
            if (si + 1 < strlen(src) && (uint8_t)src[si+1] >= 0x80) {
                if (is_renderable_2byte(c, (uint8_t)src[si+1]) && di + 2 < dst_size) {
                    dst[di++] = src[si];
                    dst[di++] = src[si+1];
                }
                /* else: non-renderable (Hebrew, Cyrillic, etc.) — skip */
            }
            si += 2;
        } else if (c < 0xF0) {
            si += 3; /* 3-byte: skip (CJK, symbols, etc.) */
        } else {
            si += 4; /* 4-byte: skip (emojis, etc.) */
        }
    }
    dst[di] = '\0';
}

/* Sanitize text keeping Hebrew characters (for use with Hebrew font) */
static void sanitize_text_hebrew(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    size_t slen = strlen(src);
    for (size_t si = 0; src[si] && di < dst_size - 1; ) {
        uint8_t c = (uint8_t)src[si];
        if (c < 0x80) {
            /* ASCII — keep printable */
            if (c >= 0x20) dst[di++] = src[si];
            si++;
        } else if (c < 0xC0) {
            si++; /* orphan continuation byte */
        } else if (c < 0xE0) {
            /* 2-byte UTF-8 */
            if (si + 1 < slen && (uint8_t)src[si+1] >= 0x80) {
                uint8_t b1 = (uint8_t)src[si+1];
                uint16_t cp = ((c & 0x1F) << 6) | (b1 & 0x3F);
                /* Skip Hebrew nikud/cantillation (U+0591-U+05BD, U+05BF, U+05C1-U+05C7)
                 * Keep only Hebrew letters (U+05D0-U+05EA) and Geresh/Gershayim (U+05F0-U+05F4) */
                bool is_hebrew_letter = (cp >= 0x05D0 && cp <= 0x05EA) ||
                                        (cp >= 0x05F0 && cp <= 0x05F4);
                bool is_latin_ok = is_renderable_2byte(c, b1);
                if ((is_hebrew_letter || is_latin_ok) && di + 2 < dst_size) {
                    dst[di++] = src[si];
                    dst[di++] = src[si+1];
                }
            }
            si += 2;
        } else if (c < 0xF0) {
            si += 3; /* 3-byte: skip (CJK, zero-width chars, etc.) */
        } else {
            si += 4; /* 4-byte: skip (emojis, etc.) */
        }
    }
    dst[di] = '\0';
}

/* Check if text has non-Latin content that was stripped by sanitizer */
static bool has_non_latin(const char *text)
{
    for (size_t i = 0; text[i]; ) {
        uint8_t c = (uint8_t)text[i];
        if (c < 0x80) {
            i++;
        } else if (c < 0xC0) {
            i++;
        } else if (c < 0xE0) {
            if (!is_renderable_2byte(c, (uint8_t)text[i+1])) return true;
            i += 2;
        } else if (c < 0xF0) {
            i += 3;
        } else {
            i += 4;
        }
    }
    return false;
}

/* Check if text contains Hebrew characters */
static bool has_hebrew(const char *text)
{
    for (size_t i = 0; text[i]; ) {
        uint8_t c = (uint8_t)text[i];
        if (c < 0x80) {
            i++;
        } else if (c < 0xC0) {
            i++;
        } else if (c < 0xE0) {
            if (i + 1 < strlen(text) && is_hebrew_2byte(c, (uint8_t)text[i+1])) return true;
            i += 2;
        } else if (c < 0xF0) {
            i += 3;
        } else {
            i += 4;
        }
    }
    return false;
}

static void extract_short_response(char *dst, const char *src, size_t dst_size)
{
    /* Check if source has Hebrew or other non-Latin text */
    bool hebrew = has_hebrew(src);
    bool non_latin = has_non_latin(src);

    if (hebrew) {
        /* Keep Hebrew text — we have a Hebrew font now */
        sanitize_text_hebrew(dst, src, dst_size);
    } else {
        char clean[512];
        ui_sanitize_text(clean, src, sizeof(clean));

        size_t clean_len = strlen(clean);
        while (clean_len > 0 && (clean[clean_len-1] == ' ' || clean[clean_len-1] == ',' ||
                                  clean[clean_len-1] == '!' || clean[clean_len-1] == '.'))
            clean[--clean_len] = '\0';
        size_t start = 0;
        while (start < clean_len && (clean[start] == ' ' || clean[start] == ','))
            start++;

        if (non_latin && (clean_len - start) < 2) {
            strncpy(dst, LV_SYMBOL_OK, dst_size - 1);
            dst[dst_size - 1] = '\0';
            return;
        }

        const char *text = clean + start;
        size_t tlen = clean_len - start;
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : tlen;
        if (len > 40) len = 40;
        if (len >= dst_size) len = dst_size - 1;
        memcpy(dst, text, len);
        dst[len] = '\0';

        while (len > 0 && (dst[len-1] == ' ' || dst[len-1] == '.' || dst[len-1] == ','))
            dst[--len] = '\0';
    }

    /* Truncate for display — first line, max ~40 UTF-8 chars */
    char *nl = strchr(dst, '\n');
    if (nl) *nl = '\0';
    /* Truncate long display text */
    size_t dlen = strlen(dst);
    if (dlen > 60) {
        dst[57] = '.';
        dst[58] = '.';
        dst[59] = '.';
        dst[60] = '\0';
    }
}

/* ── Button event callbacks ──────────────────────────────────────────── */

static void play_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_events) {
        ESP_LOGI(TAG, "Play button pressed");
        xEventGroupSetBits(s_events, UI_TTS_PLAY_BIT);
    }
}

static void details_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_events) {
        ESP_LOGI(TAG, "Details button pressed");
        xEventGroupSetBits(s_events, UI_DETAILS_BIT);
    }
}

static void web_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_events) {
        ESP_LOGI(TAG, "Web button pressed");
        xEventGroupSetBits(s_events, UI_WEBSERVER_TOGGLE_BIT);
    }
}

static void tasks_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_events) {
        ESP_LOGI(TAG, "Tasks button pressed");
        xEventGroupSetBits(s_events, UI_TASKS_SCREEN_BIT);
    }
}

static void camera_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Camera button CLICKED (s_events=%p)", (void*)s_events);
    if (s_events) {
        xEventGroupSetBits(s_events, UI_CAMERA_BIT);
    }
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Cancel button CLICKED");
    if (s_events) {
        xEventGroupSetBits(s_events, UI_CANCEL_BIT);
    }
}

/* Callback for touching the big label (Ready) in IDLE → start recording */
static void big_label_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_events && s_state == UI_STATE_IDLE) {
        if (s_showing_activity) {
            /* Tap on activity display → toggle detail in sub_label */
            if (s_activity_detail[0]) {
                ESP_LOGI(TAG, "Activity tapped — showing detail: %s", s_activity_detail);
                lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
                lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
                lv_label_set_text(s_sub_label, s_activity_detail);
            }
        } else {
            ESP_LOGI(TAG, "Ready label touched — start recording");
            xEventGroupSetBits(s_events, UI_KNOB_PRESSED_BIT);
        }
    }
}

/* Screen-wide touch callback — plays tick and signals touch event */
static void screen_touch_cb(lv_event_t *e)
{
    (void)e;
    /* Log touch coordinates for debugging */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        ESP_LOGI(TAG, "Touch at (%d, %d) state=%d", p.x, p.y, s_state);
    }
    if (s_events) {
        xEventGroupSetBits(s_events, UI_TOUCH_BIT);
    }
}

/* ── Thinking animation helpers ──────────────────────────────────────── */

/* Show/hide thinking spinner arcs + detail label, hide/show regular labels */
static void show_thinking_anim(bool show)
{
    if (show) {
        if (s_think_arc1)  lv_obj_clear_flag(s_think_arc1,  LV_OBJ_FLAG_HIDDEN);
        if (s_think_arc2)  lv_obj_clear_flag(s_think_arc2,  LV_OBJ_FLAG_HIDDEN);
        if (s_think_arc3)  lv_obj_clear_flag(s_think_arc3,  LV_OBJ_FLAG_HIDDEN);
        if (s_think_label) lv_obj_clear_flag(s_think_label, LV_OBJ_FLAG_HIDDEN);
        if (s_big_label)   lv_obj_add_flag(s_big_label,     LV_OBJ_FLAG_HIDDEN);
        if (s_sub_label)   lv_obj_add_flag(s_sub_label,     LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_think_arc1)  lv_obj_add_flag(s_think_arc1,    LV_OBJ_FLAG_HIDDEN);
        if (s_think_arc2)  lv_obj_add_flag(s_think_arc2,    LV_OBJ_FLAG_HIDDEN);
        if (s_think_arc3)  lv_obj_add_flag(s_think_arc3,    LV_OBJ_FLAG_HIDDEN);
        if (s_think_label) lv_obj_add_flag(s_think_label,   LV_OBJ_FLAG_HIDDEN);
        if (s_big_label)   lv_obj_clear_flag(s_big_label,   LV_OBJ_FLAG_HIDDEN);
        if (s_sub_label)   lv_obj_clear_flag(s_sub_label,   LV_OBJ_FLAG_HIDDEN);
    }
}

/* Initialize thinking animation — must be called inside lvgl_port_lock() */
static void ui_init_thinking_anim(void)
{
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
    /* M5Stick: 240×135 landscape — two concentric arcs + label below */
    s_think_arc1 = lv_spinner_create(s_scr, 1200, 120);
    lv_obj_set_size(s_think_arc1, 80, 80);
    lv_obj_align(s_think_arc1, LV_ALIGN_CENTER, 0, -18);
    lv_obj_set_style_arc_color(s_think_arc1, lv_color_hex(0xFF8C00), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_think_arc1, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_think_arc1, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_think_arc1, 6, LV_PART_MAIN);
    lv_obj_add_flag(s_think_arc1, LV_OBJ_FLAG_HIDDEN);

    s_think_arc2 = lv_spinner_create(s_scr, 2200, 80);
    lv_obj_set_size(s_think_arc2, 58, 58);
    lv_obj_align(s_think_arc2, LV_ALIGN_CENTER, 0, -18);
    lv_obj_set_style_arc_color(s_think_arc2, lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_think_arc2, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_think_arc2, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_think_arc2, 6, LV_PART_MAIN);
    lv_obj_add_flag(s_think_arc2, LV_OBJ_FLAG_HIDDEN);

    s_think_arc3 = NULL;  /* no third arc on small screen */

    s_think_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_think_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_think_label, lv_color_hex(0xFF8C00), 0);
    lv_obj_set_style_text_align(s_think_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_think_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_think_label, 180);
    lv_obj_align(s_think_label, LV_ALIGN_CENTER, 0, 32);
    lv_label_set_text(s_think_label, "Thinking...");
    lv_obj_add_flag(s_think_label, LV_OBJ_FLAG_HIDDEN);
#else
    /* SenseCap: 412×412 round — three concentric arcs, center detail */
    s_think_arc1 = lv_spinner_create(s_scr, 1200, 120);   /* outer: orange, fast */
    lv_obj_set_size(s_think_arc1, 300, 300);
    lv_obj_center(s_think_arc1);
    lv_obj_set_style_arc_color(s_think_arc1, lv_color_hex(0xFF8C00), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_think_arc1, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_think_arc1, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_think_arc1, 10, LV_PART_MAIN);
    lv_obj_add_flag(s_think_arc1, LV_OBJ_FLAG_HIDDEN);

    s_think_arc2 = lv_spinner_create(s_scr, 2100, 90);    /* middle: cyan, medium */
    lv_obj_set_size(s_think_arc2, 240, 240);
    lv_obj_center(s_think_arc2);
    lv_obj_set_style_arc_color(s_think_arc2, lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_think_arc2, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_think_arc2, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_think_arc2, 10, LV_PART_MAIN);
    lv_obj_add_flag(s_think_arc2, LV_OBJ_FLAG_HIDDEN);

    s_think_arc3 = lv_spinner_create(s_scr, 3000, 60);    /* inner: pink, slowest */
    lv_obj_set_size(s_think_arc3, 180, 180);
    lv_obj_center(s_think_arc3);
    lv_obj_set_style_arc_color(s_think_arc3, lv_color_hex(0xFF69B4), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_think_arc3, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_think_arc3, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_think_arc3, 10, LV_PART_MAIN);
    lv_obj_add_flag(s_think_arc3, LV_OBJ_FLAG_HIDDEN);

    s_think_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_think_label, &FONT_BIG_SM, 0);
    lv_obj_set_style_text_color(s_think_label, lv_color_hex(0xFF8C00), 0);
    lv_obj_set_style_text_align(s_think_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_think_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_think_label, 150);
    lv_obj_center(s_think_label);
    lv_label_set_text(s_think_label, "Thinking...");
    lv_obj_add_flag(s_think_label, LV_OBJ_FLAG_HIDDEN);
#endif
}

/* ── Create a round icon button ──────────────────────────────────────── */
static lv_obj_t *create_icon_btn(lv_obj_t *parent, const char *icon,
                                  lv_color_t bg_color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, C_BORDER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    /* Pressed state: brighter color + white border flash */
    lv_obj_set_style_bg_color(btn, lv_color_lighten(bg_color, 80), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(btn, 10); /* larger touch target */

    return btn;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

/* Small-screen layout (M5StickCPlus2: 240×135) */
#if defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)

static lv_obj_t *create_small_btn(lv_obj_t *parent, const char *icon,
                                   lv_color_t bg_color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 30, 26);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, C_BORDER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, lv_color_lighten(bg_color, 80), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(btn, 5);
    return btn;
}

esp_err_t ui_init(void)
{
    lv_disp_t *disp = board_get_lvgl_disp();
    if (!disp) { ESP_LOGE(TAG, "No LVGL display"); return ESP_FAIL; }
    if (!lvgl_port_lock(1000)) return ESP_FAIL;

    s_scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Status bar (top, compact) ── */
    s_status_bar = lv_obj_create(s_scr);
    lv_obj_set_size(s_status_bar, 220, 18);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 1);
    lv_obj_set_style_bg_color(s_status_bar, C_BAR_BG, 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_status_bar, 4, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_status_bar, 6, 0);
    lv_obj_set_style_pad_ver(s_status_bar, 1, 0);
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_12, 0);

    s_oc_dot = lv_obj_create(s_status_bar);
    lv_obj_set_size(s_oc_dot, 7, 7);
    lv_obj_set_style_radius(s_oc_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_oc_dot, C_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(s_oc_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_oc_dot, 0, 0);

    s_batt_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_batt_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(s_batt_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_12, 0);

    s_web_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_web_label, "");
    lv_obj_set_style_text_color(s_web_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_web_label, &lv_font_montserrat_12, 0);

    /* ── Task / progress area ── */
    s_task_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_task_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_task_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_task_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_task_label, 220);
    lv_obj_align(s_task_label, LV_ALIGN_TOP_MID, 0, 22);
    lv_label_set_text(s_task_label, "");

    /* ── Big center label (20pt status) ── */
    s_big_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
    lv_obj_set_style_text_color(s_big_label, C_TEXT, 0);
    lv_obj_set_style_text_align(s_big_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_big_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_big_label, 220);
    lv_obj_set_style_max_height(s_big_label, 75, 0);
    lv_obj_align(s_big_label, LV_ALIGN_CENTER, 0, -5);
    lv_label_set_text(s_big_label, "BOOT");
    lv_obj_add_flag(s_big_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_big_label, big_label_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── Sub label (12pt) ── */
    s_sub_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_sub_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sub_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sub_label, 220);
    lv_obj_align_to(s_sub_label, s_big_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    lv_label_set_text(s_sub_label, "");

    /* ── Info strip ── */
    s_info_line1 = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_info_line1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_info_line1, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_info_line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_info_line1, 220);
    lv_obj_align(s_info_line1, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(s_info_line1, "");

    s_info_line2 = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_info_line2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_info_line2, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_info_line2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_info_line2, 220);
    lv_obj_align(s_info_line2, LV_ALIGN_CENTER, 0, 42);
    lv_label_set_text(s_info_line2, "");

    /* M5Stick uses physical buttons — no touch button bar needed.
     * Create hidden stubs so LVGL references don't crash. */
    s_btn_bar = lv_obj_create(s_scr);
    lv_obj_add_flag(s_btn_bar, LV_OBJ_FLAG_HIDDEN);
    s_play_btn = lv_obj_create(s_btn_bar);
    lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    s_details_btn = lv_obj_create(s_btn_bar);
    lv_obj_add_flag(s_details_btn, LV_OBJ_FLAG_HIDDEN);
    s_camera_btn = lv_obj_create(s_btn_bar);
    lv_obj_add_flag(s_camera_btn, LV_OBJ_FLAG_HIDDEN);
    s_cancel_btn = lv_obj_create(s_btn_bar);
    lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
    s_web_btn = lv_obj_create(s_btn_bar);
    lv_obj_add_flag(s_web_btn, LV_OBJ_FLAG_HIDDEN);
    s_tasks_btn = lv_obj_create(s_btn_bar);
    lv_obj_add_flag(s_tasks_btn, LV_OBJ_FLAG_HIDDEN);

    /* Thinking animation arcs */
    ui_init_thinking_anim();

    lvgl_port_unlock();

    s_full_response[0] = '\0';
    ESP_LOGI(TAG, "UI initialized (compact 240x135, no touch buttons)");
    return ESP_OK;
}

#else /* SenseCAP Watcher (412×412 round display) */

esp_err_t ui_init(void)
{
    lv_disp_t *disp = board_get_lvgl_disp();
    if (!disp) {
        ESP_LOGE(TAG, "No LVGL display");
        return ESP_FAIL;
    }

    if (!lvgl_port_lock(1000)) return ESP_FAIL;

    s_scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE); /* prevent scroll from canceling button clicks */

    /* ── Status chips (top, offset along the round display arc) ── */
    s_status_bar = lv_obj_create(s_scr);
    lv_obj_set_size(s_status_bar, 412, 84);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 0, 0);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_CLICKABLE);

    s_status_chip_left = create_status_chip(s_status_bar, 56, 28);
    lv_obj_set_pos(s_status_chip_left, 88, 31);

    s_status_chip_center = create_status_chip(s_status_bar, 82, 30);
    lv_obj_set_pos(s_status_chip_center, 165, 16);
    lv_obj_set_style_pad_column(s_status_chip_center, 6, 0);

    s_status_chip_right = create_status_chip(s_status_bar, 56, 28);
    lv_obj_set_pos(s_status_chip_right, 268, 31);

    /* WiFi icon */
    s_wifi_label = lv_label_create(s_status_chip_left);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);

    /* OC status dot */
    s_oc_dot = lv_obj_create(s_status_chip_center);
    lv_obj_set_size(s_oc_dot, 10, 10);
    lv_obj_set_style_radius(s_oc_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_oc_dot, C_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(s_oc_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_oc_dot, 0, 0);

    /* Battery label */
    s_batt_label = lv_label_create(s_status_chip_right);
    lv_label_set_text(s_batt_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(s_batt_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);

    /* Web server indicator */
    s_web_label = lv_label_create(s_status_chip_center);
    lv_label_set_text(s_web_label, "");
    lv_obj_set_style_text_color(s_web_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_web_label, &lv_font_montserrat_14, 0);

    /* ── Task / progress area ── */
    s_task_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_task_label, &FONT_TASK, 0);
    lv_obj_set_style_text_color(s_task_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_task_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_task_label, 300);
    lv_obj_align(s_task_label, LV_ALIGN_TOP_MID, 0, 72);
    lv_label_set_text(s_task_label, "");

    /* ── Big center label (48pt status word) ── */
    s_big_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
    lv_obj_set_style_text_color(s_big_label, C_TEXT, 0);
    lv_obj_set_style_text_align(s_big_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_big_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_big_label, 340);
    lv_obj_set_style_max_height(s_big_label, 180, 0);  /* prevent overlap with buttons */
    lv_obj_align(s_big_label, LV_ALIGN_CENTER, 0, -30);
    lv_label_set_text(s_big_label, "BOOT");
    /* Make big label clickable (tap "Ready" to record) */
    lv_obj_add_flag(s_big_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_big_label, big_label_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── Sub label (28pt — timer, hint, response detail) ── */
    s_sub_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
    lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sub_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sub_label, 340);
    lv_obj_align_to(s_sub_label, s_big_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_label_set_text(s_sub_label, "");

    /* ── Info strip — 20pt for server info ── */
    s_info_line1 = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_info_line1, &FONT_INFO, 0);
    lv_obj_set_style_text_color(s_info_line1, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_info_line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_info_line1, 340);
    lv_obj_align(s_info_line1, LV_ALIGN_CENTER, 0, 55);
    lv_label_set_text(s_info_line1, "");

    s_info_line2 = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_info_line2, &FONT_INFO, 0);
    lv_obj_set_style_text_color(s_info_line2, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_info_line2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_info_line2, 340);
    lv_obj_align(s_info_line2, LV_ALIGN_CENTER, 0, 80);
    lv_label_set_text(s_info_line2, "");

    /* ── Bottom button bar (arc-aligned to 412×412 circular display) ──
     * Center (206,206), R=206. Buttons follow the circle edge with 30px
     * inset. 5 buttons at angles ±36° and ±18° from bottom center.
     * Wider spread so 44px buttons don't overlap. */
    s_btn_bar = lv_obj_create(s_scr);
    lv_obj_set_size(s_btn_bar, 412, 120);
    lv_obj_align(s_btn_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_btn_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_bar, 0, 0);
    lv_obj_set_style_pad_all(s_btn_bar, 0, 0);
    lv_obj_clear_flag(s_btn_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_btn_bar, LV_OBJ_FLAG_CLICKABLE); /* let clicks pass through to child buttons */

    /* Button positions computed on inner circle (R=176, 30px inset).
     * Angles from bottom center: -36°, -18°, 0°, +18°, +36°.
     * x = 206 + R*sin(a),  y = 206 + R*cos(a) - bar_top.
     * sin(18°)=0.309, cos(18°)=0.951, sin(36°)=0.588, cos(36°)=0.809 */
    #define BTN_R  176
    #define CX     206
    #define CY     206
    #define BAR_TOP 292  /* btn_bar top: 412 - 120 */
    static const int btn_x[] = {
        CX - (int)(BTN_R * 0.588f),  /* -36°: 206-103=103 */
        CX - (int)(BTN_R * 0.309f),  /* -18°: 206-54=152 */
        CX,                          /*   0°: 206 */
        CX + (int)(BTN_R * 0.309f),  /* +18°: 206+54=260 */
        CX + (int)(BTN_R * 0.588f),  /* +36°: 206+103=309 */
    };
    static const int btn_y[] = {
        CY + (int)(BTN_R * 0.809f) - BAR_TOP,  /* -36°: 206+142-292=56 */
        CY + (int)(BTN_R * 0.951f) - BAR_TOP,  /* -18°: 206+167-292=81 */
        CY + BTN_R - BAR_TOP,                   /*   0°: 206+176-292=90 */
        CY + (int)(BTN_R * 0.951f) - BAR_TOP,  /* +18° */
        CY + (int)(BTN_R * 0.809f) - BAR_TOP,  /* +36° */
    };

    /* Play button */
    s_play_btn = create_icon_btn(s_btn_bar, LV_SYMBOL_PLAY, C_TEAL, play_btn_cb);
    lv_obj_set_pos(s_play_btn, btn_x[0] - 22, btn_y[0] - 22);
    lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);

    /* Details button */
    s_details_btn = create_icon_btn(s_btn_bar, LV_SYMBOL_LIST, C_BLUE, details_btn_cb);
    lv_obj_set_pos(s_details_btn, btn_x[1] - 22, btn_y[1] - 22);
    lv_obj_add_flag(s_details_btn, LV_OBJ_FLAG_HIDDEN);

    /* Camera button */
    s_camera_btn = create_icon_btn(s_btn_bar, LV_SYMBOL_IMAGE, C_ORANGE, camera_btn_cb);
    lv_obj_set_pos(s_camera_btn, btn_x[2] - 22, btn_y[2] - 22);

    /* Cancel button (shown only during recording) */
    s_cancel_btn = create_icon_btn(s_btn_bar, LV_SYMBOL_CLOSE, C_RED, cancel_btn_cb);
    lv_obj_set_pos(s_cancel_btn, btn_x[2] - 22, btn_y[2] - 22);
    lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);

    /* Web server button */
    s_web_btn = create_icon_btn(s_btn_bar, LV_SYMBOL_WIFI, C_BAR_BG, web_btn_cb);
    lv_obj_set_pos(s_web_btn, btn_x[3] - 22, btn_y[3] - 22);

    /* Tasks button */
    s_tasks_btn = create_icon_btn(s_btn_bar, LV_SYMBOL_LIST " ", C_PURPLE, tasks_btn_cb);
    lv_obj_set_pos(s_tasks_btn, btn_x[4] - 22, btn_y[4] - 22);

    /* Thinking animation arcs */
    ui_init_thinking_anim();

    /* Screen-wide touch event for tick sound + activity reset */
    lv_obj_add_event_cb(s_scr, screen_touch_cb, LV_EVENT_PRESSED, NULL);

    lvgl_port_unlock();

    s_full_response[0] = '\0';
    ESP_LOGI(TAG, "UI initialized (round 412x412, 48pt big)");
    return ESP_OK;
}

#endif /* CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2 */

void ui_set_event_group(void *event_group)
{
    s_events = (EventGroupHandle_t)event_group;
}

/* ── State machine ────────────────────────────────────────────────────── */

void ui_set_state(ui_state_t state)
{
    s_state = state;
    if (!s_big_label) return;
    if (!lvgl_port_lock(200)) return;

    /* Always reset thinking animation on state change */
    show_thinking_anim(false);

    /* Hide action buttons by default — show only when relevant */
    lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_details_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
#if BOARD_HAS_CAMERA
    lv_obj_clear_flag(s_camera_btn, LV_OBJ_FLAG_HIDDEN);
#else
    lv_obj_add_flag(s_camera_btn, LV_OBJ_FLAG_HIDDEN);
#endif

    /* Reset big_label font to 48pt + LTR ONLY for states that set big_label text.
     * RESPONSE/TTS_LOADING/TTS_PLAYING keep whatever font ui_set_response() set
     * (important for Hebrew RTL rendering). */
    if (state != UI_STATE_RESPONSE && state != UI_STATE_TTS_LOADING &&
        state != UI_STATE_TTS_PLAYING) {
        lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
        lv_obj_set_style_base_dir(s_big_label, LV_BASE_DIR_LTR, 0);
    }

    /* Clear overlapping content on any state transition */
    lv_label_set_text(s_info_line1, "");
    lv_label_set_text(s_info_line2, "");
    lv_label_set_text(s_task_label, "");

    switch (state) {
    case UI_STATE_BOOT:
        lv_obj_set_style_text_color(s_big_label, C_TEXT_DIM, 0);
        lv_obj_set_style_text_font(s_big_label, &FONT_BIG_SM, 0);
        lv_label_set_text(s_big_label, "STARTING");
        lv_label_set_text(s_sub_label, "");
        break;

    case UI_STATE_CONNECTING:
        lv_obj_set_style_text_color(s_big_label, C_ORANGE, 0);
        lv_obj_set_style_text_font(s_big_label, &FONT_BIG_SM, 0);
        lv_label_set_text(s_big_label, "CONNECTING");
        lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
        lv_label_set_text(s_sub_label, "");
        break;

    case UI_STATE_IDLE:
        lv_obj_set_style_text_color(s_big_label, C_GREEN, 0);
        lv_label_set_text(s_big_label, "READY");
        lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        lv_label_set_text(s_sub_label, "A:Talk B:Web C:Tasks");
#else
        lv_label_set_text(s_sub_label, "Tap or Wheel");
#endif
        break;

    case UI_STATE_LISTENING:
        lv_obj_set_style_text_color(s_big_label, C_RED, 0);
        lv_label_set_text(s_big_label, "REC");
        lv_obj_set_style_text_color(s_sub_label, C_RED, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        lv_label_set_text(s_sub_label, "A:Cancel  B:Cancel");
#else
        lv_label_set_text(s_sub_label, LV_SYMBOL_AUDIO " Recording");
        /* Show cancel button, hide camera (they share same position) */
        lv_obj_add_flag(s_camera_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
#endif
        break;

    case UI_STATE_SENDING:
        lv_obj_set_style_text_color(s_big_label, C_BLUE, 0);
        lv_label_set_text(s_big_label, "SEND");
        lv_obj_set_style_text_color(s_sub_label, C_BLUE, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        lv_label_set_text(s_sub_label, "B:Cancel");
#else
        lv_label_set_text(s_sub_label, LV_SYMBOL_UPLOAD);
#endif
        break;

    case UI_STATE_THINKING:
        /* Show colorful multi-arc spinner animation */
        show_thinking_anim(true);
        if (s_think_label) lv_label_set_text(s_think_label, "Thinking...");
        break;

    case UI_STATE_STREAMING:
        lv_obj_set_style_text_color(s_big_label, C_PURPLE, 0);
        lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
        lv_label_set_text(s_big_label, "...");
        lv_obj_set_style_text_color(s_sub_label, C_PURPLE, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
        lv_label_set_text(s_sub_label, "Receiving");
        break;

    case UI_STATE_RESPONSE:
        /* big_label and sub_label set by ui_set_response() */
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        /* No touch buttons on M5Stick */
#else
        lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_details_btn, LV_OBJ_FLAG_HIDDEN);
#endif
        break;

    case UI_STATE_TTS_LOADING:
        lv_obj_set_style_text_color(s_sub_label, C_TEAL, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        lv_label_set_text(s_sub_label, "Loading... B:Stop");
#else
        lv_label_set_text(s_sub_label, "Loading audio...");
        lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
#endif
        break;

    case UI_STATE_TTS_PLAYING:
        lv_obj_set_style_text_color(s_sub_label, C_TEAL, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
#ifdef CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2
        lv_label_set_text(s_sub_label, LV_SYMBOL_VOLUME_MAX " B:Stop");
#else
        lv_label_set_text(s_sub_label, LV_SYMBOL_VOLUME_MAX " Playing");
        lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        /* Change play icon to pause */
        lv_obj_t *play_lbl = lv_obj_get_child(s_play_btn, 0);
        if (play_lbl) lv_label_set_text(play_lbl, LV_SYMBOL_PAUSE);
#endif
        break;

    case UI_STATE_ERROR:
        lv_obj_set_style_text_color(s_big_label, C_RED, 0);
        lv_label_set_text(s_big_label, "ERROR");
        lv_obj_set_style_text_color(s_sub_label, C_RED, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
        break;
    }

    lvgl_port_unlock();
}

ui_state_t ui_get_state(void)
{
    return s_state;
}

/* ── Status bar updates ───────────────────────────────────────────────── */

void ui_set_wifi_status(bool connected, int rssi)
{
    if (!s_wifi_label) return;
    if (!lvgl_port_lock(100)) return;

    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, connected ? C_GREEN : C_RED, 0);
    lvgl_port_unlock();
}

void ui_set_battery_status(int percent, bool charging)
{
    if (!s_batt_label) return;
    if (!lvgl_port_lock(100)) return;

    const char *icon;
    lv_color_t color;
    if (charging) {
        icon = LV_SYMBOL_CHARGE;
        color = C_BLUE;
    } else if (percent > 75) {
        icon = LV_SYMBOL_BATTERY_FULL;
        color = C_GREEN;
    } else if (percent > 25) {
        icon = LV_SYMBOL_BATTERY_2;
        color = C_ORANGE;
    } else {
        icon = LV_SYMBOL_BATTERY_1;
        color = C_RED;
    }
    lv_label_set_text(s_batt_label, icon);
    lv_obj_set_style_text_color(s_batt_label, color, 0);
    lvgl_port_unlock();
}

void ui_set_openclaw_connected(bool connected)
{
    if (!s_oc_dot) return;
    if (!lvgl_port_lock(100)) return;
    lv_obj_set_style_bg_color(s_oc_dot, connected ? C_GREEN : C_RED, 0);
    lvgl_port_unlock();
}

void ui_set_cost(const char *cost_str)
{
    (void)cost_str;
}

/* ── Server info display ─────────────────────────────────────────────── */

void ui_set_server_info(const openclaw_info_t *info)
{
    if (!s_info_line1 || !info) return;
    /* Only show info in IDLE or BOOT state */
    if (s_state != UI_STATE_IDLE && s_state != UI_STATE_BOOT) return;
    if (!lvgl_port_lock(100)) return;

    char buf1[80], buf2[80];

    /* Detect if OpenClaw is actively processing (from any source) */
    bool oc_active = info->is_active;

    /* If OpenClaw is active from an external source, update main label */
    if (oc_active && s_state == UI_STATE_IDLE) {
        s_showing_activity = true;

        /* Carousel: pick which run to display */
        const char *display_detail = info->active_detail;
        int64_t display_started = info->active_started_ms;
        int run_count = info->active_runs_count;

        if (run_count > 1) {
            /* Find the carousel_index-th active slot */
            int idx = info->carousel_index % run_count;
            int found = 0;
            for (int i = 0; i < OC_MAX_ACTIVE_RUNS; i++) {
                if (info->active_runs[i].active) {
                    if (found == idx) {
                        display_detail = info->active_runs[i].detail;
                        display_started = info->active_runs[i].started_ms;
                        break;
                    }
                    found++;
                }
            }
        }

        /* Cache full detail for tap-to-reveal */
        if (display_detail && display_detail[0]) {
            if (run_count > 1) {
                snprintf(s_activity_detail, sizeof(s_activity_detail),
                         "[%d/%d] %s (run: %.8s%s)", 
                         (info->carousel_index % run_count) + 1, run_count,
                         display_detail,
                         info->active_run_id[0] ? info->active_run_id : "?",
                         strlen(info->active_run_id) > 8 ? "..." : "");
            } else {
                snprintf(s_activity_detail, sizeof(s_activity_detail),
                         "%s (run: %.8s%s)", display_detail,
                         info->active_run_id[0] ? info->active_run_id : "?",
                         strlen(info->active_run_id) > 8 ? "..." : "");
            }
        } else {
            snprintf(s_activity_detail, sizeof(s_activity_detail),
                     "External activity (tap for info)");
        }
        /* Show activity detail as the main text when available */
        const char *display_text = "ACTIVE";
        if (display_detail && display_detail[0]) {
            display_text = display_detail;
        }
        const char *cur = lv_label_get_text(s_big_label);
        if (!cur || strcmp(cur, display_text) != 0) {
            ESP_LOGI(TAG, "OC active (ext=%d, runs=%d, last=%ds) → %s display",
                     info->is_external, run_count, info->last_activity_sec, display_text);
        }
        lv_obj_set_style_text_color(s_big_label, C_BLUE, 0);
        /* Shrink font for longer detail text */
        if (strlen(display_text) > 8) {
            lv_obj_set_style_text_font(s_big_label, &FONT_BIG_SM, 0);
        } else {
            lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
        }
        lv_label_set_text(s_big_label, display_text);
        lv_obj_set_style_text_color(s_sub_label, C_BLUE, 0);
        lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
        /* Show elapsed time as sub-label + run count */
        if (display_started > 0) {
            struct timeval tv; gettimeofday(&tv, NULL);
            int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
            int secs = (int)((now_ms - display_started) / 1000);
            char sub[48];
            if (run_count > 1) {
                snprintf(sub, sizeof(sub), "%ds [%d/%d] tap for info",
                         secs, (info->carousel_index % run_count) + 1, run_count);
            } else {
                snprintf(sub, sizeof(sub), "%ds - tap for info", secs);
            }
            lv_label_set_text(s_sub_label, sub);
        } else {
            lv_label_set_text(s_sub_label, "Processing... tap for info");
        }
    } else if (s_state == UI_STATE_IDLE) {
        s_showing_activity = false;
        s_activity_detail[0] = '\0';
        /* Restore normal IDLE display when no longer active */
        const char *cur = lv_label_get_text(s_big_label);
        if (cur && strcmp(cur, "READY") != 0 && strcmp(cur, "Tap or Wheel") != 0) {
            /* Was showing activity text, restore to READY */
            ESP_LOGI(TAG, "OC idle (last=%ds) → READY display", info->last_activity_sec);
            lv_obj_set_style_text_color(s_big_label, C_GREEN, 0);
            lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
            lv_label_set_text(s_big_label, "READY");
            lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
            lv_obj_set_style_text_font(s_sub_label, &FONT_MED, 0);
            lv_label_set_text(s_sub_label, "Tap or Wheel");
        }
    }

    /* Line 1: WA status when linked, blank otherwise */
    const char *wa = "";
    if (info->wa_status[0]) {
        if (strcmp(info->wa_status, "on") == 0 || strcmp(info->wa_status, "linked") == 0) wa = LV_SYMBOL_CALL;
    }

    if (info->last_activity_sec < 10) {
        /* Gateway just became active — show refresh icon + WA */
        snprintf(buf1, sizeof(buf1), LV_SYMBOL_REFRESH " Active%s%s",
                 wa[0] ? "  " : "", wa);
    } else {
        /* Idle — only show WA status if linked, nothing otherwise */
        snprintf(buf1, sizeof(buf1), "%s", wa);
    }
    lv_label_set_text(s_info_line1, buf1);

    /* Line 2: only show "Active now" when recently active, blank otherwise */
    if (info->last_activity_sec < 10) {
        snprintf(buf2, sizeof(buf2), "Active now");
        lv_obj_set_style_text_color(s_info_line2, C_BLUE, 0);
    } else {
        buf2[0] = '\0';  /* hide "Last: X ago" — too much clutter */
        lv_obj_set_style_text_color(s_info_line2, C_TEXT_DIM, 0);
    }
    lv_label_set_text(s_info_line2, buf2);

    lvgl_port_unlock();
}

/* ── External activity overlay ───────────────────────────────────────── */
void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms)
{
    if (!s_info_line2) return;
    if (s_state != UI_STATE_IDLE) return;
    if (!lvgl_port_lock(100)) return;

    if (active) {
        char buf[80];
        int secs = elapsed_ms / 1000;
        if (detail && detail[0]) {
            snprintf(buf, sizeof(buf), "%s (%ds)", detail, secs);
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " Active %ds", secs);
        }
        lv_label_set_text(s_info_line2, buf);
        lv_obj_set_style_text_color(s_info_line2, C_BLUE, 0);
    }

    lvgl_port_unlock();
}

/* ── Task / progress display ─────────────────────────────────────────── */

void ui_set_task_info(const char *task_text)
{
    if (!s_task_label || !task_text) return;
    if (!lvgl_port_lock(100)) return;
    lv_label_set_text(s_task_label, task_text);
    lvgl_port_unlock();
}

/* Update task area with detailed info from structured data */
void ui_set_task_info_detailed(const openclaw_info_t *info)
{
    if (!s_task_label || !info || !info->has_tasks) return;
    if (!lvgl_port_lock(100)) return;

    if (info->tasks_running > 0) {
        /* Find first running task and show its name + elapsed time */
        for (int i = 0; i < info->task_count; i++) {
            if (info->tasks[i].running && info->tasks[i].running_at_ms > 0) {
                char buf[80];
                /* Calculate elapsed seconds from running_at_ms (epoch ms) */
                struct timeval tv;
                gettimeofday(&tv, NULL);
                int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                int elapsed_s = (int)((now_ms - info->tasks[i].running_at_ms) / 1000);
                if (elapsed_s < 0) elapsed_s = 0;
                if (elapsed_s < 60) {
                    snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " %s %ds",
                             info->tasks[i].name, elapsed_s);
                } else {
                    snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " %s %dm%ds",
                             info->tasks[i].name, elapsed_s / 60, elapsed_s % 60);
                }
                lv_label_set_text(s_task_label, buf);
                lv_obj_set_style_text_color(s_task_label, C_ORANGE, 0);
                break;
            }
        }
    } else if (info->task_count > 0) {
        lv_label_set_text(s_task_label, info->task_summary);
        lv_obj_set_style_text_color(s_task_label, C_TEXT_DIM, 0);
    } else {
        lv_label_set_text(s_task_label, "");
    }

    lvgl_port_unlock();
}

/* ── Response display ─────────────────────────────────────────────────── */

void ui_set_response(const char *short_text, const char *full_text)
{
    if (!s_big_label) return;

    /* Store full response for TTS — keep original text (not sanitized) */
    if (full_text) {
        strncpy(s_full_response, full_text, sizeof(s_full_response) - 1);
        s_full_response[sizeof(s_full_response) - 1] = '\0';
    }

    bool hebrew = full_text ? has_hebrew(full_text) : false;
    bool non_latin = full_text ? has_non_latin(full_text) : false;

    ESP_LOGI(TAG, "Response display: hebrew=%d non_latin=%d len=%d",
             hebrew, non_latin, full_text ? (int)strlen(full_text) : 0);

    if (!lvgl_port_lock(200)) return;

    /* Extract and display short response */
    char display[128];
    if (short_text && short_text[0]) {
        if (hebrew) {
            sanitize_text_hebrew(display, short_text, sizeof(display));
        } else {
            ui_sanitize_text(display, short_text, sizeof(display));
        }
    } else if (full_text) {
        extract_short_response(display, full_text, sizeof(display));
    } else {
        strcpy(display, "OK");
    }

    /* If display is the LV_SYMBOL_OK (non-Latin fallback), treat as OK */
    bool is_symbol_ok = (strcmp(display, LV_SYMBOL_OK) == 0);

    /* Color based on content */
    lv_color_t color = C_TEXT;
    if (is_symbol_ok || strcasecmp(display, "OK") == 0 || strcasecmp(display, "Done") == 0 ||
        strcasecmp(display, "Yes") == 0 || strcasecmp(display, "Sure") == 0 ||
        strcasecmp(display, "Ready") == 0 || strcasecmp(display, "Got it") == 0) {
        color = C_GREEN;
    } else if (strcasecmp(display, "No") == 0 || strcasecmp(display, "Error") == 0 ||
               strcasecmp(display, "Problem") == 0 || strcasecmp(display, "Failed") == 0) {
        color = C_RED;
    } else if (strcasecmp(display, "Working") == 0 || strcasecmp(display, "Busy") == 0 ||
               strcasecmp(display, "Wait") == 0) {
        color = C_ORANGE;
    }

    /* Font selection: Hebrew → Hebrew font, Latin → Montserrat */
    size_t dlen = strlen(display);
    if (hebrew) {
        /* Use Hebrew font with RTL base direction */
        lv_obj_set_style_base_dir(s_big_label, LV_BASE_DIR_RTL, 0);
        if (dlen <= 12) {
            lv_obj_set_style_text_font(s_big_label, &FONT_HEB_BIG, 0);
        } else {
            lv_obj_set_style_text_font(s_big_label, &FONT_HEBREW, 0);
        }
        ESP_LOGI(TAG, "Hebrew font, RTL, display='%s' (%d bytes)",
                 display, (int)dlen);
    } else {
        lv_obj_set_style_base_dir(s_big_label, LV_BASE_DIR_LTR, 0);
        if (dlen <= 6) {
            lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
        } else if (dlen <= 14) {
            lv_obj_set_style_text_font(s_big_label, &FONT_BIG_SM, 0);
        } else {
            lv_obj_set_style_text_font(s_big_label, &FONT_MED, 0);
        }
    }

    lv_obj_set_style_text_color(s_big_label, color, 0);
    lv_label_set_text(s_big_label, display);

    /* Sub label hint — always use Montserrat (Latin) for hint text */
    lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
    lv_obj_set_style_base_dir(s_sub_label, LV_BASE_DIR_LTR, 0);
    if (non_latin && !hebrew) {
        /* Non-Latin, non-Hebrew (e.g. Arabic, CJK) — hint to use TTS */
        lv_obj_set_style_text_color(s_sub_label, C_TEAL, 0);
        lv_label_set_text(s_sub_label, "Tap " LV_SYMBOL_PLAY " to hear");
    } else if (full_text && strlen(full_text) > dlen + 5) {
        lv_obj_set_style_text_color(s_sub_label, C_BLUE, 0);
        lv_label_set_text(s_sub_label, "Tap " LV_SYMBOL_PLAY " or " LV_SYMBOL_LIST);
    } else {
        lv_label_set_text(s_sub_label, "");
    }

    /* Clear info/task lines during response to prevent overlay */
    lv_label_set_text(s_info_line1, "");
    lv_label_set_text(s_info_line2, "");
    lv_label_set_text(s_task_label, "");

    lvgl_port_unlock();
}

const char *ui_get_full_response(void)
{
    return s_full_response;
}

lv_obj_t *ui_get_main_screen(void)
{
    return s_scr;
}

/* ── Thinking timer ───────────────────────────────────────────────────── */

void ui_set_thinking_time(uint32_t elapsed_ms)
{
    if (!s_think_label || s_state != UI_STATE_THINKING) return;
    if (!lvgl_port_lock(100)) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu.%lus",
             (unsigned long)(elapsed_ms / 1000),
             (unsigned long)((elapsed_ms % 1000) / 100));
    lv_label_set_text(s_think_label, buf);
    lvgl_port_unlock();
}

void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms)
{
    if (!s_think_label || s_state != UI_STATE_THINKING) return;
    if (!lvgl_port_lock(100)) return;

    char buf[80];
    if (detail && detail[0]) {
        snprintf(buf, sizeof(buf), "%s\n%lu.%lus",
                 detail,
                 (unsigned long)(elapsed_ms / 1000),
                 (unsigned long)((elapsed_ms % 1000) / 100));
        /* Green accent for "OK" results */
        lv_color_t col = (strstr(detail, "OK") || strstr(detail, "ok"))
                         ? lv_color_hex(0x3FB950) : lv_color_hex(0xFF8C00);
        lv_obj_set_style_text_color(s_think_label, col, 0);
    } else {
        snprintf(buf, sizeof(buf), "Thinking...\n%lu.%lus",
                 (unsigned long)(elapsed_ms / 1000),
                 (unsigned long)((elapsed_ms % 1000) / 100));
        lv_obj_set_style_text_color(s_think_label, lv_color_hex(0xFF8C00), 0);
    }
    lv_label_set_text(s_think_label, buf);

    lvgl_port_unlock();
}

/* ── Status message (boot/connecting) ─────────────────────────────────── */

void ui_set_status_message(const char *msg)
{
    if (!s_sub_label || !msg) return;
    if (!lvgl_port_lock(100)) return;

    lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
    lv_obj_set_style_text_color(s_sub_label, C_TEXT_DIM, 0);
    lv_label_set_text(s_sub_label, msg);
    lvgl_port_unlock();
}

/* ── Web server status indicator ─────────────────────────────────────── */

void ui_set_webserver_status(bool running)
{
    if (!s_web_label) return;
    if (!lvgl_port_lock(100)) return;

    if (running) {
        lv_label_set_text(s_web_label, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(s_web_label, C_BLUE, 0);
        /* Update web button color */
        if (s_web_btn) {
            lv_obj_set_style_bg_color(s_web_btn, C_BLUE, 0);
        }
    } else {
        lv_label_set_text(s_web_label, "");
        if (s_web_btn) {
            lv_obj_set_style_bg_color(s_web_btn, C_BAR_BG, 0);
        }
    }

    lvgl_port_unlock();
}

/* ── Camera preview ──────────────────────────────────────────────────── */
void ui_show_camera_preview(const uint8_t *jpeg, size_t jpeg_size)
{
    if (!s_big_label || !s_sub_label) return;
    if (!lvgl_port_lock(200)) return;

    lv_obj_set_style_text_font(s_big_label, &FONT_BIG, 0);
    lv_obj_set_style_text_color(s_big_label, C_GREEN, 0);
    lv_obj_set_style_base_dir(s_big_label, LV_BASE_DIR_LTR, 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f KB", (float)jpeg_size / 1024.0f);
    lv_label_set_text(s_big_label, buf);

    lv_obj_set_style_text_font(s_sub_label, &FONT_SUB, 0);
    lv_obj_set_style_text_color(s_sub_label, C_TEAL, 0);
    lv_label_set_text(s_sub_label, "Image ready - talk to send");

    lvgl_port_unlock();
}

#endif /* BOARD_HAS_DISPLAY */
