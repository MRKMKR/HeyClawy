/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * OpenClaw Gateway WebSocket client implementation.
 *
 * Protocol: JSON text frames over WebSocket
 * - Server sends connect.challenge event with nonce
 * - Client sends connect req with device identity (ED25519 signed)
 * - Server responds hello-ok → client is connected with operator scopes
 * - Chat send: {type:"req", id, method:"chat.send", params:{sessionKey, message, idempotencyKey}}
 * - Chat event: {type:"event", event:"chat", payload:{state:"delta"|"final", message:{content:[{text}]}}}
 */

#include "openclaw_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "ed25519.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include <sys/time.h>

static const char *TAG = "openclaw";

#define MAX_RESPONSE_LEN 4096
#define MAX_MSG_ID 99999

/* Format tool name for display: "memory_search" → "Memory", "exec" → "Exec" */
static void format_tool_label(char *dst, size_t dst_sz, const char *tool_name)
{
    if (!tool_name || !tool_name[0]) { dst[0] = '\0'; return; }
    /* Take first word (before _), capitalize first letter */
    size_t i = 0;
    while (tool_name[i] && tool_name[i] != '_' && i < dst_sz - 1) {
        dst[i] = (i == 0) ? (char)toupper((unsigned char)tool_name[i]) : tool_name[i];
        i++;
    }
    dst[i] = '\0';
}

// Base64url encoding (no padding)
static const char b64url_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t base64url_encode(char *out, size_t out_max, const uint8_t *in, size_t in_len)
{
    size_t oi = 0, i = 0;
    while (i < in_len) {
        size_t rem = in_len - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i+1] << 8;
        if (rem > 2) v |= (uint32_t)in[i+2];

        if (oi < out_max) out[oi++] = b64url_table[(v >> 18) & 0x3F];
        if (oi < out_max) out[oi++] = b64url_table[(v >> 12) & 0x3F];
        if (rem > 1 && oi < out_max) out[oi++] = b64url_table[(v >> 6) & 0x3F];
        if (rem > 2 && oi < out_max) out[oi++] = b64url_table[v & 0x3F];
        i += 3;
    }
    if (oi < out_max) out[oi] = '\0';
    return oi;
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 0;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        out[i] = (hex_nibble(hex[i*2]) << 4) | hex_nibble(hex[i*2+1]);
    }
}

static struct {
    esp_websocket_client_handle_t ws;
    openclaw_state_t state;
    openclaw_state_cb_t state_cb;
    openclaw_chat_cb_t chat_cb;
    openclaw_notify_cb_t notify_cb;
    char response_buf[MAX_RESPONSE_LEN];
    size_t response_len;
    char host[64];
    uint16_t port;
    char token[128];
    char session_key[64];
    char nonce[48];
    int64_t chat_start_time;
    uint32_t msg_id;
    // ED25519 device identity
    uint8_t ed_seed[32];       // 32-byte seed (private key material)
    uint8_t ed_pubkey[32];     // 32-byte public key
    uint8_t ed_privkey[64];    // 64-byte expanded private key (seed + pubkey per NaCl convention)
    char device_id[65];        // SHA-256 hex of public key
    bool has_device_key;
    // Fragmented message reassembly
    char *frag_buf;
    size_t frag_len;
    size_t frag_total;
    // Server info from snapshot + health events
    openclaw_info_t info;
    // Flag set when external activity detected (for wake-from-sleep)
    volatile bool external_activity_detected;
    // Cooldown: epoch_ms when is_active was last explicitly cleared (prevent health fallback override)
    int64_t active_cleared_ms;
    // Track device-initiated run IDs (ring buffer for notification detection)
    char device_run_ids[8][48];
    int device_run_id_idx;
    // Track cron-originated session keys (ring buffer for notification filtering)
    char cron_sessions[4][80];
    int cron_session_idx;
} s_oc;

static void reset_fragment_buffer(void)
{
    free(s_oc.frag_buf);
    s_oc.frag_buf = NULL;
    s_oc.frag_len = 0;
    s_oc.frag_total = 0;
}

static void set_state(openclaw_state_t st)
{
    s_oc.state = st;
    if (s_oc.state_cb) s_oc.state_cb(st);
}

/* Clear activity tracking and set cooldown to prevent health fallback from re-activating */
static void clear_activity(void)
{
    s_oc.info.is_active = false;
    s_oc.info.is_external = false;
    s_oc.info.active_detail[0] = '\0';
    s_oc.info.active_run_id[0] = '\0';
    s_oc.info.active_session_key[0] = '\0';
    s_oc.info.active_started_ms = 0;
    s_oc.info.last_activity_sec = 30;
    struct timeval tv; gettimeofday(&tv, NULL);
    s_oc.active_cleared_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ── Multi-run carousel tracking ────────────────────────────────────── */

static openclaw_active_run_t *find_run_slot(const char *run_id)
{
    if (!run_id || !run_id[0]) return NULL;
    for (int i = 0; i < OC_MAX_ACTIVE_RUNS; i++) {
        if (s_oc.info.active_runs[i].active &&
            strcmp(s_oc.info.active_runs[i].run_id, run_id) == 0)
            return &s_oc.info.active_runs[i];
    }
    return NULL;
}

static openclaw_active_run_t *add_run_slot(const char *run_id, const char *sess_key, const char *source)
{
    if (!run_id || !run_id[0]) return NULL;
    /* Find existing or free slot */
    openclaw_active_run_t *slot = find_run_slot(run_id);
    if (slot) return slot;
    for (int i = 0; i < OC_MAX_ACTIVE_RUNS; i++) {
        if (!s_oc.info.active_runs[i].active) {
            slot = &s_oc.info.active_runs[i];
            break;
        }
    }
    if (!slot) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < OC_MAX_ACTIVE_RUNS; i++) {
            if (s_oc.info.active_runs[i].started_ms < s_oc.info.active_runs[oldest].started_ms)
                oldest = i;
        }
        slot = &s_oc.info.active_runs[oldest];
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    strncpy(slot->run_id, run_id, sizeof(slot->run_id) - 1);
    if (sess_key) strncpy(slot->session_key, sess_key, sizeof(slot->session_key) - 1);
    if (source) strncpy(slot->source, source, sizeof(slot->source) - 1);
    struct timeval tv; gettimeofday(&tv, NULL);
    slot->started_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    snprintf(slot->detail, sizeof(slot->detail), "Thinking...");
    s_oc.info.active_runs_count++;
    return slot;
}

static void remove_run_slot(const char *run_id)
{
    openclaw_active_run_t *slot = find_run_slot(run_id);
    if (slot) {
        slot->active = false;
        s_oc.info.active_runs_count--;
        if (s_oc.info.active_runs_count < 0) s_oc.info.active_runs_count = 0;
    }
    /* If no active runs left, clear overall activity */
    if (s_oc.info.active_runs_count == 0 && s_oc.info.is_external) {
        clear_activity();
    }
}

/* Check if a runId was initiated by this device */
static bool is_device_run(const char *run_id)
{
    if (!run_id || !run_id[0]) return false;
    for (int i = 0; i < 8; i++) {
        if (s_oc.device_run_ids[i][0] && strcmp(s_oc.device_run_ids[i], run_id) == 0)
            return true;
    }
    return false;
}

static void track_device_run(const char *run_id)
{
    if (!run_id || !run_id[0]) return;
    strncpy(s_oc.device_run_ids[s_oc.device_run_id_idx], run_id,
            sizeof(s_oc.device_run_ids[0]) - 1);
    s_oc.device_run_id_idx = (s_oc.device_run_id_idx + 1) % 8;
}

/* Track cron-originated session keys for notification filtering */
static void track_cron_session(const char *sess_key)
{
    if (!sess_key || !sess_key[0]) return;
    /* Don't add duplicates */
    for (int i = 0; i < 4; i++) {
        if (s_oc.cron_sessions[i][0] && strcmp(s_oc.cron_sessions[i], sess_key) == 0) return;
    }
    strncpy(s_oc.cron_sessions[s_oc.cron_session_idx], sess_key,
            sizeof(s_oc.cron_sessions[0]) - 1);
    s_oc.cron_session_idx = (s_oc.cron_session_idx + 1) % 4;
}

static bool is_cron_session(const char *sess_key)
{
    if (!sess_key || !sess_key[0]) return false;
    for (int i = 0; i < 4; i++) {
        if (s_oc.cron_sessions[i][0] && strcmp(s_oc.cron_sessions[i], sess_key) == 0)
            return true;
    }
    return false;
}

static char *next_id(void)
{
    static char id_buf[16];
    s_oc.msg_id = (s_oc.msg_id + 1) % MAX_MSG_ID;
    snprintf(id_buf, sizeof(id_buf), "%" PRIu32, s_oc.msg_id);
    return id_buf;
}

