#include "light_param_handler.h"

#include <esp_log.h>
#include <string.h>

#include "audio_queue.h"
#include "cJSON.h"
#include "light_control.h"
#include "light_manager.h"
#include "param_handler.h"
#include "sensor_control.h"
#include "tts_list.h"

static const char* TAG = "LIGHT_PARAM";

/* ========================================================================== */
/* 辅助函数                                                                    */
/* ========================================================================== */

/**
 * @brief 从JSON对象中获取整数值（支持数字和字符串类型）
 */
static int get_json_int(cJSON* obj, const char* key, int default_val)
{
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (item) {
        if (cJSON_IsNumber(item))
            return item->valueint;
        if (cJSON_IsString(item))
            return atoi(item->valuestring);
    }
    return default_val;
}

/* ========================================================================== */
/* lighting 参数处理                                                           */
/* ========================================================================== */

/**
 * @brief 处理 lighting 参数设置
 *
 * 协议格式:
 * {
 *   "mode": 0/1/2,
 *   "state": {"1": 0/1, "2": 0/1, "3": 0/1},
 *   "brightness": {"1": 10-100, "2": 10-100, "3": 10-100}
 * }
 *
 * 映射关系: 1=上发光板, 2=下发光板, 3=氛围灯
 */
static bool handle_lighting_param(const char* key, cJSON* value)
{
    if (!cJSON_IsObject(value)) {
        ESP_LOGE(TAG, "lighting参数必须是JSON对象");
        return false;
    }

    light_manager_t* light_mgr = get_light_manager();
    if (light_mgr == NULL) {
        ESP_LOGE(TAG, "无法获取light_manager实例");
        return false;
    }

    ESP_LOGI(TAG, "处理lighting参数");

    /* 解析mode字段 */
    int mode = get_json_int(value, "mode", -1);
    ESP_LOGI(TAG, "  mode: %d", mode);

    /* mode语音提示 */
    if (mode == 1) {
        audio_queue_play(BOOK_READ, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
    }
    else if (mode == 2) {
        audio_queue_play(ELEC_READ, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
    }

    /* 解析state和brightness字段 */
    cJSON* state = cJSON_GetObjectItem(value, "state");
    cJSON* brightness = cJSON_GetObjectItem(value, "brightness");

    if (brightness && cJSON_IsObject(brightness)) {
        /* 协议映射: 1=上发光板, 2=下发光板, 3=氛围灯 */
        const light_id_t light_map[] = {LIGHT_ID_UPPER, LIGHT_ID_LOWER, LIGHT_ID_AMBIENT};

        /* 记录灯光状态变化(用于语音提示判断) */
        bool light_turned_on[3] = {false, false, false};
        int last_changed_light = -1;

        /* 标记为远端控制，避免触发CMD 7同步 */
        brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_REMOTE;
