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
