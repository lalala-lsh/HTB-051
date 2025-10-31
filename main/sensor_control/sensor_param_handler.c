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

