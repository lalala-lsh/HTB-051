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
