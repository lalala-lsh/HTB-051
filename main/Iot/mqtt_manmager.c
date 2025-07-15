#include "mqtt_manmager.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/idf_additions.h"
#include "system_info.h"

#include "blufi_example.h"
#include "device_params.h"
#include "protocol.h"
#include "protocol_parse.h"
#include "sntp_service.h"

#include <mqtt_client.h>

#include <esp_log.h>
#include <freertos/event_groups.h>
#include <mbedtls/md.h>
#include <string.h>
#include <time.h>

#define TAG "mqtt_client"

/* 灯光同步事件位 */
#define LIGHT_SYNC_BIT         BIT0
#define LIGHT_SYNC_DEBOUNCE_MS 500

/* 每日同步时间窗口配置 (7:00:00 - 9:00:00) */
#define DAILY_SYNC_START_HOUR     7
#define DAILY_SYNC_END_HOUR       9
#define DAILY_SYNC_WINDOW_SECONDS ((DAILY_SYNC_END_HOUR - DAILY_SYNC_START_HOUR) * 3600)

struct mqtt_client_config
{
    char publish_topic[MQTT_TOPIC_SIZE];
    char subscribe_topic[MQTT_TOPIC_SIZE];

    esp_mqtt_client_config_t mqtt_cfg;
};

static mqtt_client_config_t* mqtt_client_cfg = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL; // MQTT客户端句柄
static mqtt_state_t mqtt_state = MQTT_STATE_IDLE;
static TaskHandle_t heartbeat_task_handle = NULL;
static TaskHandle_t light_sync_task_handle = NULL;
static TaskHandle_t daily_sync_task_handle = NULL;
static EventGroupHandle_t mqtt_event_group = NULL;

/* 前向声明 */
static void light_sync_task(void* pvParameters);
static void daily_sync_task(void* pvParameters);

static void hash(unsigned char* output, const unsigned char* input, size_t input_len,
                 mbedtls_md_type_t md_type)
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, input, input_len);
    mbedtls_md_finish(&ctx, output);
    mbedtls_md_free(&ctx);
}

static unsigned int mod(unsigned char* hash_value, unsigned int mod_num)
{
    unsigned int result = 0;
    for (int i = 0; i < 16; i++) {
        result <<= 8;            // 每次将结果左移8位
        result += hash_value[i]; // 加上当前字节
        result %= mod_num;       // 取模
    }
    if (result == 0) {
        result = mod_num;
    }
    return result;
}

/**
 * @brief 创建MQTT主题
 * @note 根据设备MAC地址计算哈希值来确定主题
 */
static void create_mqtt_topic(mqtt_client_config_t* manager)
{
    // 获取设备信息
    const char* device_sn = get_device_sn();
    const char* device_mac = get_device_mac();

    unsigned char hash_value[16];
    hash(hash_value, (unsigned char*)device_mac, strlen(device_mac), MBEDTLS_MD_MD5);
    unsigned int result = mod(hash_value, 2);

    // 构建发布和订阅主题
    snprintf(manager->publish_topic, sizeof(manager->publish_topic), "%s%u/%s/htb_pub",
             MQTT_CONFIG_TOPIC_PREFIX, result, device_sn);

    snprintf(manager->subscribe_topic, sizeof(manager->subscribe_topic), "%s%s/htb_sub",
             MQTT_CONFIG_TOPIC_PREFIX, device_sn);
}

mqtt_client_config_t* mqtt_client_create(void)
{
    mqtt_client_config_t* manager =
        (mqtt_client_config_t*)heap_caps_calloc(1, sizeof(mqtt_client_config_t), MALLOC_CAP_SPIRAM);
    if (manager == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for mqtt_client_config");
        return NULL;
    }

    // 创建MQTT主题
    create_mqtt_topic(manager);

    // 获取设备SN
    const char* device_sn = get_device_sn();

    // 动态分配client_id内存
    size_t client_id_len = strlen("htb-") + strlen(device_sn) + 1;
    manager->mqtt_cfg.credentials.client_id =
        (char*)heap_caps_calloc(1, client_id_len, MALLOC_CAP_SPIRAM);
    if (manager->mqtt_cfg.credentials.client_id == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for client_id");
        heap_caps_free(manager);
        return NULL;
    }
    snprintf(manager->mqtt_cfg.credentials.client_id, client_id_len, "htb-%s", device_sn);

    manager->mqtt_cfg.broker.address.uri = MQTT_CONFIG_BROKER_URL;
    manager->mqtt_cfg.credentials.username = MQTT_CONFIG_USERNAME;
    manager->mqtt_cfg.credentials.authentication.password = MQTT_CONFIG_PASSWORD;

    // 构建遗嘱消息
    char* will_msg = build_will_message();
    if (!will_msg) {
        ESP_LOGE(TAG, "创建遗嘱消息失败");
        return NULL;
    }

    manager->mqtt_cfg.session.last_will.topic = manager->publish_topic;
    manager->mqtt_cfg.session.last_will.msg = will_msg;
    manager->mqtt_cfg.session.last_will.msg_len = strlen(will_msg);
    manager->mqtt_cfg.session.last_will.qos = MQTT_LWT_QOS;
    manager->mqtt_cfg.session.last_will.retain = MQTT_LWT_RETAIN;
    manager->mqtt_cfg.session.keepalive = 120;

    return manager;
}

