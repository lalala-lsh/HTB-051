#include "device_params.h"

#include <esp_err.h>
#include <esp_log.h>

#include "light_manager.h"
#include "light_control.h"
#include "settings.h"
#include "audio_queue.h"
#include "mp3_player.h"
#include "constant_light_control.h"
#include "sensor_control.h"
#include "tts_list.h"

#include <stdint.h>

#define TAG "device_params"

struct device_params
{
    uint8_t up_light_state;
    uint8_t lower_light_state;
    uint8_t ambient_light_state;

    uint8_t up_light_brightness;
    uint8_t lower_light_brightness;
    uint8_t ambient_light_brightness;

    therapy_state_t therapy_state;
    uint8_t therapy_bright[3];

    uint8_t music_state;
    uint8_t voice_state;
    uint8_t music_volume;

    uint8_t constant_light_state;
    uint8_t pir_state;

    uint8_t dim_timeout;
    uint8_t off_timeout;

    uint8_t therapy_focus_state;
    uint8_t therapy_sleep_state;
    uint8_t therapy_sleep_duration;
    uint8_t focus_source;
};

static device_params_t dev_params = {0};

esp_err_t device_params_init(void)
{
    /* 首先尝试只读模式打开 */
    settings_t* nvs = settings_start("device_params", false);
    if (nvs == NULL) {
        /* 命名空间不存在，使用读写模式创建并初始化默认值 */
        ESP_LOGI(TAG, "First boot, creating device_params with defaults");
        nvs = settings_start("device_params", true);
        if (nvs == NULL) {
            ESP_LOGE(TAG, "Failed to create device_params namespace");
            return ESP_FAIL;
        }

        /* 设置默认值 */
        dev_params.up_light_state = 0;
        dev_params.lower_light_state = 0;
        dev_params.ambient_light_state = 0;

        dev_params.up_light_brightness = 60;
        dev_params.lower_light_brightness = 60;
        dev_params.ambient_light_brightness = 60;

        dev_params.therapy_state = 0;
        dev_params.therapy_bright[0] = 60;
        dev_params.therapy_bright[1] = 60;
        dev_params.therapy_bright[2] = 100;

        dev_params.music_state = 1;
        dev_params.voice_state = 1;
        dev_params.music_volume = 5;

        dev_params.constant_light_state = 1;
        dev_params.pir_state = 1;
        dev_params.dim_timeout = 20;
        dev_params.off_timeout = 5;

        dev_params.therapy_focus_state = 1;
        dev_params.therapy_sleep_state = 1;
        dev_params.therapy_sleep_duration = 30;
        dev_params.focus_source = FOCUS_SOURCE_AUDIO;

        /* 保存默认值到NVS */
        settings_set_int(nvs, "up_state", dev_params.up_light_state);
        settings_set_int(nvs, "lower_state", dev_params.lower_light_state);
        settings_set_int(nvs, "ambient_state", dev_params.ambient_light_state);
        settings_set_int(nvs, "up_bright", dev_params.up_light_brightness);
        settings_set_int(nvs, "lower_bright", dev_params.lower_light_brightness);
        settings_set_int(nvs, "ambient_bright", dev_params.ambient_light_brightness);
        settings_set_int(nvs, "therapy_state", dev_params.therapy_state);
        settings_set_int(nvs, "therapy_b0", dev_params.therapy_bright[0]);
        settings_set_int(nvs, "therapy_b1", dev_params.therapy_bright[1]);
        settings_set_int(nvs, "therapy_b2", dev_params.therapy_bright[2]);
        settings_set_int(nvs, "music_state", dev_params.music_state);
        settings_set_int(nvs, "voice_state", dev_params.voice_state);
        settings_set_int(nvs, "music_vol", dev_params.music_volume);
        settings_set_int(nvs, "const_light", dev_params.constant_light_state);
        settings_set_int(nvs, "pir_state", dev_params.pir_state);
        settings_set_int(nvs, "dim_timeout", dev_params.dim_timeout);
        settings_set_int(nvs, "off_timeout", dev_params.off_timeout);
        settings_set_int(nvs, "focus_state", dev_params.therapy_focus_state);
        settings_set_int(nvs, "sleep_state", dev_params.therapy_sleep_state);
        settings_set_int(nvs, "sleep_dur", dev_params.therapy_sleep_duration);
        settings_set_int(nvs, "focus_source", dev_params.focus_source);

        settings_end(nvs);

        ESP_LOGI(TAG, "Default device params saved to NVS");
        print_device_parmas();
        return ESP_OK;
    }

    /* 读取灯光状态 */
    dev_params.up_light_state = (uint8_t)settings_get_int(nvs, "up_state", 0);
    dev_params.lower_light_state = (uint8_t)settings_get_int(nvs, "lower_state", 0);
    dev_params.ambient_light_state = (uint8_t)settings_get_int(nvs, "ambient_state", 0);

    /* 读取灯光亮度 */
    dev_params.up_light_brightness = (uint8_t)settings_get_int(nvs, "up_bright", 60);
    dev_params.lower_light_brightness = (uint8_t)settings_get_int(nvs, "lower_bright", 60);
    dev_params.ambient_light_brightness = (uint8_t)settings_get_int(nvs, "ambient_bright", 60);

    /* 读取光疗状态 */
    dev_params.therapy_state = (therapy_state_t)settings_get_int(nvs, "therapy_state", 0);
    dev_params.therapy_bright[0] = (uint8_t)settings_get_int(nvs, "therapy_b0", 60);
    dev_params.therapy_bright[1] = (uint8_t)settings_get_int(nvs, "therapy_b1", 60);
    dev_params.therapy_bright[2] = (uint8_t)settings_get_int(nvs, "therapy_b2", 100);

    /* 读取音频状态 */
    dev_params.music_state = (uint8_t)settings_get_int(nvs, "music_state", 1);
    dev_params.voice_state = (uint8_t)settings_get_int(nvs, "voice_state", 1);
    dev_params.music_volume = (uint8_t)settings_get_int(nvs, "music_vol", 3);

    /* 读取其他参数 */
    dev_params.constant_light_state = (uint8_t)settings_get_int(nvs, "const_light", 1);
    dev_params.pir_state = (uint8_t)settings_get_int(nvs, "pir_state", 1);
    dev_params.dim_timeout = (uint8_t)settings_get_int(nvs, "dim_timeout", 5);
    dev_params.off_timeout = (uint8_t)settings_get_int(nvs, "off_timeout", 20);

    /* 读取光疗开关参数 */
    dev_params.therapy_focus_state = (uint8_t)settings_get_int(nvs, "focus_state", 1);
    dev_params.therapy_sleep_state = (uint8_t)settings_get_int(nvs, "sleep_state", 1);
    dev_params.therapy_sleep_duration = (uint8_t)settings_get_int(nvs, "sleep_dur", 30);
    dev_params.focus_source = (uint8_t)settings_get_int(nvs, "focus_source", FOCUS_SOURCE_AUDIO);
    if (dev_params.focus_source > FOCUS_SOURCE_BUZZER) {
        dev_params.focus_source = FOCUS_SOURCE_AUDIO;
    }

    settings_end(nvs);

    print_device_parmas();
    return ESP_OK;
}

