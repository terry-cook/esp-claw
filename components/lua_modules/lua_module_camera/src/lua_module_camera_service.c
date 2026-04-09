/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_camera_service.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"

#define CLAW_CAMERA_DEFAULT_TIMEOUT_MS   5000
#define CLAW_CAMERA_SETTLE_TIMEOUT_MS   30000  /* settle at open time, allow slow SPI sensors */
#define CLAW_CAMERA_BUFFER_COUNT            3
#define CLAW_CAMERA_STREAM_SETTLE_FRAMES    3  /* reduced: 3 frames enough for AE/AWB stabilize */
#define CLAW_CAMERA_JPEG_QUALITY           80
#define CLAW_CAMERA_MIN_JPEG_BUFFER_SIZE (64 * 1024)

static const char *TAG = "claw_camera";

static esp_err_t claw_camera_settle_stream_locked(int64_t deadline_us);
static void claw_camera_fourcc_to_string(uint32_t pixel_format, char out[5]);

typedef struct {
    SemaphoreHandle_t lock;
    bool opened;
    bool streaming;
    uint32_t settled_frames;
    int fd;
    uint8_t *buffers[CLAW_CAMERA_BUFFER_COUNT];
    size_t buffer_lengths[CLAW_CAMERA_BUFFER_COUNT];
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    jpeg_enc_handle_t jpeg_encoder;
    uint8_t *jpeg_buffer;
    uint32_t jpeg_buffer_size;
} claw_camera_state_t;

static StaticSemaphore_t s_camera_lock_buf;
static claw_camera_state_t s_camera = {
    .fd = -1,
};

