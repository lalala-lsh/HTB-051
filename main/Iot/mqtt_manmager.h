#ifndef _MQTT_MANMAGER_H_
#define _MQTT_MANMAGER_H_

#include "esp_bit_defs.h"
#include "esp_err.h"
#include "protocol.h"

#define ON_LINE_SERVER 1

#if ON_LINE_SERVER
#define MQTT_CONFIG_BROKER_URL "mqtt://emqx.eyenice.cn:1883" /**< 在线服务器地址 */
#define MQTT_CONFIG_PASSWORD "6g7JqYe1mRmq0ZY_"              /**< 在线服务器密码 */
#else
#define MQTT_CONFIG_BROKER_URL "mqtt://110.87.103.170:6883" /**< 本地服务器地址 */
#define MQTT_CONFIG_PASSWORD "Dj7Byhm!ZX3aE8V8"             /**< 本地服务器密码 */
#endif

#define MQTT_CONFIG_USERNAME "htb_device"
#define MQTT_CONFIG_TOPIC_PREFIX "/topic/dld_051/"

typedef struct mqtt_client_config mqtt_client_config_t;

/**
 * @brief MQTT连接状态枚举
 */
typedef enum {
    MQTT_STATE_IDLE,           /**< 初始空闲状态 */
    MQTT_STATE_CONNECTED,      /**< 已连接到MQTT服务器 */
    MQTT_STATE_DISCONNECTED,   /**< 与MQTT服务器断开连接 */
    MQTT_STATE_SUBSCRIBED,     /**< 已成功订阅主题 */
    MQTT_STATE_UNSUBSCRIBED,   /**< 已取消订阅主题 */
    MQTT_STATE_PUBLISHED,      /**< 消息发布成功 */
    MQTT_STATE_DATA,           /**< 收到MQTT消息数据 */
    MQTT_STATE_ERROR,          /**< 发生MQTT错误 */
    MQTT_STATE_BEFORE_CONNECT, /**< 连接前状态 */
} mqtt_state_t;

// MQTT配置参数
#define MQTT_LWT_QOS 1
#define MQTT_LWT_RETAIN 0 // 不需要保留消息
#define MQTT_QOS 1

// MQTT缓冲区配置
#define MQTT_TOPIC_BUF_SIZE 64
#define MQTT_DEVICE_STR_SIZE 20
#define MQTT_TOPIC_SIZE 64
#define MQTT_CLIENT_ID_LEN 64

// MQTT重连配置
#define MQTT_RECONNECT_DELAY_MIN 1000      // 最小重连延迟(毫秒)
#define MQTT_RECONNECT_DELAY_MAX 60000     // 最大重连延迟(毫秒)
#define MQTT_RECONNECT_ATTEMPTS_MAX 10     // 最大重连尝试次数
#define MQTT_HEARTBEAT_INTERVAL 15 * 60000 // 心跳间隔(毫秒)

/*******************************************************************************
 * Public API
 ******************************************************************************/

/**
 * @brief 初始化MQTT客户端
 * @note 创建MQTT客户端配置并注册事件处理器，但不启动连接
 */
void mqtt_client_init(void);

/**
 * @brief 启动MQTT客户端
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t mqtt_client_start(void);

/**
 * @brief 停止MQTT客户端
 * @return esp_err_t ESP_OK成功，其他失败
 * @note 不能在MQTT事件回调中调用此函数
 */
esp_err_t mqtt_client_stop(void);

/**
 * @brief 获取当前MQTT连接状态
 * @return mqtt_state_t 当前状态
 */
mqtt_state_t mqtt_client_get_state(void);

/**
 * @brief 获取订阅主题
 * @return const char* 订阅主题字符串，未初始化时返回NULL
 */
const char* mqtt_client_get_subscribe_topic(void);

/**
 * @brief 获取发布主题
 * @return const char* 发布主题字符串，未初始化时返回NULL
 */
const char* mqtt_client_get_publish_topic(void);

/**
 * @brief 通知灯光状态变化，触发MQTT同步
 * @note 内部有500ms防抖，频繁调用只会发送一次
 */
void mqtt_notify_light_change(void);

/**
 * @brief 发布光疗训练记录到MQTT
 * @param record 训练记录
 */
void mqtt_publish_therapy_record(const work_record_t* record);

/**
 * @brief 发布设备解绑消息
 * @note 仅发布，不等待服务器解绑回复
 */
void mqtt_publish_unbind_message(void);

#endif // _MQTT_MANMAGER_H_
