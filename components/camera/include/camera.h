/*
 * SPDX-FileCopyrightText: 2024-2026 HeyClawy Contributors
 * SPDX-License-Identifier: MIT
 *
 * Camera abstraction — captures JPEG images via SSCMA (Himax HX6538)
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize camera (SSCMA client over UART + IO expander reset)
 * Must be called after board_init() since it needs the IO expander.
 * @return ESP_OK on success
 */
esp_err_t camera_init(void);

/**
 * @brief Capture a single JPEG image
 * Allocates JPEG buffer in PSRAM. Caller must free with free().
 *
 * @param[out] jpeg_out    Pointer to receive JPEG data (PSRAM-allocated)
 * @param[out] jpeg_size   Size of JPEG data in bytes
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no image, ESP_FAIL on error
 */
esp_err_t camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_size);

/**
 * @brief Run one inference pass and report whether a face/person-like target is present.
 *
 * Uses the currently loaded SSCMA model on the Himax side. This is intended for
 * device-local presence checks such as keeping the display awake when someone is
 * in front of the Watcher.
 *
 * @param[out] present     true when a qualifying detection is present
 * @param[out] best_score  optional 0-100 score for the best detection
 * @return ESP_OK on success
 */
esp_err_t camera_check_presence(bool *present, int *best_score);

/**
 * @brief Check if camera is initialized and ready
 * @return true if ready
 */
bool camera_is_ready(void);

/**
 * @brief Deinitialize camera (power down)
 */
void camera_deinit(void);

#ifdef __cplusplus
}
#endif
