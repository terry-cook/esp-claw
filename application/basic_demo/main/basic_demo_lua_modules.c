/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "basic_demo_lua_modules.h"

#include "lua_module_delay.h"
#include "lua_module_event_publisher.h"
#include "lua_module_gpio.h"
#include "lua_module_led_strip.h"
#include "lua_module_storage.h"

esp_err_t basic_demo_lua_modules_register(void)
{
    esp_err_t err;

    err = lua_module_delay_register();
    if (err != ESP_OK) {
        return err;
    }

    err = lua_module_storage_register();
    if (err != ESP_OK) {
        return err;
    }

    err = lua_module_gpio_register();
    if (err != ESP_OK) {
        return err;
    }

    err = lua_module_led_strip_register();
    if (err != ESP_OK) {
        return err;
    }

    return lua_module_event_publisher_register();
}
