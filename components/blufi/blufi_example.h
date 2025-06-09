/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */


#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#if CONFIG_BT_ENABLED
#include "esp_blufi_api.h"
#endif

#define BLUFI_EXAMPLE_TAG "BLUFI_EXAMPLE"
#define BLUFI_INFO(fmt, ...)   ESP_LOGI(BLUFI_EXAMPLE_TAG, fmt, ##__VA_ARGS__)
#define BLUFI_ERROR(fmt, ...)  ESP_LOGE(BLUFI_EXAMPLE_TAG, fmt, ##__VA_ARGS__)

void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

int blufi_security_init(void);
void blufi_security_deinit(void);
int esp_blufi_gap_register_callback(void);
esp_err_t esp_blufi_host_init(void);
esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *callbacks);
esp_err_t esp_blufi_host_deinit(void);
esp_err_t esp_blufi_controller_init(void);
esp_err_t esp_blufi_controller_deinit(void);

/**
 * @brief 初始化BluFi和WiFi
 *
 * 该函数执行以下操作:
 * 1. 初始化NVS Flash
 * 2. 初始化WiFi
 * 3. 初始化蓝牙控制器
 * 4. 初始化BluFi主机协议栈和回调函数
 */
void blufi_init(void);

/**
 * @brief 反初始化BluFi,关闭所有蓝牙功能,保留WiFi功能
 *
 * 该函数按照正确的顺序清理BluFi和蓝牙资源:
 * 1. 断开BLE连接(如果已连接)
 * 2. 停止BluFi广播
 * 3. 反初始化BluFi主机协议栈(Bluedroid或NimBLE)
 * 4. 反初始化蓝牙控制器
 * 5. 释放蓝牙控制器内存
 *
 * @note WiFi功能和事件处理器会被保留,继续正常工作
 * @return esp_err_t ESP_OK表示成功,其他值表示失败
 */
esp_err_t blufi_deinit(void);

/**
 * @brief 重新初始化BluFi进行配网
 *
 * 该函数用于在blufi_deinit()后重新启动BluFi进行配网。
 * 执行步骤:
 * 1. 重新申请蓝牙控制器内存(ESP32特有)
 * 2. 初始化蓝牙控制器
 * 3. 初始化BluFi主机协议栈和回调
 * 4. 启动BluFi广播
 *
 * @note 该函数假定WiFi已经初始化(通过blufi_init()或其他方式)
 * @return esp_err_t ESP_OK表示成功,其他值表示失败
 */
esp_err_t blufi_reinit(void);

EventGroupHandle_t get_wifi_event_group(void);
