/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_check.h"
#include "esp_log.h"

#include "cap_lua_internal.h"

static const char *TAG = "cap_lua";

static char s_lua_base_dir[128] = CAP_LUA_DEFAULT_BASE_DIR;
static cap_lua_module_t s_modules[CAP_LUA_MAX_MODULES];
static size_t s_module_count;
static bool s_builtin_modules_registered;
static bool s_module_registration_locked;

static esp_err_t cap_lua_build_simple_request(const char *string_key,
                                              const char *string_value,
                                              const char *string_key2,
                                              const char *string_value2,
                                              bool has_bool,
                                              const char *bool_key,
                                              bool bool_value,
                                              bool has_number,
                                              const char *number_key,
                                              uint32_t number_value,
                                              char **json_out)
{
    cJSON *root = NULL;

    if (!json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (string_key && string_value && !cJSON_AddStringToObject(root, string_key, string_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (string_key2 && string_value2 &&
            !cJSON_AddStringToObject(root, string_key2, string_value2)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (has_bool && bool_key && !cJSON_AddBoolToObject(root, bool_key, bool_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (has_number && number_key &&
            !cJSON_AddNumberToObject(root, number_key, (double)number_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *json_out ? ESP_OK : ESP_ERR_NO_MEM;
}

const char *cap_lua_get_base_dir(void)
{
    return s_lua_base_dir;
}

bool cap_lua_path_is_valid(const char *path)
{
    size_t base_len;
    size_t path_len;

    if (!path) {
        return false;
    }

    base_len = strlen(s_lua_base_dir);
    if (strncmp(path, s_lua_base_dir, base_len) != 0 || path[base_len] != '/') {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }

    path_len = strlen(path);
    return path_len > 4 && strcmp(path + path_len - 4, ".lua") == 0;
}

esp_err_t cap_lua_resolve_path(const char *path, char *resolved, size_t resolved_size)
{
    int written;

    if (!path || !path[0] || !resolved || resolved_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (path[0] == '/') {
        if (!cap_lua_path_is_valid(path)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(resolved, path, resolved_size);
        return ESP_OK;
    }

    if (strstr(path, "..") != NULL || strchr(path, '/') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(resolved, resolved_size, "%s/%s", s_lua_base_dir, path);
    if (written < 0 || (size_t)written >= resolved_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!cap_lua_path_is_valid(resolved)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t cap_lua_ensure_base_dir(void)
{
    if (mkdir(s_lua_base_dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create Lua base dir %s", s_lua_base_dir);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_lua_build_args_json(cJSON *root, char **args_json_out)
{
    cJSON *args = NULL;
    cJSON *payload = NULL;

    if (!args_json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *args_json_out = NULL;

    args = cJSON_GetObjectItem(root, "args");
    if (cJSON_IsObject(args) || cJSON_IsArray(args)) {
        payload = cJSON_Duplicate(args, 1);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        *args_json_out = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!*args_json_out) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static esp_err_t cap_lua_group_init(void)
{
    ESP_RETURN_ON_ERROR(cap_lua_register_builtin_modules(),
                        TAG,
                        "Failed to register builtin Lua modules");
    s_module_registration_locked = true;
    ESP_RETURN_ON_ERROR(cap_lua_ensure_base_dir(), TAG, "Failed to create base dir");
    ESP_RETURN_ON_ERROR(cap_lua_runtime_init(), TAG, "Failed to init runtime");
    ESP_RETURN_ON_ERROR(cap_lua_async_init(), TAG, "Failed to init async runner");
    return ESP_OK;
}

static esp_err_t cap_lua_group_start(void)
{
    return cap_lua_async_start();
}

static esp_err_t cap_lua_list_scripts_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *prefix = NULL;
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    size_t offset = 0;
    int count = 0;

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    root = cJSON_Parse(input_json);
    if (root) {
        cJSON *prefix_item = cJSON_GetObjectItem(root, "prefix");
        if (cJSON_IsString(prefix_item) && prefix_item->valuestring[0]) {
            prefix = prefix_item->valuestring;
        }
    }

    if (prefix && strncmp(prefix, s_lua_base_dir, strlen(s_lua_base_dir)) != 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: prefix must stay under %s", s_lua_base_dir);
        return ESP_ERR_INVALID_ARG;
    }

    dir = opendir(s_lua_base_dir);
    if (!dir) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot open %s", s_lua_base_dir);
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL && offset < output_size - 1) {
        char full_path[384];

        if (entry->d_name[0] == '.') {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", s_lua_base_dir, entry->d_name);
        if (!cap_lua_path_is_valid(full_path)) {
            continue;
        }
        if (prefix && strncmp(full_path, prefix, strlen(prefix)) != 0) {
            continue;
        }

        offset += snprintf(output + offset, output_size - offset, "%s\n", full_path);
        count++;
    }

    closedir(dir);
    cJSON_Delete(root);
    if (count == 0) {
        snprintf(output, output_size, "(no Lua scripts found)");
    }
    return ESP_OK;
}

static esp_err_t cap_lua_write_script_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    const char *content = NULL;
    char resolved_path[192];
    cJSON *overwrite_item = NULL;
    bool overwrite = true;
    struct stat st = {0};
    FILE *file = NULL;
    size_t content_len = 0;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    overwrite_item = cJSON_GetObjectItem(root, "overwrite");
    if (cJSON_IsBool(overwrite_item)) {
        overwrite = cJSON_IsTrue(overwrite_item);
    }

    if (cap_lua_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be a .lua file under %s", s_lua_base_dir);
        return ESP_ERR_INVALID_ARG;
    }
    if (!content) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing content");
        return ESP_ERR_INVALID_ARG;
    }

    content_len = strlen(content);
    if (content_len > CAP_LUA_MAX_SCRIPT_SIZE) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: script exceeds %d bytes", CAP_LUA_MAX_SCRIPT_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!overwrite && stat(resolved_path, &st) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: script already exists: %s", resolved_path);
        return ESP_ERR_INVALID_STATE;
    }

    if (cap_lua_ensure_base_dir() != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to ensure Lua base dir");
        return ESP_FAIL;
    }

    file = fopen(resolved_path, "w");
    if (!file) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot open %s for writing", resolved_path);
        return ESP_FAIL;
    }
    if (fwrite(content, 1, content_len, file) != content_len) {
        fclose(file);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to write %s", resolved_path);
        return ESP_FAIL;
    }

    fclose(file);
    cJSON_Delete(root);
    snprintf(output, output_size, "OK: wrote Lua script %s (%d bytes)", resolved_path, (int)content_len);
    return ESP_OK;
}

static esp_err_t cap_lua_run_script_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[192];
    cJSON *timeout_item = NULL;
    char *args_json = NULL;
    uint32_t timeout_ms = 0;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_lua_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be a .lua file under %s", s_lua_base_dir);
        return ESP_ERR_INVALID_ARG;
    }

    timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_item && (!cJSON_IsNumber(timeout_item) || timeout_item->valueint <= 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: timeout_ms must be a positive integer");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (uint32_t)timeout_item->valueint;
    }

    err = cap_lua_build_args_json(root, &args_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(args_json);
        snprintf(output, output_size, "Error: failed to prepare Lua args");
        return err;
    }

    err = cap_lua_runtime_execute_file(resolved_path,
                                       args_json,
                                       timeout_ms,
                                       output,
                                       output_size);
    free(args_json);
    return err;
}

static esp_err_t cap_lua_run_script_async_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[192];
    cJSON *timeout_item = NULL;
    char *args_json = NULL;
    char request_path[192] = {0};
    uint32_t timeout_ms = 0;
    cap_lua_async_job_t job = {0};
    char job_id[16] = {0};
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_lua_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be a .lua file under %s", s_lua_base_dir);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(request_path, path ? path : resolved_path, sizeof(request_path));

    timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_item && (!cJSON_IsNumber(timeout_item) || timeout_item->valueint <= 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: timeout_ms must be a positive integer");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (uint32_t)timeout_item->valueint;
    }

    err = cap_lua_build_args_json(root, &args_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(args_json);
        snprintf(output, output_size, "Error: failed to prepare Lua args");
        return err;
    }

    strlcpy(job.path, resolved_path, sizeof(job.path));
    job.args_json = args_json;
    job.timeout_ms = timeout_ms;
    job.created_at = time(NULL);
    err = cap_lua_async_submit(&job, job_id, sizeof(job_id));
    free(args_json);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NO_MEM) {
            snprintf(output, output_size, "Error: Lua async concurrency limit reached");
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(output, output_size, "Error: Lua async runner is not ready");
        } else {
            snprintf(output, output_size, "Error: failed to queue async Lua job (%s)",
                     esp_err_to_name(err));
        }
        return err;
    }

    snprintf(output, output_size, "Queued Lua job %s for %s", job_id, request_path);
    return ESP_OK;
}

static esp_err_t cap_lua_list_async_jobs_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = NULL;
    const char *status = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (root) {
        status = cJSON_GetStringValue(cJSON_GetObjectItem(root, "status"));
        if (status &&
                strcmp(status, "all") != 0 &&
                strcmp(status, "queued") != 0 &&
                strcmp(status, "running") != 0 &&
                strcmp(status, "done") != 0 &&
                strcmp(status, "failed") != 0 &&
                strcmp(status, "timeout") != 0) {
            cJSON_Delete(root);
            snprintf(output,
                     output_size,
                     "Error: status must be one of all, queued, running, done, failed, timeout");
            return ESP_ERR_INVALID_ARG;
        }
    }

    err = cap_lua_async_list_jobs(status, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_get_async_job_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *job_id = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!job_id || !job_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing job_id");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_lua_async_get_job(job_id, output, output_size);
    cJSON_Delete(root);
    return err;
}

static const claw_cap_descriptor_t s_lua_descriptors[] = {
    {
        .id = "lua_list_scripts",
        .name = "lua_list_scripts",
        .family = "automation",
        .description = "List managed Lua scripts under the configured Lua base directory.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"}}}",
        .execute = cap_lua_list_scripts_execute,
    },
    {
        .id = "lua_write_script",
        .name = "lua_write_script",
        .family = "automation",
        .description = "Write a managed Lua script under the configured Lua base directory.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"content\"]}",
        .execute = cap_lua_write_script_execute,
    },
    {
        .id = "lua_run_script",
        .name = "lua_run_script",
        .family = "automation",
        .description = "Run a managed Lua script synchronously with optional args and timeout.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"args\":{\"type\":[\"object\",\"array\"]},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
        .execute = cap_lua_run_script_execute,
    },
    {
        .id = "lua_run_script_async",
        .name = "lua_run_script_async",
        .family = "automation",
        .description = "Run a managed Lua script asynchronously and return a job identifier.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"args\":{\"type\":[\"object\",\"array\"]},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
        .execute = cap_lua_run_script_async_execute,
    },
    {
        .id = "lua_list_async_jobs",
        .name = "lua_list_async_jobs",
        .family = "automation",
        .description = "List Lua async jobs by optional status filter.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}}}",
        .execute = cap_lua_list_async_jobs_execute,
    },
    {
        .id = "lua_get_async_job",
        .name = "lua_get_async_job",
        .family = "automation",
        .description = "Get the status and summary for a specific Lua async job.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"}},\"required\":[\"job_id\"]}",
        .execute = cap_lua_get_async_job_execute,
    },
};

static const claw_cap_group_t s_lua_group = {
    .group_id = "cap_lua",
    .descriptors = s_lua_descriptors,
    .descriptor_count = sizeof(s_lua_descriptors) / sizeof(s_lua_descriptors[0]),
    .group_init = cap_lua_group_init,
    .group_start = cap_lua_group_start,
};

esp_err_t cap_lua_register_group(void)
{
    if (claw_cap_group_exists(s_lua_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_lua_group);
}

esp_err_t cap_lua_list_scripts(const char *prefix, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("prefix",
                                       prefix,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_list_scripts_execute(input_json ? input_json : "{}",
                                       NULL,
                                       output,
                                       output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_write_script(const char *path,
                               const char *content,
                               bool overwrite,
                               char *output,
                               size_t output_size)
{
    cJSON *root = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path || !content) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path) ||
            !cJSON_AddStringToObject(root, "content", content) ||
            !cJSON_AddBoolToObject(root, "overwrite", overwrite)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_write_script_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!args || (!cJSON_IsObject(args) && !cJSON_IsArray(args))) {
            cJSON_Delete(args);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(root, "args", args);
    }
    if (timeout_ms > 0 && !cJSON_AddNumberToObject(root, "timeout_ms", (double)timeout_ms)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_run_script_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_run_script_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   char *output,
                                   size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!args || (!cJSON_IsObject(args) && !cJSON_IsArray(args))) {
            cJSON_Delete(args);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(root, "args", args);
    }
    if (timeout_ms > 0 && !cJSON_AddNumberToObject(root, "timeout_ms", (double)timeout_ms)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_run_script_async_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_list_jobs(const char *status, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("status",
                                       status,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_list_async_jobs_execute(input_json ? input_json : "{}",
                                          NULL,
                                          output,
                                          output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_get_job(const char *job_id, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("job_id",
                                       job_id,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_get_async_job_execute(input_json ? input_json : "{}",
                                        NULL,
                                        output,
                                        output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_set_base_dir(const char *base_dir)
{
    if (!base_dir || !base_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_lua_base_dir, base_dir, sizeof(s_lua_base_dir));
    return ESP_OK;
}

esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn)
{
    size_t i;

    if (!name || !name[0] || !open_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_registration_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_module_count >= CAP_LUA_MAX_MODULES) {
        return ESP_ERR_NO_MEM;
    }

    s_modules[s_module_count].name = name;
    s_modules[s_module_count].open_fn = open_fn;
    s_module_count++;
    return ESP_OK;
}

esp_err_t cap_lua_register_modules(const cap_lua_module_t *modules, size_t count)
{
    size_t i;
    esp_err_t err;

    if (!modules || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < count; i++) {
        err = cap_lua_register_module(modules[i].name, modules[i].open_fn);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t cap_lua_register_builtin_modules(void)
{
    s_builtin_modules_registered = true;
    return ESP_OK;
}

size_t cap_lua_get_module_count(void)
{
    return s_module_count;
}

const cap_lua_module_t *cap_lua_get_module(size_t index)
{
    if (index >= s_module_count) {
        return NULL;
    }

    return &s_modules[index];
}
