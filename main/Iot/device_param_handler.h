#ifndef _DEVICE_PARAM_HANDLER_H_
#define _DEVICE_PARAM_HANDLER_H_

#include <esp_err.h>

/**
 * @brief 初始化设备参数处理器
 *
 * 注册 music_state, voice_state, constant_light_state, volume 参数的处理函数
 *
 * @return ESP_OK 成功
 */
esp_err_t device_param_handler_init(void);

#endif // _DEVICE_PARAM_HANDLER_H_

