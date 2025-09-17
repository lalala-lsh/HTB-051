#include "light_manager.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_queue.h"
#include "device_params.h"
#include "mqtt_manmager.h"
#include "tts_list.h"

#define TAG "light_manager"

// 默认渐变时间（毫秒）
#define DEFAULT_FADE_TIME_MS 100

// 红光光疗模式常量
#define RED_THERAPY_FIXED_DUTY 4096  // 红光光疗模式固定占空比
#define MIN_THERAPY_RECORD_TIME 300   // 最小记录时间（秒）
#define THERAPY_AUTO_CLOSE_TIME_MS (60 * 60 * 1000)  // 1小时自动关闭（护眼模式用）
#define THERAPY_FOCUS_DURATION_MS  (30 * 60 * 1000)  // 30分钟自动关闭（专注模式用）

// 红光闪烁参数
#define RED_BLINK_FADE_TIME_MS 1000   // 红光闪烁渐变时间（毫秒）
#define RED_BLINK_TIMER_PERIOD_MS 1000  // 红光闪烁定时器周期（毫秒）

// 互斥锁保护宏（简化代码，使用递归锁API）
#define LOCK(manager)                                                                              \
    do {                                                                                           \
        if (manager->mutex != NULL) {                                                              \
            xSemaphoreTakeRecursive(manager->mutex, portMAX_DELAY);                                \
        }                                                                                          \
    } while (0)

#define UNLOCK(manager)                                                                            \
    do {                                                                                           \
        if (manager->mutex != NULL) {                                                              \
            xSemaphoreGiveRecursive(manager->mutex);                                               \
        }                                                                                          \
    } while (0)

/**
 * @brief 单个灯的状态
 */
typedef struct
{
    bool is_on;             // 开关状态
    uint32_t duty;          // 当前duty值（0-8191）
    uint8_t brightness;     // 亮度百分比（0-100）
    ledc_channel_t channel; // LEDC通道号
} light_state_t;

/**
 * @brief 灯光管理器完整结构定义
 */
struct light_manager
{
    // 灯光状态数组
    light_state_t lights[LIGHT_ID_MAX];

    // 红光特殊状态
    red_light_mode_t red_mode;
    bool buzzer_on;

    // 红光三种模式的亮度配置
    // therapy_bright[0]: NORMAL模式(护眼,5kHz)的亮度百分比
    // therapy_bright[1]: THERAPY模式(专注,40Hz)的亮度百分比
    // therapy_bright[2]: SLEEP模式(助眠,40Hz,上限20%)的亮度百分比(1-100,内部缩放)
    uint8_t therapy_bright[3];
    focus_source_t focus_source;

    // 按键5组合状态
    combo_state_t combo_state;

    // 全局亮度百分比（0-100）
    uint8_t global_brightness;

    // NVS存储句柄
    settings_t* settings;

    // 渐变时间（毫秒）
    uint32_t fade_time_ms;

    // 初始化标志
    bool initialized;

    // 状态变化回调
    light_change_callback_t change_callback;
    void* callback_arg;

    // 互斥锁保护（并发安全）
    SemaphoreHandle_t mutex;

    // 40hz定时器句柄
    TimerHandle_t auto_close_40hz;

    // 光疗训练记录
    time_t therapy_start_time;  // 训练开始时间戳
    bool therapy_recording;     // 是否正在记录

    // 红光闪烁相关
    TimerHandle_t blink_timer;  // 闪烁定时器句柄
    bool is_blinking;           // 是否正在闪烁
    bool blink_direction;       // 闪烁方向（true=变亮，false=变暗）
};

// =============================================================================
// 私有函数声明
// =============================================================================

static esp_err_t open_nvs_if_needed(light_manager_t* manager);
static esp_err_t close_nvs_if_all_off(light_manager_t* manager);
static uint32_t brightness_percent_to_duty(uint8_t percent);
static uint8_t brightness_level_to_percent(brightness_level_t level);
static void notify_change(light_manager_t* manager, light_change_type_t type, int light_id);
static void auto_close_40hz_timer_callback(TimerHandle_t xTimer);
static void red_blink_timer_callback(TimerHandle_t xTimer);
static esp_err_t apply_red_mode_off(light_manager_t* manager, bool use_fade, bool save_record);
static esp_err_t apply_red_mode_normal(light_manager_t* manager, uint8_t brightness, bool use_fade, bool play_audio);
static esp_err_t apply_red_mode_therapy(light_manager_t* manager, uint8_t brightness, bool use_fade, bool play_audio);
static esp_err_t apply_red_mode_sleep(light_manager_t* manager, uint8_t brightness, bool use_fade, bool play_audio);
static esp_err_t apply_focus_source_audio(light_manager_t* manager, bool start_audio);
static esp_err_t apply_focus_source_buzzer(light_manager_t* manager);
static void publish_therapy_record_if_valid(light_manager_t* manager, red_light_mode_t mode);
static uint8_t sleep_brightness_level_to_percent(brightness_level_t level);
static void sync_combo_state_from_lights(light_manager_t* manager);

// =============================================================================
// 生命周期管理
// =============================================================================

