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
        }

        if (!time_synced) {
            // 切换到下一个服务器
            current_server = (current_server + 1) % NTP_SERVERS_COUNT;
            esp_sntp_stop();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_sntp_setservername(0, NTP_SERVERS[current_server]);
            esp_sntp_init();
            ESP_LOGW(TAG, "同步失败，切换到下一个服务器");
        }
    }

    // 时间同步成功后，打印当前时间
    time_t now;
    time(&now);
    char strftime_buf[64];
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "时间同步成功，当前时间: %s", strftime_buf);

    // 时间同步成功后，设置事件位
    if (time_synced) {
        xEventGroupSetBits(sntp_event_group, SNTP_SYNCED_BIT);
        ESP_LOGI(TAG, "时间同步成功，设置同步事件位");
    }

    vTaskDelete(sntp_task_handle);
}

esp_err_t sntp_wait_sync(uint32_t timeout_ms)
{
    if (sntp_event_group == NULL) {
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(sntp_event_group,
                                          SNTP_SYNCED_BIT,
                                          pdFALSE,
                                          pdTRUE,
                                          pdMS_TO_TICKS(timeout_ms));

    if (bits & SNTP_SYNCED_BIT) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
} 

esp_err_t sntp_service_init(void)
{
    // 创建事件组
    sntp_event_group = xEventGroupCreate();
    if (sntp_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create sntp event group");
        return ESP_FAIL;
    }

    // 创建SNTP同步任务
    xTaskCreate(sntp_sync_task,
                "sntp_sync_task",
                4096,
                NULL,
                5,
                &sntp_task_handle);

    if (sntp_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create sntp task");
        vEventGroupDelete(sntp_event_group);
        return ESP_FAIL;
    }

    return ESP_OK;
}
