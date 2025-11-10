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
            ESP_LOGI(TAG, "ACL connection complete: status=%d", param->acl_conn_cmpl_stat.stat);
            break;

        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            ESP_LOGI(TAG, "ACL disconnection complete");
            break;

        default:
            break;
    }
}

/**
 * @brief A2DP 连接状态回调
 */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected");
                g_a2dp_sink.state = A2DP_SINK_STATE_CONNECTED;
                trigger_event(A2DP_SINK_EVENT_CONNECTED, NULL);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP disconnected");
                g_a2dp_sink.state = A2DP_SINK_STATE_INITIALIZED;
                trigger_event(A2DP_SINK_EVENT_DISCONNECTED, NULL);
            }
            break;

        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP audio started");
                trigger_event(A2DP_SINK_EVENT_AUDIO_START, NULL);
            } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "A2DP audio stopped");
                trigger_event(A2DP_SINK_EVENT_AUDIO_STOP, NULL);
            }
            break;

        default:
            break;
    }
}

/**
 * @brief 确保BT协议栈已启动（仅首次调用时初始化，之后保持活跃）
 */
static esp_err_t ensure_bt_stack_up(void)
{
    if (g_a2dp_sink.bt_stack_up) {
        ESP_LOGI(TAG, "BT stack already up, reusing");
        return ESP_OK;
    }

    /* BT Controller */
    esp_bt_controller_status_t ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGI(TAG, "BT Controller not enabled, initializing...");
        if (ctrl_status == ESP_BT_CONTROLLER_STATUS_INITED) {
            esp_bt_controller_deinit();
        }
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_bt_controller_init(&bt_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }
        ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
            esp_bt_controller_deinit();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "BT Controller initialized in BTDM mode");
    }

    /* Bluedroid */
    esp_bluedroid_status_t bd_status = esp_bluedroid_get_status();
    if (bd_status != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGI(TAG, "Bluedroid not enabled, initializing...");
        if (bd_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
            esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
            bluedroid_cfg.ssp_en = true;
            esp_err_t ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
                return ESP_FAIL;
            }
        }
        esp_err_t ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Bluedroid initialized and enabled with SSP");
    }

    /* SSP */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));

    /* GAP callback */
    esp_bt_gap_register_callback(bt_app_gap_cb);

    /* 默认不可发现 */
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    g_a2dp_sink.bt_stack_up = true;
    ESP_LOGI(TAG, "BT stack is up and ready");
    return ESP_OK;
}