static void send_connect(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "connect");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddNumberToObject(params, "minProtocol", 3);
    cJSON_AddNumberToObject(params, "maxProtocol", 3);

    cJSON *client = cJSON_AddObjectToObject(params, "client");
    cJSON_AddStringToObject(client, "id", "gateway-client");
    cJSON_AddStringToObject(client, "displayName", "HeyClawy");
    cJSON_AddStringToObject(client, "version", "0.2.0");
#if CONFIG_IDF_TARGET_ESP32S3
    cJSON_AddStringToObject(client, "platform", "esp32s3");
#else
    cJSON_AddStringToObject(client, "platform", "esp32");
#endif
    cJSON_AddStringToObject(client, "mode", "cli");

    cJSON_AddStringToObject(params, "role", "operator");

    cJSON *scopes = cJSON_AddArrayToObject(params, "scopes");
    cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.read"));
    cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.write"));
    cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.admin"));

    /* Request tool event broadcasting */
    cJSON *caps = cJSON_AddArrayToObject(params, "caps");
    cJSON_AddItemToArray(caps, cJSON_CreateString("tool-events"));

    if (strlen(s_oc.token) > 0) {
        cJSON *auth = cJSON_AddObjectToObject(params, "auth");
        cJSON_AddStringToObject(auth, "token", s_oc.token);
    }

    // Add device identity block with ED25519 signature
    if (s_oc.has_device_key) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t epoch_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

        // Build auth payload: v2|deviceId|clientId|clientMode|role|scopes|signedAtMs|token|nonce
        char auth_payload[512];
        snprintf(auth_payload, sizeof(auth_payload),
                 "v2|%s|gateway-client|cli|operator|operator.read,operator.write,operator.admin|%" PRId64 "|%s|%s",
                 s_oc.device_id, epoch_ms, s_oc.token, s_oc.nonce);

        // ED25519 sign
        uint8_t signature[64];
        ed25519_sign(signature, (const uint8_t *)auth_payload, strlen(auth_payload),
                     s_oc.ed_pubkey, s_oc.ed_privkey);

        // Base64url encode public key and signature
        char pub_b64[64];
        base64url_encode(pub_b64, sizeof(pub_b64), s_oc.ed_pubkey, 32);
        char sig_b64[128];
        base64url_encode(sig_b64, sizeof(sig_b64), signature, 64);

        cJSON *device = cJSON_AddObjectToObject(params, "device");
        cJSON_AddStringToObject(device, "id", s_oc.device_id);
        cJSON_AddStringToObject(device, "publicKey", pub_b64);
        cJSON_AddStringToObject(device, "signature", sig_b64);
        cJSON_AddNumberToObject(device, "signedAt", (double)epoch_ms);
        cJSON_AddStringToObject(device, "nonce", s_oc.nonce);

        ESP_LOGI(TAG, "Device auth: id=%s", s_oc.device_id);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        int jlen = strlen(json_str);
        ESP_LOGI(TAG, "Connect req (%d bytes): %.200s%s", jlen, json_str, jlen > 200 ? "..." : "");
        esp_websocket_client_send_text(s_oc.ws, json_str, jlen, pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Sent connect request (device_auth=%s)", s_oc.has_device_key ? "yes" : "no");
        free(json_str);
    }
    cJSON_Delete(root);

    set_state(OPENCLAW_STATE_AUTHENTICATING);
}