light_manager_t* light_manager_create(void)
{
    // 分配内存（优先SPIRAM，失败则使用内部RAM）
    light_manager_t* manager =
        (light_manager_t*)heap_caps_calloc(1, sizeof(light_manager_t), MALLOC_CAP_SPIRAM);
    if (manager == NULL) {
        ESP_LOGW(TAG, "SPIRAM allocation failed, trying internal RAM");
        manager = (light_manager_t*)calloc(1, sizeof(light_manager_t));
        if (manager == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for light manager");
            return NULL;
        }
    }

    // 初始化灯光配置
    manager->lights[LIGHT_ID_AMBIENT].channel = AMBIENT_LIGHT_CHANNEL;
    manager->lights[LIGHT_ID_LOWER].channel = LOWER_LIGHT_CHANNEL;
    manager->lights[LIGHT_ID_UPPER].channel = UPPER_LIGHT_CHANNEL;
    manager->lights[LIGHT_ID_RED].channel = LED_R_CHANNEL;

    // 初始化所有灯的默认状态
    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        manager->lights[i].is_on = false;
        manager->lights[i].brightness = 60; // 默认60%亮度
        manager->lights[i].duty = brightness_percent_to_duty(60);
    }

    // 初始化其他状态
    manager->red_mode = RED_LIGHT_MODE_OFF;
    manager->buzzer_on = false;
    manager->therapy_bright[0] = 60; // NORMAL模式默认亮度60%
    manager->therapy_bright[1] = 60; // THERAPY模式默认亮度60%
    manager->focus_source = FOCUS_SOURCE_AUDIO;
    manager->combo_state = COMBO_STATE_ALL_OFF;
    manager->global_brightness = 60; // 全局默认亮度60%
    manager->settings = NULL;
    manager->fade_time_ms = DEFAULT_FADE_TIME_MS;
    manager->initialized = false;
    manager->change_callback = NULL;
    manager->callback_arg = NULL;
    manager->therapy_start_time = 0;
    manager->therapy_recording = false;
    manager->is_blinking = false;
    manager->blink_direction = true;

    // 创建递归互斥锁（允许同一任务多次获取，支持嵌套调用）
    manager->mutex = xSemaphoreCreateRecursiveMutex();
    if (manager->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create recursive mutex");
        free(manager);
        return NULL;
    }

    manager->auto_close_40hz = xTimerCreate("auto_close_40hz", pdMS_TO_TICKS(THERAPY_AUTO_CLOSE_TIME_MS),
                                            pdFALSE, manager, auto_close_40hz_timer_callback);

    // 创建红光闪烁定时器（使用常量定义周期）
    manager->blink_timer = xTimerCreate("red_blink", pdMS_TO_TICKS(RED_BLINK_TIMER_PERIOD_MS),
                                       pdTRUE, manager, red_blink_timer_callback);
    if (manager->blink_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create blink timer");
        vSemaphoreDelete(manager->mutex);
        free(manager);
        return NULL;
    }

    ESP_LOGI(TAG, "Light manager created with mutex protection");
    return manager;
}

void light_manager_destroy(light_manager_t* manager)
{
    if (manager == NULL) {
        return;
    }

    // 停止闪烁（如果正在闪烁）
    if (manager->is_blinking && manager->blink_timer != NULL) {
        xTimerStop(manager->blink_timer, 0);
    }

    // 删除定时器
    if (manager->blink_timer != NULL) {
        xTimerDelete(manager->blink_timer, 0);
        manager->blink_timer = NULL;
    }

    if (manager->auto_close_40hz != NULL) {
        xTimerDelete(manager->auto_close_40hz, 0);
        manager->auto_close_40hz = NULL;
    }

    // 关闭NVS（如果打开）
    if (manager->settings != NULL) {
        settings_end(manager->settings);
        manager->settings = NULL;
    }

    // 删除互斥锁
    if (manager->mutex != NULL) {
        vSemaphoreDelete(manager->mutex);
        manager->mutex = NULL;
    }

    // 释放内存
    heap_caps_free(manager);
    ESP_LOGI(TAG, "Light manager destroyed");
}

esp_err_t light_manager_init(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 加载NVS状态
    light_manager_load_state(manager);

    manager->initialized = true;
    ESP_LOGI(TAG, "Light manager initialized");
    return ESP_OK;
}

// =============================================================================
// 灯光控制
// =============================================================================

esp_err_t light_manager_turn_on(light_manager_t* manager, light_id_t light_id, bool use_fade)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (light_id >= LIGHT_ID_MAX) {
        ESP_LOGE(TAG, "Invalid light ID: %d", light_id);
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    // 打开NVS（如果需要）
    open_nvs_if_needed(manager);

    light_state_t* light = &manager->lights[light_id];

    // 如果已经开启，直接返回
    if (light->is_on) {
        ESP_LOGD(TAG, "Light %d already ON", light_id);
        UNLOCK(manager);
        return ESP_OK;
    }

    // 使用该灯自己保存的亮度百分比（从NVS加载或上次设置的值）
    // 注意：不使用global_brightness，因为每个灯独立保存亮度
    light->duty = brightness_percent_to_duty(light->brightness);

    // 应用到硬件
    uint32_t fade_time = use_fade ? manager->fade_time_ms : 0;
    esp_err_t ret = light_set_duty_with_time(light->channel, light->duty, fade_time);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set duty for light %d", light_id);
        UNLOCK(manager);
        return ret;
    }

    light->is_on = true;
    ESP_LOGD(TAG, "Light %d turned ON (duty=%lu, brightness=%d, fade=%lums)", light_id, light->duty,
             light->brightness, fade_time);

    light_manager_sync_indicator_leds(manager);

    // 通知状态变化
    notify_change(manager, LIGHT_CHANGE_ON, light_id);

    UNLOCK(manager);
    return ESP_OK;
}

