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
            ESP_LOGI(TAG, "其他MQTT事件: %d", event->event_id);
            break;
    }
}

void mqtt_client_init(void)
{
    mqtt_client_cfg = mqtt_client_create();

    // 创建MQTT客户端
    mqtt_client = esp_mqtt_client_init(&mqtt_client_cfg->mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT客户端初始化失败");
        return;
    }

    // 创建事件组
    mqtt_event_group = xEventGroupCreate();
    if (mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "创建MQTT事件组失败");
        return;
    }

    // 创建灯光同步任务
    BaseType_t ret =
        xTaskCreate(light_sync_task, "light_sync", 4096, NULL, 5, &light_sync_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建灯光同步任务失败");
    }

    // 创建每日版本同步任务
    ret = xTaskCreate(daily_sync_task, "daily_sync", 4096, NULL, 4, &daily_sync_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建每日同步任务失败");
    }

    // 注册事件处理
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

esp_err_t mqtt_client_start(void)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT客户端未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    // 开始MQTT客户端
    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动MQTT客户端失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t mqtt_client_stop(void)
{
    if (mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT客户端未初始化或已停止");
        return ESP_OK;
    }

    // 只停止客户端，不销毁（这样可以再次start）
    esp_err_t err = esp_mqtt_client_stop(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "停止MQTT客户端失败: %s", esp_err_to_name(err));
        return err;
    }

    mqtt_state = MQTT_STATE_IDLE;
    return ESP_OK;
}

mqtt_state_t mqtt_client_get_state(void)
{
    return mqtt_state;
}

const char* mqtt_client_get_subscribe_topic(void)
{
    if (mqtt_client_cfg == NULL) {
        return NULL;
    }
    return mqtt_client_cfg->subscribe_topic;
}

const char* mqtt_client_get_publish_topic(void)
{
    if (mqtt_client_cfg == NULL) {
        return NULL;
    }
    return mqtt_client_cfg->publish_topic;
}

/**
 * @brief 灯光同步任务
 * @note 等待事件 -> 防抖 -> 发送同步消息
 */
static void light_sync_task(void* pvParameters)
{
    while (1) {
        // 等待灯光变化事件
        EventBits_t bits =
            xEventGroupWaitBits(mqtt_event_group, LIGHT_SYNC_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & LIGHT_SYNC_BIT) {
            // 防抖：等待500ms，如果期间有新事件则重新计时
            while (1) {
                EventBits_t debounce_bits =
                    xEventGroupWaitBits(mqtt_event_group, LIGHT_SYNC_BIT, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(LIGHT_SYNC_DEBOUNCE_MS));

                if (!(debounce_bits & LIGHT_SYNC_BIT)) {
                    // 超时无新事件，可以发送了
                    break;
                }
                // 有新事件，继续等待
            }

            // 检查MQTT是否已连接
            if (mqtt_state != MQTT_STATE_CONNECTED || mqtt_client == NULL) {
                ESP_LOGW(TAG, "MQTT未连接，丢弃灯光同步");
                continue;
            }

            // 构建并发送同步消息
            char* sync_msg = build_params_sync_message();
            if (sync_msg) {
                ESP_LOGD(TAG, "灯光同步消息: %s", sync_msg);
                esp_mqtt_client_publish(mqtt_client, mqtt_client_cfg->publish_topic, sync_msg, 0,
                                        MQTT_QOS, 0);
                free(sync_msg);
            }
            else {
                ESP_LOGE(TAG, "构建灯光同步消息失败");
            }
        }
    }
}

void mqtt_notify_light_change(void)
{
    if (mqtt_event_group != NULL) {
        xEventGroupSetBits(mqtt_event_group, LIGHT_SYNC_BIT);
    }
}

/**
 * @brief 计算距离下一个同步时间点的秒数
 * @param now 当前时间的tm结构
 * @param random_offset 在时间窗口内的随机偏移（秒）
 * @return 距离下一个同步时间点的秒数
 */
static uint32_t calculate_seconds_until_sync(struct tm* now, uint32_t random_offset)
{
    // 计算今天同步时间点的秒数（从0点开始）
    uint32_t sync_time_today = DAILY_SYNC_START_HOUR * 3600 + random_offset;

    // 计算当前时间的秒数（从0点开始）
    uint32_t current_seconds = now->tm_hour * 3600 + now->tm_min * 60 + now->tm_sec;

    if (current_seconds < sync_time_today) {
        // 今天的同步时间还没到
        return sync_time_today - current_seconds;
    }
    else {
        // 今天的同步时间已过，等待明天
        return (24 * 3600 - current_seconds) + sync_time_today;
    }
}

