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
    return dev_params.up_light_state;
}

uint8_t device_params_get_lower_light_state(void)
{
    return dev_params.lower_light_state;
}

uint8_t device_params_get_ambient_light_state(void)
{
    return dev_params.ambient_light_state;
}

uint8_t device_params_get_up_light_brightness(void)
{
    return dev_params.up_light_brightness;
}

uint8_t device_params_get_lower_light_brightness(void)
{
    return dev_params.lower_light_brightness;
}

uint8_t device_params_get_ambient_light_brightness(void)
{
    return dev_params.ambient_light_brightness;
}

therapy_state_t device_params_get_therapy_state(void)
{
    return dev_params.therapy_state;
}

uint8_t device_params_get_therapy_bright(int index)
{
    if (index >= 0 && index < 3) {
        return dev_params.therapy_bright[index];
    }
    return 0;
}

uint8_t device_params_get_music_state(void)
{
    return dev_params.music_state;
}

uint8_t device_params_get_voice_state(void)
{
    return dev_params.voice_state;
}

uint8_t device_params_get_music_volume(void)
{
    return dev_params.music_volume;
}

uint8_t device_params_get_therapy_focus_state(void)
{
    return dev_params.therapy_focus_state;
}

uint8_t device_params_get_therapy_sleep_state(void)
{
    return dev_params.therapy_sleep_state;
}

uint8_t device_params_get_therapy_sleep_duration(void)
{
    return dev_params.therapy_sleep_duration;
}

uint8_t device_params_get_focus_source(void)
{
    return dev_params.focus_source;
}

uint8_t device_params_get_constant_light_state(void)
{
    return dev_params.constant_light_state;
}

uint8_t device_params_get_pir_state(void)
{
    return dev_params.pir_state;
}

uint8_t device_params_get_dim_timeout(void)
{
    return dev_params.dim_timeout;
}

uint8_t device_params_get_off_timeout(void)
{
    return dev_params.off_timeout;
}

/* ============ Setter 函数实现 ============ */

void device_params_set_music_state(uint8_t state)
{
    dev_params.music_state = state;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "music_state", state);
        settings_end(nvs);
    }

    /* 实时应用到运行时 */
    audio_queue_set_music_enabled(state == 1);

    /* 开启时检查红光模式和PIR状态 */
    if (state == 1) {
        light_manager_t* mgr = get_light_manager();
        if (mgr != NULL) {
            red_light_mode_t red_mode = light_manager_get_red_mode(mgr);

            // 检查PIR调暗状态
            bool is_pir_dimmed = sensor_control_is_pir_dimmed();

            if (red_mode == RED_LIGHT_MODE_NORMAL && !is_pir_dimmed) {
                // 音乐开关只控制护眼模式背景音乐
                ESP_LOGI(TAG, "检测到红光模式(%d)且非PIR调暗,启动背景音乐", red_mode);
                audio_queue_set_background_music(MUSIC, true);
                audio_queue_play_loop(MUSIC, AUDIO_TYPE_MUSIC_CTRL, AUDIO_PRIORITY_LOW);
            }
            else if (is_pir_dimmed) {
                ESP_LOGI(TAG, "PIR调暗期间,暂不启动音乐(检测到运动后自动恢复)");
            }
        }
    }

    ESP_LOGI(TAG, "music_state已设置为%d", state);
}

void device_params_set_voice_state(uint8_t state)
{
    dev_params.voice_state = state;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "voice_state", state);
        settings_end(nvs);
    }

    /* 实时应用到运行时 */
    audio_queue_set_voice_enabled(state == 1);
    ESP_LOGI(TAG, "voice_state已设置为%d", state);
}