static void handle_message(const char *data, int len)
{
    ESP_LOGD(TAG, "WS msg (%d bytes): %.200s%s", len, data, len > 200 ? "..." : "");

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON message: %.160s%s",
                 data ? data : "(null)", (data && len > 160) ? "..." : "");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) goto cleanup;

    if (strcmp(type, "event") == 0) {
        // Handle server events
        const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(root, "event"));
        if (!event) goto cleanup;

        if (strcmp(event, "connect.challenge") == 0) {
            ESP_LOGI(TAG, "Got connect.challenge");
            // Extract nonce for connect request
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (payload) {
                const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "nonce"));
                if (nonce) {
                    strncpy(s_oc.nonce, nonce, sizeof(s_oc.nonce) - 1);
                }
            }
            send_connect();
        } else if (strcmp(event, "chat") == 0) {
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (!payload) goto cleanup;

            const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "state"));
            if (!state) goto cleanup;

            /* Extract runId and sessionKey for tracking */
            const char *run_id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "runId"));
            const char *sess_key = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "sessionKey"));

            /* Only process chat events if we initiated a chat request.
             * External sources (hooks, WhatsApp, WebUI, TUI) also generate chat events
             * but we should not display them as our response. */
            if (!s_oc.chat_cb) {
                /* --- External activity tracking --- */
                if (strcmp(state, "delta") == 0) {
                    if (!s_oc.info.is_active || !s_oc.info.is_external) {
                        ESP_LOGI(TAG, "External activity started (chat delta) run=%s",
                                 run_id ? run_id : "?");
                    }
                    s_oc.info.is_active = true;
                    s_oc.info.is_external = true;
                    s_oc.info.last_activity_sec = 0;
                    s_oc.info.last_activity_min = 0;
                    s_oc.external_activity_detected = true;
                    if (run_id) strncpy(s_oc.info.active_run_id, run_id, sizeof(s_oc.info.active_run_id) - 1);
                    if (sess_key) strncpy(s_oc.info.active_session_key, sess_key, sizeof(s_oc.info.active_session_key) - 1);
                    if (s_oc.info.active_started_ms == 0) {
                        struct timeval tv; gettimeofday(&tv, NULL);
                        s_oc.info.active_started_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                    }
                    snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Responding...");
                } else if (strcmp(state, "final") == 0 || strcmp(state, "error") == 0 || strcmp(state, "aborted") == 0) {
                    ESP_LOGI(TAG, "External activity ended (chat %s) run=%s", state,
                             run_id ? run_id : "?");
                    /* Extract notification text from final message */
                    if (strcmp(state, "final") == 0 && s_oc.notify_cb) {
                        cJSON *message = cJSON_GetObjectItem(payload, "message");
                        if (message) {
                            cJSON *content = cJSON_GetObjectItem(message, "content");
                            if (content && cJSON_IsArray(content)) {
                                cJSON *item;
                                cJSON_ArrayForEach(item, content) {
                                    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                                    if (text && strlen(text) > 0) {
                                        const char *src = is_cron_session(sess_key) ? "cron" : "external";
                                        ESP_LOGI(TAG, "Notification from %s chat: %.80s...", src, text);
                                        s_oc.notify_cb(text, src);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    remove_run_slot(run_id);
                }
                goto cleanup;
            }

            /* --- Device-initiated chat processing --- */
            if (strcmp(state, "delta") == 0) {
                /* Track this run as device-initiated */
                if (run_id) track_device_run(run_id);
                set_state(OPENCLAW_STATE_CHAT_STREAMING);
                cJSON *message = cJSON_GetObjectItem(payload, "message");
                if (message) {
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && cJSON_IsArray(content)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, content) {
                            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                            if (text) {
                                size_t tlen = strlen(text);
                                if (s_oc.response_len + tlen < MAX_RESPONSE_LEN - 1) {
                                    memcpy(s_oc.response_buf + s_oc.response_len, text, tlen);
                                    s_oc.response_len += tlen;
                                    s_oc.response_buf[s_oc.response_len] = '\0';
                                }
                                if (s_oc.chat_cb) s_oc.chat_cb(text, false);
                            }
                        }
                    }
                }
            } else if (strcmp(state, "final") == 0) {
                /* Final event: authoritative source, replaces streaming deltas */
                cJSON *message = cJSON_GetObjectItem(payload, "message");
                if (message) {
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && cJSON_IsArray(content)) {
                        s_oc.response_buf[0] = '\0';
                        s_oc.response_len = 0;
                        cJSON *item;
                        cJSON_ArrayForEach(item, content) {
                            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                            if (text) {
                                size_t tlen = strlen(text);
                                if (s_oc.response_len + tlen < MAX_RESPONSE_LEN - 1) {
                                    memcpy(s_oc.response_buf + s_oc.response_len, text, tlen);
                                    s_oc.response_len += tlen;
                                    s_oc.response_buf[s_oc.response_len] = '\0';
                                }
                            }
                        }
                    }
                }
                if (s_oc.chat_cb) s_oc.chat_cb(s_oc.response_buf, true);
                s_oc.chat_cb = NULL;  /* Done — prevent late events from misclassifying */
                set_state(OPENCLAW_STATE_CONNECTED);
                clear_activity();
            } else if (strcmp(state, "error") == 0) {
                const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "errorMessage"));
                ESP_LOGE(TAG, "Chat error: %s", err ? err : "unknown");
                if (s_oc.chat_cb) s_oc.chat_cb(err ? err : "Error", true);
                s_oc.chat_cb = NULL;
                set_state(OPENCLAW_STATE_CONNECTED);
                clear_activity();
            } else if (strcmp(state, "aborted") == 0) {
                ESP_LOGW(TAG, "Chat aborted (run=%s)", run_id ? run_id : "?");
                if (s_oc.chat_cb) s_oc.chat_cb("Aborted", true);
                s_oc.chat_cb = NULL;
                set_state(OPENCLAW_STATE_CONNECTED);
                clear_activity();
            }
        } else if (strcmp(event, "agent") == 0) {
            /* Agent lifecycle, tool use, and thinking events */
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (!payload) goto cleanup;

            const char *stream = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "stream"));
            const char *run_id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "runId"));
            const char *sess_key = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "sessionKey"));

            if (!stream) goto cleanup;

            /* Track activity from agent events (any source) */
            if (!s_oc.chat_cb) {
                /* External agent event */
                if (strcmp(stream, "lifecycle") == 0) {
                    cJSON *data = cJSON_GetObjectItem(payload, "data");
                    const char *phase = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "phase")) : NULL;
                    if (phase && strcmp(phase, "start") == 0) {
                        ESP_LOGI(TAG, "Agent run started (external) run=%s", run_id ? run_id : "?");
                        /* Add to multi-run tracker */
                        const char *src = "external";
                        openclaw_active_run_t *slot = add_run_slot(run_id, sess_key, src);
                        if (slot) snprintf(slot->detail, sizeof(slot->detail), "Thinking...");
                        s_oc.info.is_active = true;
                        s_oc.info.is_external = true;
                        s_oc.info.last_activity_sec = 0;
                        s_oc.info.last_activity_min = 0;
                        s_oc.external_activity_detected = true;
                        if (run_id) strncpy(s_oc.info.active_run_id, run_id, sizeof(s_oc.info.active_run_id) - 1);
                        if (sess_key) strncpy(s_oc.info.active_session_key, sess_key, sizeof(s_oc.info.active_session_key) - 1);
                        struct timeval tv; gettimeofday(&tv, NULL);
                        s_oc.info.active_started_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                        snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Thinking...");
                    } else if (phase && (strcmp(phase, "end") == 0 || strcmp(phase, "error") == 0)) {
                        ESP_LOGI(TAG, "Agent run ended (external, phase=%s) run=%s", phase, run_id ? run_id : "?");
                        remove_run_slot(run_id);
                    }
                } else if (strcmp(stream, "tool") == 0) {
                    cJSON *data = cJSON_GetObjectItem(payload, "data");
                    const char *tool_name = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "name")) : NULL;
                    const char *phase = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "phase")) : NULL;
                    char label[32] = {0};
                    if (tool_name) {
                        format_tool_label(label, sizeof(label), tool_name);
                        ESP_LOGI(TAG, "Agent using tool: %s → %s (external, phase=%s)",
                                 tool_name, label, phase ? phase : "?");
                        if (phase && strcmp(phase, "result") == 0) {
                            snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail),
                                     "%s OK", label);
                        } else {
                            snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail),
                                     "%s...", label);
                        }
                    }
                    /* Update multi-run slot detail */
                    openclaw_active_run_t *slot = find_run_slot(run_id);
                    if (slot && label[0]) {
                        if (phase && strcmp(phase, "result") == 0)
                            snprintf(slot->detail, sizeof(slot->detail), "%s OK", label);
                        else
                            snprintf(slot->detail, sizeof(slot->detail), "%s...", label);
                    }
                    s_oc.info.is_active = true;
                    s_oc.info.is_external = true;  /* tool events imply external context */
                    s_oc.info.last_activity_sec = 0;
                    s_oc.external_activity_detected = true;
                } else if (strcmp(stream, "assistant") == 0) {
                    snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Responding");
                    s_oc.info.is_active = true;
                    s_oc.info.is_external = true;  /* assistant stream without lifecycle start */
                    s_oc.info.last_activity_sec = 0;
                } else if (strcmp(stream, "compaction") == 0) {
                    cJSON *data = cJSON_GetObjectItem(payload, "data");
                    const char *phase = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "phase")) : NULL;
                    if (phase && strcmp(phase, "start") == 0) {
                        snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Compact");
                    }
                }
            } else {
                /* Device-initiated agent event — update detail for UI */
                if (strcmp(stream, "tool") == 0) {
                    cJSON *data = cJSON_GetObjectItem(payload, "data");
                    const char *tool_name = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "name")) : NULL;
                    const char *phase = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "phase")) : NULL;
                    if (tool_name) {
                        char label[32];
                        format_tool_label(label, sizeof(label), tool_name);
                        ESP_LOGI(TAG, "Agent using tool: %s → %s (device, phase=%s)",
                                 tool_name, label, phase ? phase : "?");
                        if (phase && strcmp(phase, "result") == 0) {
                            snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail),
                                     "%s OK", label);
                        } else {
                            snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail),
                                     "%s...", label);
                        }
                    }
                } else if (strcmp(stream, "lifecycle") == 0) {
                    cJSON *data = cJSON_GetObjectItem(payload, "data");
                    const char *phase = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "phase")) : NULL;
                    if (phase && strcmp(phase, "start") == 0) {
                        s_oc.info.is_active = true;
                        snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Thinking");
                    } else if (phase && (strcmp(phase, "end") == 0 || strcmp(phase, "error") == 0)) {
                        s_oc.info.active_detail[0] = '\0';
                    }
                } else if (strcmp(stream, "assistant") == 0) {
                    snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Responding");
                } else if (strcmp(stream, "compaction") == 0) {
                    cJSON *data = cJSON_GetObjectItem(payload, "data");
                    const char *phase = data ? cJSON_GetStringValue(cJSON_GetObjectItem(data, "phase")) : NULL;
                    if (phase && strcmp(phase, "start") == 0) {
                        snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail), "Compact");
                    }
                }
            }
        } else if (strcmp(event, "cron") == 0) {
            /* Cron job execution events */
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (!payload) goto cleanup;

            const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "action"));
            const char *job_id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "jobId"));
            const char *status_str = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "status"));
            const char *sess_key = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "sessionKey"));

            if (action) {
                ESP_LOGI(TAG, "Cron event: action=%s job=%s status=%s",
                         action, job_id ? job_id : "?", status_str ? status_str : "?");

                /* Find job name from our cached task list */
                const char *job_name = "Cron job";
                for (int i = 0; i < s_oc.info.task_count; i++) {
                    if (job_id && strcmp(s_oc.info.tasks[i].id, job_id) == 0) {
                        job_name = s_oc.info.tasks[i].name;
                        break;
                    }
                }

                if (strcmp(action, "start") == 0 || (status_str && strcmp(status_str, "running") == 0)) {
                    s_oc.info.is_active = true;
                    s_oc.info.is_external = true;
                    s_oc.info.last_activity_sec = 0;
                    s_oc.external_activity_detected = true;
                    strncpy(s_oc.info.active_source, "cron", sizeof(s_oc.info.active_source));
                    if (sess_key) {
                        strncpy(s_oc.info.active_session_key, sess_key, sizeof(s_oc.info.active_session_key) - 1);
                        track_cron_session(sess_key);
                    }
                    struct timeval tv; gettimeofday(&tv, NULL);
                    s_oc.info.active_started_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                    snprintf(s_oc.info.active_detail, sizeof(s_oc.info.active_detail),
                             "Cron: %.50s", job_name);
                } else if (strcmp(action, "end") == 0 || strcmp(action, "error") == 0 ||
                           (status_str && (strcmp(status_str, "ok") == 0 || strcmp(status_str, "error") == 0))) {
                    clear_activity();
                }
            }
        } else if (strcmp(event, "tick") == 0) {
            // Heartbeat tick - update uptime
            s_oc.info.uptime_min++;
        } else if (strcmp(event, "health") == 0) {
            /* Server pushes health every ~30s — parse fully */
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (payload) {
                s_oc.info.ok = cJSON_IsTrue(cJSON_GetObjectItem(payload, "ok"));

                /* Uptime */
                cJSON *uptime = cJSON_GetObjectItem(payload, "uptimeMs");
                if (uptime && cJSON_IsNumber(uptime)) {
                    s_oc.info.uptime_min = (uint32_t)(uptime->valuedouble / 60000.0);
                }

                /* Sessions */
                cJSON *sessions = cJSON_GetObjectItem(payload, "sessions");
                if (sessions) {
                    cJSON *count = cJSON_GetObjectItem(sessions, "count");
                    if (count && cJSON_IsNumber(count)) s_oc.info.session_count = count->valueint;

                    /* Recent sessions — get most recent age */
                    cJSON *recent = cJSON_GetObjectItem(sessions, "recent");
                    if (recent && cJSON_IsArray(recent) && cJSON_GetArraySize(recent) > 0) {
                        cJSON *newest = cJSON_GetArrayItem(recent, 0);
                        cJSON *age = cJSON_GetObjectItem(newest, "age");
                        if (age && cJSON_IsNumber(age)) {
                            int age_sec = (int)(age->valuedouble / 1000.0);
                            /* Always update from health — needed for stale run detection */
                            s_oc.info.last_activity_sec = age_sec;
                            s_oc.info.last_activity_min = age_sec / 60;
                        }
                    }
                }

                /* Channels */
                cJSON *channels = cJSON_GetObjectItem(payload, "channels");
                if (channels) {
                    cJSON *wa = cJSON_GetObjectItem(channels, "whatsapp");
                    if (wa) {
                        bool connected = cJSON_IsTrue(cJSON_GetObjectItem(wa, "connected"));
                        bool linked = cJSON_IsTrue(cJSON_GetObjectItem(wa, "linked"));
                        bool configured = cJSON_IsTrue(cJSON_GetObjectItem(wa, "configured"));
                        if (connected) strncpy(s_oc.info.wa_status, "on", sizeof(s_oc.info.wa_status));
                        else if (linked) strncpy(s_oc.info.wa_status, "linked", sizeof(s_oc.info.wa_status));
                        else if (configured) strncpy(s_oc.info.wa_status, "off", sizeof(s_oc.info.wa_status));
                    }
                }

                /* Health fallback: only log recent activity, don't set is_active.
                 * Real-time event tracking (agent/chat events) handles activity state.
                 * The old health fallback caused "Processing..." ghosting. */
                if (!s_oc.info.is_active && s_oc.info.last_activity_sec < 5) {
                    ESP_LOGD(TAG, "Health shows recent activity (last=%ds) — not overriding event tracking",
                             s_oc.info.last_activity_sec);
                }
                /* Auto-clear stale activity: if health shows OC idle for 30s+
                 * but we still have is_active set, the run-end event was lost.
                 * Clear regardless of is_external — any stale active state should be cleared. */
                if (s_oc.info.is_active && s_oc.info.last_activity_sec >= 30) {
                    ESP_LOGW(TAG, "Health: stale activity detected (last=%ds, runs=%d, run=%s) — clearing",
                             s_oc.info.last_activity_sec, s_oc.info.active_runs_count,
                             s_oc.info.active_run_id[0] ? s_oc.info.active_run_id : "(none)");
                    /* Clear all tracked run slots */
                    for (int i = 0; i < OC_MAX_ACTIVE_RUNS; i++) {
                        s_oc.info.active_runs[i].active = false;
                    }
                    s_oc.info.active_runs_count = 0;
                    clear_activity();
                }
            }
        } else {
            ESP_LOGD(TAG, "Unhandled event: %s", event);
        }
    } else if (strcmp(type, "res") == 0) {
        // Response to our request
        ESP_LOGI(TAG, "Response received (id=%s state=%d)",
                 cJSON_GetStringValue(cJSON_GetObjectItem(root, "id")), s_oc.state);
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (error) {
            const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
            ESP_LOGE(TAG, "Request error: %s", msg ? msg : "unknown");
            /* Only transition to error if we're authenticating — other errors are non-fatal */
            if (s_oc.state == OPENCLAW_STATE_AUTHENTICATING) {
                set_state(OPENCLAW_STATE_ERROR);
            }
        } else {
            cJSON *payload = cJSON_GetObjectItem(root, "payload");

            /* Check for usage.status response (has "providers" key) */
            if (payload) {
                cJSON *providers = cJSON_GetObjectItem(payload, "providers");
                if (providers && cJSON_IsArray(providers)) {
                    if (cJSON_GetArraySize(providers) > 0) {
                        cJSON *first_prov = cJSON_GetArrayItem(providers, 0);
                        if (first_prov) {
                            const char *pname = cJSON_GetStringValue(cJSON_GetObjectItem(first_prov, "displayName"));
                            if (!pname) pname = cJSON_GetStringValue(cJSON_GetObjectItem(first_prov, "provider"));
                            if (pname) strncpy(s_oc.info.provider_name, pname, sizeof(s_oc.info.provider_name) - 1);

                            cJSON *windows = cJSON_GetObjectItem(first_prov, "windows");
                            if (windows && cJSON_IsArray(windows) && cJSON_GetArraySize(windows) > 0) {
                                cJSON *win = cJSON_GetArrayItem(windows, 0);
                                cJSON *pct = cJSON_GetObjectItem(win, "usedPercent");
                                if (pct && cJSON_IsNumber(pct)) {
                                    s_oc.info.usage_percent = (int)(pct->valuedouble + 0.5);
                                }
                                const char *lbl = cJSON_GetStringValue(cJSON_GetObjectItem(win, "label"));
                                if (lbl) strncpy(s_oc.info.usage_label, lbl, sizeof(s_oc.info.usage_label) - 1);
                            }
                            s_oc.info.has_usage = true;
                            ESP_LOGI(TAG, "Usage: %s %d%% (%s)",
                                     s_oc.info.provider_name, s_oc.info.usage_percent, s_oc.info.usage_label);
                        }
                    } else {
                        /* Empty providers array — no usage tracking configured */
                        s_oc.info.usage_empty = true;
                        ESP_LOGD(TAG, "Usage: no providers configured");
                    }
                    goto cleanup;
                }

                /* Check for health response (has "ok" and "agents") */
                cJSON *h_ok = cJSON_GetObjectItem(payload, "ok");
                cJSON *h_agents = cJSON_GetObjectItem(payload, "agents");
                if (h_ok && h_agents && cJSON_IsArray(h_agents)) {
                    s_oc.info.ok = cJSON_IsTrue(h_ok);
                    cJSON *first_agent = cJSON_GetArrayItem(h_agents, 0);
                    if (first_agent) {
                        const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(first_agent, "agentId"));
                        if (aid) strncpy(s_oc.info.agent_id, aid, sizeof(s_oc.info.agent_id) - 1);
                        cJSON *sessions = cJSON_GetObjectItem(first_agent, "sessions");
                        if (sessions) {
                            cJSON *count = cJSON_GetObjectItem(sessions, "count");
                            if (count && cJSON_IsNumber(count))
                                s_oc.info.session_count = (int)count->valuedouble;
                            cJSON *recent = cJSON_GetObjectItem(sessions, "recent");
                            if (recent && cJSON_IsArray(recent)) {
                                cJSON *newest = cJSON_GetArrayItem(recent, 0);
                                if (newest) {
                                    cJSON *age = cJSON_GetObjectItem(newest, "age");
                                    if (age && cJSON_IsNumber(age)) {
                                        s_oc.info.last_activity_sec = (int)(age->valuedouble / 1000.0);
                                        s_oc.info.last_activity_min = s_oc.info.last_activity_sec / 60;
                                    }
                                }
                            }
                        }
                    }
                    cJSON *channels = cJSON_GetObjectItem(payload, "channels");
                    if (channels) {
                        cJSON *wa = cJSON_GetObjectItem(channels, "whatsapp");
                        if (wa) {
                            bool connected = cJSON_IsTrue(cJSON_GetObjectItem(wa, "connected"));
                            bool linked = cJSON_IsTrue(cJSON_GetObjectItem(wa, "linked"));
                            bool configured = cJSON_IsTrue(cJSON_GetObjectItem(wa, "configured"));
                            if (connected) strncpy(s_oc.info.wa_status, "on", sizeof(s_oc.info.wa_status));
                            else if (linked) strncpy(s_oc.info.wa_status, "linked", sizeof(s_oc.info.wa_status));
                            else if (configured) strncpy(s_oc.info.wa_status, "off", sizeof(s_oc.info.wa_status));
                        }
                    }
                    ESP_LOGI(TAG, "Health: ok=%d agent=%s sess=%d last=%ds wa=%s",
                             s_oc.info.ok, s_oc.info.agent_id,
                             s_oc.info.session_count, s_oc.info.last_activity_sec,
                             s_oc.info.wa_status);
                    goto cleanup;
                }

                /* Check for cron.list response (has "jobs" key) */
                cJSON *crons = cJSON_GetObjectItem(payload, "jobs");
                if (crons && cJSON_IsArray(crons)) {
                    int n = cJSON_GetArraySize(crons);
                    int active = 0, running = 0;
                    int count = n < OC_MAX_TASKS ? n : OC_MAX_TASKS;

                    for (int i = 0; i < count; i++) {
                        cJSON *cron = cJSON_GetArrayItem(crons, i);
                        if (!cron) continue;
                        openclaw_task_t *t = &s_oc.info.tasks[i];
                        memset(t, 0, sizeof(*t));

                        cJSON *jid = cJSON_GetObjectItem(cron, "id");
                        if (jid && jid->valuestring) strncpy(t->id, jid->valuestring, sizeof(t->id) - 1);

                        cJSON *jname = cJSON_GetObjectItem(cron, "name");
                        if (jname && jname->valuestring) strncpy(t->name, jname->valuestring, sizeof(t->name) - 1);

                        t->enabled = cJSON_IsTrue(cJSON_GetObjectItem(cron, "enabled"));
                        if (t->enabled) active++;

                        /* Schedule */
                        cJSON *sched = cJSON_GetObjectItem(cron, "schedule");
                        if (sched) {
                            cJSON *kind = cJSON_GetObjectItem(sched, "kind");
                            if (kind && kind->valuestring) {
                                strncpy(t->schedule_kind, kind->valuestring, sizeof(t->schedule_kind) - 1);
                                if (strcmp(t->schedule_kind, "every") == 0) {
                                    cJSON *every = cJSON_GetObjectItem(sched, "everyMs");
                                    if (every) snprintf(t->schedule_expr, sizeof(t->schedule_expr), "%dms", (int)every->valuedouble);
                                } else if (strcmp(t->schedule_kind, "cron") == 0) {
                                    cJSON *expr = cJSON_GetObjectItem(sched, "expr");
                                    if (expr && expr->valuestring) strncpy(t->schedule_expr, expr->valuestring, sizeof(t->schedule_expr) - 1);
                                } else if (strcmp(t->schedule_kind, "at") == 0) {
                                    cJSON *at = cJSON_GetObjectItem(sched, "at");
                                    if (at && at->valuestring) strncpy(t->schedule_expr, at->valuestring, sizeof(t->schedule_expr) - 1);
                                }
                            }
                        }

                        /* State */
                        cJSON *state = cJSON_GetObjectItem(cron, "state");
                        if (state) {
                            cJSON *runAt = cJSON_GetObjectItem(state, "runningAtMs");
                            if (runAt && cJSON_IsNumber(runAt) && runAt->valuedouble > 0) {
                                t->running = true;
                                t->running_at_ms = (int64_t)runAt->valuedouble;
                                running++;
                            }
                            cJSON *nextRun = cJSON_GetObjectItem(state, "nextRunAtMs");
                            if (nextRun && cJSON_IsNumber(nextRun)) t->next_run_at_ms = (int64_t)nextRun->valuedouble;
                            cJSON *lastRun = cJSON_GetObjectItem(state, "lastRunAtMs");
                            if (lastRun && cJSON_IsNumber(lastRun)) t->last_run_at_ms = (int64_t)lastRun->valuedouble;
                            cJSON *lastDur = cJSON_GetObjectItem(state, "lastDurationMs");
                            if (lastDur && cJSON_IsNumber(lastDur)) t->last_duration_ms = (int)lastDur->valuedouble;
                            cJSON *lastSt = cJSON_GetObjectItem(state, "lastStatus");
                            if (lastSt && lastSt->valuestring) strncpy(t->last_status, lastSt->valuestring, sizeof(t->last_status) - 1);
                            cJSON *lastErr = cJSON_GetObjectItem(state, "lastError");
                            if (lastErr && lastErr->valuestring) strncpy(t->last_error, lastErr->valuestring, sizeof(t->last_error) - 1);
                            cJSON *consErr = cJSON_GetObjectItem(state, "consecutiveErrors");
                            if (consErr && cJSON_IsNumber(consErr)) t->consecutive_errors = (int)consErr->valuedouble;
                        }
                    }

                    s_oc.info.task_count = count;
                    s_oc.info.tasks_active = active;
                    s_oc.info.tasks_running = running;
                    snprintf(s_oc.info.task_summary, sizeof(s_oc.info.task_summary),
                             "%d tasks (%d active%s)", n, active,
                             running > 0 ? ", running" : "");
                    s_oc.info.has_tasks = true;
                    ESP_LOGI(TAG, "Cron: %s", s_oc.info.task_summary);
                    goto cleanup;
                }
            }

            // Success response - if we were authenticating, we're now connected
            if (s_oc.state == OPENCLAW_STATE_AUTHENTICATING) {
                /* Extract snapshot data from hello-ok response */
                if (payload) {
                    cJSON *server = cJSON_GetObjectItem(payload, "server");
                    if (server) {
                        const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(server, "version"));
                        if (ver) strncpy(s_oc.info.version, ver, sizeof(s_oc.info.version) - 1);
                    }
                    cJSON *snapshot = cJSON_GetObjectItem(payload, "snapshot");
                    if (snapshot) {
                        cJSON *uptime = cJSON_GetObjectItem(snapshot, "uptimeMs");
                        if (uptime && cJSON_IsNumber(uptime))
                            s_oc.info.uptime_min = (uint32_t)(uptime->valuedouble / 60000.0);

                        cJSON *health = cJSON_GetObjectItem(snapshot, "health");
                        if (health) {
                            s_oc.info.ok = cJSON_IsTrue(cJSON_GetObjectItem(health, "ok"));
                            /* WhatsApp channel status */
                            cJSON *channels = cJSON_GetObjectItem(health, "channels");
                            if (channels) {
                                cJSON *wa = cJSON_GetObjectItem(channels, "whatsapp");
                                if (wa) {
                                    bool configured = cJSON_IsTrue(cJSON_GetObjectItem(wa, "configured"));
                                    bool connected = cJSON_IsTrue(cJSON_GetObjectItem(wa, "connected"));
                                    bool linked = cJSON_IsTrue(cJSON_GetObjectItem(wa, "linked"));
                                    if (connected) strncpy(s_oc.info.wa_status, "on", sizeof(s_oc.info.wa_status));
                                    else if (linked) strncpy(s_oc.info.wa_status, "linked", sizeof(s_oc.info.wa_status));
                                    else if (configured) strncpy(s_oc.info.wa_status, "off", sizeof(s_oc.info.wa_status));
                                    else strncpy(s_oc.info.wa_status, "n/a", sizeof(s_oc.info.wa_status));
                                }
                            }
                            /* Agent info */
                            cJSON *agents = cJSON_GetObjectItem(health, "agents");
                            if (agents && cJSON_IsArray(agents)) {
                                cJSON *first = cJSON_GetArrayItem(agents, 0);
                                if (first) {
                                    const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(first, "agentId"));
                                    if (aid) strncpy(s_oc.info.agent_id, aid, sizeof(s_oc.info.agent_id) - 1);
                                    cJSON *sessions = cJSON_GetObjectItem(first, "sessions");
                                    if (sessions) {
                                        cJSON *count = cJSON_GetObjectItem(sessions, "count");
                                        if (count) s_oc.info.session_count = (int)count->valuedouble;
                                        cJSON *recent = cJSON_GetObjectItem(sessions, "recent");
                                        if (recent && cJSON_IsArray(recent)) {
                                            cJSON *newest = cJSON_GetArrayItem(recent, 0);
                                            if (newest) {
                                                cJSON *age = cJSON_GetObjectItem(newest, "age");
                                                if (age) {
                                                    s_oc.info.last_activity_sec = (int)(age->valuedouble / 1000.0);
                                                    s_oc.info.last_activity_min = s_oc.info.last_activity_sec / 60;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ESP_LOGI(TAG, "Snapshot: v%s up=%lum wa=%s agent=%s sess=%d last=%dm",
                             s_oc.info.version, (unsigned long)s_oc.info.uptime_min,
                             s_oc.info.wa_status, s_oc.info.agent_id,
                             s_oc.info.session_count, s_oc.info.last_activity_min);
                }
                set_state(OPENCLAW_STATE_CONNECTED);
                /* Auth complete — relax WS ping from 1s to 30s to save power.
                 * The 1s ping was only needed to flush challenge data during handshake. */
                esp_websocket_client_set_ping_interval_sec(s_oc.ws, 30);
                ESP_LOGI(TAG, "Connected to OpenClaw gateway");
            }
        }
    }

cleanup:
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *)data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_oc.msg_id = 0;  /* Reset message IDs for clean session */
        /* Free any stale fragmentation buffer from previous connection */
        reset_fragment_buffer();
        /* Reset ping to 1s so challenge data is flushed before server's 10s handshake timeout.
         * This is critical on reconnect — after auth the ping was relaxed to 30s. */
        esp_websocket_client_set_ping_interval_sec(s_oc.ws, 1);
        set_state(OPENCLAW_STATE_CONNECTING);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (state was %d)", s_oc.state);
        reset_fragment_buffer();
        set_state(OPENCLAW_STATE_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ws_data->op_code == 0x08) {
            /* Close frame — log the close code */
            uint16_t close_code = 0;
            if (ws_data->data_len >= 2 && ws_data->data_ptr) {
                close_code = ((uint8_t)ws_data->data_ptr[0] << 8) | (uint8_t)ws_data->data_ptr[1];
            }
            ESP_LOGW(TAG, "WS CLOSE frame: code=%d", close_code);
            set_state(OPENCLAW_STATE_DISCONNECTED);
            break;
        }
        if (ws_data->op_code == 0x0a) {
            /* Pong — skip verbose logging */
            break;
        }
        ESP_LOGD(TAG, "WS DATA: op=0x%02x plen=%d dlen=%d off=%d",
                 ws_data->op_code, ws_data->payload_len, ws_data->data_len, ws_data->payload_offset);
        if (ws_data->op_code == 0x01 || ws_data->op_code == 0x00) {  // Text or continuation
            bool fragmented = (ws_data->payload_offset > 0) ||
                              (ws_data->payload_len > ws_data->data_len) ||
                              (s_oc.frag_buf != NULL);
            if (fragmented) {
                ESP_LOGD(TAG, "WS FRAG: off=%d len=%d total=%d", ws_data->payload_offset, ws_data->data_len, ws_data->payload_len);
                if (ws_data->payload_offset == 0) {
                    /* First fragment: allocate reassembly buffer from PSRAM */
                    reset_fragment_buffer();
                    s_oc.frag_buf = heap_caps_malloc(ws_data->payload_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!s_oc.frag_buf) {
                        ESP_LOGE(TAG, "Failed to allocate %d-byte WebSocket fragment buffer", ws_data->payload_len + 1);
                        break;
                    }
                    s_oc.frag_total = ws_data->payload_len;
                    s_oc.frag_len = 0;
                }
                if (!s_oc.frag_buf) {
                    ESP_LOGW(TAG, "Ignoring continuation frame without fragment buffer (off=%d len=%d total=%d)",
                             ws_data->payload_offset, ws_data->data_len, ws_data->payload_len);
                    break;
                }
                if (s_oc.frag_len + ws_data->data_len > s_oc.frag_total) {
                    ESP_LOGW(TAG, "Dropping oversized fragmented message (have=%u add=%d total=%u)",
                             (unsigned)s_oc.frag_len, ws_data->data_len, (unsigned)s_oc.frag_total);
                    reset_fragment_buffer();
                    break;
                }
                memcpy(s_oc.frag_buf + s_oc.frag_len, ws_data->data_ptr, ws_data->data_len);
                s_oc.frag_len += ws_data->data_len;
                if (s_oc.frag_len >= s_oc.frag_total) {
                    s_oc.frag_buf[s_oc.frag_len] = '\0';
                    handle_message(s_oc.frag_buf, (int)s_oc.frag_len);
                    reset_fragment_buffer();
                }
            } else {
                handle_message((const char *)ws_data->data_ptr, ws_data->data_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        reset_fragment_buffer();
        set_state(OPENCLAW_STATE_ERROR);
        break;

    default:
        break;
    }
}

esp_err_t openclaw_init(const openclaw_config_t *config, openclaw_state_cb_t state_cb)
{
    memset(&s_oc, 0, sizeof(s_oc));
    s_oc.state_cb = state_cb;
    strncpy(s_oc.host, config->host, sizeof(s_oc.host) - 1);
    s_oc.port = config->port;
    if (config->token) {
        strncpy(s_oc.token, config->token, sizeof(s_oc.token) - 1);
    }
    if (config->session_key && config->session_key[0]) {
        strncpy(s_oc.session_key, config->session_key, sizeof(s_oc.session_key) - 1);
    } else {
        strncpy(s_oc.session_key, "default", sizeof(s_oc.session_key) - 1);
    }

    // Initialize ED25519 device identity from hex seed
    if (config->device_key_hex && strlen(config->device_key_hex) == 64) {
        hex_to_bytes(config->device_key_hex, s_oc.ed_seed, 32);
        ed25519_create_keypair(s_oc.ed_pubkey, s_oc.ed_privkey, s_oc.ed_seed);

        // Device ID = SHA-256(raw_public_key).hex()
        uint8_t hash[32];
        mbedtls_sha256(s_oc.ed_pubkey, 32, hash, 0);
        for (int i = 0; i < 32; i++) {
            sprintf(s_oc.device_id + i*2, "%02x", hash[i]);
        }
        s_oc.has_device_key = true;
        ESP_LOGI(TAG, "Device identity: %s", s_oc.device_id);
    }

    ESP_LOGI(TAG, "OpenClaw client initialized (host=%s, port=%d)", s_oc.host, s_oc.port);
    return ESP_OK;
}

void openclaw_set_notify_cb(openclaw_notify_cb_t cb)
{
    s_oc.notify_cb = cb;
}

esp_err_t openclaw_connect(void)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d", s_oc.host, s_oc.port);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 16384,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 500,
        .task_stack = 8192,
        .ping_interval_sec = 1,      // Must be low: lib skips reads until PING triggers PONG,
                                      // which flushes challenge data buffered during WS upgrade.
                                      // Default 10s causes handshake timeout (server=10s).
    };

    s_oc.ws = esp_websocket_client_init(&ws_cfg);
    if (!s_oc.ws) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_oc.ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    set_state(OPENCLAW_STATE_CONNECTING);
    ESP_LOGI(TAG, "Connecting to %s", uri);
    return esp_websocket_client_start(s_oc.ws);
}

esp_err_t openclaw_disconnect(void)
{
    if (s_oc.ws) {
        esp_websocket_client_stop(s_oc.ws);
        esp_websocket_client_destroy(s_oc.ws);
        s_oc.ws = NULL;
    }
    set_state(OPENCLAW_STATE_DISCONNECTED);
    return ESP_OK;
}

openclaw_state_t openclaw_get_state(void)
{
    return s_oc.state;
}

esp_err_t openclaw_chat_send(const char *message, openclaw_chat_cb_t response_cb)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected to OpenClaw (state=%d)", s_oc.state);
        return ESP_ERR_INVALID_STATE;
    }

    s_oc.chat_cb = response_cb;
    s_oc.response_buf[0] = '\0';
    s_oc.response_len = 0;
    s_oc.chat_start_time = esp_timer_get_time();

    /* Prepend dual-response instruction: first line = short label, rest = spoken response.
     * Last line must be [LISTEN] if you expect user to reply, or [END] if conversation is done. */
    static const char PREFIX[] =
        "[Respond in TWO parts on separate lines:\n"
        "Line 1: A very brief label (1-5 words max, e.g. \"Done\", \"Yes, on it\", \"Sure thing\")\n"
        "Line 2+: A natural spoken response (1-3 sentences, conversational tone)\n"
        "LAST LINE: Write exactly [LISTEN] if you expect the user to respond (e.g. you asked a question, "
        "need clarification, or the conversation naturally continues), "
        "or [END] if no further reply is expected.\n"
        "DEVICE COMMANDS: If the user asks to change a device setting, include the appropriate "
        "command tag(s) BEFORE the last line. Available commands:\n"
        "[DEVICE:volume=0-100] - speaker volume\n"
        "[DEVICE:brightness=0-100] - screen brightness\n"
        "[DEVICE:rgb=rainbow|aurora|starfield|fire|ocean|off|on|R,G,B] - LED effect\n"
        "[DEVICE:sleep=MINUTES] - idle sleep timeout (0=never)\n"
        "[DEVICE:webserver=on|off] - web config panel\n"
        "[DEVICE:auto_read=on|off] - auto read responses aloud\n"
        "[DEVICE:reboot] - restart device\n"
        "Example:\nVolume up\nI've turned the volume up to 80 percent.\n[DEVICE:volume=80]\n[END]\n"
        "Example 2:\nSure thing\nI've set that up for you. Want me to change anything else?\n[LISTEN]]\n\n";

    size_t prefix_len = sizeof(PREFIX) - 1;
    size_t msg_len = strlen(message);
    char *full_msg = malloc(prefix_len + msg_len + 1);
    if (!full_msg) return ESP_ERR_NO_MEM;
    memcpy(full_msg, PREFIX, prefix_len);
    memcpy(full_msg + prefix_len, message, msg_len + 1);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "chat.send");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", s_oc.session_key);
    cJSON_AddStringToObject(params, "message", full_msg);

    // Generate unique idempotency key
    char idem_key[32];
    snprintf(idem_key, sizeof(idem_key), "hc-%" PRIu64 "-%" PRIu32, (uint64_t)(esp_timer_get_time() / 1000), s_oc.msg_id);
    cJSON_AddStringToObject(params, "idempotencyKey", idem_key);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        set_state(OPENCLAW_STATE_CHAT_SENDING);
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(5000));
        set_state(OPENCLAW_STATE_CHAT_THINKING);
        ESP_LOGI(TAG, "Chat sent: %.80s%s", message, msg_len > 80 ? "..." : "");
        free(json_str);
    }
    cJSON_Delete(root);
    free(full_msg);

    return ESP_OK;
}

