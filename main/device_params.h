#ifndef _DEVICE_PARAMS_H_
#define _DEVICE_PARAMS_H_

#include "light_manager.h"
#include <stdint.h>
#include <esp_err.h>

typedef struct device_params device_params_t;

/**
 * @brief 从NVS加载设备参数
 * @return ESP_OK成功，ESP_FAIL失败
 */
esp_err_t device_params_init(void);

/**
 * @brief 获取设备参数指针（仅用于内部模块）
 * @return 设备参数指针
 */
device_params_t* get_device_params(void);

void print_device_parmas(void);

/* ============ Getter 函数 ============ */

/* 灯光状态 */
uint8_t device_params_get_up_light_state(void);
uint8_t device_params_get_lower_light_state(void);
uint8_t device_params_get_ambient_light_state(void);

/* 灯光亮度 */
uint8_t device_params_get_up_light_brightness(void);
uint8_t device_params_get_lower_light_brightness(void);
uint8_t device_params_get_ambient_light_brightness(void);

/* 光疗参数 */
therapy_state_t device_params_get_therapy_state(void);
uint8_t device_params_get_therapy_bright(int index);

/* 音频参数 */
uint8_t device_params_get_music_state(void);
uint8_t device_params_get_voice_state(void);
uint8_t device_params_get_music_volume(void);

/* 光疗开关 */
uint8_t device_params_get_therapy_focus_state(void);
uint8_t device_params_get_therapy_sleep_state(void);
uint8_t device_params_get_therapy_sleep_duration(void);
uint8_t device_params_get_focus_source(void);

/* 其他参数 */
uint8_t device_params_get_constant_light_state(void);
uint8_t device_params_get_pir_state(void);
uint8_t device_params_get_dim_timeout(void);
uint8_t device_params_get_off_timeout(void);

/* ============ Setter 函数 ============ */

/**
 * @brief 设置音乐功能开关(保存NVS+实时应用)
 * @param state 0:关闭, 1:开启
 */
void device_params_set_music_state(uint8_t state);

/**
 * @brief 设置语音功能开关(保存NVS+实时应用)
 * @param state 0:关闭, 1:开启
 */
void device_params_set_voice_state(uint8_t state);

/**
 * @brief 设置音量(保存NVS+实时应用)
 * @param volume 音量档位(1-5)
 */
void device_params_set_music_volume(uint8_t volume);

/**
 * @brief 设置恒光控制开关(保存NVS+下次灯光开启时生效)
 * @param state 0:关闭, 1:开启
 */
void device_params_set_constant_light_state(uint8_t state);

/**
 * @brief 设置PIR功能开关(保存NVS+下次灯光开启时生效)
 * @param state 0:关闭, 1:开启
 */
void device_params_set_pir_state(uint8_t state);

/**
 * @brief 设置dim_timeout定时器时长(保存NVS+实时应用)
 * @param minutes 分钟数(1-45)
 */
void device_params_set_dim_timeout(uint8_t minutes);

/**
 * @brief 设置off_timeout定时器时长(保存NVS+实时应用)
 * @param minutes 分钟数(1-10)
 */
void device_params_set_off_timeout(uint8_t minutes);

/**
 * @brief 设置专注模式开关(保存NVS)
 * @param state 0:关闭, 1:开启
 */
void device_params_set_therapy_focus_state(uint8_t state);

/**
 * @brief 设置助眠模式开关(保存NVS)
 * @param state 0:关闭, 1:开启
 */
void device_params_set_therapy_sleep_state(uint8_t state);

/**
 * @brief 设置助眠模式时长(保存NVS)
 * @param minutes 分钟数(10-60, 步进10)
 */
void device_params_set_therapy_sleep_duration(uint8_t minutes);

/**
 * @brief 设置专注模式音源(保存NVS)
 * @param source 0:40Hz音频, 1:40Hz蜂鸣器
 */
void device_params_set_focus_source(uint8_t source);

#endif // _DEVICE_PARAMS_H_
