#include "sensor_param_handler.h"

#include <esp_log.h>

#include "cJSON.h"
#include "device_params.h"
#include "param_handler.h"
#include "sensor_control.h"

static const char* TAG = "SENSOR_PARAM";

/* ========================================================================== */
/* pir_state 参数处理                                                          */
/* ========================================================================== */

/**
 * @brief 处理 pir_state (PIR人体感应开关) 参数设置
 * @param key 参数键名
 * @param value 参数值 (0=关闭, 1=开启)
 */
static bool handle_pir_state(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val == 0 || val == 1) {
        device_params_set_pir_state((uint8_t)val);
        sensor_control_set_pir_enabled(val == 1);
        ESP_LOGI(TAG, "设置pir_state=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "pir_state值无效: %d", val);
    return false;
}

/* ========================================================================== */
/* dim_timeout 参数处理                                                        */
/* ========================================================================== */

/**
 * @brief 处理 dim_timeout (调暗超时时间) 参数设置
 * @param key 参数键名
 * @param value 参数值 (1-45分钟)
 */
static bool handle_dim_timeout(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val >= 1 && val <= 45) {
        device_params_set_dim_timeout((uint8_t)val);
        sensor_control_set_dim_timeout((uint8_t)val);
        ESP_LOGI(TAG, "设置dim_timeout=%d分钟", val);
        return true;
    }

    ESP_LOGW(TAG, "dim_timeout值无效(1-45): %d", val);
    return false;
}

/* ========================================================================== */
/* off_timeout 参数处理                                                        */
/* ========================================================================== */

/**
 * @brief 处理 off_timeout (关灯超时时间) 参数设置
 * @param key 参数键名
 * @param value 参数值 (1-10分钟)
 */
static bool handle_off_timeout(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val >= 1 && val <= 10) {
        device_params_set_off_timeout((uint8_t)val);
        sensor_control_set_off_timeout((uint8_t)val);
        ESP_LOGI(TAG, "设置off_timeout=%d分钟", val);
        return true;
    }

    ESP_LOGW(TAG, "off_timeout值无效(1-10): %d", val);
    return false;
}

/* ========================================================================== */
/* logs 参数处理                                                               */
/* ========================================================================== */

/**
 * @brief 处理 logs (设备日志) 参数设置
 * @param key 参数键名
 * @param value 参数值 (-1=清空日志, 0=查看本地日志, 1=获取远程日志)
 *
 * 当 value=0 或 1 时，将 value 节点原地替换为 PIR 日志字符串数组，
 * 这样 protocol_parse 构建 CMD 309 回复时会自动带上日志数据。
 */
static bool handle_logs(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -999;

    switch (val) {
    case -1:
        pir_log_clear();
        ESP_LOGI(TAG, "清空PIR日志");
        return true;
    case 0:
    case 1: {
        cJSON* log_array = pir_log_get_json_array();
        if (!log_array) {
            ESP_LOGE(TAG, "获取PIR日志失败");
            return false;
        }

        /* 将 value 节点从 number 原地替换为 array，
         * 把 log_array 的子节点链转移到 value 上 */
        value->type = cJSON_Array;
        value->valueint = 0;
        value->valuedouble = 0;
        value->child = log_array->child;
        log_array->child = NULL;
        cJSON_Delete(log_array);

        ESP_LOGI(TAG, "获取PIR日志成功 (mode=%d)", val);
        return true;
    }
    default:
        ESP_LOGW(TAG, "logs值无效: %d (需要-1/0/1)", val);
        return false;
    }
}

/* ========================================================================== */
/* 初始化                                                                      */
/* ========================================================================== */

esp_err_t sensor_param_handler_init(void)
{
    esp_err_t ret;

    ret = param_handler_register("pir_state", handle_pir_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册pir_state处理器失败");
        return ret;
    }

    ret = param_handler_register("dim_timeout", handle_dim_timeout);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册dim_timeout处理器失败");
        return ret;
    }

    ret = param_handler_register("off_timeout", handle_off_timeout);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册off_timeout处理器失败");
        return ret;
    }

    ret = param_handler_register("logs", handle_logs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册logs处理器失败");
        return ret;
    }

    ESP_LOGI(TAG, "传感器参数处理器初始化完成");
    return ESP_OK;
}

