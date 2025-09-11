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
