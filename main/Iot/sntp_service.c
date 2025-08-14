#include "sntp_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_sntp.h>

#include <time.h>

#include "blufi_example.h"

static const char *TAG = "SNTP_SERVICE";

// SNTP任务句柄
static TaskHandle_t sntp_task_handle = NULL;

// 添加事件组句柄
static EventGroupHandle_t sntp_event_group = NULL;

extern const int CONNECTED_BIT;

// NTP服务器列表
static const char *NTP_SERVERS[] = {
    "ntp.aliyun.com",         // 阿里云主NTP服务器
    "ntp1.aliyun.com",        // 阿里云备用NTP服务器
    "ntp2.aliyun.com",        // 阿里云备用NTP服务器
    "ntp.tencent.com",        // 腾讯云主NTP服务器
    "time1.cloud.tencent.com",// 腾讯云备用NTP服务器
    "cn.ntp.org.cn",          // 中国NTP服务器
    "cn.pool.ntp.org",        // 中国NTP服务器池
};

#define SNTP_RETRY_COUNT  10

#define NTP_SERVERS_COUNT (sizeof(NTP_SERVERS) / sizeof(char*))

#define SNTP_SYNC_INTERVAL (5 * 60 * 1000)

static void sntp_sync_task(void *pvParameters)
{
    // 等待WiFi连接
    xEventGroupWaitBits(get_wifi_event_group(),
                       CONNECTED_BIT,
                       pdFALSE,
                       pdTRUE,
                       portMAX_DELAY);

    // 初始化SNTP
    ESP_LOGI(TAG, "Initializing SNTP");

    setenv("TZ", "CST-8", 1);
    tzset();

    // 配置SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVERS[0]);  // 先设置第一个服务器
    esp_sntp_init();

    int current_server = 0;
    bool time_synced = false;

    while (!time_synced)
    {
        ESP_LOGI(TAG, "尝试与NTP服务器同步: %s", NTP_SERVERS[current_server]);
        
        // 等待同步完成
        int retry = 0;
        while (retry < SNTP_RETRY_COUNT) {  // 每个服务器尝试10次
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                time_synced = true;
                break;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            retry++;
