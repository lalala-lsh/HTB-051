#ifndef _SENSOR_PARAM_HANDLER_H_
#define _SENSOR_PARAM_HANDLER_H_

#include <esp_err.h>

/**
 * @brief 初始化传感器参数处理器
 *
 * 注册 pir_state, dim_timeout, off_timeout 参数的处理函数到 param_handler
 *
 * @return ESP_OK 成功
 */
esp_err_t sensor_param_handler_init(void);

#endif // _SENSOR_PARAM_HANDLER_H_

