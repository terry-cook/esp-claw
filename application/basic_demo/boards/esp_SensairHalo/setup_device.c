/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "esp_board_device.h"
#include "esp_codec_dev.h"
#include "periph_i2c.h"
#include "periph_gpio.h"
#include "gen_board_device_custom.h"
#include "dev_audio_codec.h"
#include "touch_ic_bs8112a3.h"
#include "dev_custom.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "SETUP_DEVICE";

static const ili9341_lcd_init_cmd_t s_vendor_init_cmds[] = {
    {0x11, NULL, 0, 120},
    {0x3A, (uint8_t []){0x05}, 1, 0},
    {0xB2, (uint8_t []){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0},
    {0xB7, (uint8_t []){0x05}, 1, 0},
    {0xBB, (uint8_t []){0x21}, 1, 0},
    {0xC0, (uint8_t []){0x2C}, 1, 0},
    {0xC2, (uint8_t []){0x01}, 1, 0},
    {0xC3, (uint8_t []){0x15}, 1, 0},
    {0xC6, (uint8_t []){0x0F}, 1, 0},
    {0xD0, (uint8_t []){0xA7}, 1, 0},
    {0xD0, (uint8_t []){0xA4, 0xA1}, 2, 0},
    {0xD6, (uint8_t []){0xA1}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x05, 0x0E, 0x08, 0x0A, 0x17, 0x39, 0x54,
                        0x4E, 0x37, 0x12, 0x12, 0x31, 0x37}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x10, 0x14, 0x0D, 0x0B, 0x05, 0x39, 0x44,
                        0x4D, 0x38, 0x14, 0x14, 0x2E, 0x35}, 14, 0},
    {0xE4, (uint8_t []){0x23, 0x00, 0x00}, 3, 0},
    {0x21, NULL, 0, 0},
    {0x29, NULL, 0, 0},
    {0x2C, NULL, 0, 0},
};

static const ili9341_vendor_config_t vendor_config = {
    .init_cmds = s_vendor_init_cmds,
    .init_cmds_size = sizeof(s_vendor_init_cmds) / sizeof(s_vendor_init_cmds[0]),
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    panel_dev_cfg.vendor_config = (void *)&vendor_config;
    int ret = esp_lcd_new_panel_ili9341(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_panel_factory_entry_t", "New ili9341 panel failed");
        return ret;
    }
    esp_lcd_panel_set_gap(*ret_panel, 36, 0);
    return ESP_OK;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));
    if (touch_cfg.int_gpio_num != GPIO_NUM_NC) {
        ESP_LOGW("lcd_touch_factory_entry_t", "Touch interrupt supported!");
        touch_cfg.interrupt_callback = NULL;
    }
    esp_err_t ret = esp_lcd_touch_new_i2c_cst816s(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_touch_factory_entry_t", "Failed to create gt911 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

static int touch_button_init(void *config, int cfg_size, void **device_handle)
{
    esp_err_t ret = ESP_OK;
    dev_custom_touch_button_config_t *touch_button_cfg = (dev_custom_touch_button_config_t *)config;
    ESP_LOGI(TAG, "Initializing %s device", touch_button_cfg->name);

    ESP_RETURN_ON_FALSE(strcmp(touch_button_cfg->chip, "bs8112") == 0, ESP_ERR_INVALID_ARG,
                        TAG, "Unsupported chip: %s", touch_button_cfg->chip);

    // Try to get existing I2C bus handle first
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    esp_board_periph_ref_handle(touch_button_cfg->peripheral_name, (void**)&i2c_bus_handle);
    ESP_RETURN_ON_FALSE(i2c_bus_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "Failed to get I2C bus handle");

    // Use default chip configuration
    bs8112a3_chip_config_t default_chip_cfg = TOUCH_IC_BS8112A3_DEFAULT_CHIP_CONFIG();
    default_chip_cfg.lsc_mode = BS8112A3_LSC_MODE_NORMAL;
    default_chip_cfg.irq_oms = BS8112A3_IRQ_OMS_LEVEL_HOLD;
    default_chip_cfg.k12_mode = BS8112A3_K12_MODE_IRQ;

    touch_ic_bs8112a3_config_t bs8112_cfg = {
        .i2c_bus_handle = i2c_bus_handle,
        .device_address = BS8112A3_I2C_ADDR,
        .scl_speed_hz = 400000,
        .irq_pin = touch_button_cfg->irq_pin,
        .chip_config = &default_chip_cfg,
    };

    touch_ic_bs8112a3_handle_t touch_handle = NULL;
    ret = touch_ic_bs8112a3_create(&bs8112_cfg, &touch_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create BS8112A3 touch IC instance");

    // Store handle in device_handle
    *device_handle = (void *)touch_handle;
    return ESP_OK;
}

static int touch_button_deinit(void *device_handle)
{
    if (!device_handle) {
        return ESP_OK;
    }

    touch_ic_bs8112a3_handle_t touch_handle = (touch_ic_bs8112a3_handle_t)device_handle;
    esp_err_t ret = touch_ic_bs8112a3_delete(touch_handle);

    return ret;
}

CUSTOM_DEVICE_IMPLEMENT(touch_button, touch_button_init, touch_button_deinit);