/* Send a detail request — asks OpenClaw for full elaboration of last response */
esp_err_t openclaw_chat_send_details(openclaw_chat_cb_t response_cb)
{
    return openclaw_chat_send(
        "Please provide the full detailed response to your last answer. Be thorough.",
        response_cb);
}

/* ─── WAV header + Base64 helpers for audio sending ─── */

static size_t base64_encode(char *out, size_t out_max, const uint8_t *in, size_t in_len);

/* Send text + JPEG image to OpenClaw */
esp_err_t openclaw_chat_send_with_image(const char *message,
                                         const uint8_t *jpeg, size_t jpeg_size,
                                         openclaw_chat_cb_t response_cb)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected (state=%d)", s_oc.state);
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg || jpeg_size == 0) {
        return openclaw_chat_send(message, response_cb);
    }

    s_oc.chat_cb = response_cb;
    s_oc.response_buf[0] = '\0';
    s_oc.response_len = 0;
    s_oc.chat_start_time = esp_timer_get_time();

    /* Base64 encode image */
    static const char img_prefix[] = "data:image/jpeg;base64,";
    size_t b64_len = ((jpeg_size + 2) / 3) * 4;
    size_t content_len = sizeof(img_prefix) - 1 + b64_len;
    char *b64_image = heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64_image) return ESP_ERR_NO_MEM;
    memcpy(b64_image, img_prefix, sizeof(img_prefix) - 1);
    base64_encode(b64_image + sizeof(img_prefix) - 1, b64_len + 1, jpeg, jpeg_size);
    ESP_LOGI(TAG, "Image base64: %u chars (%u bytes JPEG)", (unsigned)content_len, (unsigned)jpeg_size);

    /* Build message with prefix */
    static const char PREFIX[] =
        "[Respond in TWO parts on separate lines:\n"
        "Line 1: A very brief label (1-5 words max)\n"
        "Line 2+: A natural spoken response (1-3 sentences). The user attached an image from their device camera.\n"
        "LAST LINE: Write exactly [LISTEN] if you expect the user to respond, "
        "or [END] if no further reply is expected.\n"
        "If the user asks to change a device setting, also include command tags: "
        "[DEVICE:volume=0-100] [DEVICE:brightness=0-100] [DEVICE:rgb=rainbow|aurora|starfield|fire|ocean|off] "
        "[DEVICE:sleep=MINUTES] [DEVICE:webserver=on|off] [DEVICE:reboot]]\n\n";
    size_t prefix_len = sizeof(PREFIX) - 1;
    size_t msg_len = strlen(message);
    char *full_msg = malloc(prefix_len + msg_len + 1);
    if (!full_msg) { free(b64_image); return ESP_ERR_NO_MEM; }
    memcpy(full_msg, PREFIX, prefix_len);
    memcpy(full_msg + prefix_len, message, msg_len + 1);

    /* Build JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "chat.send");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", s_oc.session_key);
    cJSON_AddStringToObject(params, "message", full_msg);

    char idem_key[32];
    snprintf(idem_key, sizeof(idem_key), "hc-%" PRIu64 "-%" PRIu32,
             (uint64_t)(esp_timer_get_time() / 1000), s_oc.msg_id);
    cJSON_AddStringToObject(params, "idempotencyKey", idem_key);

    /* Add image attachment */
    cJSON *attachments = cJSON_AddArrayToObject(params, "attachments");
    cJSON *img_att = cJSON_CreateObject();
    cJSON_AddStringToObject(img_att, "type", "image");
    cJSON_AddStringToObject(img_att, "mimeType", "image/jpeg");
    cJSON_AddStringToObject(img_att, "fileName", "camera.jpg");
    cJSON_AddStringToObject(img_att, "content", b64_image);
    cJSON_AddItemToArray(attachments, img_att);

    char *json_str = cJSON_PrintUnformatted(root);
    free(b64_image);
    free(full_msg);

    if (json_str) {
        int jlen = strlen(json_str);
        set_state(OPENCLAW_STATE_CHAT_SENDING);
        ESP_LOGI(TAG, "Sending text+image chat (%d bytes JSON)", jlen);
        esp_websocket_client_send_text(s_oc.ws, json_str, jlen, pdMS_TO_TICKS(30000));
        set_state(OPENCLAW_STATE_CHAT_THINKING);
        free(json_str);
    }
    cJSON_Delete(root);

    return ESP_OK;
}

/* ─── WAV header + Base64 helpers for audio sending ─── */

static const char b64_std_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(char *out, size_t out_max, const uint8_t *in, size_t in_len)
{
    size_t oi = 0, i = 0;
    while (i < in_len && oi + 4 < out_max) {
        uint32_t a = in[i++];
        uint32_t b = (i < in_len) ? in[i++] : 0;
        uint32_t c = (i < in_len) ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[oi++] = b64_std_table[(triple >> 18) & 0x3F];
        out[oi++] = b64_std_table[(triple >> 12) & 0x3F];
        out[oi++] = (i > in_len + 1) ? '=' : b64_std_table[(triple >> 6) & 0x3F];
        out[oi++] = (i > in_len)     ? '=' : b64_std_table[triple & 0x3F];
    }
    if (oi < out_max) out[oi] = '\0';
    return oi;
}

/* Build a WAV file in memory from 16-bit mono PCM. Returns malloc'd buffer. */
static uint8_t *build_wav(const int16_t *pcm, size_t num_samples, int sample_rate, size_t *out_len)
{
    uint32_t data_size = (uint32_t)(num_samples * 2);
    uint32_t file_size = 44 + data_size;
    uint8_t *buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;

    /* RIFF header */
    memcpy(buf, "RIFF", 4);
    uint32_t chunk_size = file_size - 8;
    memcpy(buf + 4, &chunk_size, 4);
    memcpy(buf + 8, "WAVE", 4);

    /* fmt sub-chunk */
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1; /* PCM */
    memcpy(buf + 20, &audio_fmt, 2);
    uint16_t channels = 1;
    memcpy(buf + 22, &channels, 2);
    uint32_t sr = (uint32_t)sample_rate;
    memcpy(buf + 24, &sr, 4);
    uint32_t byte_rate = sr * 2;
    memcpy(buf + 28, &byte_rate, 4);
    uint16_t block_align = 2;
    memcpy(buf + 32, &block_align, 2);
    uint16_t bits = 16;
    memcpy(buf + 34, &bits, 2);

    /* data sub-chunk */
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &data_size, 4);
    memcpy(buf + 44, pcm, data_size);

    *out_len = file_size;
    return buf;
}

