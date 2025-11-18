/**
 * @file audio_mode.h
 * @brief 音频模式管理器接口
 *
 * 该模块负责协调本地MP3播放和A2DP蓝牙音频之间的切换。
 * 两种模式互斥,同一时间只能有一种模式处于活动状态。
 */

#ifndef __AUDIO_MODE_H__
#define __AUDIO_MODE_H__

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音频播放模式枚举
 */
typedef enum {
    AUDIO_PLAYBACK_MODE_LOCAL,      /*!< 本地模式: MP3播放器 + audio_queue */
    AUDIO_PLAYBACK_MODE_A2DP,       /*!< A2DP模式: 蓝牙音频接收 */
} audio_playback_mode_t;

/**
 * @brief 初始化音频模式管理器
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t audio_mode_init(void);

/**
 * @brief 获取当前音频模式
 *
 * @return audio_playback_mode_t 当前模式
 */
audio_playback_mode_t audio_mode_get_current(void);

/**
 * @brief 切换到A2DP模式
 *
 * 执行以下操作:
 * 1. 停止本地音频播放
 * 2. 禁用 audio_queue
 * 3. 初始化并启动 A2DP Sink
 * 4. 设备变为可发现状态,等待手机连接
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 *         - ESP_ERR_INVALID_STATE 已经在A2DP模式
 */
esp_err_t audio_mode_switch_to_a2dp(void);

/**
 * @brief 切换到本地模式
 *
 * 执行以下操作:
 * 1. 停止并反初始化 A2DP Sink
 * 2. 启用 audio_queue
 * 3. 恢复本地音频播放能力
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 *         - ESP_ERR_INVALID_STATE 已经在本地模式
 */
esp_err_t audio_mode_switch_to_local(void);

/**
 * @brief 检查是否处于A2DP模式
 *
 * @return true 处于A2DP模式
 * @return false 处于本地模式
 */
bool audio_mode_is_a2dp(void);

/**
 * @brief 检查是否处于本地模式
 *
 * @return true 处于本地模式
 * @return false 处于A2DP模式
 */
bool audio_mode_is_local(void);

#ifdef __cplusplus
}
#endif

#endif // __AUDIO_MODE_H__

