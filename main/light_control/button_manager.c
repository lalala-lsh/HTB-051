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
                                uint8_t priority,
                                uint32_t stack_size)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (manager->task_handle != NULL) {
        ESP_LOGW(TAG, "Task already running");
        return ESP_ERR_INVALID_STATE;
    }

    // 初始化硬件
    touch_button_init();

    // 创建按键扫描任务
    BaseType_t ret = xTaskCreate(button_scan_task,
                                  "button_scan",
                                  stack_size,
                                  manager,
                                  priority,
                                  &manager->task_handle);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button scan task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Button manager started (period=%dms, debounce=%d)",
             manager->poll_period_ms,
             manager->debounce_count_threshold);

    return ESP_OK;
}

/**
 * @brief 停止按键扫描任务
 */
esp_err_t button_manager_stop(button_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (manager->task_handle != NULL) {
        vTaskDelete(manager->task_handle);
        manager->task_handle = NULL;
        ESP_LOGI(TAG, "Button scan task stopped");
    }

    return ESP_OK;
}

/**
 * @brief 设置按键配置
 */
esp_err_t button_manager_set_config(button_manager_t* manager,
                                     uint8_t button_id,
                                     button_type_t type,
                                     uint16_t long_press_time_ms)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (button_id >= 8) {
        ESP_LOGE(TAG, "Invalid button ID: %d", button_id);
        return ESP_ERR_INVALID_ARG;
    }

    manager->configs[button_id].id = button_id;
    manager->configs[button_id].type = type;
    manager->configs[button_id].hold_time_ms = 0;
    manager->configs[button_id].long_press_time_ms = long_press_time_ms;
    manager->configs[button_id].enabled = true;

    return ESP_OK;
}

/**
 * @brief 应用默认配置
 */
esp_err_t button_manager_apply_default_config(button_manager_t* manager)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 按键0-4: 滑动按键
    for (int i = 0; i <= 4; i++) {
        button_manager_set_config(manager, i, BUTTON_TYPE_SLIDE, 0);
    }

    // 按键5: 单击环境光+下光组合控制,长按2秒关闭所有灯光
    button_manager_set_config(manager, 5, BUTTON_TYPE_LONG_PRESS,
                              BUTTON5_LONG_PRESS_TIME_MS);

    // 按键6: 2秒切换专注模式音源,5秒切换A2DP模式
    button_manager_set_config(manager, 6, BUTTON_TYPE_LONG_PRESS, BUTTON6_LONG_PRESS_TIME_MS);
    manager->configs[6].hold_time_ms = BUTTON6_HOLD_TIME_MS;

    // 按键7: 长按按键(支持单击+长按恢复出厂设置)
    button_manager_set_config(manager, 7, BUTTON_TYPE_LONG_PRESS, BUTTON7_LONG_PRESS_TIME_MS);

    ESP_LOGI(TAG, "Default config applied");
    return ESP_OK;
}

/**
 * @brief 设置轮询周期
 */
esp_err_t button_manager_set_poll_period(button_manager_t* manager,
                                          uint16_t period_ms)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (period_ms == 0) {
        ESP_LOGE(TAG, "Invalid poll period: %d", period_ms);
        return ESP_ERR_INVALID_ARG;
    }

    manager->poll_period_ms = period_ms;
    return ESP_OK;
}

/**
 * @brief 设置消抖阈值
 */
esp_err_t button_manager_set_debounce_threshold(button_manager_t* manager,
                                                 uint8_t threshold)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (threshold == 0) {
        ESP_LOGE(TAG, "Invalid debounce threshold: %d", threshold);
        return ESP_ERR_INVALID_ARG;
    }

    manager->debounce_count_threshold = threshold;
    return ESP_OK;
}

/**
 * @brief 注册按键事件回调函数
 */
esp_err_t button_manager_register_callback(button_manager_t* manager,
                                            button_event_callback_t callback,
                                            void* arg)
{
    if (manager == NULL) {
        ESP_LOGE(TAG, "Manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    manager->callback = callback;
    manager->callback_arg = arg;

    return ESP_OK;
}

/**
 * @brief 按键扫描任务
 */
static void button_scan_task(void* arg)
{
    button_manager_t* manager = (button_manager_t*)arg;

    ESP_LOGI(TAG, "Button scan task started");

    while (1) {
        // 1. 读取硬件原始数据
        uint8_t flag = touch_button_read_flag();
        uint8_t id = touch_button_read_id();
        // ESP_LOGI(TAG, "flag = %d, id = %d", flag, id);

        // 2. 检测滑动（按键ID变化）
        if (flag == 0 && manager->last_stable_flag == 0) {
            // 有按键按下，且上次也有按键按下
            if (id != manager->last_stable_id) {
                // 按键ID发生变化，触发滑动处理
                handle_slide_change(manager, manager->last_stable_id, id);