esp_err_t light_manager_turn_off(light_manager_t* manager, light_id_t light_id, bool use_fade)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (light_id >= LIGHT_ID_MAX) {
        ESP_LOGE(TAG, "Invalid light ID: %d", light_id);
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    light_state_t* light = &manager->lights[light_id];

    // 如果已经关闭，直接返回
    if (!light->is_on) {
        ESP_LOGD(TAG, "Light %d already OFF", light_id);
        UNLOCK(manager);
        return ESP_OK;
    }

    // 关闭灯光
    uint32_t fade_time = use_fade ? manager->fade_time_ms : 0;
    esp_err_t ret = light_set_duty_with_time(light->channel, 0, fade_time);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off light %d", light_id);
        UNLOCK(manager);
        return ret;
    }

    light->is_on = false;
    ESP_LOGD(TAG, "Light %d turned OFF (fade=%lums)", light_id, fade_time);

    light_manager_sync_indicator_leds(manager);

    // 通知状态变化
    notify_change(manager, LIGHT_CHANGE_OFF, light_id);

    // 检查是否所有灯都关闭，如果是则关闭NVS
    close_nvs_if_all_off(manager);

    UNLOCK(manager);
    return ESP_OK;
}

esp_err_t light_manager_toggle(light_manager_t* manager, light_id_t light_id, bool use_fade)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (light_id >= LIGHT_ID_MAX) {
        ESP_LOGE(TAG, "Invalid light ID: %d", light_id);
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);
    bool is_on = manager->lights[light_id].is_on;
    UNLOCK(manager);

    if (is_on) {
        return light_manager_turn_off(manager, light_id, use_fade);
    }
    else {
        audio_queue_play(UP_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
        return light_manager_turn_on(manager, light_id, use_fade);
    }
}

// =============================================================================
// 亮度控制
// =============================================================================

esp_err_t light_manager_set_brightness_level(light_manager_t* manager, brightness_level_t level)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (level > BRIGHTNESS_LEVEL_100) {
        ESP_LOGE(TAG, "Invalid brightness level: %d", level);
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    // 档位 → 百分比转换
    uint8_t percent = brightness_level_to_percent(level);
    manager->global_brightness = percent;
    uint32_t duty = brightness_percent_to_duty(percent);

    ESP_LOGD(TAG, "Setting global brightness to level %d (%d%%, duty=%lu)", level, percent, duty);

    // 遍历所有开启的灯，统一更新亮度
    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        if (!manager->lights[i].is_on) {
            continue;
        }

        manager->lights[i].brightness = percent;

        // 红光特殊处理
        if (i == LIGHT_ID_RED) {
            if (manager->red_mode == RED_LIGHT_MODE_NORMAL) {
                manager->therapy_bright[0] = percent;
                manager->lights[i].duty = duty;
                esp_err_t ret = light_set_duty_with_time(manager->lights[i].channel, duty, manager->fade_time_ms);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to set brightness for red NORMAL mode");
                    UNLOCK(manager);
                    return ret;
                }
                ESP_LOGI(TAG, "Red NORMAL mode brightness updated to %d%%", percent);
            }
            else if (manager->red_mode == RED_LIGHT_MODE_THERAPY) {
                manager->therapy_bright[1] = percent;
                manager->lights[i].duty = RED_THERAPY_FIXED_DUTY;
                /* 专注模式已移除蜂鸣器，仅更新亮度配置 */
                ESP_LOGI(TAG, "Red THERAPY mode brightness config updated to %d%%", percent);
            }
            else if (manager->red_mode == RED_LIGHT_MODE_SLEEP) {
                uint8_t sleep_percent = sleep_brightness_level_to_percent(level);
                manager->therapy_bright[2] = percent;
                manager->lights[i].brightness = percent;
                manager->lights[i].duty = brightness_percent_to_duty(sleep_percent);
                esp_err_t ret = light_set_duty_with_time(manager->lights[i].channel,
                                                         manager->lights[i].duty, manager->fade_time_ms);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to set brightness for red SLEEP mode");
                    UNLOCK(manager);
                    return ret;
                }
                ESP_LOGI(TAG, "Red SLEEP mode brightness updated to %d%% (actual %d%%)", percent, sleep_percent);
            }
        }
        else {
            // 非红光：正常调节亮度
            manager->lights[i].duty = duty;
            esp_err_t ret = light_set_duty_with_time(manager->lights[i].channel, duty, manager->fade_time_ms);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set brightness for light %d", i);
                UNLOCK(manager);
                return ret;
            }
            ESP_LOGD(TAG, "Light %d brightness updated to %d%%", i, percent);
        }
    }

    // 通知亮度变化（light_id=-1表示全局）
    notify_change(manager, LIGHT_CHANGE_BRIGHTNESS, -1);

    UNLOCK(manager);
    return ESP_OK;
}

