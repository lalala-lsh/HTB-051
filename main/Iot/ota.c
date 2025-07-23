#include "ota.h"

#include <esp_log.h>

#include <stdbool.h>
#include <string.h>
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"

#include "audio_queue.h"
#include "tts_list.h"
#include "light_control.h"

static const char *TAG = "OTA";

static OTA_STATUS ota_status = OTA_STATUS_OFF;
static char ota_url[128] = {0};

/* ============ 私有函数声明 ============ */

static esp_err_t validate_image_header(esp_app_desc_t *new_app_info);
static void ota_task(void *pvParameter);
static void ota_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static void restore_outputs_after_early_failure(light_manager_t *manager);

/* ============ 私有函数实现 ============ */

/**
 * @brief 验证固件头部信息
 */
static esp_err_t validate_image_header(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;

    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "当前版本: %s", running_app_info.version);
    }

#ifndef CONFIG_EXAMPLE_SKIP_VERSION_CHECK
    if (memcmp(new_app_info->version, running_app_info.version, sizeof(new_app_info->version)) == 0) {
        ESP_LOGW(TAG, "版本相同，跳过更新");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "新版本: %s", new_app_info->version);
#endif

    return ESP_OK;
}

/**
 * @brief OTA 升级任务
 */
static void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "OTA任务启动, URL: %s", ota_url);

    /* OTA期间临时关闭当前输出，不改变用户保存状态 */
    light_manager_t* manager = get_light_manager();
    if (manager != NULL) {
        light_manager_suspend_outputs_for_ota(manager);
    }

    /* 获取灯光管理器并启动红光闪烁 */
    if (manager != NULL) {
        light_manager_start_red_blink(manager);
        ESP_LOGI(TAG, "红光闪烁已启动");
    }

    esp_http_client_config_t http_config = {
        .url = ota_url,
        .timeout_ms = 5000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA Begin 失败");
        ota_status = OTA_STATUS_FAIL;
        restore_outputs_after_early_failure(manager);
        vTaskDelete(NULL);
        return;
    }

    /* 验证固件头部 */
    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(ota_handle, &app_desc);
    if (err != ESP_OK || validate_image_header(&app_desc) != ESP_OK) {
        ESP_LOGE(TAG, "固件验证失败");
        esp_https_ota_abort(ota_handle);
        ota_status = OTA_STATUS_FAIL;
        restore_outputs_after_early_failure(manager);
        vTaskDelete(NULL);
        return;
    }

    /* 下载固件 */
    while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        ESP_LOGD(TAG, "已下载: %d bytes", esp_https_ota_get_image_len_read(ota_handle));
    }

    /* 完成升级 */
    if (esp_https_ota_is_complete_data_received(ota_handle)) {
        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA升级成功，即将重启");
            ota_status = OTA_STATUS_SUCCESS;
            if (manager != NULL) {
                light_manager_stop_red_blink(manager);
            }
            audio_queue_play(OTA_SUCCESS, AUDIO_TYPE_OTA, AUDIO_PRIORITY_URGENT, false);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    }

    ESP_LOGE(TAG, "OTA升级失败");
    if (manager != NULL) {
        light_manager_stop_red_blink(manager);
    }
    audio_queue_play(OTA_FAIL, AUDIO_TYPE_OTA, AUDIO_PRIORITY_URGENT, false);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_https_ota_abort(ota_handle);
    esp_restart();
    ota_status = OTA_STATUS_FAIL;
    vTaskDelete(NULL);
}

/**
 * @brief OTA 事件处理
 */
static void ota_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ESP_HTTPS_OTA_START:
            ESP_LOGI(TAG, "OTA 开始");
            audio_queue_play(OTA_UPDATE, AUDIO_TYPE_OTA, AUDIO_PRIORITY_URGENT, false);
            ota_status = OTA_STATUS_ING;
            break;
        case ESP_HTTPS_OTA_CONNECTED:
            ESP_LOGI(TAG, "已连接服务器");
            break;
        case ESP_HTTPS_OTA_FINISH:
            ESP_LOGI(TAG, "OTA 完成");
            ota_status = OTA_STATUS_SUCCESS;
            break;
        case ESP_HTTPS_OTA_ABORT:
            ESP_LOGE(TAG, "OTA 中断");
            ota_status = OTA_STATUS_FAIL;
            break;
        default:
            break;
    }
}

static void restore_outputs_after_early_failure(light_manager_t *manager)
{
    if (manager != NULL) {
        light_manager_stop_red_blink(manager);
        light_manager_restore_saved_brightness(manager);
    }
    light_control_set_buttons_enabled(true);
}

/* ============ 公开接口 ============ */

esp_err_t ota_start(const char *url)
{
    if (url == NULL) {
        ESP_LOGE(TAG, "URL 为空");
        return ESP_ERR_INVALID_ARG;
    }

    /* 复制URL到全局变量 */
    strncpy(ota_url, url, sizeof(ota_url) - 1);
    ota_url[sizeof(ota_url) - 1] = '\0';  // 确保字符串终止

    /* 注册事件处理 */
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                               &ota_event_handler, NULL));

    /* 检查回滚状态 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
                ESP_LOGE(TAG, "取消回滚失败");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "已取消回滚");
        }
    }

    light_control_set_buttons_enabled(false);

    /* 创建 OTA 任务 */
    if (xTaskCreate(ota_task, "ota_task", 4096, NULL, 9, NULL) != pdPASS) {
        ESP_LOGE(TAG, "OTA任务创建失败");
        light_control_set_buttons_enabled(true);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA任务已启动");
    return ESP_OK;
}

OTA_STATUS get_ota_state(void)
{
    return ota_status;
}
