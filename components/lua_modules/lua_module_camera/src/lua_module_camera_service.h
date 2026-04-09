/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    char pixel_format_str[5];
} claw_camera_stream_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    char pixel_format_str[5];
    size_t frame_bytes;
    int64_t timestamp_us;
} claw_camera_frame_info_t;

esp_err_t claw_camera_open(const char *dev_path);
esp_err_t claw_camera_get_stream_info(claw_camera_stream_info_t *out_info);
esp_err_t claw_camera_capture_jpeg(int timeout_ms, uint8_t **jpeg_data, size_t *jpeg_bytes,
                                   claw_camera_frame_info_t *out_info);
esp_err_t claw_camera_close(void);
void claw_camera_free_buffer(void *buffer);

#ifdef __cplusplus
}
#endif
