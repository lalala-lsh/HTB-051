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
 * @brief 启动WiFi智能重连机制
 *
 * 创建并启动周期扫描定时器,通过扫描检测目标WiFi是否可用
 */
static void start_wifi_smart_reconnect(void)
{
    /* 创建扫描定时器(如果未创建) */
    if (wifi_scan_timer == NULL) {
        wifi_scan_timer = xTimerCreate("wifi_scan",
                                        pdMS_TO_TICKS(WIFI_SCAN_INTERVAL_MS),
                                        pdTRUE,  // 自动重载
                                        NULL,
                                        wifi_scan_timer_callback);

        if (wifi_scan_timer == NULL) {
            BLUFI_ERROR("Failed to create WiFi scan timer\n");
            return;
        }
    }

    /* 重置扫描状态 */
    wifi_scan_attempt_count = 0;
    wifi_is_scanning_for_reconnect = false;

    /* 启动定时器 */
    if (xTimerStart(wifi_scan_timer, 0) == pdPASS) {
        BLUFI_INFO("Smart WiFi reconnect started (scan every %d seconds, max %d attempts)\n",
                   WIFI_SCAN_INTERVAL_MS / 1000, WIFI_SCAN_MAX_ATTEMPTS);
    }
    else {
        BLUFI_ERROR("Failed to start WiFi scan timer\n");
    }
}

/**
 * @brief 停止WiFi智能重连机制
 */
static void stop_wifi_smart_reconnect(void)
{
    if (wifi_scan_timer != NULL) {
        xTimerStop(wifi_scan_timer, 0);
    }
    wifi_scan_attempt_count = 0;
    wifi_is_scanning_for_reconnect = false;
}