void device_params_set_music_volume(uint8_t volume)
{
    if (volume < 1 || volume > 5) {
        ESP_LOGW(TAG, "音量超出范围(1-5): %d", volume);
        return;
    }

    dev_params.music_volume = volume;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "music_vol", volume);
        settings_end(nvs);
    }

    /* 实时应用到运行时: 协议1-5档 → 硬件80-100映射 */
    int hw_vol = volume * 5 + 75;
    mp3_player_set_volume(hw_vol);
    ESP_LOGI(TAG, "music_volume已设置为%d(硬件音量%d)", volume, hw_vol);
}

void device_params_set_constant_light_state(uint8_t state)
{
    dev_params.constant_light_state = state;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "const_light", state);
        settings_end(nvs);
    }

    /* 下次灯光开启时生效 */
    constant_light_set_enabled(state == 1);
    ESP_LOGI(TAG, "constant_light_state已设置为%d(下次灯光开启时生效)", state);
}

void device_params_set_pir_state(uint8_t state)
{
    dev_params.pir_state = state;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "pir_state", state);
        settings_end(nvs);
    }

    /* 下次灯光开启时生效 */
    sensor_control_set_pir_enabled(state == 1);
    ESP_LOGI(TAG, "pir_state已设置为%d(下次灯光开启时生效)", state);
}

void device_params_set_dim_timeout(uint8_t minutes)
{
    if (minutes < 1 || minutes > 45) {
        ESP_LOGW(TAG, "dim_timeout超出范围(1-45): %d", minutes);
        return;
    }

    dev_params.dim_timeout = minutes;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "dim_timeout", minutes);
        settings_end(nvs);
    }

    /* 实时应用到运行时 */
    sensor_control_set_dim_timeout(minutes);
    ESP_LOGI(TAG, "dim_timeout已设置为%d分钟", minutes);
}

void device_params_set_off_timeout(uint8_t minutes)
{
    if (minutes < 1 || minutes > 10) {
        ESP_LOGW(TAG, "off_timeout超出范围(1-10): %d", minutes);
        return;
    }

    dev_params.off_timeout = minutes;

    /* 保存到NVS */
    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "off_timeout", minutes);
        settings_end(nvs);
    }

    /* 实时应用到运行时 */
    sensor_control_set_off_timeout(minutes);
    ESP_LOGI(TAG, "off_timeout已设置为%d分钟", minutes);
}

void device_params_set_therapy_focus_state(uint8_t state)
{
    dev_params.therapy_focus_state = (state == 1) ? 1 : 0;

    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "focus_state", dev_params.therapy_focus_state);
        settings_end(nvs);
    }

    ESP_LOGI(TAG, "therapy_focus_state已设置为%d", dev_params.therapy_focus_state);
}

void device_params_set_therapy_sleep_state(uint8_t state)
{
    dev_params.therapy_sleep_state = (state == 1) ? 1 : 0;

    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "sleep_state", dev_params.therapy_sleep_state);
        settings_end(nvs);
    }

    ESP_LOGI(TAG, "therapy_sleep_state已设置为%d", dev_params.therapy_sleep_state);
}

void device_params_set_therapy_sleep_duration(uint8_t minutes)
{
    if (minutes < 10 || minutes > 60 || (minutes % 10 != 0)) {
        ESP_LOGW(TAG, "sleep_duration超出范围(10-60,步进10): %d", minutes);
        return;
    }

    dev_params.therapy_sleep_duration = minutes;

    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "sleep_dur", minutes);
        settings_end(nvs);
    }

    ESP_LOGI(TAG, "therapy_sleep_duration已设置为%d分钟", minutes);
}

void device_params_set_focus_source(uint8_t source)
{
    dev_params.focus_source = (source == FOCUS_SOURCE_BUZZER) ? FOCUS_SOURCE_BUZZER : FOCUS_SOURCE_AUDIO;

    settings_t* nvs = settings_start("device_params", true);
    if (nvs) {
        settings_set_int(nvs, "focus_source", dev_params.focus_source);
        settings_end(nvs);
    }

    ESP_LOGI(TAG, "focus_source已设置为%d", dev_params.focus_source);
}
