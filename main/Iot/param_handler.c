#include "param_handler.h"

#include <esp_log.h>
#include <string.h>

static const char* TAG = "PARAM_HANDLER";

/* 最大支持的处理器数量 */
#define MAX_HANDLERS 16

/* 处理器注册表项 */
typedef struct {
    const char* key;
    param_handler_fn handler;
} handler_entry_t;

/* 处理器注册表 */
static handler_entry_t s_handlers[MAX_HANDLERS];
static int s_handler_count = 0;

void param_handler_init(void)
{
    s_handler_count = 0;
    memset(s_handlers, 0, sizeof(s_handlers));
    ESP_LOGI(TAG, "参数处理器模块初始化完成");
}

esp_err_t param_handler_register(const char* key, param_handler_fn handler)
{
    if (key == NULL || handler == NULL) {
        ESP_LOGE(TAG, "注册参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_handler_count >= MAX_HANDLERS) {
        ESP_LOGE(TAG, "处理器注册表已满");
        return ESP_ERR_NO_MEM;
    }

    /* 检查是否已注册 */
    for (int i = 0; i < s_handler_count; i++) {
        if (strcmp(s_handlers[i].key, key) == 0) {
            ESP_LOGW(TAG, "参数 '%s' 已注册，覆盖旧处理器", key);
            s_handlers[i].handler = handler;
            return ESP_OK;
        }
    }

    /* 注册新处理器 */
    s_handlers[s_handler_count].key = key;
    s_handlers[s_handler_count].handler = handler;
    s_handler_count++;

    ESP_LOGI(TAG, "注册参数处理器: %s (总计: %d)", key, s_handler_count);
    return ESP_OK;
}

bool param_handler_process(const char* key, cJSON* value)
{
    if (key == NULL || value == NULL) {
        ESP_LOGE(TAG, "参数无效");
        return false;
    }

    /* 查找处理器 */
    for (int i = 0; i < s_handler_count; i++) {
        if (strcmp(s_handlers[i].key, key) == 0) {
            ESP_LOGD(TAG, "处理参数: %s", key);
            return s_handlers[i].handler(key, value);
        }
    }

    ESP_LOGW(TAG, "未找到参数 '%s' 的处理器", key);
    return false;
}

bool param_handler_process_multi(cJSON* value_obj)
{
    if (value_obj == NULL || !cJSON_IsObject(value_obj)) {
        ESP_LOGE(TAG, "multi_set的SET_VALUE必须是JSON对象");
        return false;
    }

    bool all_success = true;
    int total_params = 0;
    int success_count = 0;

    ESP_LOGI(TAG, "开始处理multi_set多参数设置");

    /* 遍历SET_VALUE对象中的所有键值对 */
    cJSON* param = NULL;
    cJSON_ArrayForEach(param, value_obj)
    {
        if (param->string == NULL) {
            ESP_LOGW(TAG, "跳过无效参数(缺少key)");
            continue;
        }

        total_params++;
        ESP_LOGI(TAG, "处理参数[%d/%d]: %s", total_params, cJSON_GetArraySize(value_obj),
                 param->string);

        /* 调用处理器 */
        bool result = param_handler_process(param->string, param);
        if (result) {
            success_count++;
            ESP_LOGI(TAG, "  ✓ 参数 '%s' 设置成功", param->string);
        }
        else {
            all_success = false;
            ESP_LOGW(TAG, "  ✗ 参数 '%s' 设置失败", param->string);
        }
    }

    ESP_LOGI(TAG, "multi_set处理完成: 总计%d个参数, 成功%d个, 失败%d个", total_params, success_count,
             total_params - success_count);

    return all_success;
}

