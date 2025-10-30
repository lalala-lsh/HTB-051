#ifndef _SENSOR_CONTROL_H_
#define _SENSOR_CONTROL_H_

#include "device_params.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#define DIM_TIMEOUT      -1
#define DIM_TIMEOUT_TIMEOUT_MS   ((DIM_TIMEOUT == -1) ? \
                                     (device_params_get_dim_timeout() * 60 * 1000) : (DIM_TIMEOUT * (60 * 1000)))

#define OFF_TIMEOUT      -1
#define OFF_TIMEOUT_TIMEOUT_MS   ((OFF_TIMEOUT == -1) ? \
                                     (device_params_get_off_timeout() * 60 * 1000) : (OFF_TIMEOUT * (60 * 1000)))

/* 亮度变化来源标识 */
typedef enum {
    BRIGHTNESS_CHANGE_SOURCE_EXTERNAL = 0,       // 本地按键
    BRIGHTNESS_CHANGE_SOURCE_PIR = 1,            // PIR控制
    BRIGHTNESS_CHANGE_SOURCE_CONSTANT_LIGHT = 2, // 恒光控制
    BRIGHTNESS_CHANGE_SOURCE_REMOTE = 3          // 远端MQTT控制(CMD 209)
} brightness_change_source_t;

extern volatile uint8_t brightness_change_source;

/**
 * @brief 初始化传感器控制模块
 */
void sensor_control_init(void);

/**
 * @brief 设置PIR功能启用状态
 * @param enabled true启用,false禁用
 * @note 配置保存后,下次灯光开启时生效
 */
void sensor_control_set_pir_enabled(bool enabled);

/**
 * @brief 获取PIR功能启用状态
 * @return true启用,false禁用
 */
bool sensor_control_get_pir_enabled(void);

/**
 * @brief 设置dim_timeout定时器时长
 * @param minutes 分钟数(1-45)
 */
void sensor_control_set_dim_timeout(uint8_t minutes);

/**
 * @brief 设置off_timeout定时器时长
 * @param minutes 分钟数(1-10)
 */
void sensor_control_set_off_timeout(uint8_t minutes);

/**
 * @brief 查询PIR是否处于调暗状态
 *
 * @return true 处于调暗状态（准备关灯），false 正常状态
 */
bool sensor_control_is_pir_dimmed(void);

/**
 * @brief 清空PIR日志环形缓冲区
 */
void pir_log_clear(void);

/**
 * @brief 获取PIR日志环形缓冲区内容(cJSON数组)
 * @return cJSON* 字符串数组，调用者负责释放；失败返回NULL
 */
cJSON* pir_log_get_json_array(void);

#endif // _SENSOR_CONTROL_H_