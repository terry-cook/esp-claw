/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cap_lua_async";

typedef struct {
    bool used;
    cap_lua_job_status_t status;
    char job_id[9];
    char path[192];
    char *args_json;
    char *summary;
    uint32_t timeout_ms;
    time_t created_at;
    time_t started_at;
    time_t finished_at;
    TaskHandle_t task_handle;
} cap_lua_job_record_t;

typedef struct {
    int slot;
    char job_id[9];
    char path[192];
    char *args_json;
    uint32_t timeout_ms;
} cap_lua_job_ctx_t;

static SemaphoreHandle_t s_job_lock;
static cap_lua_job_record_t s_jobs[CAP_LUA_ASYNC_MAX_JOBS];
static size_t s_running_jobs;
static bool s_runner_started;

static const char *cap_lua_job_status_name(cap_lua_job_status_t status)
{
    switch (status) {
    case CAP_LUA_JOB_QUEUED:
        return "queued";
    case CAP_LUA_JOB_RUNNING:
        return "running";
    case CAP_LUA_JOB_DONE:
        return "done";
    case CAP_LUA_JOB_FAILED:
        return "failed";
    case CAP_LUA_JOB_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

static bool cap_lua_job_status_matches(cap_lua_job_status_t status, const char *filter)
{
    if (!filter || !filter[0] || strcmp(filter, "all") == 0) {
        return true;
    }

    return strcmp(cap_lua_job_status_name(status), filter) == 0;
}

static void cap_lua_generate_job_id(char *job_id, size_t size)
{
    snprintf(job_id, size, "%08x", (unsigned)esp_random());
}

static int cap_lua_find_reusable_slot_locked(void)
{
    int oldest_terminal = -1;
    int i;

    for (i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (!s_jobs[i].used) {
            return i;
        }
        if (s_jobs[i].status == CAP_LUA_JOB_DONE ||
                s_jobs[i].status == CAP_LUA_JOB_FAILED ||
                s_jobs[i].status == CAP_LUA_JOB_TIMEOUT) {
            if (oldest_terminal < 0 || s_jobs[i].finished_at < s_jobs[oldest_terminal].finished_at) {
                oldest_terminal = i;
            }
        }
    }

    return oldest_terminal;
}

static int cap_lua_find_slot_by_id_locked(const char *job_id)
{
    int i;

    for (i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        if (s_jobs[i].used && strcmp(s_jobs[i].job_id, job_id) == 0) {
            return i;
        }
    }

    return -1;
}

static void cap_lua_clear_slot(cap_lua_job_record_t *job)
{
    if (!job) {
        return;
    }
    free(job->args_json);
    free(job->summary);
    memset(job, 0, sizeof(*job));
}

static void cap_lua_finish_job(cap_lua_job_ctx_t *ctx,
                               bool ok,
                               bool timed_out,
                               const char *summary)
{
    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    if (ctx->slot >= 0 &&
            ctx->slot < CAP_LUA_ASYNC_MAX_JOBS &&
            s_jobs[ctx->slot].used &&
            strcmp(s_jobs[ctx->slot].job_id, ctx->job_id) == 0) {
        s_jobs[ctx->slot].status = timed_out ? CAP_LUA_JOB_TIMEOUT :
                                   (ok ? CAP_LUA_JOB_DONE : CAP_LUA_JOB_FAILED);
        s_jobs[ctx->slot].finished_at = time(NULL);
        s_jobs[ctx->slot].task_handle = NULL;
        if (summary && summary[0]) {
            free(s_jobs[ctx->slot].summary);
            s_jobs[ctx->slot].summary = strdup(summary);
        }
    }

    if (s_running_jobs > 0) {
        s_running_jobs--;
    }

    xSemaphoreGive(s_job_lock);
}

static void cap_lua_job_task(void *arg)
{
    cap_lua_job_ctx_t *ctx = (cap_lua_job_ctx_t *)arg;
    char *output = NULL;
    esp_err_t err;
    bool timed_out = false;

    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    output = calloc(1, CAP_LUA_OUTPUT_SIZE);
    if (!output) {
        cap_lua_finish_job(ctx, false, false, "failed to allocate output buffer");
        free(ctx->args_json);
        free(ctx);
        vTaskDelete(NULL);
        return;
    }

    err = cap_lua_runtime_execute_file(ctx->path,
                                       ctx->args_json,
                                       ctx->timeout_ms,
                                       output,
                                       CAP_LUA_OUTPUT_SIZE);
    if (err != ESP_OK && strstr(output, "execution timed out") != NULL) {
        timed_out = true;
    }

    cap_lua_finish_job(ctx, err == ESP_OK, timed_out, output);
    free(output);
    free(ctx->args_json);
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t cap_lua_async_init(void)
{
    int i;

    if (!s_job_lock) {
        s_job_lock = xSemaphoreCreateMutex();
    }
    if (!s_job_lock) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < CAP_LUA_ASYNC_MAX_JOBS; i++) {
        cap_lua_clear_slot(&s_jobs[i]);
    }
    memset(s_jobs, 0, sizeof(s_jobs));
    s_running_jobs = 0;
    s_runner_started = false;
    return ESP_OK;
}

esp_err_t cap_lua_async_start(void)
{
    if (!s_job_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    s_runner_started = true;
    return ESP_OK;
}

esp_err_t cap_lua_async_submit(const cap_lua_async_job_t *job,
                               char *job_id_out,
                               size_t job_id_out_size)
{
    cap_lua_job_ctx_t *ctx = NULL;
    int slot = -1;
    time_t now = time(NULL);

    if (!job || !job->path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_job_lock || !s_runner_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    cap_lua_generate_job_id(ctx->job_id, sizeof(ctx->job_id));
    strlcpy(ctx->path, job->path, sizeof(ctx->path));
    ctx->timeout_ms = job->timeout_ms;
    if (job->args_json) {
        ctx->args_json = strdup(job->args_json);
        if (!ctx->args_json) {
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_TIMEOUT;
    }

    if (s_running_jobs >= CAP_LUA_ASYNC_MAX_CONCURRENT) {
        xSemaphoreGive(s_job_lock);
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    slot = cap_lua_find_reusable_slot_locked();
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    cap_lua_clear_slot(&s_jobs[slot]);
    s_jobs[slot].used = true;
    s_jobs[slot].status = CAP_LUA_JOB_QUEUED;
    s_jobs[slot].created_at = job->created_at ? job->created_at : now;
    strlcpy(s_jobs[slot].job_id, ctx->job_id, sizeof(s_jobs[slot].job_id));
    strlcpy(s_jobs[slot].path, ctx->path, sizeof(s_jobs[slot].path));
    if (ctx->args_json) {
        s_jobs[slot].args_json = strdup(ctx->args_json);
        if (!s_jobs[slot].args_json) {
            cap_lua_clear_slot(&s_jobs[slot]);
            xSemaphoreGive(s_job_lock);
            free(ctx->args_json);
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    s_jobs[slot].timeout_ms = job->timeout_ms;
    ctx->slot = slot;
    s_running_jobs++;
    xSemaphoreGive(s_job_lock);

    if (xTaskCreate(cap_lua_job_task,
                    "cap_lua_async",
                    CAP_LUA_ASYNC_STACK,
                    ctx,
                    CAP_LUA_ASYNC_PRIO,
                    &s_jobs[slot].task_handle) != pdPASS) {
        if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            cap_lua_clear_slot(&s_jobs[slot]);
            if (s_running_jobs > 0) {
                s_running_jobs--;
            }
            xSemaphoreGive(s_job_lock);
        }
        free(ctx->args_json);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_jobs[slot].status = CAP_LUA_JOB_RUNNING;
        s_jobs[slot].started_at = time(NULL);
        xSemaphoreGive(s_job_lock);
    }

    if (job_id_out && job_id_out_size > 0) {
        strlcpy(job_id_out, ctx->job_id, job_id_out_size);
    }

    ESP_LOGI(TAG, "Queued Lua async job %s for %s", ctx->job_id, ctx->path);
    return ESP_OK;
}

esp_err_t cap_lua_async_list_jobs(const char *status_filter,
                                  char *output,
                                  size_t output_size)
{
    size_t offset = 0;
    int i;
    int shown = 0;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (i = 0; i < CAP_LUA_ASYNC_MAX_JOBS && offset < output_size - 1; i++) {
        int written;

        if (!s_jobs[i].used || !cap_lua_job_status_matches(s_jobs[i].status, status_filter)) {
            continue;
        }

        written = snprintf(output + offset,
                           output_size - offset,
                           "%s | %s | %s\n",
                           s_jobs[i].job_id,
                           cap_lua_job_status_name(s_jobs[i].status),
                           s_jobs[i].path);
        if (written < 0 || (size_t)written >= output_size - offset) {
            break;
        }

        offset += (size_t)written;
        shown++;
    }

    xSemaphoreGive(s_job_lock);
    if (shown == 0) {
        snprintf(output, output_size, "(no Lua async jobs)");
    }
    return ESP_OK;
}

esp_err_t cap_lua_async_get_job(const char *job_id,
                                char *output,
                                size_t output_size)
{
    int slot = -1;

    if (!job_id || !job_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    if (xSemaphoreTake(s_job_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    slot = cap_lua_find_slot_by_id_locked(job_id);
    if (slot < 0) {
        xSemaphoreGive(s_job_lock);
        snprintf(output, output_size, "Error: Lua async job not found: %s", job_id);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(output,
             output_size,
             "job_id=%s\nstatus=%s\npath=%s\nsummary=%s",
             s_jobs[slot].job_id,
             cap_lua_job_status_name(s_jobs[slot].status),
             s_jobs[slot].path,
             (s_jobs[slot].summary && s_jobs[slot].summary[0]) ? s_jobs[slot].summary : "(empty)");
    xSemaphoreGive(s_job_lock);
    return ESP_OK;
}