brightness_level_t light_manager_get_brightness_level(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return BRIGHTNESS_LEVEL_100;
    }

    LOCK(manager);

    // 将当前百分比值映射回最接近的档位枚举（用于外部查询）
    uint8_t percent = manager->global_brightness;
    brightness_level_t level;
    if (percent <= 25)
        level = BRIGHTNESS_LEVEL_10;
    else if (percent <= 50)
        level = BRIGHTNESS_LEVEL_40;
    else if (percent <= 70)
        level = BRIGHTNESS_LEVEL_60;
    else if (percent <= 90)
        level = BRIGHTNESS_LEVEL_80;
    else
        level = BRIGHTNESS_LEVEL_100;

    UNLOCK(manager);
    return level;
}

esp_err_t light_manager_set_temporary_brightness_level(light_manager_t* manager,
                                                       brightness_level_t level)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (level > BRIGHTNESS_LEVEL_100) {
        ESP_LOGE(TAG, "Invalid brightness level: %d", level);
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    // 档位 → 百分比转换
    uint8_t percent = brightness_level_to_percent(level);
    uint32_t duty = brightness_percent_to_duty(percent);

    ESP_LOGI(TAG, "设置临时亮度: %d%% (不修改保存值)", percent);

    // 遍历所有开启的灯，只修改duty，不修改brightness字段
    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        if (!manager->lights[i].is_on) {
            continue;
        }

        // 助眠模式下红光不受临时亮度影响（PIR/恒光隔离）
        if (i == LIGHT_ID_RED && manager->red_mode == RED_LIGHT_MODE_SLEEP) {
            continue;
        }

        if (i == LIGHT_ID_RED && manager->red_mode == RED_LIGHT_MODE_THERAPY) {
            /* 专注模式已移除蜂鸣器，临时亮度不作用于红光固定占空比 */
            ESP_LOGD(TAG, "THERAPY模式临时调暗: 红光保持固定占空比%d", RED_THERAPY_FIXED_DUTY);
        }
        else {
            // 非红光或红光NORMAL模式：正常调节亮度
            esp_err_t ret = light_set_duty_with_time(manager->lights[i].channel, duty, manager->fade_time_ms);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "设置临时亮度失败 light=%d", i);
                UNLOCK(manager);
                return ret;
            }
            if (i == LIGHT_ID_RED && manager->red_mode == RED_LIGHT_MODE_NORMAL) {
                ESP_LOGI(TAG, "红光护眼模式临时调暗到%d%% (brightness保持=%d%%)",
                         percent, manager->lights[i].brightness);
            }
            ESP_LOGD(TAG, "Light %d 临时duty=%lu (brightness保持=%d%%)", i, duty,
                     manager->lights[i].brightness);
        }
    }

    // 不调用notify_change()，避免触发回调

    UNLOCK(manager);
    return ESP_OK;
}

esp_err_t light_manager_restore_saved_brightness(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        if (!manager->lights[i].is_on) {
            continue;
        }

        uint32_t duty = manager->lights[i].duty;

        if (i == LIGHT_ID_RED) {
            if (manager->red_mode == RED_LIGHT_MODE_NORMAL) {
                duty = brightness_percent_to_duty(manager->lights[i].brightness);
            }
            else if (manager->red_mode == RED_LIGHT_MODE_THERAPY) {
                duty = RED_THERAPY_FIXED_DUTY;
            }
            else if (manager->red_mode == RED_LIGHT_MODE_SLEEP) {
                uint8_t actual_percent = (uint8_t)(manager->lights[i].brightness * 20 / 100);
                if (actual_percent < 1) {
                    actual_percent = 1;
                }
                duty = brightness_percent_to_duty(actual_percent);
            }
            else {
                continue;
            }
        }
        else {
            duty = brightness_percent_to_duty(manager->lights[i].brightness);
        }

        manager->lights[i].duty = duty;
        esp_err_t ret = light_set_duty_with_time(manager->lights[i].channel,
                                                 duty, manager->fade_time_ms);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "恢复保存亮度失败 light=%d", i);
            UNLOCK(manager);
            return ret;
        }

        ESP_LOGI(TAG, "恢复保存亮度 light=%d brightness=%d%% duty=%lu",
                 i, manager->lights[i].brightness, duty);
    }

    UNLOCK(manager);
    return ESP_OK;
}

// =============================================================================
// 模式控制
// =============================================================================

