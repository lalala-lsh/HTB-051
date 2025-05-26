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
