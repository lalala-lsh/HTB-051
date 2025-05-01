/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"

#include "periph_sdcard.h"
#include "led_indicator.h"
#include "periph_adc_button.h"
#include "i2c_bus.h"

#define ES7243E_ADDR 0x20
#define ES7243_ADDR  0x26

static const char *TAG = "AUDIO_BOARD";

static audio_board_handle_t board_handle = 0;

static int audio_board_probe_adc(uint8_t addr)
{
    static i2c_bus_handle_t i2c_handle = NULL;
    int res = 0;

    if (i2c_handle == NULL) {
        i2c_config_t es_i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,
        };
        res = get_i2c_pins(I2C_NUM_0, &es_i2c_cfg);
        if (res != 0) {
            return res;
        }
        i2c_handle = i2c_bus_create(I2C_NUM_0, &es_i2c_cfg);
    }

    if (i2c_handle) {
        return i2c_bus_probe_addr(i2c_handle, addr);
    }
    return ESP_FAIL;
}

audio_board_handle_t audio_board_init(void)
{
    if (board_handle) {
        ESP_LOGW(TAG, "The board has already been initialized!");
        return board_handle;
    }
    board_handle = (audio_board_handle_t) audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, board_handle, return NULL);
    board_handle->audio_hal = audio_board_codec_init();
    // board_handle->adc_hal = audio_board_adc_init();
    return board_handle;
}

audio_hal_handle_t audio_board_adc_init(void)
{
    audio_hal_handle_t adc_hal = NULL;
    audio_hal_codec_config_t audio_codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    audio_codec_cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;

    // The new version of the esp32_lyrat_mini development board has replaced the ADC from ES7243 to ES7243E.
    // Check the current ADC in use and initialize it.
    if (audio_board_probe_adc(ES7243E_ADDR) == ESP_OK) {
        adc_hal = audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_ES7243E_DEFAULT_HANDLE);
    } else if (audio_board_probe_adc(ES7243_ADDR) == ESP_OK) {
        adc_hal = audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_ES7243_DEFAULT_HANDLE);
    }

    AUDIO_NULL_CHECK(TAG, adc_hal, return NULL);
    return adc_hal;
}

audio_hal_handle_t audio_board_codec_init(void)
{
    audio_hal_codec_config_t audio_codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    audio_hal_handle_t codec_hal = audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_ES8311_DEFAULT_HANDLE);
    AUDIO_NULL_CHECK(TAG, codec_hal, return NULL);
    return codec_hal;
}

display_service_handle_t audio_board_led_init(void)
{
    led_indicator_handle_t led = led_indicator_init((gpio_num_t)get_green_led_gpio());
    display_service_config_t display = {
        .based_cfg = {
            .task_stack = 0,
            .task_prio  = 0,
