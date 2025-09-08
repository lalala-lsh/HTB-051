#ifndef _LIGHT_CONTROL_H_
#define _LIGHT_CONTROL_H_

#include "light_manager.h"

void light_control_start(void);

/**
 * @brief 设置本地按键事件是否响应
 *
 * @param enabled true启用按键,false忽略按键事件
 */
void light_control_set_buttons_enabled(bool enabled);

/**
 * @brief 停止灯光控制模块
 * 
 * 停止并释放按键管理器和灯光管理器资源。
 * 用于OTA升级前清理、深度睡眠前清理等场景。
 */
void light_control_stop(void);

light_manager_t* get_light_manager(void);

void factory_reset(void);

#endif // _LIGHT_CONTROL_H_
