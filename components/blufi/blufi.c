/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
 * This is a demo for bluetooth config wifi connection to ap. You can config ESP32 to connect a
 * softap or config ESP32 as a softap to be connected by other device. APP can be downloaded from
 * github android source code: https://github.com/EspressifApp/EspBlufi iOS source code:
 * https://github.com/EspressifApp/EspBlufiForiOS
 ****************************************************************************/

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "blufi_example.h"
#include "esp_blufi_api.h"
#include "mqtt_manmager.h"

#include "esp_blufi.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#endif

#include "system_info.h"

#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY CONFIG_EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY
#define EXAMPLE_INVALID_REASON                255
#define EXAMPLE_INVALID_RSSI                  -128

/* WiFi智能重连配置 */
#define WIFI_SCAN_INTERVAL_MS       30000  // 扫描间隔30秒
#define WIFI_SCAN_MAX_ATTEMPTS      20     // 最多扫描20次(10分钟)
#define WIFI_SCAN_ACTIVE_MIN_MS     100    // 主动扫描最小时长
#define WIFI_SCAN_ACTIVE_MAX_MS     300    // 主动扫描最大时长

static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param);

/* 前向声明blufi_reinit,供WiFi重连失败时调用 */
esp_err_t blufi_reinit(void);

#define WIFI_LIST_NUM 10

static wifi_config_t sta_config;
static wifi_config_t ap_config;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static uint8_t example_wifi_retry = 0;

/* store the station info for send back to phone */
static bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;

/* BluFi去初始化状态标志 */
static bool blufi_is_deinitialized = false;
static bool blufi_is_deinitializing = false;  // 防止断开回调中重启广播
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
static bool gl_sta_is_connecting = false;
static esp_blufi_extra_info_t gl_sta_conn_info;

/* WiFi智能重连相关全局变量 */
static TimerHandle_t wifi_scan_timer = NULL;  // 扫描定时器
static uint8_t wifi_scan_attempt_count = 0;   // 扫描尝试次数
static bool wifi_is_scanning_for_reconnect = false;  // 标识是否在进行重连扫描

static uint8_t blufi_service_uuid128[32] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    // first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
};

static esp_ble_adv_data_t blufi_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006, // slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval = 0x0010, // slave connection max interval, Time = max_interval * 1.25 msec
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 16,
    .p_service_uuid = blufi_service_uuid128,
    .flag = 0x6,
};

EventGroupHandle_t get_wifi_event_group(void)
{
    return wifi_event_group;
}

/**
 * @brief 启动BluFi广播,使用系统信息中的设备名称
 *
 * 该函数在BluFi初始化完成时调用,会设置设备名称并配置广播数据
 * 支持Bluedroid和NimBLE两种协议栈
 */
static void mine_esp_blufi_adv_start(void)
{
    /* 获取设备名称 */
    const char* device_name = get_device_name();

#if CONFIG_BT_BLUEDROID_ENABLED
    /* Bluedroid: 使用esp_ble_gap_set_device_name设置 */
    esp_err_t ret = esp_ble_gap_set_device_name(device_name);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to set device name: %s\n", esp_err_to_name(ret));
    }
    else {
        BLUFI_INFO("Device name set to: %s\n", device_name);
    }

    /* 配置广播数据 */
    ret = esp_ble_gap_config_adv_data(&blufi_adv_data);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to config adv data: %s\n", esp_err_to_name(ret));
    }
#elif CONFIG_BT_NIMBLE_ENABLED
    /* NimBLE: 使用ble_svc_gap_device_name_set设置 */
    int rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        BLUFI_ERROR("Failed to set device name for NimBLE: %d\n", rc);
    }
    else {
        BLUFI_INFO("Device name set to: %s (NimBLE)\n", device_name);
    }
#endif

    BLUFI_INFO("BluFi advertising initialized with device name: %s\n", device_name);
}

static void example_record_wifi_conn_info(int rssi, uint8_t reason)
{
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (gl_sta_is_connecting) {
        gl_sta_conn_info.sta_max_conn_retry_set = true;
        gl_sta_conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    }
    else {
        gl_sta_conn_info.sta_conn_rssi_set = true;
        gl_sta_conn_info.sta_conn_rssi = rssi;
        gl_sta_conn_info.sta_conn_end_reason_set = true;
        gl_sta_conn_info.sta_conn_end_reason = reason;
    }
}

static void example_wifi_connect(void)
{
    example_wifi_retry = 0;
    gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
}

