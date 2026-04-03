/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cap_lua.h"
#include "lauxlib.h"

static int lua_module_storage_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return luaL_error(L, "mkdir failed for %s", path);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_module_storage_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t content_len = 0;
    const char *content = luaL_checklstring(L, 2, &content_len);
    FILE *file = NULL;

    file = fopen(path, "w");
    if (!file) {
        return luaL_error(L, "cannot open file for writing: %s", path);
    }

    if (fwrite(content, 1, content_len, file) != content_len) {
        fclose(file);
        return luaL_error(L, "short write to %s", path);
    }

    fclose(file);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_module_storage_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *file = NULL;
    long size = 0;
    char *buf = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return luaL_error(L, "cannot open file for reading: %s", path);
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return luaL_error(L, "seek failed for %s", path);
    }

    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return luaL_error(L, "tell failed for %s", path);
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return luaL_error(L, "seek failed for %s", path);
    }

    buf = calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(file);
        return luaL_error(L, "failed to allocate read buffer");
    }

    if (size > 0 && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return luaL_error(L, "read failed for %s", path);
    }

    fclose(file);
    lua_pushlstring(L, buf, (size_t)size);
    free(buf);
    return 1;
}

int luaopen_storage(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_module_storage_mkdir);
    lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, lua_module_storage_write_file);
    lua_setfield(L, -2, "write_file");
    lua_pushcfunction(L, lua_module_storage_read_file);
    lua_setfield(L, -2, "read_file");
    return 1;
}

esp_err_t lua_module_storage_register(void)
{
    return cap_lua_register_module("storage", luaopen_storage);
}