esp_err_t light_manager_cycle_combo(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    sync_combo_state_from_lights(manager);
    combo_state_t current_state = manager->combo_state;
    combo_state_t next_state = current_state;
    esp_err_t ret = ESP_OK;

    switch (current_state) {
        case COMBO_STATE_ALL_OFF:
            // 状态0 → 状态1: 环境光ON + 下光ON
            ret = light_manager_turn_on(manager, LIGHT_ID_AMBIENT, true);
            if (ret == ESP_OK) {
                ret = light_manager_turn_on(manager, LIGHT_ID_LOWER, true);
            }
            if (ret == ESP_OK) {
                next_state = COMBO_STATE_BOTH_ON;
                audio_queue_play(DOWM_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
                ESP_LOGI(TAG, "Combo: ALL_OFF -> BOTH_ON");
            }
            break;

        case COMBO_STATE_BOTH_ON:
            // 状态1 → 状态2: 环境光保持（不动作） + 下光OFF
            ret = light_manager_turn_off(manager, LIGHT_ID_LOWER, true);
            // 环境光不操作，保持开启状态
            if (ret == ESP_OK) {
                next_state = COMBO_STATE_AMBIENT_ONLY;
                audio_queue_play(AROUND_LIGHT, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, true);
                ESP_LOGI(TAG, "Combo: BOTH_ON -> AMBIENT_ONLY");
            }
            break;

        case COMBO_STATE_AMBIENT_ONLY:
            // 状态2 → 状态0: 环境光OFF + 下光已经是OFF
            ret = light_manager_turn_off(manager, LIGHT_ID_AMBIENT, true);
            if (ret == ESP_OK) {
                next_state = COMBO_STATE_ALL_OFF;
                ESP_LOGI(TAG, "Combo: AMBIENT_ONLY -> ALL_OFF");
            }
            break;

        default:
            ESP_LOGE(TAG, "Invalid combo state: %d", current_state);
            UNLOCK(manager);
            return ESP_ERR_INVALID_STATE;
    }

    if (ret == ESP_OK) {
        manager->combo_state = next_state;
        light_manager_sync_indicator_leds(manager);
    }

    UNLOCK(manager);
    return ret;
}

esp_err_t light_manager_reset_combo_state(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);
    if (manager->combo_state != COMBO_STATE_ALL_OFF) {
        ESP_LOGI(TAG, "Combo state reset: %d -> %d", manager->combo_state, COMBO_STATE_ALL_OFF);
    }
    manager->combo_state = COMBO_STATE_ALL_OFF;
    UNLOCK(manager);
    return ESP_OK;
}

esp_err_t light_manager_cycle_red_mode(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    red_light_mode_t current_mode = manager->red_mode;
    bool focus_enabled = (device_params_get_therapy_focus_state() == 1);
    bool sleep_enabled = (device_params_get_therapy_sleep_state() == 1);

    /* 确定下一个模式（动态循环，护眼始终存在） */
    red_light_mode_t next_mode;
    switch (current_mode) {
        case RED_LIGHT_MODE_OFF:
            next_mode = RED_LIGHT_MODE_NORMAL;
            break;
        case RED_LIGHT_MODE_NORMAL:
            if (focus_enabled)
                next_mode = RED_LIGHT_MODE_THERAPY;
            else if (sleep_enabled)
                next_mode = RED_LIGHT_MODE_SLEEP;
            else
                next_mode = RED_LIGHT_MODE_OFF;
            break;
        case RED_LIGHT_MODE_THERAPY:
            if (sleep_enabled)
                next_mode = RED_LIGHT_MODE_SLEEP;
            else
                next_mode = RED_LIGHT_MODE_OFF;
            break;
        case RED_LIGHT_MODE_SLEEP:
            next_mode = RED_LIGHT_MODE_OFF;
            break;
        default:
            ESP_LOGE(TAG, "Invalid red mode: %d", current_mode);
            UNLOCK(manager);
            return ESP_ERR_INVALID_STATE;
    }

    /* 从当前模式切走时先上报记录 */
    if (current_mode != RED_LIGHT_MODE_OFF) {
        publish_therapy_record_if_valid(manager, current_mode);
    }

    esp_err_t ret = ESP_OK;
    switch (next_mode) {
        case RED_LIGHT_MODE_OFF:
            ret = apply_red_mode_off(manager, false, false);
            notify_change(manager, LIGHT_CHANGE_OFF, LIGHT_ID_RED);
            break;
        case RED_LIGHT_MODE_NORMAL:
            ret = apply_red_mode_normal(manager, manager->therapy_bright[0], true, true);
            notify_change(manager, LIGHT_CHANGE_ON, LIGHT_ID_RED);
            break;
        case RED_LIGHT_MODE_THERAPY:
            ret = apply_red_mode_therapy(manager, manager->therapy_bright[1], true, true);
            notify_change(manager, LIGHT_CHANGE_ON, LIGHT_ID_RED);
            break;
        case RED_LIGHT_MODE_SLEEP:
            ret = apply_red_mode_sleep(manager, manager->therapy_bright[2], true, true);
            notify_change(manager, LIGHT_CHANGE_ON, LIGHT_ID_RED);
            break;
    }

    if (ret == ESP_OK) {
        manager->red_mode = next_mode;
        light_manager_sync_indicator_leds(manager);
        ESP_LOGI(TAG, "Red mode: %d -> %d", current_mode, next_mode);
    }

    UNLOCK(manager);
    return ret;
}

red_light_mode_t light_manager_get_red_mode(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return RED_LIGHT_MODE_OFF;
    }

    LOCK(manager);
    red_light_mode_t mode = manager->red_mode;
    UNLOCK(manager);
    return mode;
}

uint8_t light_manager_get_therapy_brightness(light_manager_t* manager, int mode)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return 0;
    }

    if (mode < 0 || mode > 2) {
        ESP_LOGE(TAG, "Invalid mode: %d", mode);
        return 0;
    }

    LOCK(manager);
    uint8_t brightness = manager->therapy_bright[mode];
    UNLOCK(manager);
    return brightness;
}

focus_source_t light_manager_get_focus_source(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return FOCUS_SOURCE_AUDIO;
    }

    LOCK(manager);
    focus_source_t source = manager->focus_source;
    UNLOCK(manager);
    return source;
}

