#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "blufi_example.h"
#include "device_params.h"
#include "light_control.h"
#include "mqtt_manmager.h"
#include "portmacro.h"
#include "protocol.h"
#include "sensor_control.h"
#include "sntp_service.h"
#include "system_info.h"
#include "Audio/mp3_player.h"
#include "Audio/audio_queue.h"
#include "Audio/audio_update.h"
#include "Audio/tts_list.h"
#include "factory.h"
#include "Iot/param_handler.h"
#include "Iot/device_param_handler.h"
#include "light_control/light_param_handler.h"
#include "sensor_control/sensor_param_handler.h"

#define TAG "main"
#define WELCOME_AUDIO_WAIT_MS 5000

static void log_clear(void);

void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化系统
    system_info_init();

    print_system_info();

    device_params_init();

    light_control_start();

    // 初始化MP3播放器(必须在sensor_control_init之前,因为需要初始化I2C总线)
    mp3_player_init();

    // 初始化音频队列管理器
    audio_queue_init();
    // 初始化音频更新模块（清理遗留临时文件）
    audio_update_init();

    // 初始化传感器控制(依赖MP3播放器初始化的I2C总线)
    sensor_control_init();

    blufi_init();

    sntp_service_init();

    /* 初始化参数处理器模块 */
    param_handler_init();
    device_param_handler_init();
    light_param_handler_init();
    sensor_param_handler_init();

    mqtt_client_init();

    audio_queue_play(WELCOME, AUDIO_TYPE_SYSTEM, AUDIO_PRIORITY_NORMAL, false);
    if (audio_queue_wait_for_prompts_idle(WELCOME_AUDIO_WAIT_MS) == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "等待欢迎语音完成超时,启用按键");
    }
    light_control_set_buttons_enabled(true);

    flash_sn_init();

    log_clear();

    while (1) {
        print_heap_stats();
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

static void log_clear(void)
{
    // esp_log_level_set("*", ESP_LOG_ERROR);

    esp_log_level_set("AUDIO_THREAD", ESP_LOG_ERROR);
    esp_log_level_set("I2C_BUS", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_HAL", ESP_LOG_ERROR);
    esp_log_level_set("ESP_AUDIO_TASK", ESP_LOG_ERROR);
    esp_log_level_set("ESP_DECODER", ESP_LOG_ERROR);
    esp_log_level_set("I2S", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_FORGE", ESP_LOG_ERROR);
    esp_log_level_set("ESP_AUDIO_CTRL", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_ERROR);
    esp_log_level_set("TONE_PARTITION", ESP_LOG_ERROR);
    esp_log_level_set("TONE_STREAM", ESP_LOG_ERROR);
    esp_log_level_set("MP3_DECODER", ESP_LOG_ERROR);
    esp_log_level_set("I2S_STREAM", ESP_LOG_ERROR);
    esp_log_level_set("RSP_FILTER", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_EVT", ESP_LOG_ERROR);
}
