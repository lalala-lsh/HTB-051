#ifndef _LIGHT_PARAM_HANDLER_H_
#define _LIGHT_PARAM_HANDLER_H_

#include <esp_err.h>

/**
 * @brief 初始化灯光参数处理器
 *
 * 注册 lighting 和 therapy 参数的处理函数到 param_handler
 *
 * @return ESP_OK 成功
 */
esp_err_t light_param_handler_init(void);

#endif // _LIGHT_PARAM_HANDLER_H_

