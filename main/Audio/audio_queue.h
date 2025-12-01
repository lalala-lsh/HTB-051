/**
 * @file audio_queue.h
 * @brief 音频队列管理器头文件
 * @author GYJ
 * @date 2025
 */

#ifndef AUDIO_QUEUE_H
#define AUDIO_QUEUE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音频优先级定义 */
#define AUDIO_PRIORITY_URGENT   0   // 最高优先级:紧急提示音(OTA升级)
#define AUDIO_PRIORITY_HIGH     1   // 高优先级:重要提示音(模式切换、灯光提示)
#define AUDIO_PRIORITY_NORMAL   2   // 普通优先级:一般提示音(欢迎语音)
#define AUDIO_PRIORITY_LOW      3   // 低优先级:背景音乐

/* 防抖时间配置(毫秒) */
#define AUDIO_DEBOUNCE_TIMER_MS        500    // 定时器提醒防抖0.5秒
#define AUDIO_DEBOUNCE_POWER_MS        2000   // 电量检测防抖2秒
#define AUDIO_DEBOUNCE_MUSIC_CTRL_MS   1000   // 音乐控制防抖1秒
#define AUDIO_DEBOUNCE_DEFAULT_MS      500    // 默认防抖0.5秒

/* 音频类型定义 */
typedef enum {
    AUDIO_TYPE_SYSTEM = 0,      // 系统音频
    AUDIO_TYPE_TIMER,           // 定时器提醒
    AUDIO_TYPE_POWER,           // 电量相关
    AUDIO_TYPE_MUSIC_CTRL,      // 音乐控制
    AUDIO_TYPE_OTA,             // OTA升级
    AUDIO_TYPE_FUNCTION,        // 功能开关
    AUDIO_TYPE_MAX
} audio_type_t;

/* 音频播放模式 */
typedef enum {
    AUDIO_PLAY_MODE_ONCE = 0,   // 单次播放
    AUDIO_PLAY_MODE_LOOP        // 循环播放
} audio_play_mode_t;

/* 音频请求结构体 */
typedef struct {
    const char* file_path;      // 音频文件路径
    audio_type_t type;          // 音频类型
    uint8_t priority;           // 优先级
    audio_play_mode_t mode;     // 播放模式
    bool need_resume_music;     // 播放完成后是否需要恢复背景音乐
    uint32_t debounce_ms;       // 防抖时间
} audio_request_t;

/**
 * @brief 初始化音频队列管理器
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_init(void);

/**
 * @brief 反初始化音频队列管理器
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_deinit(void);

/**
 * @brief 请求播放音频(带防抖)
 * @param file_path 音频文件路径
 * @param type 音频类型
 * @param priority 优先级(0-3,0最高)
 * @param need_resume_music 播放完成后是否恢复背景音乐
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_play(const char* file_path, audio_type_t type, uint8_t priority, bool need_resume_music);

/**
 * @brief 请求播放音频循环(背景音乐)
 * @param file_path 音频文件路径
 * @param type 音频类型
 * @param priority 优先级
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_play_loop(const char* file_path, audio_type_t type, uint8_t priority);

/**
