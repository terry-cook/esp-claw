/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file setup_device.c
 * @brief LilyGO T-Display-S3 custom device initialization.
 *
 * The ST7789 LCD on T-Display-S3 is connected via an 8-bit parallel (Intel 8080
 * / i80) bus.  This interface is not covered by esp_board_manager's built-in
 * display_lcd SPI/DSI paths, so we register a fully custom device here.
 *
 * Pin mapping (from T-Display-S3 schematic / pin_config.h):
 *   Power enable : GPIO15   (must be HIGH before using the LCD)
 *   Backlight    : GPIO38   (PWM via LEDC, active HIGH — controlled separately
 *                            by the lcd_brightness device)
 *   Reset        : GPIO5
 *   CS           : GPIO6
 *   DC           : GPIO7
 *   WR           : GPIO8
 *   RD           : GPIO9
 *   D0..D7       : GPIO39/40/41/42/45/46/47/48
 *   Resolution   : 320 x 170 (landscape)
 */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "gen_board_device_custom.h"
#include "dev_display_lcd.h"

static const char *TAG = "lilygo_t_display_s3";

/* ── Pin definitions ─────────────────────────────────────────── */
#define LCD_PIN_POWER_ON    15
#define LCD_PIN_RST          5
#define LCD_PIN_CS           6
#define LCD_PIN_DC           7
#define LCD_PIN_WR           8
#define LCD_PIN_RD           9
#define LCD_PIN_D0          39
#define LCD_PIN_D1          40
#define LCD_PIN_D2          41
#define LCD_PIN_D3          42
#define LCD_PIN_D4          45
#define LCD_PIN_D5          46
#define LCD_PIN_D6          47
#define LCD_PIN_D7          48

/* ── Display parameters ──────────────────────────────────────── */
#define LCD_H_RES           320
#define LCD_V_RES           170
/* ST7789 physical frame is 240 rows; 170-row panel is offset by 35 */
#define LCD_Y_GAP            35
/* i80 pixel clock — keep ≤ 20 MHz for reliable operation */
#define LCD_PIXEL_CLK_HZ    (20 * 1000 * 1000)
#define LCD_CMD_BITS          8
#define LCD_PARAM_BITS        8
/* Maximum DMA transfer: one full frame in RGB565 */
#define LCD_MAX_TRANSFER_BYTES  (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))

/* ── Static storage ──────────────────────────────────────────── */
static dev_display_lcd_handles_t s_lcd_handles;

/* Device config that the application reads via esp_board_manager_get_device_config().
 * "i80" sub_type → treated as panel-IO interface (colour-swap enabled in Lua/emote). */
static const dev_display_lcd_config_t s_lcd_config = {
    .sub_type   = "i80",
    .lcd_width  = LCD_H_RES,
    .lcd_height = LCD_V_RES,
};

/* ── Custom device init ──────────────────────────────────────── */
static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device_handle is NULL");

    esp_err_t ret;

    /* 1. Enable LCD power rail (GPIO15 must be HIGH) */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = BIT64(LCD_PIN_POWER_ON),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&pwr_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "GPIO power-on config failed");
    gpio_set_level(LCD_PIN_POWER_ON, 1);

    /* 2. Create the i80 parallel bus */
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src        = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num    = LCD_PIN_DC,
        .wr_gpio_num    = LCD_PIN_WR,
        .data_gpio_nums = {
            LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3,
            LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7,
        },
        .bus_width          = 8,
        .max_transfer_bytes = LCD_MAX_TRANSFER_BYTES,
        .psram_trans_align  = 64,
        .sram_trans_align   = 4,
    };
    ret = esp_lcd_new_i80_bus(&bus_cfg, &i80_bus);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_lcd_new_i80_bus failed");

    /* 3. Create panel IO over the i80 bus */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
        .flags = {
            .swap_color_bytes = 0, /* handled by LVGL / emote layer */
        },
        .lcd_cmd_bits   = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };
    ret = esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        esp_lcd_del_i80_bus(i80_bus);
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i80 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. Create the ST7789 panel driver */
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(io_handle);
        esp_lcd_del_i80_bus(i80_bus);
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. Reset and initialise the panel */
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    /* ST7789 170-row panel: physical frame buffer has 240 rows, offset by 35.
     * swap_xy for landscape orientation (320 wide × 170 tall).            */
    esp_lcd_panel_set_gap(panel_handle, 0, LCD_Y_GAP);
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, true);
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    /* 6. Populate handles and update board-manager device config */
    s_lcd_handles.panel_handle = panel_handle;
    s_lcd_handles.io_handle    = io_handle;

    esp_board_device_update_config("display_lcd", (void *)&s_lcd_config);
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG, "T-Display-S3 i80 LCD ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)device_handle;
    if (handles) {
        if (handles->panel_handle) {
            esp_lcd_panel_del(handles->panel_handle);
            handles->panel_handle = NULL;
        }
        if (handles->io_handle) {
            esp_lcd_panel_io_del(handles->io_handle);
            handles->io_handle = NULL;
        }
    }
    gpio_set_level(LCD_PIN_POWER_ON, 0);
    ESP_LOGI(TAG, "T-Display-S3 i80 LCD deinitialized");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);
