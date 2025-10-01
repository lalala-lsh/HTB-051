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

        /* 第一遍: 设置所有灯光 */
        for (int i = 1; i <= 3; i++) {
            char idx[2];
            snprintf(idx, sizeof(idx), "%d", i);

            int state_val = (state && cJSON_IsObject(state)) ? get_json_int(state, idx, -1) : -1;
            int bright_val = get_json_int(brightness, idx, -1);

            if (state_val >= 0 && bright_val > 0) {
                light_id_t lid = light_map[i - 1];
                bool target_on = (state_val == 1);
                bool current_on = light_manager_is_on(light_mgr, lid);
                uint8_t current_bright = light_manager_get_light_brightness(light_mgr, lid);

                /* 只有状态或亮度发生变化才执行操作 */
                if (target_on != current_on || (target_on && bright_val != current_bright)) {
                    ESP_LOGI(TAG, "  设置灯%d: 开关=%d, 亮度=%d (当前: 开关=%d, 亮度=%d)", i,
                             state_val, bright_val, current_on, current_bright);
                    light_manager_set_light_state_percent(light_mgr, lid, target_on, bright_val,
                                                          true);

                    /* 记录从关闭到开启的状态变化 */
                    if (target_on && !current_on) {
                        light_turned_on[i - 1] = true;
                        last_changed_light = i;
                    }
                }
                else {
                    ESP_LOGD(TAG, "  灯%d状态无变化，跳过", i);
                }
            }
        }

        /* 第二遍: 语音提示(仅在mode=0常规模式下) */
        if (mode == 0 && last_changed_light != -1) {
            bool all_lights_on = light_manager_is_on(light_mgr, LIGHT_ID_UPPER) &&
                                 light_manager_is_on(light_mgr, LIGHT_ID_LOWER) &&
                                 light_manager_is_on(light_mgr, LIGHT_ID_AMBIENT);

            bool any_light_turned_on = light_turned_on[0] || light_turned_on[1] || light_turned_on[2];

            if (all_lights_on && any_light_turned_on) {
                audio_queue_play(ALL_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
                ESP_LOGI(TAG, "三个灯全部开启,播放ALL_LIGHT");
            }
            else if (any_light_turned_on) {
                if (last_changed_light == 1) {
                    audio_queue_play(UP_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
                }
                else if (last_changed_light == 2) {
                    audio_queue_play(DOWM_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
                }
                else if (last_changed_light == 3) {
                    audio_queue_play(AROUND_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
                }
                ESP_LOGI(TAG, "单个灯%d开启,播放对应语音", last_changed_light);
            }
        }

        light_manager_sync_indicator_leds(light_mgr);

        /* 恢复来源标识 */
        brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_EXTERNAL;
    }

    return true;
}

/* ========================================================================== */
/* therapy 参数处理                                                            */
/* ========================================================================== */

/**
 * @brief 处理 therapy (光疗/红光) 参数设置
 *
 * 协议格式:
 * {
 *   "state": "0"/"1"/"2"/"3",
 *   "brightness": {"1": 护眼亮度, "2": 专注亮度, "3": 助眠亮度}
 * }
 *
 * state: 0=关闭, 1=护眼(NORMAL), 2=专注(THERAPY), 3=助眠(SLEEP)
 */
static bool handle_therapy_param(const char* key, cJSON* value)
{
    if (!cJSON_IsObject(value)) {
        ESP_LOGE(TAG, "therapy参数必须是JSON对象");
        return false;
    }