static int softap_get_current_connection_number(void)
{
    esp_err_t ret;
    ret = esp_wifi_ap_get_sta_list(&gl_sta_list);
    if (ret == ESP_OK) {
        return gl_sta_list.num;
    }

    return 0;
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                             void* event_data)
{
    wifi_mode_t mode;

    switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            esp_blufi_extra_info_t info;

            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            mqtt_client_start();
            esp_wifi_get_mode(&mode);

            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            gl_sta_got_ip = true;
            if (ble_is_connected == true) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS,
                                                softap_get_current_connection_number(), &info);
            }
            else {
                BLUFI_INFO("BLUFI BLE is not connected yet\n");
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            blufi_deinit();
            break;
        }
        default:
            break;
    }
    return;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                               void* event_data)
{
    wifi_event_sta_connected_t* event;
    wifi_event_sta_disconnected_t* disconnected_event;
    wifi_mode_t mode;

    switch (event_id) {
        case WIFI_EVENT_STA_START:
            example_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            gl_sta_connected = true;
            gl_sta_is_connecting = false;
            event = (wifi_event_sta_connected_t*)event_data;
            memcpy(gl_sta_bssid, event->bssid, 6);
            memset(gl_sta_ssid, 0, sizeof(gl_sta_ssid));  // 先清零，避免旧数据残留
            memcpy(gl_sta_ssid, event->ssid, event->ssid_len);
            gl_sta_ssid_len = event->ssid_len;

            /* WiFi连接成功,停止智能重连 */
            stop_wifi_smart_reconnect();
            BLUFI_INFO("WiFi connected successfully to '%s'\n", gl_sta_ssid);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            disconnected_event = (wifi_event_sta_disconnected_t*)event_data;
            BLUFI_INFO("WiFi disconnected, reason=%d\n", disconnected_event->reason);

            /* 区分初次连接和运行期间断线 */
            if (gl_sta_connected == false) {
                /* 初次连接阶段,使用快速重连机制 */
                if (example_wifi_reconnect() == false) {
                    gl_sta_is_connecting = false;
                    example_record_wifi_conn_info(disconnected_event->rssi,
                                                  disconnected_event->reason);
                }
            }
            else {
                /* 运行期间断线:
                 * 1. 启动智能重连(周期扫描原WiFi)
                 * 2. 同时重新开启BluFi(允许用户配置新WiFi)
                 */
                BLUFI_INFO("WiFi connection lost, starting smart reconnect and BluFi...\n");
                start_wifi_smart_reconnect();

                /* 重新初始化BluFi,允许用户配置新的WiFi */
                blufi_reinit();
            }

            /* 清除连接状态 */
            gl_sta_connected = false;
            gl_sta_got_ip = false;
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            mqtt_client_stop();

            /* 记录断连信息 */
            example_record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);
            break;
        case WIFI_EVENT_AP_START:
            esp_wifi_get_mode(&mode);

            /* TODO: get config or information of softap, then set to report extra_info */
            if (ble_is_connected == true) {
                if (gl_sta_connected) {
                    esp_blufi_extra_info_t info;
                    memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                    memcpy(info.sta_bssid, gl_sta_bssid, 6);
                    info.sta_bssid_set = true;
                    info.sta_ssid = gl_sta_ssid;
                    info.sta_ssid_len = gl_sta_ssid_len;
                    esp_blufi_send_wifi_conn_report(
                        mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP,
                        softap_get_current_connection_number(), &info);
                }
                else if (gl_sta_is_connecting) {
                    esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING,
                                                    softap_get_current_connection_number(),
                                                    &gl_sta_conn_info);
                }
                else {
                    esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                                    softap_get_current_connection_number(),
                                                    &gl_sta_conn_info);
                }
            }
            else {
                BLUFI_INFO("BLUFI BLE is not connected yet\n");
            }
            break;
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t apCount = 0;
            esp_wifi_scan_get_ap_num(&apCount);

            /* 检查是否是智能重连的扫描 */
            if (wifi_is_scanning_for_reconnect) {
                /* 先停止扫描,再处理结果 */
                esp_wifi_scan_stop();
                wifi_is_scanning_for_reconnect = false;

                /* 处理智能重连扫描结果 */
                if (apCount > 0) {
                    wifi_ap_record_t* ap_list =
                        (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apCount);
                    if (ap_list) {
                        esp_wifi_scan_get_ap_records(&apCount, ap_list);
                        handle_reconnect_scan_done(apCount, ap_list);
                        free(ap_list);
                    }
                    else {
                        BLUFI_ERROR("Failed to allocate memory for reconnect scan results\n");
                    }
                }
                else {
                    BLUFI_INFO("No AP found in reconnect scan\n");
                    wifi_scan_attempt_count++;
                }
                break;
            }

            /* 原有的BluFi扫描逻辑 */
            if (apCount == 0) {
                BLUFI_INFO("Nothing AP found");
                break;
            }
            wifi_ap_record_t* ap_list =
                (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apCount);
            if (!ap_list) {
                BLUFI_ERROR("malloc error, ap_list is NULL");
                esp_wifi_clear_ap_list();
                break;
            }
            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
            esp_blufi_ap_record_t* blufi_ap_list =
                (esp_blufi_ap_record_t*)malloc(apCount * sizeof(esp_blufi_ap_record_t));
            if (!blufi_ap_list) {
                if (ap_list) {
                    free(ap_list);
                }
                BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
                break;
            }
            for (int i = 0; i < apCount; ++i) {
                blufi_ap_list[i].rssi = ap_list[i].rssi;
                memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
            }

            if (ble_is_connected == true) {
                esp_blufi_send_wifi_list(apCount, blufi_ap_list);
            }
            else {
                BLUFI_INFO("BLUFI BLE is not connected yet\n");
            }

            esp_wifi_scan_stop();
            free(ap_list);
            free(blufi_ap_list);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
            BLUFI_INFO("station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
            BLUFI_INFO("station " MACSTR " leave, AID=%d, reason=%d", MAC2STR(event->mac),
                       event->aid, event->reason);
            break;
        }

        default:
            break;
    }
    return;
}

