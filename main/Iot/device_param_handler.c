#include "device_param_handler.h"

#include <esp_log.h>

#include "cJSON.h"
#include "device_params.h"
#include "param_handler.h"

static const char* TAG = "DEVICE_PARAM";

/* ========================================================================== */
/* music_state 参数处理                                                        */
/* ========================================================================== */

/**
 * @brief 处理 music_state (音乐控制开关) 参数设置
 * @param key 参数键名
 * @param value 参数值 (0=关闭, 1=开启)
 */
static bool handle_music_state(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val == 0 || val == 1) {
        device_params_set_music_state((uint8_t)val);
        ESP_LOGI(TAG, "设置music_state=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "music_state值无效: %d", val);
    return false;
}

/* ========================================================================== */
/* voice_state 参数处理                                                        */
/* ========================================================================== */

/**
 * @brief 处理 voice_state (语音控制开关) 参数设置
 * @param key 参数键名
 * @param value 参数值 (0=关闭, 1=开启)
 */
static bool handle_voice_state(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val == 0 || val == 1) {
        device_params_set_voice_state((uint8_t)val);
        ESP_LOGI(TAG, "设置voice_state=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "voice_state值无效: %d", val);
    return false;
}

/* ========================================================================== */
/* constant_light_state 参数处理                                               */
/* ========================================================================== */

/**
 * @brief 处理 constant_light_state (恒光控制开关) 参数设置
 * @param key 参数键名
 * @param value 参数值 (0=关闭, 1=开启)
 */
static bool handle_constant_light_state(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val == 0 || val == 1) {
        device_params_set_constant_light_state((uint8_t)val);
        ESP_LOGI(TAG, "设置constant_light_state=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "constant_light_state值无效: %d", val);
    return false;
}

/* ========================================================================== */
/* volume 参数处理                                                             */
/* ========================================================================== */

/**
 * @brief 处理 volume (音量控制) 参数设置
 * @param key 参数键名
 * @param value 参数值 (1-5)
 */
static bool handle_volume(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val >= 1 && val <= 5) {
        device_params_set_music_volume((uint8_t)val);
        ESP_LOGI(TAG, "设置volume=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "volume值无效(1-5): %d", val);
    return false;
}

/* ========================================================================== */
/* therapy_focus_state 参数处理                                                */
/* ========================================================================== */
