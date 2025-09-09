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