static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param)
{
    /* actually, should post to blufi_task handle the procedure,
     * now, as a example, we do it more simply */
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            BLUFI_INFO("BLUFI init finish\n");

            mine_esp_blufi_adv_start();
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            BLUFI_INFO("BLUFI deinit finish\n");
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            BLUFI_INFO("BLUFI ble connect\n");
            ble_is_connected = true;
            esp_blufi_adv_stop();
            blufi_security_init();

            /* 发送SN码给客户端 */
            {
                /* 获取设备SN码 */
                const char* device_sn = get_device_sn();

                /* 等待连接稳定 */
                vTaskDelay(pdMS_TO_TICKS(1000));

                /* 计算SN实际长度(最大16字节,去除末尾'\0') */
                size_t sn_len = strnlen(device_sn, 16);

                /* 发送自定义数据(SN码) */
                esp_err_t ret = esp_blufi_send_custom_data((uint8_t*)device_sn, sn_len);
                if (ret == ESP_OK) {
                    BLUFI_INFO("SN code sent successfully: %.*s (len=%zu)\n", (int)sn_len,
                               device_sn, sn_len);
                }
                else {
                    BLUFI_ERROR("Failed to send SN code: %s\n", esp_err_to_name(ret));
                }
            }

            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            BLUFI_INFO("BLUFI ble disconnect\n");
            ble_is_connected = false;
            // blufi_security_deinit();

            /* 只有在非去初始化状态下才重启广播 */
            if (!blufi_is_deinitializing) {
                mine_esp_blufi_adv_start();
            } else {
                BLUFI_INFO("Deinitializing, skip restarting advertising\n");
            }
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
            BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
            ESP_ERROR_CHECK(esp_wifi_set_mode(param->wifi_mode.op_mode));
            break;
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
            BLUFI_INFO("BLUFI requset wifi connect to AP\n");
            /* there is no wifi callback when the device has already connected to this wifi
            so disconnect wifi before connection.
            */
            esp_wifi_disconnect();
            example_wifi_connect();
            break;
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            BLUFI_INFO("BLUFI requset wifi disconnect from AP\n");
            esp_wifi_disconnect();
            break;
        case ESP_BLUFI_EVENT_REPORT_ERROR:
            BLUFI_ERROR("BLUFI report error, error code %d\n", param->report_error.state);
            esp_blufi_send_error_info(param->report_error.state);
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
            wifi_mode_t mode;
            esp_blufi_extra_info_t info;

            esp_wifi_get_mode(&mode);

            if (gl_sta_connected) {
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, gl_sta_bssid, 6);
                info.sta_bssid_set = true;
                info.sta_ssid = gl_sta_ssid;
                info.sta_ssid_len = gl_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(
                    mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP,
                    softap_get_current_connection_number(), &info);
            }
            else if (gl_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING,
                                                softap_get_current_connection_number(),
                                                &gl_sta_conn_info);
            }
            else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                                softap_get_current_connection_number(),
                                                &gl_sta_conn_info);
            }
            BLUFI_INFO("BLUFI get wifi status from AP\n");

            break;
        }
        case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
            BLUFI_INFO("blufi close a gatt connection");
            esp_blufi_disconnect();
            break;
        case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
            /* TODO */
            break;
        case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            sta_config.sta.bssid_set = 1;
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
            BLUFI_INFO("Recv STA BSSID %s\n", sta_config.sta.ssid);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_SSID:
            /* 收到新WiFi配置，停止智能重连（如果正在运行） */
            stop_wifi_smart_reconnect();

            strncpy((char*)sta_config.sta.ssid, (char*)param->sta_ssid.ssid,
                    param->sta_ssid.ssid_len);
            sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);

            /* 更新目标SSID用于后续可能的重连 */
            memset(gl_sta_ssid, 0, sizeof(gl_sta_ssid));
            memcpy(gl_sta_ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
            gl_sta_ssid_len = param->sta_ssid.ssid_len;

            BLUFI_INFO("Recv STA SSID %s\n", sta_config.sta.ssid);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
            strncpy((char*)sta_config.sta.password, (char*)param->sta_passwd.passwd,
                    param->sta_passwd.passwd_len);
            sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
            BLUFI_INFO("Recv STA PASSWORD %s\n", sta_config.sta.password);
            break;
        case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
            strncpy((char*)ap_config.ap.ssid, (char*)param->softap_ssid.ssid,
                    param->softap_ssid.ssid_len);
            ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
            ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            BLUFI_INFO("Recv SOFTAP SSID %s, ssid len %d\n", ap_config.ap.ssid,
                       ap_config.ap.ssid_len);
            break;
        case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
            strncpy((char*)ap_config.ap.password, (char*)param->softap_passwd.passwd,
                    param->softap_passwd.passwd_len);
            ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            BLUFI_INFO("Recv SOFTAP PASSWORD %s len = %d\n", ap_config.ap.password,
                       param->softap_passwd.passwd_len);
            break;
        case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
            if (param->softap_max_conn_num.max_conn_num > 4) {
                return;
            }
            ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            BLUFI_INFO("Recv SOFTAP MAX CONN NUM %d\n", ap_config.ap.max_connection);
            break;
        case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
            if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX) {
                return;
            }
            ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            BLUFI_INFO("Recv SOFTAP AUTH MODE %d\n", ap_config.ap.authmode);
            break;
        case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
            if (param->softap_channel.channel > 13) {
                return;
            }
            ap_config.ap.channel = param->softap_channel.channel;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            BLUFI_INFO("Recv SOFTAP CHANNEL %d\n", ap_config.ap.channel);
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
            wifi_scan_config_t scanConf = {
                .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false};
            esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
            if (ret != ESP_OK) {
                esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
            }
            break;
        }
        case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
            BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
            esp_log_buffer_hex("Custom Data", param->custom_data.data, param->custom_data.data_len);
            break;
        case ESP_BLUFI_EVENT_RECV_USERNAME:
            /* Not handle currently */
            break;
        case ESP_BLUFI_EVENT_RECV_CA_CERT:
            /* Not handle currently */
            break;
        case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
            /* Not handle currently */
            break;
        case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
            /* Not handle currently */
            break;
        case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
            /* Not handle currently */
            break;
            ;
        case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
            /* Not handle currently */
            break;
        default:
            break;
    }
}

