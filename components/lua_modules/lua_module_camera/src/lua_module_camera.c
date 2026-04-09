/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_camera.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cap_lua.h"
#include "lauxlib.h"
#include "lua_module_camera_service.h"

#define LUA_MODULE_CAMERA_NAME "camera"
#define LUA_MODULE_CAMERA_BASE_DIR "/fatfs/data"

static bool lua_module_camera_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if (path == NULL || suffix == NULL) {
        return false;
    }

    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return false;
    }

    path += path_len - suffix_len;
    for (size_t i = 0; i < suffix_len; ++i) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

static bool lua_module_camera_save_path_is_valid(const char *path)
{
    size_t base_len;

    if (path == NULL) {
        return false;
    }

    base_len = strlen(LUA_MODULE_CAMERA_BASE_DIR);
    if (strncmp(path, LUA_MODULE_CAMERA_BASE_DIR, base_len) != 0 || path[base_len] != '/') {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }

    return lua_module_camera_has_suffix(path, ".jpg") ||
           lua_module_camera_has_suffix(path, ".jpeg");
}

/* camera.open(dev_path)
 * Opens the camera device. Must be called before info() or capture().
 * Returns true on success, or raises an error. */
static int lua_module_camera_open(lua_State *L)
{
    const char *dev_path = luaL_checkstring(L, 1);
    esp_err_t err = claw_camera_open(dev_path);

    if (err != ESP_OK) {
        return luaL_error(L, "camera open failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* camera.info()
 * Returns a table with stream info: width, height, pixel_format, pixel_format_raw.
 * Requires the camera to be opened first. */
static int lua_module_camera_info(lua_State *L)
{
    claw_camera_stream_info_t info = {0};
    esp_err_t err = claw_camera_get_stream_info(&info);

    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera info failed: camera not opened, call camera.open() first");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera info failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushinteger(L, info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)info.pixel_format);
    lua_setfield(L, -2, "pixel_format_raw");
    lua_pushstring(L, info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    return 1;
}

/* camera.capture(save_path [, timeout_ms])
 * Captures a JPEG frame and writes it to save_path.
 * save_path must be a .jpg/.jpeg file under /fatfs/data/.
 * Returns a table with: path, bytes, width, height, pixel_format, pixel_format_raw,
 *                        frame_bytes, timestamp_us. */
static int lua_module_camera_capture(lua_State *L)
{
    claw_camera_frame_info_t frame_info = {0};
    const char *path = luaL_checkstring(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_bytes = 0;
    FILE *file = NULL;
    esp_err_t err;

    if (!lua_module_camera_save_path_is_valid(path)) {
        return luaL_error(L, "save path must be a .jpg/.jpeg file under %s",
                          LUA_MODULE_CAMERA_BASE_DIR);
    }
    if (timeout_ms < 0) {
        return luaL_error(L, "timeout_ms must be non-negative");
    }

    err = claw_camera_capture_jpeg(timeout_ms, &jpeg_data, &jpeg_bytes, &frame_info);
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera capture failed: camera not opened, call camera.open() first");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera capture failed: %s", esp_err_to_name(err));
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        claw_camera_free_buffer(jpeg_data);
        return luaL_error(L, "camera capture failed: cannot open %s (errno=%d)", path, errno);
    }

    if (fwrite(jpeg_data, 1, jpeg_bytes, file) != jpeg_bytes) {
        fclose(file);
        unlink(path);
        claw_camera_free_buffer(jpeg_data);
        return luaL_error(L, "camera capture failed: short write to %s", path);
    }

    fclose(file);
    claw_camera_free_buffer(jpeg_data);

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, (lua_Integer)jpeg_bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, frame_info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, frame_info.height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)frame_info.pixel_format);
    lua_setfield(L, -2, "pixel_format_raw");
    lua_pushstring(L, frame_info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, (lua_Integer)frame_info.frame_bytes);
    lua_setfield(L, -2, "frame_bytes");
    lua_pushinteger(L, (lua_Integer)frame_info.timestamp_us);
    lua_setfield(L, -2, "timestamp_us");
    return 1;
}

/* camera.close()
 * Closes the camera device and releases all resources.
 * Returns true on success, or raises an error. */
static int lua_module_camera_close(lua_State *L)
{
    esp_err_t err = claw_camera_close();

    if (err != ESP_OK) {
        return luaL_error(L, "camera close failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_camera(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"open",    lua_module_camera_open},
        {"info",    lua_module_camera_info},
        {"capture", lua_module_camera_capture},
        {"close",   lua_module_camera_close},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_camera_register(void)
{
    return cap_lua_register_module(LUA_MODULE_CAMERA_NAME, luaopen_camera);
}
