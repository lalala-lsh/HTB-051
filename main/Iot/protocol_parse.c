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
/* ========================================================================== */

/**
 * @brief 处理设备解绑回复
 * @param response JSON对象
 * @return ESP_OK成功，其他值表示失败
 */
static esp_err_t handle_unbind_response(const cJSON* response)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char* rid = get_request_id(response);
    if (rid == NULL) {
        ESP_LOGE(TAG, "获取请求ID失败");
        return ESP_FAIL;
    }

    bool success = is_response_success(response);
    ESP_LOGI(TAG, "设备解绑回复: %s, RID: %s", success ? "成功" : "失败", rid);

    if (success) {
        // TODO: 设备解绑成功，清除本地配置，返回未绑定状态
    }
    else {
        // TODO: 设备解绑失败，可能需要重试
    }

    return ESP_OK;
}

/**
 * @brief 处理开机同步协议版本回复
 * @param response JSON对象
 * @return ESP_OK成功，其他值表示失败
 */
static esp_err_t handle_boot_sync_response(const cJSON* response)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char* rid = get_request_id(response);
    if (rid == NULL) {
        ESP_LOGE(TAG, "获取请求ID失败");
        return ESP_FAIL;
    }

    bool success = is_response_success(response);
    ESP_LOGI(TAG, "开机同步回复: %s, RID: %s", success ? "成功" : "失败", rid);

    if (success) {
        /* 检查是否有固件升级包；有包时本轮跳过音频更新 */
        cJSON* package_obj = cJSON_GetObjectItem(response, "PACKAGE");
        if (package_obj != NULL && cJSON_IsString(package_obj)) {
            const char* url = package_obj->valuestring;
            if (url != NULL && strlen(url) > 0) {
                ESP_LOGI(TAG, "发现新固件包: %s", url);
                ota_start(url);
                ESP_LOGI(TAG, "检测到固件OTA，本轮跳过音频更新");
                return ESP_OK;
            }
            else {
                ESP_LOGI(TAG, "固件包URL为空,跳过OTA升级");
            }
        }
        else {
            ESP_LOGI(TAG, "没有新的固件包");
        }

        /* 处理音频更新数组 AUDIO: [{url, md5, size}] */
        cJSON* audio_array = cJSON_GetObjectItem(response, "AUDIO");
        if (audio_array != NULL) {
            if (!cJSON_IsArray(audio_array)) {
                ESP_LOGW(TAG, "AUDIO字段存在但不是数组，已忽略");
            } else {
                esp_err_t audio_ret = audio_update_start_from_json(audio_array);
                if (audio_ret == ESP_OK) {
                    ESP_LOGI(TAG, "音频更新任务已触发");
                } else {
                    ESP_LOGW(TAG, "音频更新未启动: %s", esp_err_to_name(audio_ret));
                }
            }
        } else {
            ESP_LOGI(TAG, "未发现音频更新字段AUDIO");
        }

        // TODO: 同步完成，设置设备状态为在线
    }
    else {
        // TODO: 同步失败，稍后重试
    }

    return ESP_OK;
}

/* ========================================================================== */
/* SET_VALUE 类型规范化                                                        */
/* ========================================================================== */

/**
 * @brief 规范化therapy对象中的state字段为数字类型
 *
 * 服务端/App可能将state以字符串形式下发（如 "0"），设备回显时需统一为数字类型
 */
static void normalize_therapy_value(cJSON* therapy_obj)
{
    if (!therapy_obj || !cJSON_IsObject(therapy_obj)) return;

    cJSON* state = cJSON_GetObjectItem(therapy_obj, "state");
    if (state && cJSON_IsString(state)) {
        int val = atoi(state->valuestring);
        cJSON_ReplaceItemInObject(therapy_obj, "state", cJSON_CreateNumber(val));
    }
}

/**
 * @brief 规范化SET_VALUE中的数据类型，确保回显的值类型正确
 */
