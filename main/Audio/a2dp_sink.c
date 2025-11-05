/**
 * @file a2dp_sink.c
 * @brief A2DP Sink (蓝牙音箱) 模块实现
 *
 * 基于ESP-ADF的a2dp_sink_stream实现蓝牙音箱功能
 *
 * 设计要点：BT协议栈(bluedroid + controller)在首次init时启动，之后保持活跃不拆除。
 * 仅管理音频管道和A2DP/AVRC配置文件的创建/销毁。避免esp_bluedroid_disable()死锁问题。
 */

#include "a2dp_sink.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "esp_peripherals.h"
#include "a2dp_stream.h"
#include "board.h"

#include "system_info.h"

static const char *TAG = "A2DP_SINK";

/* A2DP Sink 全局状态 */
static struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t a2dp_stream;
    audio_element_handle_t i2s_stream;
    audio_event_iface_handle_t evt;
    audio_board_handle_t board_handle;

    a2dp_sink_state_t state;
    a2dp_sink_event_cb_t event_callback;
    bool initialized;
    bool bt_stack_up;
    TaskHandle_t task_handle;
} g_a2dp_sink = {0};

/* 前向声明 */
static void a2dp_sink_task(void *pvParameters);
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/**
 * @brief 触发事件回调
 */
static void trigger_event(a2dp_sink_event_t event, void *param)
{
    if (g_a2dp_sink.event_callback != NULL) {
        g_a2dp_sink.event_callback(event, param);
    }
}

/**
 * @brief 经典蓝牙 GAP 回调 - 处理发现、配对等事件
 */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "SSP confirmation request, auto-accepting...");
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "SSP passkey: %"PRIu32, param->key_notif.passkey);
            break;

        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGW(TAG, "Unexpected SSP key request");
            break;

        case ESP_BT_GAP_MODE_CHG_EVT:
            ESP_LOGI(TAG, "GAP mode changed: mode=%d", param->mode_chg.mode);
            break;

        case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