static esp_err_t claw_camera_lock(void)
{
    if (s_camera.lock == NULL) {
        s_camera.lock = xSemaphoreCreateMutexStatic(&s_camera_lock_buf);
    }

    if (xSemaphoreTake(s_camera.lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Camera mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void claw_camera_unlock(void)
{
    if (s_camera.lock != NULL) {
        xSemaphoreGive(s_camera.lock);
    }
}

static int claw_camera_effective_timeout_ms(int timeout_ms)
{
    return timeout_ms > 0 ? timeout_ms : CLAW_CAMERA_DEFAULT_TIMEOUT_MS;
}

static int claw_camera_remaining_timeout_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();

    if (remaining_us <= 0) {
        return 0;
    }
    if (remaining_us > INT32_MAX * 1000LL) {
        return INT32_MAX;
    }
    return (int)((remaining_us + 999LL) / 1000LL);
}

static void claw_camera_fourcc_to_string(uint32_t pixel_format, char out[5])
{
    out[0] = (char)(pixel_format & 0xff);
    out[1] = (char)((pixel_format >> 8) & 0xff);
    out[2] = (char)((pixel_format >> 16) & 0xff);
    out[3] = (char)((pixel_format >> 24) & 0xff);
    out[4] = '\0';

    for (int i = 0; i < 4; ++i) {
        if (out[i] < 32 || out[i] > 126) {
            out[i] = '.';
        }
    }
}

static void claw_camera_release_encoder_locked(void)
{
    if (s_camera.jpeg_buffer != NULL) {
        jpeg_free_align(s_camera.jpeg_buffer);
        s_camera.jpeg_buffer = NULL;
    }
    if (s_camera.jpeg_encoder != NULL) {
        jpeg_enc_close(s_camera.jpeg_encoder);
        s_camera.jpeg_encoder = NULL;
    }
    s_camera.jpeg_buffer_size = 0;
}

static void claw_camera_close_locked(void)
{
    if (s_camera.streaming && s_camera.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_camera.fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed (errno=%d)", errno);
        }
    }

    for (uint32_t i = 0; i < s_camera.buffer_count; ++i) {
        if (s_camera.buffers[i] != NULL) {
            munmap(s_camera.buffers[i], s_camera.buffer_lengths[i]);
            s_camera.buffers[i] = NULL;
        }
        s_camera.buffer_lengths[i] = 0;
    }
    s_camera.buffer_count = 0;

    claw_camera_release_encoder_locked();

    if (s_camera.fd >= 0) {
        close(s_camera.fd);
        s_camera.fd = -1;
    }

    s_camera.opened = false;
    s_camera.streaming = false;
    s_camera.settled_frames = 0;
    s_camera.width = 0;
    s_camera.height = 0;
    s_camera.pixel_format = 0;
}

static esp_err_t claw_camera_prepare_encoder_locked(void)
{
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    uint64_t input_size = 0;
    int jpeg_err;

    if (s_camera.pixel_format == V4L2_PIX_FMT_JPEG) {
        return ESP_OK;
    }
    if (s_camera.jpeg_encoder != NULL && s_camera.jpeg_buffer != NULL) {
        return ESP_OK;
    }

    config.width = s_camera.width;
    config.height = s_camera.height;
    config.quality = CLAW_CAMERA_JPEG_QUALITY;

    switch (s_camera.pixel_format) {
    case V4L2_PIX_FMT_UYVY:
        config.src_type = JPEG_PIXEL_FORMAT_CbYCrY;
        config.subsampling = JPEG_SUBSAMPLE_422;
        input_size = (uint64_t)s_camera.width * s_camera.height * 2U;
        break;
    case V4L2_PIX_FMT_YUYV:
        config.src_type = JPEG_PIXEL_FORMAT_YCbYCr;
        config.subsampling = JPEG_SUBSAMPLE_422;
        input_size = (uint64_t)s_camera.width * s_camera.height * 2U;
        break;
    case V4L2_PIX_FMT_RGB565:
        config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        config.subsampling = JPEG_SUBSAMPLE_422;
        input_size = (uint64_t)s_camera.width * s_camera.height * 2U;
        break;
    case V4L2_PIX_FMT_RGB565X:
        config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        config.subsampling = JPEG_SUBSAMPLE_422;
        input_size = (uint64_t)s_camera.width * s_camera.height * 2U;
        break;
    default:
        ESP_LOGE(TAG, "prepare_encoder: unsupported pixel format 0x%08" PRIx32, s_camera.pixel_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (input_size == 0 || input_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_err = jpeg_enc_open(&config, &s_camera.jpeg_encoder);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_enc_open failed: %d", jpeg_err);
        s_camera.jpeg_encoder = NULL;
        return ESP_FAIL;
    }

    s_camera.jpeg_buffer_size = (uint32_t)(input_size * 3U / 4U);
    if (s_camera.jpeg_buffer_size < CLAW_CAMERA_MIN_JPEG_BUFFER_SIZE) {
        s_camera.jpeg_buffer_size = CLAW_CAMERA_MIN_JPEG_BUFFER_SIZE;
    }

    s_camera.jpeg_buffer = jpeg_calloc_align(s_camera.jpeg_buffer_size, 128);
    if (s_camera.jpeg_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer");
        claw_camera_release_encoder_locked();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t claw_camera_open_locked(const char *dev_path)
{
    struct v4l2_format format = {0};
    struct v4l2_requestbuffers request = {0};
    char pixel_format[5] = {0};
    esp_err_t err;

    if (s_camera.opened) {
        return ESP_OK;
    }

    if (dev_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_camera.fd = open(dev_path, O_RDWR);
    if (s_camera.fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d)", dev_path, errno);
        s_camera.fd = -1;
        return ESP_ERR_NOT_FOUND;
    }

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera.fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed (errno=%d)", errno);
        claw_camera_close_locked();
        return ESP_FAIL;
    }

    s_camera.width = format.fmt.pix.width;
    s_camera.height = format.fmt.pix.height;
    s_camera.pixel_format = format.fmt.pix.pixelformat;
    claw_camera_fourcc_to_string(s_camera.pixel_format, pixel_format);

    request.count = CLAW_CAMERA_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_camera.fd, VIDIOC_REQBUFS, &request) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed (errno=%d)", errno);
        claw_camera_close_locked();
        return ESP_FAIL;
    }
    if (request.count == 0) {
        ESP_LOGE(TAG, "Camera buffer request returned 0");
        claw_camera_close_locked();
        return ESP_ERR_NO_MEM;
    }

    s_camera.buffer_count = request.count > CLAW_CAMERA_BUFFER_COUNT ?
                            CLAW_CAMERA_BUFFER_COUNT : request.count;

    for (uint32_t i = 0; i < s_camera.buffer_count; ++i) {
        struct v4l2_buffer buffer = {0};

        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (ioctl(s_camera.fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed (errno=%d)", errno);
            claw_camera_close_locked();
            return ESP_FAIL;
        }

        s_camera.buffers[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, s_camera.fd, buffer.m.offset);
        if (s_camera.buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "Failed to mmap camera buffer (errno=%d)", errno);
            s_camera.buffers[i] = NULL;
            claw_camera_close_locked();
            return ESP_ERR_NO_MEM;
        }
        s_camera.buffer_lengths[i] = buffer.length;

        if (ioctl(s_camera.fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "Initial VIDIOC_QBUF failed (errno=%d)", errno);
            claw_camera_close_locked();
            return ESP_FAIL;
        }
    }

    err = claw_camera_prepare_encoder_locked();
    if (err != ESP_OK) {
        claw_camera_close_locked();
        return err;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed (errno=%d)", errno);
        claw_camera_close_locked();
        return ESP_FAIL;
    }

    s_camera.streaming = true;
    s_camera.opened = true;
    s_camera.settled_frames = 0;

    ESP_LOGI(TAG, "Camera opened: path=%s size=%ux%u format=%s",
             dev_path, (unsigned)s_camera.width, (unsigned)s_camera.height,
             pixel_format);

    /* Settle the stream at open time with a dedicated timeout so that capture()
     * calls are not penalised by the sensor warm-up delay. */
    int64_t settle_deadline_us =
        esp_timer_get_time() + ((int64_t)CLAW_CAMERA_SETTLE_TIMEOUT_MS * 1000LL);
    err = claw_camera_settle_stream_locked(settle_deadline_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream settle failed during open (err=0x%x)", err);
        claw_camera_close_locked();
        return err;
    }

    return ESP_OK;
}

static esp_err_t claw_camera_dequeue_buffer_locked(int timeout_ms, struct v4l2_buffer *buffer)
{
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    if (buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* esp_video VFS does not implement poll(); use VIDIOC_S_DQBUF_TIMEOUT to
     * set a per-call timeout and block directly in VIDIOC_DQBUF. */
    if (ioctl(s_camera.fd, VIDIOC_S_DQBUF_TIMEOUT, &tv) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_DQBUF_TIMEOUT failed (errno=%d)", errno);
        return ESP_FAIL;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer->memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_camera.fd, VIDIOC_DQBUF, buffer) != 0) {
        if (errno == ETIMEDOUT) {
            ESP_LOGW(TAG, "VIDIOC_DQBUF timeout after %d ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGW(TAG, "VIDIOC_DQBUF failed (errno=%d)", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t claw_camera_requeue_buffer_locked(struct v4l2_buffer *buffer)
{
    if (buffer == NULL || buffer->index >= s_camera.buffer_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ioctl(s_camera.fd, VIDIOC_QBUF, buffer) != 0) {
        ESP_LOGW(TAG, "VIDIOC_QBUF failed (errno=%d)", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t claw_camera_validate_buffer_locked(const struct v4l2_buffer *buffer,
                                                    size_t *frame_bytes)
{
    if (buffer == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((buffer->flags & V4L2_BUF_FLAG_DONE) == 0 || (buffer->flags & V4L2_BUF_FLAG_ERROR) != 0) {
        return ESP_FAIL;
    }
    if (buffer->index >= s_camera.buffer_count) {
        return ESP_FAIL;
    }
    if (s_camera.buffers[buffer->index] == NULL || s_camera.buffer_lengths[buffer->index] == 0) {
        return ESP_FAIL;
    }
    if (buffer->bytesused > s_camera.buffer_lengths[buffer->index]) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (buffer->bytesused == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    *frame_bytes = buffer->bytesused;
    return ESP_OK;
}

static esp_err_t claw_camera_settle_stream_locked(int64_t deadline_us)
{
    while (s_camera.settled_frames < CLAW_CAMERA_STREAM_SETTLE_FRAMES) {
        struct v4l2_buffer buffer = {0};
        size_t frame_bytes = 0;
        int remaining_ms = claw_camera_remaining_timeout_ms(deadline_us);
        esp_err_t err;

        if (remaining_ms <= 0) {
            ESP_LOGW(TAG, "settle: timeout (settled=%" PRIu32 "/%d)",
                     s_camera.settled_frames, CLAW_CAMERA_STREAM_SETTLE_FRAMES);
            return ESP_ERR_TIMEOUT;
        }

        err = claw_camera_dequeue_buffer_locked(remaining_ms, &buffer);
        if (err != ESP_OK) {
            claw_camera_close_locked();
            return err;
        }

        err = claw_camera_validate_buffer_locked(&buffer, &frame_bytes);
        if (claw_camera_requeue_buffer_locked(&buffer) != ESP_OK) {
            claw_camera_close_locked();
            return ESP_FAIL;
        }

        if (err == ESP_OK && frame_bytes > 0) {
            s_camera.settled_frames++;
        }
    }
    return ESP_OK;
}

static esp_err_t claw_camera_get_frame_locked(int timeout_ms, struct v4l2_buffer *buffer,
                                              uint8_t **frame_data, size_t *frame_bytes,
                                              int64_t *timestamp_us)
{
    int64_t deadline_us;
    esp_err_t err;

    if (buffer == NULL || frame_data == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_camera.opened) {
        return ESP_ERR_INVALID_STATE;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)claw_camera_effective_timeout_ms(timeout_ms) * 1000LL);

    err = claw_camera_settle_stream_locked(deadline_us);
    if (err != ESP_OK) {
        return err;
    }

    while (claw_camera_remaining_timeout_ms(deadline_us) > 0) {
        int remaining_ms = claw_camera_remaining_timeout_ms(deadline_us);

        err = claw_camera_dequeue_buffer_locked(remaining_ms, buffer);
        if (err != ESP_OK) {
            claw_camera_close_locked();
            return err;
        }

        err = claw_camera_validate_buffer_locked(buffer, frame_bytes);
        if (err == ESP_OK) {
            *frame_data = s_camera.buffers[buffer->index];
            if (timestamp_us != NULL) {
                *timestamp_us = esp_timer_get_time();
            }
            return ESP_OK;
        }

        if (claw_camera_requeue_buffer_locked(buffer) != ESP_OK) {
            claw_camera_close_locked();
            return ESP_FAIL;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t claw_camera_copy_frame_and_requeue_locked(struct v4l2_buffer *buffer,
                                                           const uint8_t *frame_data,
                                                           size_t frame_bytes,
                                                           uint8_t **out_frame_copy)
{
    uint8_t *frame_copy;

    if (buffer == NULL || frame_data == NULL || out_frame_copy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    frame_copy = malloc(frame_bytes);
    if (frame_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(frame_copy, frame_data, frame_bytes);

    if (claw_camera_requeue_buffer_locked(buffer) != ESP_OK) {
        free(frame_copy);
        claw_camera_close_locked();
        return ESP_FAIL;
    }

    *out_frame_copy = frame_copy;
    return ESP_OK;
}

esp_err_t claw_camera_open(const char *dev_path)
{
    esp_err_t err = claw_camera_lock();

    if (err != ESP_OK) {
        return err;
    }

    err = claw_camera_open_locked(dev_path);
    claw_camera_unlock();
    return err;
}

esp_err_t claw_camera_get_stream_info(claw_camera_stream_info_t *out_info)
{
    esp_err_t err;

    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));

    err = claw_camera_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_camera.opened) {
        claw_camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    out_info->width = s_camera.width;
    out_info->height = s_camera.height;
    out_info->pixel_format = s_camera.pixel_format;
    claw_camera_fourcc_to_string(s_camera.pixel_format, out_info->pixel_format_str);

    claw_camera_unlock();
    return ESP_OK;
}

esp_err_t claw_camera_capture_jpeg(int timeout_ms, uint8_t **jpeg_data, size_t *jpeg_bytes,
                                   claw_camera_frame_info_t *out_info)
{
    struct v4l2_buffer buffer = {0};
    uint8_t *frame_data = NULL;
    uint8_t *frame_copy = NULL;
    size_t frame_bytes = 0;
    int64_t timestamp_us = 0;
    uint8_t *copy = NULL;
    size_t encoded_size = 0;
    esp_err_t err;

    if (jpeg_data == NULL || jpeg_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *jpeg_data = NULL;
    *jpeg_bytes = 0;
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }

    err = claw_camera_lock();
    if (err != ESP_OK) {
        return err;
    }

    err = claw_camera_get_frame_locked(timeout_ms, &buffer, &frame_data, &frame_bytes, &timestamp_us);
    if (err != ESP_OK) {
        claw_camera_unlock();
        return err;
    }

    err = claw_camera_copy_frame_and_requeue_locked(&buffer, frame_data, frame_bytes, &frame_copy);
    if (err != ESP_OK) {
        claw_camera_unlock();
        return err;
    }

    if (s_camera.pixel_format == V4L2_PIX_FMT_JPEG) {
        encoded_size = frame_bytes;
        copy = frame_copy;
        frame_copy = NULL;
    } else {
        int out_len = 0;

        err = claw_camera_prepare_encoder_locked();
        if (err == ESP_OK) {
            if (jpeg_enc_process(s_camera.jpeg_encoder, frame_copy, (uint32_t)frame_bytes,
                                 s_camera.jpeg_buffer, s_camera.jpeg_buffer_size,
                                 &out_len) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "jpeg_enc_process failed");
                err = ESP_FAIL;
            } else if (out_len <= 0) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                encoded_size = (size_t)out_len;
                copy = malloc(encoded_size);
                if (copy == NULL) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    memcpy(copy, s_camera.jpeg_buffer, encoded_size);
                }
            }
        }
    }

    if (err == ESP_OK) {
        *jpeg_data = copy;
        *jpeg_bytes = encoded_size;
        if (out_info != NULL) {
            out_info->width = s_camera.width;
            out_info->height = s_camera.height;
            out_info->pixel_format = s_camera.pixel_format;
            claw_camera_fourcc_to_string(s_camera.pixel_format, out_info->pixel_format_str);
            out_info->frame_bytes = frame_bytes;
            out_info->timestamp_us = timestamp_us;
        }
    } else if (copy != NULL) {
        free(copy);
    }

    free(frame_copy);
    claw_camera_unlock();
    return err;
}

esp_err_t claw_camera_close(void)
{
    esp_err_t err = claw_camera_lock();

    if (err != ESP_OK) {
        return err;
    }

    claw_camera_close_locked();
    claw_camera_unlock();
    return ESP_OK;
}

void claw_camera_free_buffer(void *buffer)
{
    free(buffer);
}
