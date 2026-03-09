/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * Camera module — SSCMA client (Himax HX6538) over SPI
 * Uses SPI2 bus shared with SD card. IO expander for reset.
 */

#include "camera.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "camera";

#if !BOARD_HAS_CAMERA
/* Stubs for boards without camera */
esp_err_t camera_init(void) { ESP_LOGI(TAG, "No camera on this board"); return ESP_OK; }
esp_err_t camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_size) { (void)jpeg_out; (void)jpeg_size; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_check_presence(bool *present, int *best_score) { if (present) *present = false; if (best_score) *best_score = 0; return ESP_ERR_NOT_SUPPORTED; }
bool camera_is_ready(void) { return false; }
void camera_deinit(void) {}
#else /* BOARD_HAS_CAMERA */

#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "sscma_client_io.h"
#include "sscma_client_ops.h"

#include <string.h>
#include "mbedtls/base64.h"

static sscma_client_io_handle_t s_sscma_io = NULL;
static sscma_client_handle_t s_sscma_client = NULL;
static bool s_initialized = false;

// Captured image buffer (PSRAM)
static uint8_t *s_captured_jpeg = NULL;
static size_t s_captured_size = 0;
static SemaphoreHandle_t s_capture_sem = NULL;
static SemaphoreHandle_t s_presence_sem = NULL;
static SemaphoreHandle_t s_camera_mutex = NULL;
static bool s_presence_detected = false;
static int s_presence_score = 0;

static bool has_presence_labels(void)
{
    if (!s_sscma_client) return false;

    for (int i = 0; i < SSCMA_CLIENT_MODEL_MAX_CLASSES; i++) {
        const char *label = s_sscma_client->model.classes[i];
        if (!label || !label[0]) continue;
        if (strstr(label, "face") != NULL || strstr(label, "person") != NULL) {
            return true;
        }
    }

    return false;
}

static bool is_presence_target(uint8_t target)
{
    if (!s_sscma_client) return false;

    const char *label = NULL;
    if (target < SSCMA_CLIENT_MODEL_MAX_CLASSES) {
        label = s_sscma_client->model.classes[target];
    }

    if (!label || !label[0]) {
        return false;
    }

    return strstr(label, "face") != NULL || strstr(label, "person") != NULL;
}

static bool box_looks_like_presence(const sscma_client_box_t *box, bool require_labeled_target)
{
    if (!box) return false;

    if (require_labeled_target && !is_presence_target(box->target)) {
        return false;
    }

    uint32_t area = (uint32_t)box->w * (uint32_t)box->h;

    /* Unlabeled models need stronger guards to avoid false wakeups. */
    if (!require_labeled_target) {
        if (box->score < 63) return false;
        if (area < 1800) return false;
        return true;
    }

    if (box->score < 75) return false;
    if (area < 900) return false;
    return true;
}

// Callback for SSCMA events — receives image data
static void sscma_on_event(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    if (reply == NULL) return;

    char *image_data = NULL;
    int image_size = 0;

    esp_err_t ret = sscma_utils_fetch_image_from_reply(reply, &image_data, &image_size);
    if (ret == ESP_OK && image_data != NULL && image_size > 0) {
        // SSCMA returns images as base64 strings — decode to raw JPEG
        size_t decoded_len = 0;
        int rc = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)image_data, image_size);
        if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_len == 0) {
            ESP_LOGE(TAG, "Base64 decode failed (len=%d, rc=%d)", image_size, rc);
            free(image_data);
            return;
        }

        // Free previous capture
        if (s_captured_jpeg) {
            free(s_captured_jpeg);
            s_captured_jpeg = NULL;
        }

        // Allocate and decode to PSRAM
        s_captured_jpeg = heap_caps_malloc(decoded_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_captured_jpeg) {
            size_t actual_len = 0;
            mbedtls_base64_decode(s_captured_jpeg, decoded_len, &actual_len,
                                  (const unsigned char *)image_data, image_size);
            s_captured_size = actual_len;

            // Verify JPEG header (FF D8 FF)
            if (actual_len >= 3 && s_captured_jpeg[0] == 0xFF &&
                s_captured_jpeg[1] == 0xD8 && s_captured_jpeg[2] == 0xFF) {
                ESP_LOGI(TAG, "Captured JPEG: %d bytes (decoded from %d b64 chars)", (int)actual_len, image_size);
            } else {
                ESP_LOGW(TAG, "Captured %d bytes but NOT valid JPEG (header: %02X %02X %02X)",
                         (int)actual_len,
                         actual_len > 0 ? s_captured_jpeg[0] : 0,
                         actual_len > 1 ? s_captured_jpeg[1] : 0,
                         actual_len > 2 ? s_captured_jpeg[2] : 0);
            }
        } else {
            ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for JPEG", (int)decoded_len);
            s_captured_size = 0;
        }

        free(image_data);

        // Signal capture complete
        if (s_capture_sem) {
            xSemaphoreGive(s_capture_sem);
        }
    }

    sscma_client_box_t *boxes = NULL;
    int num_boxes = 0;
    ret = sscma_utils_fetch_boxes_from_reply(reply, &boxes, &num_boxes);
    if (ret == ESP_OK) {
        bool present = false;
        int best_score = 0;
        bool require_labeled_target = has_presence_labels();

        if (boxes != NULL && num_boxes > 0) {
            for (int i = 0; i < num_boxes; i++) {
                uint32_t area = (uint32_t)boxes[i].w * (uint32_t)boxes[i].h;
                ESP_LOGI(TAG, "Presence box[%d]: score=%u target=%u x=%u y=%u w=%u h=%u area=%lu labeled=%d",
                         i, boxes[i].score, boxes[i].target, boxes[i].x, boxes[i].y,
                         boxes[i].w, boxes[i].h, (unsigned long)area, require_labeled_target ? 1 : 0);
                if (boxes[i].score > best_score) best_score = boxes[i].score;
                if (box_looks_like_presence(&boxes[i], require_labeled_target)) {
                    present = true;
                }
            }
            free(boxes);
        } else {
            ESP_LOGI(TAG, "Presence invoke returned no boxes");
        }

        s_presence_detected = present;
        s_presence_score = best_score;

        if (s_presence_sem) {
            xSemaphoreGive(s_presence_sem);
        }
    }
}