void print_device_parmas(void)
{
    ESP_LOGI(TAG, "device params init :");
    ESP_LOGI(TAG, "   up_state:%d", dev_params.up_light_state);
    ESP_LOGI(TAG, "   lower_state:%d", dev_params.lower_light_state);
    ESP_LOGI(TAG, "   ambient_state:%d", dev_params.ambient_light_state);

    ESP_LOGI(TAG, "   up_bright:%d", dev_params.up_light_brightness);
    ESP_LOGI(TAG, "   lower_bright:%d", dev_params.lower_light_brightness);
    ESP_LOGI(TAG, "   ambient_bright:%d", dev_params.ambient_light_brightness);

    ESP_LOGI(TAG, "   therapy_state:%d", dev_params.therapy_state);
    ESP_LOGI(TAG, "   therapy_b0:%d", dev_params.therapy_bright[0]);
    ESP_LOGI(TAG, "   therapy_b1:%d", dev_params.therapy_bright[1]);
    ESP_LOGI(TAG, "   therapy_b2:%d", dev_params.therapy_bright[2]);

    ESP_LOGI(TAG, "   music_state:%d", dev_params.music_state);
    ESP_LOGI(TAG, "   voice_state:%d", dev_params.voice_state);
    ESP_LOGI(TAG, "   music_vol:%d", dev_params.music_volume);

    ESP_LOGI(TAG, "   const_light:%d", dev_params.constant_light_state);
    ESP_LOGI(TAG, "   pir_state:%d", dev_params.pir_state);
    ESP_LOGI(TAG, "   dim_timeout:%d", dev_params.dim_timeout);
    ESP_LOGI(TAG, "   off_timeout:%d", dev_params.off_timeout);

    ESP_LOGI(TAG, "   focus_state:%d", dev_params.therapy_focus_state);
    ESP_LOGI(TAG, "   sleep_state:%d", dev_params.therapy_sleep_state);
    ESP_LOGI(TAG, "   sleep_dur:%d", dev_params.therapy_sleep_duration);
    ESP_LOGI(TAG, "   focus_source:%d", dev_params.focus_source);
}

device_params_t* get_device_params(void)
{
    return &dev_params;
}

/* ============ Getter 函数实现 ============ */

uint8_t device_params_get_up_light_state(void)
{