/**
 * @brief 每日版本同步任务
 * @note 在7:00-9:00之间随机时间上报版本信息，减轻服务器压力
 */
static void daily_sync_task(void* pvParameters)
{
    // 等待SNTP时间同步完成
    ESP_LOGI(TAG, "每日同步任务：等待SNTP时间同步...");
    sntp_wait_sync(portMAX_DELAY);
    ESP_LOGI(TAG, "每日同步任务：SNTP时间同步完成");

    // 生成本设备的随机偏移（0 ~ 7200秒，即0~2小时）
    // 使用设备MAC作为种子的一部分，使同一设备每次启动的随机偏移相对稳定
    uint32_t random_offset = esp_random() % DAILY_SYNC_WINDOW_SECONDS;

    while (1) {
        // 获取当前时间
        time_t now_time;
        struct tm now_tm;
        time(&now_time);
        localtime_r(&now_time, &now_tm);

        // 计算距离下一个同步时间点的秒数
        uint32_t wait_seconds = calculate_seconds_until_sync(&now_tm, random_offset);

        // 计算目标同步时间用于日志显示
        uint32_t sync_hour = DAILY_SYNC_START_HOUR + (random_offset / 3600);
        uint32_t sync_min = (random_offset % 3600) / 60;
        uint32_t sync_sec = random_offset % 60;

        ESP_LOGI(TAG, "每日同步：下次同步时间 %02lu:%02lu:%02lu，等待 %lu 秒", sync_hour, sync_min,
                 sync_sec, wait_seconds);

        // 等待到同步时间
        vTaskDelay(pdMS_TO_TICKS(wait_seconds * 1000));

        // 检查MQTT是否已连接
        if (mqtt_state != MQTT_STATE_CONNECTED || mqtt_client == NULL) {
            ESP_LOGW(TAG, "每日同步：MQTT未连接，跳过本次同步");
            // 等待1小时后重试
            vTaskDelay(pdMS_TO_TICKS(3600 * 1000));
            continue;
        }

        // 发送同步消息（复用开机同步消息）
        char* sync_msg = build_boot_sync_message();
        if (sync_msg) {
            ESP_LOGI(TAG, "每日同步：发送版本同步消息");
            esp_mqtt_client_publish(mqtt_client, mqtt_client_cfg->publish_topic, sync_msg, 0,
                                    MQTT_QOS, 0);
            free(sync_msg);
        }
        else {
            ESP_LOGE(TAG, "每日同步：构建同步消息失败");
        }

        // 生成下一天的新随机偏移
        random_offset = esp_random() % DAILY_SYNC_WINDOW_SECONDS;
    }
}

void mqtt_publish_therapy_record(const work_record_t* record)
{
    if (record == NULL) {
        return;
    }

    if (mqtt_state != MQTT_STATE_CONNECTED || mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT未连接，光疗记录丢弃");
        return;
    }

    char* msg = build_realtime_report_message(record);
    if (msg) {
        ESP_LOGI(TAG, "上报光疗记录: mode=%d, work_time=%d秒",
                 record->mode, record->work_time);
        esp_mqtt_client_publish(mqtt_client, mqtt_client_cfg->publish_topic,
                               msg, 0, MQTT_QOS, 0);
        free(msg);
    }
}

void mqtt_publish_unbind_message(void)
{
    if (mqtt_state != MQTT_STATE_CONNECTED || mqtt_client == NULL || mqtt_client_cfg == NULL) {
        ESP_LOGW(TAG, "MQTT未连接，跳过设备解绑消息发布");
        return;
    }

    char* msg = build_unbind_message();
    if (msg == NULL) {
        ESP_LOGE(TAG, "构建设备解绑消息失败");
        return;
    }

    int msg_id = esp_mqtt_client_publish(mqtt_client, mqtt_client_cfg->publish_topic,
                                         msg, 0, MQTT_QOS, 0);
    ESP_LOGI(TAG, "已发布设备解绑消息，不等待回复, msg_id=%d", msg_id);
    free(msg);
}
