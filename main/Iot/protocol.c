#include "protocol.h"
#include <cJSON.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "device_params.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "light_control.h"
#include "light_manager.h"
#include "system_info.h"

#define TAG "protocol"

#define DEVICE_TYPE "LAMP"

extern system_info_t sys_info;

// 释放cjson_protocol_t资源
static void free_protocol_js(cjson_protocol_t* pt)
{
    if (pt) {
        if (pt->protocol_js) {
            cJSON_Delete(pt->protocol_js);
            pt->protocol_js = NULL; // 重置指针，因为结构体是静态的
        }
        // pt现在是静态结构体，不需要free
    }
}

char* generate_message_rid(void)
{
    // 使用静态缓冲区，避免动态分配，节省内存
    static char rid[64];

    // 初始化缓冲区
    memset(rid, 0, 64);

    time_t rawtime;
    uint64_t current_time_ms = esp_timer_get_time() / 1000;
    int ms_pre = current_time_ms % 1000;

    // 使用更安全的方式构建rid
    char time_str[16] = {0}; // 足够存储时间戳
    char ms_str[8] = {0};    // 足够存储毫秒

    // 分别格式化各个部分
    snprintf(time_str, sizeof(time_str), "%lld", time(&rawtime));
    snprintf(ms_str, sizeof(ms_str), "%d", ms_pre);

    // 组合最终的rid，使用snprintf来确保不会溢出
    snprintf(rid, 64, "%s%s%s", get_device_sn(), time_str, ms_str);

    return rid;
}

// 生成基础cJSON对象
static cjson_protocol_t* generate_cjson(const char* cmd, bool need_rid)
{
    // 使用静态结构体，避免动态分配，节省内存
    static cjson_protocol_t pt_static;
    cjson_protocol_t* pt = &pt_static;

    memset(pt, 0, sizeof(cjson_protocol_t));
    pt->protocol_js = cJSON_CreateObject();
    if (!pt->protocol_js) {
        // pt现在是静态结构体，不需要free
        return NULL;
    }

    // 将cmd转换为数字
    int cmd_num = atoi(cmd);

    // 获取当前时间戳
    time_t timestamp = time(NULL);
    const char* firmware_version = get_firmware_version();
    const char* sn = get_device_sn();

    // 添加基础字段
    cJSON_AddNumberToObject(pt->protocol_js, "CMD", cmd_num);
    cJSON_AddStringToObject(pt->protocol_js, "VERSION", firmware_version);
    cJSON_AddStringToObject(pt->protocol_js, "DEVICE_TYPE", DEVICE_TYPE);
    cJSON_AddStringToObject(pt->protocol_js, "DEVICE_CODE", sn);
    cJSON_AddNumberToObject(pt->protocol_js, "DEVICE_TIME", timestamp); // 使用时间戳
    cJSON_AddStringToObject(pt->protocol_js, "DEVICE_MAC", get_device_mac());

    // 只有在需要时才生成RID
    if (need_rid) {
        char* rid = generate_message_rid();
        if (rid) {
            cJSON_AddStringToObject(pt->protocol_js, "RID", rid);
            // rid现在是静态缓冲区，不需要free
        }
    }

    return pt;
}

/**
 * @brief 创建灯光参数对象（仅lighting + therapy）- 从light_manager获取实时状态
 * @return cJSON参数对象，需要由调用者释放
 */
