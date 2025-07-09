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

static bool handle_therapy_focus_state(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val == 0 || val == 1) {
        device_params_set_therapy_focus_state((uint8_t)val);
        ESP_LOGI(TAG, "设置therapy_focus_state=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "therapy_focus_state值无效: %d", val);
    return false;
}

/* ========================================================================== */
/* therapy_sleep_state 参数处理                                                */
/* ========================================================================== */

static bool handle_therapy_sleep_state(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val == 0 || val == 1) {
        device_params_set_therapy_sleep_state((uint8_t)val);
        ESP_LOGI(TAG, "设置therapy_sleep_state=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "therapy_sleep_state值无效: %d", val);
    return false;
}

/* ========================================================================== */
/* therapy_sleep_duration 参数处理                                             */
/* ========================================================================== */

static bool handle_therapy_sleep_duration(const char* key, cJSON* value)
{
    int val = cJSON_IsNumber(value) ? value->valueint : -1;

    if (val >= 10 && val <= 60 && (val % 10 == 0)) {
        device_params_set_therapy_sleep_duration((uint8_t)val);
        ESP_LOGI(TAG, "设置therapy_sleep_duration=%d", val);
        return true;
    }

    ESP_LOGW(TAG, "therapy_sleep_duration值无效(10-60,步进10): %d", val);
    return false;
}

/* ========================================================================== */
/* 初始化                                                                      */
/* ========================================================================== */

esp_err_t device_param_handler_init(void)
{
    esp_err_t ret;

    ret = param_handler_register("music_state", handle_music_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册music_state处理器失败");
        return ret;
    }

    ret = param_handler_register("voice_state", handle_voice_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册voice_state处理器失败");
        return ret;
    }

    ret = param_handler_register("constant_light_state", handle_constant_light_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册constant_light_state处理器失败");
        return ret;
    }

    ret = param_handler_register("volume", handle_volume);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册volume处理器失败");
        return ret;
    }

    ret = param_handler_register("therapy_focus_state", handle_therapy_focus_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册therapy_focus_state处理器失败");
        return ret;
    }

    ret = param_handler_register("therapy_sleep_state", handle_therapy_sleep_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册therapy_sleep_state处理器失败");
        return ret;
    }

    ret = param_handler_register("therapy_sleep_duration", handle_therapy_sleep_duration);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册therapy_sleep_duration处理器失败");
        return ret;
    }

    ESP_LOGI(TAG, "设备参数处理器初始化完成");
    return ESP_OK;
}