esp_err_t a2dp_sink_init(void)
{
    if (g_a2dp_sink.initialized) {
        ESP_LOGW(TAG, "A2DP Sink already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing A2DP Sink...");

    /* 获取音频板句柄 */
    g_a2dp_sink.board_handle = audio_board_init();
    if (g_a2dp_sink.board_handle == NULL) {
        ESP_LOGE(TAG, "Failed to init audio board");
        return ESP_FAIL;
    }
    audio_hal_ctrl_codec(g_a2dp_sink.board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    /* 确保BT协议栈已启动（首次init时启动，后续复用） */
    esp_err_t ret = ensure_bt_stack_up();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 设置设备名称 */
    const char *device_name = get_device_name();
    esp_bt_gap_set_device_name(device_name);
    ESP_LOGI(TAG, "Bluetooth device name set: %s", device_name);

    /* 预初始化AVRC CT/TG，确保a2dp_stream_init内部的A2DP init能正确关联AVCTP协议。
     * 异步操作，需要yield让BTC任务处理。 */
    esp_avrc_ct_init();
    esp_avrc_tg_init();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 创建音频管道 */
    ESP_LOGI(TAG, "Creating audio pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    g_a2dp_sink.pipeline = audio_pipeline_init(&pipeline_cfg);
    if (g_a2dp_sink.pipeline == NULL) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        return ESP_FAIL;
    }

    /* 创建A2DP Sink流 */
    ESP_LOGI(TAG, "Creating A2DP sink stream");
    a2dp_stream_config_t a2dp_config = {
        .type = AUDIO_STREAM_READER,
        .user_callback = {
            .user_a2d_cb = bt_app_a2d_cb,
            .user_a2d_sink_data_cb = NULL,
        },
        .audio_hal = g_a2dp_sink.board_handle->audio_hal,
    };
    g_a2dp_sink.a2dp_stream = a2dp_stream_init(&a2dp_config);
    if (g_a2dp_sink.a2dp_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create A2DP stream");
        audio_pipeline_deinit(g_a2dp_sink.pipeline);
        return ESP_FAIL;
    }

    /* 设置可发现模式 */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    ESP_LOGI(TAG, "Scan mode set: CONNECTABLE + GENERAL_DISCOVERABLE");

    /* 创建I2S流 */
    ESP_LOGI(TAG, "Creating I2S stream");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    g_a2dp_sink.i2s_stream = i2s_stream_init(&i2s_cfg);
    if (g_a2dp_sink.i2s_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create I2S stream");
        audio_element_deinit(g_a2dp_sink.a2dp_stream);
        audio_pipeline_deinit(g_a2dp_sink.pipeline);
        return ESP_FAIL;
    }

    /* 注册元素到管道并链接 */
    audio_pipeline_register(g_a2dp_sink.pipeline, g_a2dp_sink.a2dp_stream, "a2dp");
    audio_pipeline_register(g_a2dp_sink.pipeline, g_a2dp_sink.i2s_stream, "i2s");
    const char *link_tag[2] = {"a2dp", "i2s"};
    audio_pipeline_link(g_a2dp_sink.pipeline, &link_tag[0], 2);

    /* 创建事件监听器 */
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    g_a2dp_sink.evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(g_a2dp_sink.pipeline, g_a2dp_sink.evt);

    g_a2dp_sink.state = A2DP_SINK_STATE_INITIALIZED;
    g_a2dp_sink.initialized = true;

    ESP_LOGI(TAG, "A2DP Sink initialized successfully");
    return ESP_OK;
}

esp_err_t a2dp_sink_deinit(void)
{
    if (!g_a2dp_sink.initialized) {
        ESP_LOGW(TAG, "A2DP Sink not initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing A2DP Sink...");

    /* 清除事件回调，防止断开事件触发重入 */
    g_a2dp_sink.event_callback = NULL;

    /* 停止任务 */
    if (g_a2dp_sink.task_handle != NULL) {
        vTaskDelete(g_a2dp_sink.task_handle);
        g_a2dp_sink.task_handle = NULL;
    }

    /* 停止管道 */
    if (g_a2dp_sink.pipeline) {
        audio_pipeline_stop(g_a2dp_sink.pipeline);
        audio_pipeline_wait_for_stop(g_a2dp_sink.pipeline);
        audio_pipeline_terminate(g_a2dp_sink.pipeline);

        if (g_a2dp_sink.evt) {
            audio_pipeline_remove_listener(g_a2dp_sink.pipeline);
            audio_event_iface_destroy(g_a2dp_sink.evt);
            g_a2dp_sink.evt = NULL;
        }

        if (g_a2dp_sink.a2dp_stream) {
            audio_pipeline_unregister(g_a2dp_sink.pipeline, g_a2dp_sink.a2dp_stream);
            audio_element_deinit(g_a2dp_sink.a2dp_stream);
            g_a2dp_sink.a2dp_stream = NULL;
        }
        if (g_a2dp_sink.i2s_stream) {
            audio_pipeline_unregister(g_a2dp_sink.pipeline, g_a2dp_sink.i2s_stream);
            audio_element_deinit(g_a2dp_sink.i2s_stream);
            g_a2dp_sink.i2s_stream = NULL;
        }

        audio_pipeline_deinit(g_a2dp_sink.pipeline);
        g_a2dp_sink.pipeline = NULL;
    }

    /* 清理AVRC配置文件（a2dp_stream destroy只清A2DP不清AVRC）*/
    esp_avrc_ct_deinit();
    esp_avrc_tg_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* BT协议栈保持活跃，仅设置为不可发现 */
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    g_a2dp_sink.state = A2DP_SINK_STATE_IDLE;
    g_a2dp_sink.initialized = false;

    ESP_LOGI(TAG, "A2DP Sink deinitialized (BT stack kept alive)");
    return ESP_OK;
}

esp_err_t a2dp_sink_start(void)
{
    if (!g_a2dp_sink.initialized) {
        ESP_LOGE(TAG, "A2DP Sink not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting A2DP Sink...");

    const char *device_name = get_device_name();
    esp_bt_gap_set_device_name(device_name);
    ESP_LOGI(TAG, "Bluetooth device name set: %s", device_name);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    ESP_LOGI(TAG, "Scan mode set: CONNECTABLE + GENERAL_DISCOVERABLE");

    esp_err_t ret = audio_pipeline_run(g_a2dp_sink.pipeline);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to run audio pipeline: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    if (g_a2dp_sink.task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(a2dp_sink_task, "a2dp_sink_task", 4096, NULL, 10, &g_a2dp_sink.task_handle);
        if (task_ret != pdPASS) {
