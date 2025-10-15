/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bh1750.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h" // for pdMS_TO_TICKS
#include "i2c_bus.h"
#include <esp_log.h>

#define TAG "bh1750"

#define BH_1750_MEASUREMENT_ACCURACY 1.2 /*!< the typical measurement accuracy of  BH1750 sensor   \
                                          */

#define BH1750_POWER_DOWN 0x00 /*!< Command to set Power Down*/
#define BH1750_POWER_ON   0x01 /*!< Command to set Power On*/
#define I2C_CLK_SPEED     400000


extern i2c_bus_handle_t i2c_handle;

// 写入命令到BH1750
static esp_err_t bh1750_write_byte(uint8_t cmd)
{
    esp_err_t ret = i2c_bus_write_bytes(i2c_handle, BH1750_I2C_ADDRESS_DEFAULT, NULL, 0, &cmd, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "发送命令0x%02x失败: %s", cmd, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bh1750_power_down()
{
    return bh1750_write_byte(BH1750_POWER_DOWN);
}

esp_err_t bh1750_power_on()
{
    return bh1750_write_byte(BH1750_POWER_ON);
}

esp_err_t bh1750_set_measure_time(const uint8_t measure_time)
{
    uint32_t i = 0;
    uint8_t buf[2] = {0x40, 0x60}; // constant part of the the MTreg
    buf[0] |= measure_time >> 5;
    buf[1] |= measure_time & 0x1F;
    for (i = 0; i < 2; i++) {
        esp_err_t ret = bh1750_write_byte(buf[i]);
        if (ESP_OK != ret) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t bh1750_set_measure_mode(const bh1750_measure_mode_t cmd_measure)
{
    return bh1750_write_byte((uint8_t)cmd_measure);
}

esp_err_t bh1750_get_data(float* const data)
{
    uint8_t read_buffer[2];
    esp_err_t ret = i2c_bus_read_bytes_directly(i2c_handle, BH1750_I2C_ADDRESS_DEFAULT, read_buffer, 2);
    if (ESP_OK != ret) {
        return ret;
    }
    *data = ((read_buffer[0] << 8 | read_buffer[1]) / BH_1750_MEASUREMENT_ACCURACY);
    return ESP_OK;
}
