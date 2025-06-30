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