esp_err_t openclaw_chat_send_audio(const int16_t *pcm, size_t num_samples,
                                    int sample_rate, const char *text,
                                    openclaw_chat_cb_t response_cb)
{
    return openclaw_chat_send_audio_with_image(pcm, num_samples, sample_rate, text,
                                                NULL, 0, response_cb);
}

esp_err_t openclaw_chat_send_audio_with_image(const int16_t *pcm, size_t num_samples,
                                               int sample_rate, const char *text,
                                               const uint8_t *jpeg, size_t jpeg_size,
                                               openclaw_chat_cb_t response_cb)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected (state=%d)", s_oc.state);
        return ESP_ERR_INVALID_STATE;
    }

    s_oc.chat_cb = response_cb;
    s_oc.response_buf[0] = '\0';
    s_oc.response_len = 0;
    s_oc.chat_start_time = esp_timer_get_time();

    /* Build WAV in PSRAM */
    size_t wav_len = 0;
    uint8_t *wav = build_wav(pcm, num_samples, sample_rate, &wav_len);
    if (!wav) {
        ESP_LOGE(TAG, "WAV build failed (OOM)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WAV built: %u bytes (%u samples @ %dHz)", (unsigned)wav_len, (unsigned)num_samples, sample_rate);

    /* Base64 encode audio */
    static const char audio_prefix[] = "data:audio/wav;base64,";
    size_t b64_len = ((wav_len + 2) / 3) * 4;
    size_t content_len = sizeof(audio_prefix) - 1 + b64_len;
    char *b64_audio = heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64_audio) {
        free(wav);
        return ESP_ERR_NO_MEM;
    }
    memcpy(b64_audio, audio_prefix, sizeof(audio_prefix) - 1);
    base64_encode(b64_audio + sizeof(audio_prefix) - 1, b64_len + 1, wav, wav_len);
    free(wav);

    ESP_LOGI(TAG, "Audio base64: %u chars", (unsigned)content_len);

    /* Base64 encode image if provided */
    char *b64_image = NULL;
    if (jpeg && jpeg_size > 0) {
        static const char img_prefix[] = "data:image/jpeg;base64,";
        size_t img_b64_len = ((jpeg_size + 2) / 3) * 4;
        size_t img_content_len = sizeof(img_prefix) - 1 + img_b64_len;
        b64_image = heap_caps_malloc(img_content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (b64_image) {
            memcpy(b64_image, img_prefix, sizeof(img_prefix) - 1);
            base64_encode(b64_image + sizeof(img_prefix) - 1, img_b64_len + 1, jpeg, jpeg_size);
            ESP_LOGI(TAG, "Image base64: %u chars (%u bytes JPEG)", (unsigned)img_content_len, (unsigned)jpeg_size);
        } else {
            ESP_LOGW(TAG, "Image base64 OOM — sending audio only");
        }
    }

    /* Build message with short-response prefix */
    static const char PREFIX[] =
        "[You are responding to a tiny wearable screen. "
        "Answer in 1-3 words ONLY (e.g. OK, Done, Yes, No, Problem, Working on it). "
        "If you need to explain more, the user will ask for details.\n"
        "If the user asks to change a device setting, include command tags: "
        "[DEVICE:volume=0-100] [DEVICE:brightness=0-100] [DEVICE:rgb=rainbow|off] "
        "[DEVICE:sleep=MINUTES] [DEVICE:reboot]]\n\n";
    const char *msg_text = (text && text[0]) ? text : "Voice message";
    size_t full_len = sizeof(PREFIX) - 1 + strlen(msg_text);
    char *full_msg = malloc(full_len + 1);
    if (!full_msg) { free(b64_audio); free(b64_image); return ESP_ERR_NO_MEM; }
    memcpy(full_msg, PREFIX, sizeof(PREFIX) - 1);
    strcpy(full_msg + sizeof(PREFIX) - 1, msg_text);

    /* Build JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "chat.send");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", s_oc.session_key);
    cJSON_AddStringToObject(params, "message", full_msg);

    char idem_key[32];
    snprintf(idem_key, sizeof(idem_key), "hc-%" PRIu64 "-%" PRIu32,
             (uint64_t)(esp_timer_get_time() / 1000), s_oc.msg_id);
    cJSON_AddStringToObject(params, "idempotencyKey", idem_key);

    /* Add attachments */
    cJSON *attachments = cJSON_AddArrayToObject(params, "attachments");

    cJSON *audio_att = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_att, "type", "audio");
    cJSON_AddStringToObject(audio_att, "mimeType", "audio/wav");
    cJSON_AddStringToObject(audio_att, "fileName", "voice.wav");
    cJSON_AddStringToObject(audio_att, "content", b64_audio);
    cJSON_AddItemToArray(attachments, audio_att);

    if (b64_image) {
        cJSON *img_att = cJSON_CreateObject();
        cJSON_AddStringToObject(img_att, "type", "image");
        cJSON_AddStringToObject(img_att, "mimeType", "image/jpeg");
        cJSON_AddStringToObject(img_att, "fileName", "camera.jpg");
        cJSON_AddStringToObject(img_att, "content", b64_image);
        cJSON_AddItemToArray(attachments, img_att);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    free(b64_audio);
    free(b64_image);
    free(full_msg);

    if (json_str) {
        int jlen = strlen(json_str);
        set_state(OPENCLAW_STATE_CHAT_SENDING);
        ESP_LOGI(TAG, "Sending audio%s chat (%d bytes JSON, %u samples)",
                 b64_image ? "+image" : "", jlen, (unsigned)num_samples);
        esp_websocket_client_send_text(s_oc.ws, json_str, jlen, pdMS_TO_TICKS(30000));
        set_state(OPENCLAW_STATE_CHAT_THINKING);
        free(json_str);
    }
    cJSON_Delete(root);

    return ESP_OK;
}