esp_err_t light_manager_toggle_focus_source(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    if (manager->red_mode != RED_LIGHT_MODE_THERAPY) {
        UNLOCK(manager);
        return ESP_ERR_INVALID_STATE;
    }

    focus_source_t next_source = (manager->focus_source == FOCUS_SOURCE_AUDIO) ?
                                 FOCUS_SOURCE_BUZZER : FOCUS_SOURCE_AUDIO;
    esp_err_t ret = ESP_OK;

    if (next_source == FOCUS_SOURCE_BUZZER) {
        ret = apply_focus_source_buzzer(manager);
    } else {
        ret = apply_focus_source_audio(manager, true);
    }

    if (ret == ESP_OK) {
        manager->focus_source = next_source;
        device_params_set_focus_source((uint8_t)next_source);
        ESP_LOGI(TAG, "Focus source switched to %s",
                 next_source == FOCUS_SOURCE_AUDIO ? "audio" : "buzzer");
    }

    UNLOCK(manager);
    return ret;
}

combo_state_t light_manager_get_combo_state(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return COMBO_STATE_ALL_OFF;
    }

    LOCK(manager);
    combo_state_t state = manager->combo_state;
    UNLOCK(manager);
    return state;
}

// =============================================================================
// 状态查询
// =============================================================================

esp_err_t light_manager_turn_off_all(light_manager_t* manager, bool use_fade)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    esp_err_t ret = ESP_OK;
    esp_err_t first_error = ESP_OK;

    /* 上报光疗训练记录（如果>=5分钟）*/
    publish_therapy_record_if_valid(manager, manager->red_mode);
    manager->therapy_recording = false;

    /* 关闭所有灯光 */
    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        ret = light_manager_turn_off(manager, i, use_fade);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;  // 记录第一个错误
        }
    }

    /* 关闭蜂鸣器（如果红光在光疗模式） */
    if (manager->buzzer_on) {
        light_set_duty(BUZZER_CHANNEL, 0);
        manager->buzzer_on = false;
    }

    audio_queue_stop();

    /* 停止40Hz自动关闭定时器 */
    if (manager->auto_close_40hz != NULL) {
        xTimerStop(manager->auto_close_40hz, 0);
    }

    /* 重置所有状态机 */
    manager->combo_state = COMBO_STATE_ALL_OFF;
    manager->red_mode = RED_LIGHT_MODE_OFF;

    /* 恢复全局亮度为默认值（避免下次开灯使用调暗后的亮度） */
    manager->global_brightness = BRIGHTNESS_LEVEL_60;

    light_manager_sync_indicator_leds(manager);

    /* 恢复红光PWM频率为正常模式 */
    light_switch_mode(MODE_NORMAL);

    notify_change(manager, LIGHT_CHANGE_OFF, -1);

    ESP_LOGI(TAG, "All lights turned off, states reset");

    UNLOCK(manager);
    return first_error;  // 返回第一个错误（如果有）
}

esp_err_t light_manager_suspend_outputs_for_ota(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    LOCK(manager);

    esp_err_t first_error = ESP_OK;

    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        esp_err_t ret = light_set_duty_with_time(manager->lights[i].channel, 0, manager->fade_time_ms);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }

    esp_err_t ret = light_set_duty(BUZZER_CHANNEL, 0);
    if (ret != ESP_OK && first_error == ESP_OK) {
        first_error = ret;
    }

    UNLOCK(manager);

    ret = audio_queue_stop();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && first_error == ESP_OK) {
        first_error = ret;
    }

    ESP_LOGI(TAG, "OTA outputs suspended without changing saved states");
    return first_error;
}

bool light_manager_is_any_on(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return false;
    }

    LOCK(manager);
    bool is_any_on = false;
    for (int i = 0; i < LIGHT_ID_MAX; i++) {
        if (manager->lights[i].is_on) {
            is_any_on = true;
            break;
        }
    }
    UNLOCK(manager);

    return is_any_on;
}

bool light_manager_is_any_panel_on(light_manager_t* manager)
{
    if (manager == NULL) {
        return false;
    }

    LOCK(manager);
    bool panel_on = manager->lights[LIGHT_ID_AMBIENT].is_on ||
                    manager->lights[LIGHT_ID_LOWER].is_on ||
                    manager->lights[LIGHT_ID_UPPER].is_on;
    UNLOCK(manager);

    return panel_on;
}

bool light_manager_is_on(light_manager_t* manager, light_id_t light_id)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return false;
    }

    if (light_id >= LIGHT_ID_MAX) {
        ESP_LOGE(TAG, "Invalid light ID: %d", light_id);
        return false;
    }

    LOCK(manager);
    bool is_on = manager->lights[light_id].is_on;
    UNLOCK(manager);
    return is_on;
}

uint8_t light_manager_get_light_brightness(light_manager_t* manager, light_id_t light_id)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return 0;
    }

    if (light_id >= LIGHT_ID_MAX) {
        ESP_LOGE(TAG, "Invalid light ID: %d", light_id);
        return 0;
    }

    LOCK(manager);
    uint8_t brightness = manager->lights[light_id].brightness;
    UNLOCK(manager);
    return brightness;
}

