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
