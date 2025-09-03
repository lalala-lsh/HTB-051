#include "light_control.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

#include "button_manager.h"
#include "ledc_init.h"
#include "light_manager.h"
#include "mqtt_manmager.h"
#include "system_info.h"
#include "settings.h"
#include "Audio/audio_mode.h"
#include "Audio/audio_queue.h"
#include "Audio/tts_list.h"

#define TAG "light_control"

static button_manager_t* g_button_manager = NULL;
static light_manager_t* g_light_manager = NULL;
static bool g_buttons_enabled = false;

/**
 * @brief 恢复出厂设置
 *
 * 清除所有NVS命名空间数据，但保留"SNCode_NVS"命名空间（设备SN码）
 * 包括清除WiFi配置、设备参数等
 * 注意：MAC地址从硬件读取，无需备份
 */
void factory_reset(void)
{
    ESP_LOGW(TAG, "=== Factory Reset Initiated ===");

    // 1. 备份system_info命名空间的SN码（MAC地址从硬件读取，无需备份）
    // 使用栈上缓冲区，无需动态内存分配
    char sn_backup[17] = {0};
    bool has_sn_backup = false;
    
    settings_t* backup_nvs = settings_start("SNCode_NVS", false);
    if (backup_nvs != NULL) {
        esp_err_t err = settings_get_string(backup_nvs, "SNCode", 
                                             sn_backup, sizeof(sn_backup), 
                                             NULL);
        settings_end(backup_nvs);

        if (err == ESP_OK) {
            has_sn_backup = true;
            ESP_LOGI(TAG, "Backed up SN code: %s", sn_backup);
        } else {
            ESP_LOGW(TAG, "No SN code found in NVS");
        }
    }

    // 2. 通知服务器解绑，不等待解绑回复
    mqtt_publish_unbind_message();
    vTaskDelay(pdMS_TO_TICKS(200));

    // 3. 擦除整个NVS分区（包括WiFi配置）
    ESP_LOGI(TAG, "Erasing entire NVS flash...");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "NVS flash erased successfully");

    // 4. 重新初始化NVS
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize NVS: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "NVS reinitialized");

    // 5. 恢复system_info数据（仅SN码）
    if (has_sn_backup) {
        settings_t* restore_nvs = settings_start("SNCode_NVS", true);
        if (restore_nvs != NULL) {
            settings_set_string(restore_nvs, "SNCode", sn_backup);
            settings_end(restore_nvs);
            ESP_LOGI(TAG, "Restored SN code: %s", sn_backup);
        } else {
            ESP_LOGE(TAG, "Failed to create SNCode_NVS namespace for restore");
        }
    }

    ESP_LOGW(TAG, "=== Factory Reset Completed ===");
    ESP_LOGW(TAG, "Device will restart in 2 seconds...");

    // 6. 延时后重启设备
    vTaskDelay(pdMS_TO_TICKS(2000));
