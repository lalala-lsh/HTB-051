#include "button_manager.h"
#include "touch_button.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "button_manager"

/**
 * @brief 按键管理器完整结构定义
 */
struct button_manager {
    // 配置数据
    button_config_t configs[8];           // 8个按键的配置
    uint16_t poll_period_ms;              // 轮询周期(ms)
    uint8_t debounce_count_threshold;     // 消抖计数阈值

    // 运行时数据
    button_runtime_t runtime[8];          // 8个按键的运行时状态
    uint8_t last_stable_id;               // 上次稳定的按键ID
    uint8_t last_stable_flag;             // 上次稳定的标志位

    // 回调函数
    button_event_callback_t callback;     // 事件回调函数
    void* callback_arg;                   // 回调参数

    // 任务句柄
    TaskHandle_t task_handle;             // 按键扫描任务句柄
};

// 私有函数声明
static void button_scan_task(void* arg);
static void handle_button_press(button_manager_t* manager, uint8_t id);
static void handle_button_release(button_manager_t* manager, uint8_t id);
static void handle_slide_change(button_manager_t* manager, uint8_t from_id, uint8_t to_id);
static void trigger_event(button_manager_t* manager, uint8_t id, button_event_t event);
static void reset_button_state(button_manager_t* manager, uint8_t id);

/**
 * @brief 创建按键管理器实例
 */
button_manager_t* button_manager_create(void)
{
    // 分配内存
    button_manager_t* manager = (button_manager_t*)heap_caps_calloc(1, sizeof(button_manager_t), MALLOC_CAP_SPIRAM);
    if (manager == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for button manager");
        return NULL;
    }

    // 初始化默认参数
    manager->poll_period_ms = BUTTON_DEFAULT_POLL_PERIOD_MS;
    manager->debounce_count_threshold = BUTTON_DEFAULT_DEBOUNCE_THRESHOLD;
    manager->last_stable_flag = 1;  // 初始状态：无按键按下
    manager->last_stable_id = 0;
    manager->callback = NULL;
    manager->callback_arg = NULL;
    manager->task_handle = NULL;

    // 初始化所有按键状态为IDLE
    for (int i = 0; i < 8; i++) {
        manager->runtime[i].state = BUTTON_STATE_IDLE;
        manager->runtime[i].debounce_count = 0;
        manager->runtime[i].press_time_count = 0;
        manager->runtime[i].hold_event_fired = false;
        manager->runtime[i].event_fired = false;
    }

    ESP_LOGI(TAG, "Button manager created");
    return manager;
}

/**
 * @brief 销毁按键管理器实例
 */
void button_manager_destroy(button_manager_t* manager)
{
    if (manager == NULL) {
        return;
    }

    // 停止任务
    button_manager_stop(manager);

    // 释放内存
    free(manager);
    ESP_LOGI(TAG, "Button manager destroyed");
}

/**
 * @brief 启动按键扫描任务
 */
esp_err_t button_manager_start(button_manager_t* manager,