static cJSON* create_lighting_params_object(void)
{
    cJSON* params_obj = cJSON_CreateObject();
    if (!params_obj) {
        return NULL;
    }

    light_manager_t* lm = get_light_manager();

    // 添加照明参数
    cJSON* lighting = cJSON_CreateObject();
    cJSON_AddNumberToObject(lighting, "mode", 0);

    // 从light_manager获取实时开关状态
    cJSON* lighting_state = cJSON_CreateObject();
    cJSON_AddNumberToObject(lighting_state, "1", light_manager_is_on(lm, LIGHT_ID_UPPER) ? 1 : 0);
    cJSON_AddNumberToObject(lighting_state, "2", light_manager_is_on(lm, LIGHT_ID_LOWER) ? 1 : 0);
    cJSON_AddNumberToObject(lighting_state, "3", light_manager_is_on(lm, LIGHT_ID_AMBIENT) ? 1 : 0);
    cJSON_AddItemToObject(lighting, "state", lighting_state);

    // 从light_manager获取实时亮度
    cJSON* lighting_brightness = cJSON_CreateObject();
    cJSON_AddNumberToObject(lighting_brightness, "1",
                            light_manager_get_light_brightness(lm, LIGHT_ID_UPPER));
    cJSON_AddNumberToObject(lighting_brightness, "2",
                            light_manager_get_light_brightness(lm, LIGHT_ID_LOWER));
    cJSON_AddNumberToObject(lighting_brightness, "3",
                            light_manager_get_light_brightness(lm, LIGHT_ID_AMBIENT));
    cJSON_AddItemToObject(lighting, "brightness", lighting_brightness);

    cJSON_AddItemToObject(params_obj, "lighting", lighting);

    // 添加光疗参数 - 从light_manager获取实时状态
    cJSON* therapy = cJSON_CreateObject();
    cJSON_AddNumberToObject(therapy, "state", light_manager_get_red_mode(lm));

    cJSON* therapy_brightness = cJSON_CreateObject();
    cJSON_AddNumberToObject(therapy_brightness, "1", light_manager_get_therapy_brightness(lm, 0));
    cJSON_AddNumberToObject(therapy_brightness, "2", light_manager_get_therapy_brightness(lm, 1));
    cJSON_AddNumberToObject(therapy_brightness, "3", light_manager_get_therapy_brightness(lm, 2));
    cJSON_AddItemToObject(therapy, "brightness", therapy_brightness);

    cJSON_AddItemToObject(params_obj, "therapy", therapy);

    return params_obj;
}

/**
 * @brief 创建完整参数对象（包含所有设备参数）
 * @return cJSON参数对象，需要由调用者释放
 */
static cJSON* create_params_object(void)
{
    // 先创建灯光参数
    cJSON* params_obj = create_lighting_params_object();
    if (!params_obj) {
        return NULL;
    }

    // 添加其他参数
    cJSON_AddNumberToObject(params_obj, "music_state", device_params_get_music_state());
    cJSON_AddNumberToObject(params_obj, "voice_state", device_params_get_voice_state());
    cJSON_AddNumberToObject(params_obj, "volume", device_params_get_music_volume());

    cJSON_AddNumberToObject(params_obj, "constant_light_state",
                            device_params_get_constant_light_state());
    cJSON_AddNumberToObject(params_obj, "pir_state", device_params_get_pir_state());
    cJSON_AddNumberToObject(params_obj, "dim_timeout", device_params_get_dim_timeout());
    cJSON_AddNumberToObject(params_obj, "off_timeout", device_params_get_off_timeout());

    cJSON_AddNumberToObject(params_obj, "therapy_focus_state",
                            device_params_get_therapy_focus_state());
    cJSON_AddNumberToObject(params_obj, "therapy_sleep_state",
                            device_params_get_therapy_sleep_state());
    cJSON_AddNumberToObject(params_obj, "therapy_sleep_duration",
                            device_params_get_therapy_sleep_duration());

    return params_obj;
}

/**
 * @brief 构建遗嘱消息
 * @return 遗嘱消息JSON字符串，使用后需要free
 */
char* build_will_message(void)
{
    // 生成基础cJSON对象，需要自己生成RID
    cjson_protocol_t* pt = generate_cjson(WILL_MESSAGE_CMD, true); // CMD=999 表示遗嘱消息
    if (!pt) {
        ESP_LOGE(TAG, "遗嘱消息创建失败");
        return NULL;
    }

    // 转换为字符串
    char* msg_str = cJSON_PrintUnformatted(pt->protocol_js);

    // 释放资源
    free_protocol_js(pt);

    return msg_str;
}

/**
 * @brief 构建心跳包
 * @param power 设备电量百分比(0-100)
 * @return 心跳包JSON字符串，使用后需要free
 */
