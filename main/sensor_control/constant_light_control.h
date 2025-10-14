/**
 * @file constant_light_control.h
 * @brief BH1750恒光控制模块头文件
 *
 * 功能: 使用BH1750光照传感器检测环境光亮度,自动调节灯板亮度维持恒定光照
 * 特性:
 * - 5分钟采样建立基准值(中位数滤波)
 * - 实时监测调节(±10%容差)
 * - 与PIR控制协调(PIR优先)
 * - 仅操作灯板(环境光/下光/上光),不操作红光
 */

#ifndef _CONSTANT_LIGHT_CONTROL_H_
#define _CONSTANT_LIGHT_CONTROL_H_

#include <esp_err.h>
#include <stdbool.h>
#include "light_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化恒光控制模块
 *
 * 必须在以下操作之后调用:
 * - mp3_player_init() (I2C总线初始化)
 * - bh1750_power_on() 和 bh1750_set_measure_mode() (传感器初始化)
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t constant_light_init(void);

/**
 * @brief 更新恒光控制状态
 *
 * 检查灯板状态,决定是否启动/停止恒光控制
 * 应在sensor_control_task主循环中周期性调用
 */
void constant_light_update_state(void);

/**
 * @brief 外部灯光变化通知(按键/MQTT调节亮度或开关灯)
 *
 * 当检测到外部操作改变灯光状态时调用,触发重新采样建立基准值
 * 应在on_light_change回调中调用
 */
void constant_light_on_light_change_external(void);

/**
 * @brief 暂停恒光控制
 *
 * PIR调暗时调用,停止自动调节亮度
 * 应在dim_timeout_timer_callback中调用
 */
void constant_light_suspend(void);

/**
 * @brief 恢复恒光控制
 *
 * PIR恢复亮度或按键操作后调用,重新启动采样和调节
 * 应在PIR检测到人体运动后调用
 */
void constant_light_resume(void);

/**
 * @brief 销毁恒光控制模块(释放资源)
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t constant_light_deinit(void);

/**
 * @brief 设置恒光控制功能启用状态
 *
 * @param enabled true启用,false禁用
 * @note 配置保存后,下次灯光开启时生效
 */
void constant_light_set_enabled(bool enabled);

/**
 * @brief 获取恒光控制功能启用状态
 *
 * @return true启用,false禁用
 */
bool constant_light_get_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* _CONSTANT_LIGHT_CONTROL_H_ */