/**
 * @brief 心跳包发送任务
 */
static void heartbeat_task(void* pvParameters)
{
    const TickType_t heartbeat_delay = pdMS_TO_TICKS(MQTT_HEARTBEAT_INTERVAL);

    while (1) {
        if (mqtt_client != NULL) {

            // 构建并发送心跳包
            char* heartbeat_msg = build_heartbeat_message();
            // 打印心跳包内容
            ESP_LOGI(TAG, "心跳包内容: %s", heartbeat_msg);
            if (heartbeat_msg) {
                esp_mqtt_client_publish(mqtt_client, mqtt_client_cfg->publish_topic, heartbeat_msg,
                                        0, MQTT_QOS, 0);
                free(heartbeat_msg);
            }
            else {
                ESP_LOGE(TAG, "构建心跳消息失败");
            }
        }
        else {
            ESP_LOGW(TAG, "MQTT未连接，跳过心跳");
        }

        // 延时
        vTaskDelay(heartbeat_delay);
    }
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                               void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_state = MQTT_STATE_CONNECTED;

            ESP_LOGI(TAG, "MQTT连接成功");

            // 订阅主题
            esp_mqtt_client_subscribe(event->client, mqtt_client_cfg->subscribe_topic, MQTT_QOS);

            sntp_wait_sync(portMAX_DELAY);

            char* boot_sync_msg = build_boot_sync_message();
            if (boot_sync_msg) {
                // 打印开机同步消息内容
                ESP_LOGI(TAG, "开机同步消息内容: %s", boot_sync_msg);
                esp_mqtt_client_publish(event->client, mqtt_client_cfg->publish_topic,
                                        boot_sync_msg, 0, MQTT_QOS, 0);
                free(boot_sync_msg);
            }
            else {
                ESP_LOGE(TAG, "构建开机同步消息失败");
            }

            if (heartbeat_task_handle == NULL) {
                xTaskCreate(heartbeat_task,          // 任务函数
                            "heartbeat_task",        // 任务名称
                            4096,                    // 任务堆栈大小
                            NULL,                    // 任务参数
                            5,                       // 任务优先级
                            &heartbeat_task_handle); // 任务句柄
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_state = MQTT_STATE_DISCONNECTED;
            ESP_LOGI(TAG, "MQTT断开连接, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG, "MQTT订阅成功, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            mqtt_state = MQTT_STATE_UNSUBSCRIBED;
            ESP_LOGD(TAG, "MQTT取消订阅, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT发布成功, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            // ESP_LOGI(TAG, "MQTT收到数据, topic=%.*s", event->topic_len, event->topic);

            // 安全地处理接收到的数据
            if (event->data_len > 0) {
                // 打印接收到的数据（使用长度限制，因为 event->data 不是 null 结尾）
                ESP_LOGI(TAG, "接收到的数据: %.*s", event->data_len, event->data);

                // 处理收到的消息（直接传递指针和长度，避免内存分配）
                process_received_message(event->client, event->data, event->data_len);
            }

            // 这里不再需要获取信号量，因为我们已经在上面释放了
            return;

        case MQTT_EVENT_ERROR:
            mqtt_state = MQTT_STATE_ERROR;
            ESP_LOGE(TAG, "MQTT错误，类型=%d", event->error_handle->error_type);

            // 根据错误类型提供更详细的信息
            switch (event->error_handle->error_type) {
                case MQTT_ERROR_TYPE_TCP_TRANSPORT:
                    ESP_LOGE(TAG, "传输错误: %d", event->error_handle->esp_transport_sock_errno);
                    break;
                default:
                    ESP_LOGE(TAG, "未知错误");
                    break;
            }
            break;

        case MQTT_EVENT_BEFORE_CONNECT:
            mqtt_state = MQTT_STATE_BEFORE_CONNECT;
            ESP_LOGI(TAG, "MQTT准备连接");
            break;

        default:
