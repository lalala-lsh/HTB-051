#ifndef _PARAM_HANDLER_H_
#define _PARAM_HANDLER_H_

#include <esp_err.h>
#include <stdbool.h>

#include "cJSON.h"

/**
 * @brief 参数处理回调函数类型
 * @param key 参数键名
 * @param value 参数值(cJSON对象)
 * @return true 处理成功, false 处理失败
 */
typedef bool (*param_handler_fn)(const char* key, cJSON* value);

/**
 * @brief 初始化参数处理器模块
 */
void param_handler_init(void);

/**
 * @brief 注册参数处理器
 * @param key 参数键名 (如 "lighting", "therapy", "pir_state" 等)
 * @param handler 处理函数
 * @return ESP_OK 成功, ESP_ERR_NO_MEM 处理器表已满
 */
esp_err_t param_handler_register(const char* key, param_handler_fn handler);

/**
 * @brief 处理参数设置请求
 * @param key 参数键名
 * @param value 参数值
 * @return true 处理成功, false 未找到处理器或处理失败
 */
bool param_handler_process(const char* key, cJSON* value);

/**
 * @brief 处理多参数设置请求
 * @param value_obj SET_VALUE的JSON对象,包含多个参数键值对
 * @return true 全部成功, false 任一参数失败
 */
bool param_handler_process_multi(cJSON* value_obj);

#endif // _PARAM_HANDLER_H_