static void normalize_set_value(cJSON* value_obj, const char* key)
{
    if (!value_obj || !key) return;

    if (strcmp(key, "therapy") == 0) {
        normalize_therapy_value(value_obj);
    }
    else if (strcmp(key, "multi_set") == 0 && cJSON_IsObject(value_obj)) {
        cJSON* therapy = cJSON_GetObjectItem(value_obj, "therapy");
        if (therapy) {
            normalize_therapy_value(therapy);
        }
    }
}

/* ========================================================================== */
/* 命令处理函数                                                                */
/* ========================================================================== */

/**
 * @brief 处理获取设备参数请求
 * @param client MQTT客户端
 * @param request JSON对象
 * @return ESP_OK成功，其他值表示失败
 */
esp_err_t handle_get_device_params(esp_mqtt_client_handle_t client, const cJSON* request)
{
    if (client == NULL || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char* rid = get_request_id(request);
    if (rid == NULL) {
        ESP_LOGE(TAG, "获取请求ID失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "收到获取设备参数请求, RID: %s", rid);

    /* 构建回复消息 */
    char* response_msg = build_get_device_params_response(rid, true);
    if (response_msg == NULL) {
        ESP_LOGE(TAG, "构建回复消息失败");
        return ESP_FAIL;
    }

    /* 发送回复消息 */
    esp_mqtt_client_publish(client, mqtt_client_get_publish_topic(), response_msg,
                            strlen(response_msg), MQTT_QOS, 0);
    ESP_LOGI(TAG, "已发送获取设备参数回复");

    free(response_msg);
    return ESP_OK;
}

/**
 * @brief 处理设置设备参数请求
 * @param client MQTT客户端
 * @param request JSON对象
 * @return ESP_OK成功，其他值表示失败
 */
esp_err_t handle_set_device_params(esp_mqtt_client_handle_t client, const cJSON* request)
{
    if (client == NULL || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool success = false;

    /* 获取SET_KEY和SET_VALUE */
    cJSON* key_obj = cJSON_GetObjectItem(request, "SET_KEY");
    cJSON* value_obj = cJSON_GetObjectItem(request, "SET_VALUE");

    if (key_obj == NULL || !cJSON_IsString(key_obj)) {
        ESP_LOGE(TAG, "获取SET_KEY失败");
        return ESP_FAIL;
    }
    if (value_obj == NULL) {
        ESP_LOGE(TAG, "获取SET_VALUE失败");
        return ESP_FAIL;
    }

    /* 判断是否为多参数设置 */
    if (strcmp(key_obj->valuestring, "multi_set") == 0) {
        /* 多参数设置 */
        success = param_handler_process_multi(value_obj);
    }
    else {
        /* 单参数设置 */
        success = param_handler_process(key_obj->valuestring, value_obj);
    }

    /* 获取请求ID */
    const char* rid = get_request_id(request);
    if (rid == NULL) {
        ESP_LOGE(TAG, "获取请求ID失败");
        return ESP_FAIL;
    }

    /* 规范化SET_VALUE中的数据类型，确保回显值类型正确 */
    normalize_set_value(value_obj, key_obj->valuestring);

    /* 构建并发送回复消息 */
    char* response_msg = build_set_device_params_response(rid, key_obj, value_obj, success);
    if (response_msg == NULL) {
        ESP_LOGE(TAG, "构建回复消息失败");
        return ESP_FAIL;
    }

    // 打印回复消息
    ESP_LOGI(TAG, "回复消息: %s", response_msg);

    esp_mqtt_client_publish(client, mqtt_client_get_publish_topic(), response_msg,
                            strlen(response_msg), MQTT_QOS, 0);
    ESP_LOGI(TAG, "已发送设置设备参数回复: %s", success ? "SUCCESS" : "FAIL");

    free(response_msg);
    return ESP_OK;
}

/**
 * @brief 处理服务器解绑命令
 */
esp_err_t handle_server_unbind(esp_mqtt_client_handle_t client, const cJSON* json_obj)
{
    if (client == NULL || json_obj == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: wifi_reset();
    factory_reset();
    return ESP_OK;
}

/* ========================================================================== */
/* 消息入口                                                                    */
/* ========================================================================== */
