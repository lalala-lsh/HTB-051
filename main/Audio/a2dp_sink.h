/**
 * @file a2dp_sink.h
 * @brief A2DP Sink (蓝牙音箱) 模块接口
 *
 * 该模块实现蓝牙A2DP Sink功能,允许设备作为蓝牙音箱接收手机音频。
 * 基于ESP-ADF的a2dp_sink_stream实现。
 */

#ifndef __A2DP_SINK_H__
#define __A2DP_SINK_H__

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A2DP Sink 连接状态枚举
 */
typedef enum {
    A2DP_SINK_STATE_IDLE,           /*!< 空闲状态,未初始化 */
    A2DP_SINK_STATE_INITIALIZED,    /*!< 已初始化,等待连接 */
    A2DP_SINK_STATE_CONNECTING,     /*!< 正在连接 */
    A2DP_SINK_STATE_CONNECTED,      /*!< 已连接 */
    A2DP_SINK_STATE_DISCONNECTING,  /*!< 正在断开 */
    A2DP_SINK_STATE_ERROR,          /*!< 错误状态 */
} a2dp_sink_state_t;

/**
 * @brief A2DP Sink 事件类型
 */
typedef enum {
    A2DP_SINK_EVENT_CONNECTED,      /*!< A2DP源已连接 */
    A2DP_SINK_EVENT_DISCONNECTED,   /*!< A2DP源已断开 */
    A2DP_SINK_EVENT_AUDIO_START,    /*!< 音频开始播放 */
    A2DP_SINK_EVENT_AUDIO_STOP,     /*!< 音频停止播放 */
} a2dp_sink_event_t;

/**
 * @brief A2DP Sink 事件回调函数类型
 *
 * @param event 事件类型
 * @param param 事件参数(保留)
 */
typedef void (*a2dp_sink_event_cb_t)(a2dp_sink_event_t event, void *param);

/**
 * @brief 初始化 A2DP Sink
 *
 * 创建A2DP Sink音频管道:
 * A2DP Source → A2DP Sink Stream → I2S Stream → 音频Codec
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 *         - ESP_ERR_INVALID_STATE 已经初始化
 */
esp_err_t a2dp_sink_init(void);

/**
 * @brief 反初始化 A2DP Sink
 *
 * 停止A2DP服务并释放所有资源
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t a2dp_sink_deinit(void);

/**
 * @brief 开始 A2DP Sink 服务
 *
 * 使设备可被发现,等待A2DP源连接
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t a2dp_sink_start(void);

/**
 * @brief 停止 A2DP Sink 服务
 *
 * 断开当前连接,停止可发现状态
 *
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t a2dp_sink_stop(void);

/**
 * @brief 检查A2DP是否已连接
 *
 * @return true 已连接
 * @return false 未连接
 */
bool a2dp_sink_is_connected(void);

/**
 * @brief 获取A2DP Sink当前状态
 *
 * @return a2dp_sink_state_t 当前状态
 */
a2dp_sink_state_t a2dp_sink_get_state(void);

/**
 * @brief 注册A2DP Sink事件回调
 *
 * @param callback 事件回调函数
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_ERR_INVALID_ARG 参数无效
 */
esp_err_t a2dp_sink_register_callback(a2dp_sink_event_cb_t callback);

/**
 * @brief 设置A2DP音量
 *
 * @param volume 音量值,范围0-100
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t a2dp_sink_set_volume(int volume);

#ifdef __cplusplus
}
#endif

#endif // __A2DP_SINK_H__