char* build_heartbeat_message()
{
    // 生成基础cJSON对象，需要自己生成RID
    cjson_protocol_t* pt = generate_cjson(HEART_BEAT_CMD, true);
    if (!pt) {
        ESP_LOGE(TAG, "心跳消息创建失败");
        return NULL;
    }

    // 转换为字符串
    char* msg_str = cJSON_PrintUnformatted(pt->protocol_js);

    // 释放资源
    free_protocol_js(pt);

    return msg_str;
}

/**
 * @brief 构建实时上报消息
 * @param record 当前工作记录
 * @return 实时上报JSON字符串，使用后需要free
 */
char* build_realtime_report_message(const work_record_t* record)
{
    if (record == NULL) {
        ESP_LOGE(TAG, "实时上报参数无效");
        return NULL;
    }

    // 生成基础cJSON对象，需要自己生成RID
    cjson_protocol_t* pt = generate_cjson(REALTIME_REPORT_CMD, true);
    if (!pt) {
        ESP_LOGE(TAG, "实时上报消息创建失败");
        return NULL;
    }

    // 添加工作记录信息
    cJSON_AddNumberToObject(pt->protocol_js, "DEVICE_MODE", record->mode);
    cJSON_AddNumberToObject(pt->protocol_js, "DEVICE_START_TIME", record->start_time);
    cJSON_AddNumberToObject(pt->protocol_js, "DEVICE_END_TIME", record->end_time);
    cJSON_AddNumberToObject(pt->protocol_js, "DEVICE_WORK_TIME", record->work_time);

    // 转换为字符串
    char* msg_str = cJSON_PrintUnformatted(pt->protocol_js);

    // 释放资源
    free_protocol_js(pt);

    return msg_str;
}

/**
 * @brief 构建设备解绑消息
 * @return 设备解绑JSON字符串，使用后需要free
 */
char* build_unbind_message(void)
{
    // 生成基础cJSON对象，需要自己生成RID
    cjson_protocol_t* pt = generate_cjson(UNBIND_CMD, true);
    if (!pt) {
        ESP_LOGE(TAG, "设备解绑消息创建失败");
        return NULL;
    }

    // 转换为字符串
    char* msg_str = cJSON_PrintUnformatted(pt->protocol_js);

    // 释放资源
    free_protocol_js(pt);

    return msg_str;
}

/**
 * @brief 构建开机同步协议版本消息
 * @param params 设备参数
 * @return 开机同步JSON字符串，使用后需要free
 */
char* build_boot_sync_message(void)
{
    // 生成基础cJSON对象，需要自己生成RID
    cjson_protocol_t* pt = generate_cjson(BOOT_SYNC_CMD, true);
    if (!pt) {
        ESP_LOGE(TAG, "开机同步消息创建失败");
        return NULL;
    }

    // 创建参数对象
    cJSON* params_obj = create_params_object();
    if (!params_obj) {
        free_protocol_js(pt);
        return NULL;
    }

    // 添加参数对象
    cJSON_AddItemToObject(pt->protocol_js, "PARAMS", params_obj);

    // 转换为字符串
    char* msg_str = cJSON_PrintUnformatted(pt->protocol_js);

    // 释放资源
    free_protocol_js(pt);

    return msg_str;
}

/**
 * @brief 构建设备灯光参数同步消息（仅lighting + therapy）
 * @return 参数同步JSON字符串，使用后需要free
 */
char* build_params_sync_message(void)
{
    // 生成基础cJSON对象，需要自己生成RID
    cjson_protocol_t* pt = generate_cjson(PARAMS_SYNC_CMD, true);
    if (!pt) {
        ESP_LOGE(TAG, "参数同步消息创建失败");
        return NULL;
    }

    // 创建灯光参数对象
    cJSON* params_obj = create_lighting_params_object();
    if (!params_obj) {
        free_protocol_js(pt);
        return NULL;
    }

    // 添加参数对象
    cJSON_AddItemToObject(pt->protocol_js, "PARAMS", params_obj);

    // 转换为字符串
    char* msg_str = cJSON_PrintUnformatted(pt->protocol_js);

    // 释放资源
    free_protocol_js(pt);

