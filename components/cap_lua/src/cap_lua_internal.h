/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cap_lua.h"
#include "esp_err.h"

#define CAP_LUA_DEFAULT_BASE_DIR       "/spiffs/lua"
#define CAP_LUA_MAX_SCRIPT_SIZE        (16 * 1024)
#define CAP_LUA_OUTPUT_SIZE            (4 * 1024)
#define CAP_LUA_MAX_EXEC_MS            60000
#define CAP_LUA_ASYNC_MAX_JOBS         16
#define CAP_LUA_ASYNC_MAX_CONCURRENT   4
#define CAP_LUA_ASYNC_STACK            (16 * 1024)
#define CAP_LUA_ASYNC_PRIO             4
#define CAP_LUA_MAX_MODULES            16

typedef struct {
    char path[192];
    char *args_json;
    uint32_t timeout_ms;
    time_t created_at;
} cap_lua_async_job_t;

typedef enum {
    CAP_LUA_JOB_QUEUED = 0,
    CAP_LUA_JOB_RUNNING,
    CAP_LUA_JOB_DONE,
    CAP_LUA_JOB_FAILED,
    CAP_LUA_JOB_TIMEOUT,
} cap_lua_job_status_t;

const char *cap_lua_get_base_dir(void);
bool cap_lua_path_is_valid(const char *path);
esp_err_t cap_lua_resolve_path(const char *path, char *resolved, size_t resolved_size);
esp_err_t cap_lua_ensure_base_dir(void);

esp_err_t cap_lua_runtime_init(void);
esp_err_t cap_lua_runtime_execute_file(const char *path,
                                       const char *args_json,
                                       uint32_t timeout_ms,
                                       char *output,
                                       size_t output_size);
esp_err_t cap_lua_register_builtin_modules(void);
size_t cap_lua_get_module_count(void);
const cap_lua_module_t *cap_lua_get_module(size_t index);

esp_err_t cap_lua_async_init(void);
esp_err_t cap_lua_async_start(void);
esp_err_t cap_lua_async_submit(const cap_lua_async_job_t *job,
                               char *job_id_out,
                               size_t job_id_out_size);
esp_err_t cap_lua_async_list_jobs(const char *status_filter,
                                  char *output,
                                  size_t output_size);
esp_err_t cap_lua_async_get_job(const char *job_id,
                                char *output,
                                size_t output_size);