// =============================================================================
// NVS存储（预留接口）
// =============================================================================

esp_err_t light_manager_save_state(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (manager->settings == NULL) {
        ESP_LOGW(TAG, "NVS not opened, skip save");
        return ESP_OK;
    }

    // 保存各灯光亮度参数（直接存储百分比0-100，不保存状态）
    settings_set_int(manager->settings, "up_bright", manager->lights[LIGHT_ID_UPPER].brightness);
    settings_set_int(manager->settings, "lower_bright", manager->lights[LIGHT_ID_LOWER].brightness);
    settings_set_int(manager->settings, "ambient_bright",
                     manager->lights[LIGHT_ID_AMBIENT].brightness);

    // 保存红光光疗参数
    settings_set_int(manager->settings, "therapy_state", manager->red_mode);

    // 保存三种红光模式的亮度配置（直接存储百分比0-100）
    settings_set_int(manager->settings, "therapy_b0", manager->therapy_bright[0]);
    settings_set_int(manager->settings, "therapy_b1", manager->therapy_bright[1]);
    settings_set_int(manager->settings, "therapy_b2", manager->therapy_bright[2]);
    settings_set_int(manager->settings, "focus_source", manager->focus_source);

    ESP_LOGI(TAG, "Light state saved to NVS");
    return ESP_OK;
}

esp_err_t light_manager_load_state(light_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 临时打开NVS加载参数
    settings_t* settings = settings_start("device_params", false);
    if (settings == NULL) {
        ESP_LOGW(TAG, "Failed to open NVS for loading, use defaults");
        return ESP_OK; // 首次使用或NVS损坏，使用默认值
    }

    // 加载各灯光亮度参数（直接读取百分比值，默认值60%）
    int32_t up_bright = settings_get_int(settings, "up_bright", 60);
    int32_t lower_bright = settings_get_int(settings, "lower_bright", 60);
    int32_t ambient_bright = settings_get_int(settings, "ambient_bright", 60);

    // 加载三种红光模式的亮度（直接读取百分比值）
    int32_t therapy_b0 = settings_get_int(settings, "therapy_b0", 60);
    int32_t therapy_b1 = settings_get_int(settings, "therapy_b1", 60);
    int32_t therapy_b2 = settings_get_int(settings, "therapy_b2", 100);
    int32_t focus_source = settings_get_int(settings, "focus_source", FOCUS_SOURCE_AUDIO);

    // 关闭NVS
    settings_end(settings);

    // 应用到管理器（确保值在有效范围内0-100）
    manager->lights[LIGHT_ID_UPPER].brightness =
        (up_bright >= 0 && up_bright <= 100) ? (uint8_t)up_bright : 60;
    manager->lights[LIGHT_ID_LOWER].brightness =
        (lower_bright >= 0 && lower_bright <= 100) ? (uint8_t)lower_bright : 60;
    manager->lights[LIGHT_ID_AMBIENT].brightness =
        (ambient_bright >= 0 && ambient_bright <= 100) ? (uint8_t)ambient_bright : 60;

    // 加载三种红光模式的亮度配置（百分比）
    manager->therapy_bright[0] = (therapy_b0 >= 0 && therapy_b0 <= 100) ? (uint8_t)therapy_b0 : 60;
    manager->therapy_bright[1] = (therapy_b1 >= 0 && therapy_b1 <= 100) ? (uint8_t)therapy_b1 : 60;
    manager->therapy_bright[2] = (therapy_b2 >= 0 && therapy_b2 <= 100) ? (uint8_t)therapy_b2 : 100;
    manager->focus_source = (focus_source == FOCUS_SOURCE_BUZZER) ?
                            FOCUS_SOURCE_BUZZER : FOCUS_SOURCE_AUDIO;

    // 红光模式状态不恢复（始终从OFF开始，确保安全）
    manager->red_mode = RED_LIGHT_MODE_OFF;

    // 更新全局亮度为环境光亮度（作为参考）
    manager->global_brightness = manager->lights[LIGHT_ID_AMBIENT].brightness;

    ESP_LOGI(TAG,
             "Light state loaded from NVS (up=%d%%, lower=%d%%, ambient=%d%%, therapy[0]=%d%%, "
             "therapy[1]=%d%%, focus_source=%d)",
             manager->lights[LIGHT_ID_UPPER].brightness, manager->lights[LIGHT_ID_LOWER].brightness,
             manager->lights[LIGHT_ID_AMBIENT].brightness, manager->therapy_bright[0],
             manager->therapy_bright[1], manager->focus_source);

    return ESP_OK;
}

// =============================================================================
// 私有函数实现
// =============================================================================

/**
 * @brief 如果需要，打开NVS
 */
static esp_err_t open_nvs_if_needed(light_manager_t* manager)
{
    if (manager->settings == NULL) {
        manager->settings = settings_start("device_params", true);
        if (manager->settings == NULL) {
            ESP_LOGE(TAG, "Failed to open NVS");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "NVS opened");
    }
    return ESP_OK;
}

/**
 * @brief 如果所有灯都关闭，关闭NVS
 */