void blufi_init(void)
{
    esp_err_t ret;

    initialise_wifi();

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret) {
        BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
#endif

    ret = esp_blufi_host_and_cb_init(&example_callbacks);
    if (ret) {
        BLUFI_ERROR("%s initialise failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    BLUFI_INFO("BLUFI VERSION %04x\n", esp_blufi_get_version());
}

/**
 * @brief 反初始化BluFi,关闭所有蓝牙功能,保留WiFi功能
 *
 * 该函数按照正确的顺序清理BluFi和蓝牙资源:
 * 1. 停止BluFi广播
 * 2. 反初始化BluFi主机协议栈(Bluedroid或NimBLE)
 * 3. 反初始化蓝牙控制器
 * 4. 释放蓝牙控制器内存
 *
 * @note WiFi功能和事件处理器会被保留,继续正常工作
 * @return esp_err_t ESP_OK表示成功,其他值表示失败
 */
esp_err_t blufi_deinit(void)
{
    esp_err_t ret = ESP_OK;

    BLUFI_INFO("Starting BluFi deinitialization...\n");

    /* 设置去初始化标志，防止断开回调中重启广播 */
    blufi_is_deinitializing = true;

    /* 步骤1: 先停止广播，防止新连接建立 */
    BLUFI_INFO("Stopping BluFi advertising...\n");
    esp_blufi_adv_stop();
    /* 等待广播停止 */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 步骤2: 如果BLE已连接,断开所有连接 */
    if (ble_is_connected) {
        BLUFI_INFO("Disconnecting BLE connection...\n");
        esp_blufi_disconnect();
        /* 等待断开事件完成，最多等待3秒 */
        int wait_count = 0;
        while (ble_is_connected && wait_count < 30) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }

        if (ble_is_connected) {
            BLUFI_ERROR("BLE disconnect timeout after 3s, force continue...\n");
            /* 强制重置标志 */
            ble_is_connected = false;
        } else {
            BLUFI_INFO("BLE disconnected successfully\n");
        }
    }

    /* 步骤3: 额外延时1秒，确保底层HCI完全断开 */
    BLUFI_INFO("Waiting for HCI layer cleanup...\n");
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 步骤4: 反初始化BluFi host (Bluedroid或NimBLE) */
    BLUFI_INFO("Deinitializing BluFi host stack...\n");
    ret = esp_blufi_host_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("BluFi host deinit failed: %s\n", esp_err_to_name(ret));
        /* 继续执行,尝试清理控制器 */
    }
    else {
        BLUFI_INFO("BluFi host deinitialized successfully\n");
    }

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    /* 步骤5: 反初始化蓝牙控制器 */
    BLUFI_INFO("Deinitializing BT controller...\n");
    ret = esp_blufi_controller_deinit();
