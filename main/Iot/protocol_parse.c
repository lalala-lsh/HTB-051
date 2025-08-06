#include "protocol_parse.h"

#include "blufi_example.h"
#include "cJSON.h"
#include "light_control.h"
#include "mqtt_manmager.h"
#include "ota.h"
#include "audio_update.h"
#include "param_handler.h"
#include "protocol.h"

#include <esp_err.h>
#include <esp_log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "PROTOCOL_PARSE";

/* ========================================================================== */
/* JSON 解析辅助函数                                                           */
/* ========================================================================== */

/**
 * @brief 从JSON对象中获取命令码
 * @param json JSON对象
 * @return 命令码, -1表示失败
 */
static int get_cmd_from_json(const cJSON* json)
{
    if (json == NULL) {
        return -1;
    }

    cJSON* cmd_obj = cJSON_GetObjectItem(json, "CMD");
    if (cmd_obj == NULL || !cJSON_IsNumber(cmd_obj)) {
        ESP_LOGE(TAG, "JSON中没有合法的CMD字段");
        return -1;
    }

    return cmd_obj->valueint;
}

/**
 * @brief 检查响应码是否成功
 * @param json JSON对象
 * @return true成功, false失败
 */
static bool is_response_success(const cJSON* json)
{
    if (json == NULL) {
        return false;
    }

    cJSON* res_code_obj = cJSON_GetObjectItem(json, "RES_CODE");
    if (res_code_obj == NULL || !cJSON_IsString(res_code_obj)) {
        ESP_LOGE(TAG, "JSON中没有合法的RES_CODE字段");
        return false;
    }

    return strcmp(res_code_obj->valuestring, RES_CODE_SUCCESS) == 0;
}

/**
 * @brief 获取响应中的请求ID
 * @param json JSON对象
 * @return 请求ID字符串, NULL表示失败
 */
static const char* get_request_id(const cJSON* json)
{
    if (json == NULL) {
        return NULL;
    }

    cJSON* rid_obj = cJSON_GetObjectItem(json, "RID");
    if (rid_obj == NULL || !cJSON_IsString(rid_obj)) {
        ESP_LOGE(TAG, "JSON中没有合法的RID字段");
        return NULL;
    }

    return rid_obj->valuestring;
}

/* ========================================================================== */
/* 响应处理函数                                                                */