static esp_err_t close_nvs_if_all_off(light_manager_t* manager)
{
    if (!light_manager_is_any_on(manager) && manager->settings != NULL) {
        // 保存状态（预留）
        light_manager_save_state(manager);

        // 关闭NVS（自动commit）
        esp_err_t ret = settings_end(manager->settings);
        manager->settings = NULL;

        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "NVS closed and committed");
        }
        else {
            ESP_LOGE(TAG, "Failed to close NVS");
        }

        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 亮度百分比转换为duty值（计算方式，不再查表）
 * @param percent 亮度百分比（0-100）
 * @return uint32_t PWM占空比（0-8191）
 */
static uint32_t brightness_percent_to_duty(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return (uint32_t)(percent * 8191UL / 100);
}

/**
 * @brief 亮度档位转换为百分比（用于按键输入）
 * 档位枚举 -> 实际百分比值
 */
static uint8_t brightness_level_to_percent(brightness_level_t level)
{
    switch (level) {
        case BRIGHTNESS_LEVEL_10:
            return 10;
        case BRIGHTNESS_LEVEL_40:
            return 40;
        case BRIGHTNESS_LEVEL_60:
            return 60;
        case BRIGHTNESS_LEVEL_80:
            return 80;
        case BRIGHTNESS_LEVEL_100:
            return 100;
        default:
            return 60; // 默认60%
    }
}

/**
 * @brief 助眠模式亮度档位转换（上限20%，每档4%）
 */
static uint8_t sleep_brightness_level_to_percent(brightness_level_t level)
{
    switch (level) {
        case BRIGHTNESS_LEVEL_10:
            return 4;
        case BRIGHTNESS_LEVEL_40:
            return 8;
        case BRIGHTNESS_LEVEL_60:
            return 12;
        case BRIGHTNESS_LEVEL_80:
            return 16;
        case BRIGHTNESS_LEVEL_100:
            return 20;
        default:
            return 20;
    }
}

/**
 * @brief 通知状态变化
 */
static void notify_change(light_manager_t* manager, light_change_type_t type, int light_id)
{
    if (manager->change_callback != NULL) {
        manager->change_callback(type, light_id, manager->callback_arg);
    }
}

static void sync_combo_state_from_lights(light_manager_t* manager)
{
    bool ambient_on = manager->lights[LIGHT_ID_AMBIENT].is_on;
    bool lower_on = manager->lights[LIGHT_ID_LOWER].is_on;
    combo_state_t synced_state;

    if (ambient_on && lower_on) {
        synced_state = COMBO_STATE_BOTH_ON;
    } else if (ambient_on) {
        synced_state = COMBO_STATE_AMBIENT_ONLY;
    } else {
        synced_state = COMBO_STATE_ALL_OFF;
    }

    if (manager->combo_state != synced_state) {
        ESP_LOGI(TAG, "Combo state synced: %d -> %d (ambient=%d, lower=%d)",
                 manager->combo_state, synced_state, ambient_on, lower_on);
        manager->combo_state = synced_state;
    }
}

/**
 * @brief 同步面板指示灯状态（低电平点亮，高电平熄灭）
 */
void light_manager_sync_indicator_leds(light_manager_t* manager)
{
    if (manager == NULL) {
        return;
    }

    bool upper_on = manager->lights[LIGHT_ID_UPPER].is_on;
    bool lower_or_ambient_on = manager->lights[LIGHT_ID_LOWER].is_on ||
                               manager->lights[LIGHT_ID_AMBIENT].is_on;
    bool red_on = (manager->red_mode != RED_LIGHT_MODE_OFF);

    gpio_set_level(LED1_IO_NUM, upper_on ? 0 : 1);
    gpio_set_level(LED3_IO_NUM, lower_or_ambient_on ? 0 : 1);
    gpio_set_level(LED2_IO_NUM, red_on ? 0 : 1);

    ESP_LOGI(TAG, "指示灯同步: LED1=%d(上=%d), LED3=%d(下=%d,环境=%d), LED2=%d(红光模式=%d)",
             upper_on ? 0 : 1, upper_on,
             lower_or_ambient_on ? 0 : 1,
             manager->lights[LIGHT_ID_LOWER].is_on,
             manager->lights[LIGHT_ID_AMBIENT].is_on,
             red_on ? 0 : 1, manager->red_mode);
}

/**
 * @brief 发布光疗训练记录（如果满足条件）
 */
static void publish_therapy_record_if_valid(light_manager_t* manager, red_light_mode_t mode)
{
    if (!manager->therapy_recording) {
        return;
    }

    time_t now;
    time(&now);
    int work_time = (int)(now - manager->therapy_start_time);
    
    if (work_time >= MIN_THERAPY_RECORD_TIME) {
        work_record_t record = {
            .start_time = manager->therapy_start_time,
            .end_time = now,
            .work_time = work_time,
            .mode = mode
        };
        mqtt_publish_therapy_record(&record);
        ESP_LOGI(TAG, "Published therapy record: mode=%d, time=%ds", mode, work_time);
    }
}

/**
 * @brief 应用红光OFF模式
 */
static esp_err_t apply_red_mode_off(light_manager_t* manager, bool use_fade, bool save_record)
{
    // 上报训练记录（如果>=5分钟）
    if (save_record) {
        publish_therapy_record_if_valid(manager, manager->red_mode);
        manager->therapy_recording = false;
    }

    // 关闭红光并恢复正常模式定时器
    esp_err_t ret = light_switch_mode(MODE_NORMAL);
