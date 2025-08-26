#ifndef _BUTTON_MANAGER_H_
#define _BUTTON_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * @brief 按键事件类型
 */
typedef enum {
    BUTTON_EVENT_PRESSED,         // 按下事件(滑动按键)
    BUTTON_EVENT_SINGLE_CLICK,    // 单击事件(开关按键)
    BUTTON_EVENT_HOLD_2S,         // 按住2秒事件(不终止长按计时)
    BUTTON_EVENT_LONG_PRESS,      // 长按事件
    BUTTON_EVENT_RELEASED,        // 释放事件
} button_event_t;

/**
 * @brief 按键行为类型
 */
typedef enum {
    BUTTON_TYPE_SLIDE,            // 滑动按键
    BUTTON_TYPE_CLICK,            // 单击按键
    BUTTON_TYPE_LONG_PRESS,       // 长按按键(支持单击+中途按住事件+长按)
} button_type_t;

/**
 * @brief 按键状态机状态
 */
typedef enum {
    BUTTON_STATE_IDLE,
    BUTTON_STATE_DEBOUNCING,
    BUTTON_STATE_PRESSED,
    BUTTON_STATE_LONG_PRESSING,
} button_state_t;

/**
 * @brief 按键配置结构
 */
typedef struct {
    uint8_t id;                   // 按键ID (0-7)
    button_type_t type;           // 按键类型
    uint16_t hold_time_ms;        // 中途按住事件阈值(ms),0表示禁用
    uint16_t long_press_time_ms;  // 长按阈值(ms)
    bool enabled;                 // 是否启用
} button_config_t;

/**
 * @brief 按键运行时状态
 */
typedef struct {
    button_state_t state;         // 当前状态
    uint8_t debounce_count;       // 消抖计数器
    uint16_t press_time_count;    // 按下时间计数器
    bool hold_event_fired;        // 中途按住事件已触发标志
    bool event_fired;             // 事件已触发标志
} button_runtime_t;

/**
 * @brief 按键事件回调函数类型
 *
 * @param button_id 按键ID (0-7)
 * @param event 事件类型
 * @param arg 用户自定义参数
 */
typedef void (*button_event_callback_t)(uint8_t button_id,
                                         button_event_t event,
                                         void* arg);

/**
 * @brief 按键管理器结构（不透明类型）
 */
typedef struct button_manager button_manager_t;

// 默认参数配置
#define BUTTON_DEFAULT_POLL_PERIOD_MS       20     // 20ms轮询周期
#define BUTTON_DEFAULT_DEBOUNCE_THRESHOLD   3      // 3次消抖阈值

// 各按键长按阈值(ms)
#define BUTTON5_LONG_PRESS_TIME_MS          2000   // 按键5: 2秒关闭所有灯光
#define BUTTON6_HOLD_TIME_MS                2000   // 按键6: 2秒切换专注模式音源
#define BUTTON6_LONG_PRESS_TIME_MS          5000   // 按键6: 5秒切换A2DP模式
#define BUTTON7_LONG_PRESS_TIME_MS          10000  // 按键7: 10秒恢复出厂设置

// 按键ID定义
#define BUTTON_ID_0   0  // 滑动按键
#define BUTTON_ID_1   1  // 滑动按键
#define BUTTON_ID_2   2  // 滑动按键
#define BUTTON_ID_3   3  // 滑动按键
#define BUTTON_ID_4   4  // 滑动按键
#define BUTTON_ID_5   5  // 长按按键(单击+2s长按关灯)
#define BUTTON_ID_6   6  // 长按按键(单击+2s专注音源切换+5s长按)
#define BUTTON_ID_7   7  // 长按按键(单击+10s长按)