static void sscma_on_log(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    if (reply && reply->data) {
        ESP_LOGD(TAG, "SSCMA: %s", reply->data);
    }
}

esp_err_t camera_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_io_expander_handle_t io_exp = (esp_io_expander_handle_t)board_get_io_expander();
    if (io_exp == NULL) {
        ESP_LOGE(TAG, "IO expander not available — camera cannot init");
        return ESP_ERR_INVALID_STATE;
    }

    // Initialize SPI2 bus (shared with SD card)
    esp_err_t ret = board_spi2_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create SSCMA SPI IO
    const sscma_client_io_spi_config_t spi_io_config = {
        .sync_gpio_num = BOARD_IOEXP_SSCMA_SYNC,
        .cs_gpio_num = BOARD_SSCMA_SPI_CS,
        .pclk_hz = BOARD_SSCMA_SPI_CLK_HZ,
        .spi_mode = 0,
        .wait_delay = 2,
        .user_ctx = NULL,
        .io_expander = io_exp,
        .flags.sync_use_expander = true,
    };

    ret = sscma_client_new_io_spi_bus((sscma_client_spi_bus_handle_t)BOARD_SSCMA_SPI_HOST, &spi_io_config, &s_sscma_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSCMA SPI IO create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create SSCMA client
    sscma_client_config_t config = SSCMA_CLIENT_CONFIG_DEFAULT();
    config.reset_gpio_num = BOARD_IOEXP_SSCMA_RST;
    config.io_expander = io_exp;
    config.flags.reset_use_expander = true;
    config.tx_buffer_size = 4096;
    config.rx_buffer_size = 65536;  // 64KB for image data
    config.process_task_stack = 4096;
    config.monitor_task_stack = 8192;
    config.event_queue_size = 2;

    ret = sscma_client_new(s_sscma_io, &config, &s_sscma_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSCMA client create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register callbacks
    const sscma_client_callback_t callbacks = {
        .on_event = sscma_on_event,
        .on_log = sscma_on_log,
    };
    sscma_client_register_callback(s_sscma_client, &callbacks, NULL);

    // Initialize (resets AI chip, waits for ready)
    ret = sscma_client_init(s_sscma_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSCMA client init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create capture semaphore
    s_capture_sem = xSemaphoreCreateBinary();
    s_presence_sem = xSemaphoreCreateBinary();
    s_camera_mutex = xSemaphoreCreateMutex();

    // Log AI chip info
    sscma_client_info_t *info = NULL;
    if (sscma_client_get_info(s_sscma_client, &info, false) == ESP_OK && info) {
        ESP_LOGI(TAG, "AI Camera: ID=%s, FW=%s", info->id ? info->id : "?", info->fw_ver ? info->fw_ver : "?");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Camera initialized (SSCMA over SPI2)");
    return ESP_OK;
}

esp_err_t camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_size)
{
    if (!s_initialized || !s_sscma_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg_out || !jpeg_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_camera_mutex || xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Clear previous capture
    if (s_captured_jpeg) {
        free(s_captured_jpeg);
        s_captured_jpeg = NULL;
        s_captured_size = 0;
    }

    // Reset semaphore
    xSemaphoreTake(s_capture_sem, 0);

    // Request single sample (capture one frame)
    esp_err_t ret = sscma_client_sample(s_sscma_client, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSCMA sample request failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_camera_mutex);
        return ret;
    }

    // Wait for callback with image data (timeout 5s)
    if (xSemaphoreTake(s_capture_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Camera capture timeout");
        xSemaphoreGive(s_camera_mutex);
        return ESP_ERR_TIMEOUT;
    }

    if (s_captured_jpeg == NULL || s_captured_size == 0) {
        ESP_LOGE(TAG, "No image data received");
        xSemaphoreGive(s_camera_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // Transfer ownership to caller
    *jpeg_out = s_captured_jpeg;
    *jpeg_size = s_captured_size;
    s_captured_jpeg = NULL;
    s_captured_size = 0;

    ESP_LOGI(TAG, "Capture complete: %d bytes JPEG", (int)*jpeg_size);
    xSemaphoreGive(s_camera_mutex);
    return ESP_OK;
}

esp_err_t camera_check_presence(bool *present, int *best_score)
{
    if (present) *present = false;
    if (best_score) *best_score = 0;

    if (!s_initialized || !s_sscma_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_camera_mutex || !s_presence_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_presence_detected = false;
    s_presence_score = 0;
    xSemaphoreTake(s_presence_sem, 0);

    esp_err_t ret = sscma_client_invoke(s_sscma_client, 1, false, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Presence invoke failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_camera_mutex);
        return ret;
    }

    if (xSemaphoreTake(s_presence_sem, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "Presence invoke timed out");
        xSemaphoreGive(s_camera_mutex);
        return ESP_ERR_TIMEOUT;
    }

    if (present) *present = s_presence_detected;
    if (best_score) *best_score = s_presence_score;
    xSemaphoreGive(s_camera_mutex);
    return ESP_OK;
}

bool camera_is_ready(void)
{
    return s_initialized;
}

void camera_deinit(void)
{
    if (s_sscma_client) {
        sscma_client_del(s_sscma_client);
        s_sscma_client = NULL;
    }
    if (s_capture_sem) {
        vSemaphoreDelete(s_capture_sem);
        s_capture_sem = NULL;
    }
    if (s_presence_sem) {
        vSemaphoreDelete(s_presence_sem);
        s_presence_sem = NULL;
    }
    if (s_camera_mutex) {
        vSemaphoreDelete(s_camera_mutex);
        s_camera_mutex = NULL;
    }
    if (s_captured_jpeg) {
        free(s_captured_jpeg);
        s_captured_jpeg = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "Camera deinitialized");
}

#endif /* BOARD_HAS_CAMERA */