static bool example_wifi_reconnect(void)
{
    bool ret;
    if (gl_sta_is_connecting && example_wifi_retry++ < EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY) {
        BLUFI_INFO("BLUFI WiFi starts reconnection\n");
        gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
        example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
        ret = true;
    }
    else {
        ret = false;
    }
    return ret;
}

/**
 * @brief 处理重连扫描结果,查找目标WiFi并尝试连接
 *
 * @param ap_count 扫描到的AP数量
 * @param ap_list AP列表
 */
static void handle_reconnect_scan_done(uint16_t ap_count, wifi_ap_record_t* ap_list)
{
    /* 遍历扫描结果,查找目标SSID */
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(ap_list[i].ssid, gl_sta_ssid, gl_sta_ssid_len) == 0) {
            BLUFI_INFO("Target WiFi '%s' found! RSSI=%d, attempting to connect...\n",
                       gl_sta_ssid, ap_list[i].rssi);

            /* 找到目标AP,立即尝试连接 */
            BLUFI_INFO("Calling esp_wifi_connect()...\n");

            /* 先断开当前连接(如果有) */
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));  // 等待断开完成

            esp_err_t ret = esp_wifi_connect();
            BLUFI_INFO("esp_wifi_connect() returned: %s\n", esp_err_to_name(ret));

            if (ret == ESP_OK) {
                wifi_scan_attempt_count = 0;  // 重置扫描计数
                wifi_is_scanning_for_reconnect = false;

                /* 停止扫描定时器 */
                if (wifi_scan_timer != NULL) {
                    xTimerStop(wifi_scan_timer, 0);
                    BLUFI_INFO("Stopped scan timer after successful connect initiation\n");
                }
            }
            else {
                BLUFI_ERROR("Failed to connect to WiFi: %s\n", esp_err_to_name(ret));
            }
            return;
        }
    }

    /* 未找到目标AP */
    wifi_scan_attempt_count++;
    BLUFI_INFO("Target WiFi '%s' not found in scan (attempt %d/%d)\n",
               gl_sta_ssid, wifi_scan_attempt_count, WIFI_SCAN_MAX_ATTEMPTS);

    /* 检查是否达到最大尝试次数 */
    if (wifi_scan_attempt_count >= WIFI_SCAN_MAX_ATTEMPTS) {
        BLUFI_ERROR("WiFi reconnection failed after %d scans, starting BluFi for reconfiguration\n",
                    wifi_scan_attempt_count);

        /* 停止扫描定时器 */
        if (wifi_scan_timer != NULL) {
            xTimerStop(wifi_scan_timer, 0);
        }

        wifi_scan_attempt_count = 0;
        wifi_is_scanning_for_reconnect = false;

        /* 重新初始化BluFi,允许用户重新配网 */
        blufi_reinit();
    }
}

/**
 * @brief WiFi扫描定时器回调函数
 *
 * 周期性启动WiFi扫描,查找目标AP
 *
 * @param timer 定时器句柄
 */
static void wifi_scan_timer_callback(TimerHandle_t timer)
{
    /* 仅在未连接且有目标SSID时才扫描 */
    if (!gl_sta_connected && gl_sta_ssid_len > 0) {
        BLUFI_INFO("Scanning for target WiFi '%s'... (attempt %d/%d)\n",
                   gl_sta_ssid, wifi_scan_attempt_count + 1, WIFI_SCAN_MAX_ATTEMPTS);

        wifi_scan_config_t scan_config = {
            .ssid = gl_sta_ssid,  // 只扫描目标SSID,更快更省电
            .bssid = NULL,
            .channel = 0,  // 扫描所有信道
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = WIFI_SCAN_ACTIVE_MIN_MS,
            .scan_time.active.max = WIFI_SCAN_ACTIVE_MAX_MS};

        esp_err_t ret = esp_wifi_scan_start(&scan_config, false);  // 非阻塞扫描
        if (ret == ESP_OK) {
            wifi_is_scanning_for_reconnect = true;
        }
        else {
            BLUFI_ERROR("Failed to start WiFi scan: %s\n", esp_err_to_name(ret));
        }
    }
    else if (gl_sta_connected) {
        /* 如果已经连接,停止定时器 */
        if (wifi_scan_timer != NULL) {
            xTimerStop(wifi_scan_timer, 0);
        }
        wifi_is_scanning_for_reconnect = false;
    }
}

/**
