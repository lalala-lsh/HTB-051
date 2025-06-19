#include "system_info.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>
#include <stdlib.h>

#define TAG "system_info"

struct system_info {
    char version[32];     // 版本号
    char mac_addr[18];    // MAC地址 (格式 "XX:XX:XX:XX:XX:XX")
    char sn[17];          // SN号
    char device_name[32]; // 设备名称（从Kconfig获取）
};

// 静态全局变量
static system_info_t system_info = {0};

esp_err_t system_info_init(void)
{
    esp_err_t ret;

    // init version
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    ret = esp_ota_get_partition_description(running_partition, &app_desc);
    if (ret == ESP_OK) {
        strcpy(system_info.version, app_desc.version);
    } else {
        ESP_LOGE(TAG, "获取版本出错: %d", ret);
    }

    // init sn - 使用settings模块
    settings_t *settings = settings_start("SNCode_NVS", false);  // 只读模式,命名空间: system_info
    if (settings != NULL) {
        // 直接读取到目标缓冲区，无需额外分配内存
        esp_err_t err = settings_get_string(settings, "SNCode", 
                                             system_info.sn, sizeof(system_info.sn), 
                                             NULL);
        if (err == ESP_OK) {
            settings_end(settings);
            ESP_LOGD(TAG, "从NVS加载SN码: %s", system_info.sn);
        } else {
            // SN码不存在,使用默认值并写入NVS
            settings_end(settings);
            ESP_LOGW(TAG, "SN码未初始化,使用默认SN码并写入NVS");
            strncpy(system_info.sn, CONFIG_EXAMPLE_DEFAULT_SN_CODE, sizeof(system_info.sn) - 1);
            system_info.sn[sizeof(system_info.sn) - 1] = '\0';

            // 将默认SN码写入NVS
            settings_t *write_settings = settings_start("SNCode_NVS", true);  // 读写模式
            if (write_settings != NULL) {
                settings_set_string(write_settings, "SNCode", system_info.sn);
                settings_end(write_settings);
            }
        }
    } else {
        // 命名空间不存在,创建并初始化
        ESP_LOGI(TAG, "首次启动,创建system_info命名空间");
        strncpy(system_info.sn, CONFIG_EXAMPLE_DEFAULT_SN_CODE, sizeof(system_info.sn) - 1);
        system_info.sn[sizeof(system_info.sn) - 1] = '\0';

        settings_t *write_settings = settings_start("SNCode_NVS", true);  // 读写模式创建
        if (write_settings != NULL) {
            settings_set_string(write_settings, "SNCode", system_info.sn);
            settings_end(write_settings);
            ESP_LOGI(TAG, "默认SN码已保存: %s", system_info.sn);
        } else {
            ESP_LOGE(TAG, "无法创建system_info命名空间");
        }
    }

    // init mac
    uint8_t mac[6];
    ret = esp_read_mac(mac, ESP_MAC_BASE);