/* Request usage.status from OpenClaw (cost data) */
esp_err_t openclaw_request_usage(void)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "usage.status");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    (void)params;  /* empty params */

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "Requesting usage.status (id=%s)", cJSON_GetStringValue(cJSON_GetObjectItem(root, "id")));
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t openclaw_request_health(void)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "health");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    (void)params;

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGD(TAG, "Requesting health");
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

const char *openclaw_get_last_response(void)
{
    return s_oc.response_buf;
}

uint32_t openclaw_get_thinking_time_ms(void)
{
    if (s_oc.chat_start_time == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_oc.chat_start_time) / 1000);
}

esp_err_t openclaw_request_tasks(void)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "cron.list");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddBoolToObject(params, "includeDisabled", true);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "Requesting cron.list");
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

const openclaw_info_t *openclaw_get_info(void)
{
    return &s_oc.info;
}

bool openclaw_consume_external_activity(void)
{
    if (s_oc.external_activity_detected) {
        s_oc.external_activity_detected = false;
        return true;
    }
    return false;
}

/* ── Cron CRUD operations ─────────────────────────────────────────────── */

esp_err_t openclaw_cron_add(const char *name, const char *schedule_expr,
                             const char *payload_text)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) return ESP_ERR_INVALID_STATE;
    if (!name || !schedule_expr || !payload_text) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "cron.add");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "name", name);
    cJSON_AddBoolToObject(params, "enabled", true);

    /* Schedule: "every" with ms value, or "cron" expression */
    cJSON *sched = cJSON_AddObjectToObject(params, "schedule");
    /* Try to parse as integer (everyMs) */
    char *endp;
    long ms = strtol(schedule_expr, &endp, 10);
    if (*endp == '\0' && ms > 0) {
        cJSON_AddStringToObject(sched, "kind", "every");
        cJSON_AddNumberToObject(sched, "everyMs", ms);
    } else {
        cJSON_AddStringToObject(sched, "kind", "cron");
        cJSON_AddStringToObject(sched, "expr", schedule_expr);
    }

    cJSON_AddStringToObject(params, "sessionTarget", "isolated");
    cJSON_AddStringToObject(params, "wakeMode", "now");

    /* Payload: agentTurn with message (isolated) */
    cJSON *pl = cJSON_AddObjectToObject(params, "payload");
    cJSON_AddStringToObject(pl, "kind", "agentTurn");
    cJSON_AddStringToObject(pl, "message", payload_text);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "cron.add: %s (schedule=%s)", name, schedule_expr);
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t openclaw_cron_remove(const char *job_id)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) return ESP_ERR_INVALID_STATE;
    if (!job_id) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "cron.remove");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "id", job_id);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "cron.remove: %s", job_id);
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t openclaw_cron_run(const char *job_id)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) return ESP_ERR_INVALID_STATE;
    if (!job_id) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "cron.run");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "id", job_id);
    cJSON_AddStringToObject(params, "mode", "force");

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "cron.run: %s", job_id);
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t openclaw_chat_abort(void)
{
    if (s_oc.state < OPENCLAW_STATE_CONNECTED || !s_oc.ws) return ESP_ERR_INVALID_STATE;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "chat.abort");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", "main");

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "chat.abort sent");
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);

    /* Reset state */
    s_oc.chat_cb = NULL;  /* Prevent late responses from triggering callback */
    set_state(OPENCLAW_STATE_CONNECTED);
    return ESP_OK;
}

esp_err_t openclaw_chat_abort_session(const char *session_key)
{
    if (s_oc.state < OPENCLAW_STATE_CONNECTED || !s_oc.ws) return ESP_ERR_INVALID_STATE;
    if (!session_key || session_key[0] == '\0') return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "chat.abort");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", session_key);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "chat.abort sent for session=%s", session_key);
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);

    /* Clear activity state */
    s_oc.info.is_active = false;
    s_oc.info.is_external = false;
    s_oc.info.active_started_ms = 0;
    s_oc.info.active_detail[0] = '\0';
    s_oc.info.active_session_key[0] = '\0';
    s_oc.info.active_run_id[0] = '\0';
    return ESP_OK;
}

esp_err_t openclaw_cron_toggle(const char *job_id, bool enabled)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED || !s_oc.ws) return ESP_ERR_INVALID_STATE;
    if (!job_id) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "cron.update");
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "id", job_id);
    cJSON *patch = cJSON_AddObjectToObject(params, "patch");
    cJSON_AddBoolToObject(patch, "enabled", enabled);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "cron.update: %s enabled=%d", job_id, enabled);
        esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(2000));
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}
