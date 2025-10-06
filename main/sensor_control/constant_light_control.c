/**
 * @file constant_light_control.c
 * @brief BH1750恒光控制模块实现
 */

#include "constant_light_control.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <math.h>
#include <string.h>

#include "bh1750.h"
#include "device_params.h"
#include "sensor_control.h"
#include "light_control.h"

#define TAG "constant_light"

/* 可调参数 */
#define CL_SAMPLE_INTERVAL_MS 10000   // 采样间隔10秒
#define CL_SAMPLE_DURATION_MS 300000  // 采样时长5分钟
#define CL_SAMPLE_COUNT 30            // 采样次数(5min/10s)
#define CL_BASELINE_TOLERANCE 0.1f    // 基准容差±10%
#define CL_ADJUST_INTERVAL_MS 5000    // 调节间隔5秒
#define CL_STABLE_THRESHOLD 3         // 稳定阈值(连续3次在容差内)
#define CL_IGNORE_CYCLES 6            // 调节后忽略6个检测周期(30秒)
#define CL_RELATIVE_THRESHOLD 15.0f   // 相对阈值15%(触发调节)
#define CL_LARGE_CHANGE_THRESHOLD 30.0f // 大幅变化阈值30%(跳2档)
#define CL_CONSISTENT_THRESHOLD 3     // 连续检测阈值(连续3次偏差才调节)

/* 状态定义 */
typedef enum {
    CL_STATE_IDLE,      // 空闲:所有灯板关闭或功能禁用
    CL_STATE_SAMPLING,  // 采样中:读取5分钟数据建立基准值
    CL_STATE_ACTIVE,    // 活跃:基准值已建立,实时调节中
    CL_STATE_SUSPENDED, // 暂停:PIR调暗阶段暂停调节
} cl_state_t;

/* 恒光控制管理器结构体 */
typedef struct {
    // 状态管理
    cl_state_t state;
    bool enabled; // 功能总开关

    // 基准值管理
    float baseline_lux;      // 基准光照强度(lx)
    float baseline_tolerance; // 容差(±10% = 0.1)

    // 采样数据
    float sample_buffer[CL_SAMPLE_COUNT]; // 采样缓冲区
    uint16_t sample_index;                // 当前采样索引
    TickType_t sample_start_time;         // 采样开始时间

    // 调节策略
    brightness_level_t current_level; // 当前亮度档位
    uint8_t stable_count;             // 稳定计数
    uint8_t consistent_count;         // 连续偏差计数
    brightness_level_t pending_target_level; // 待执行的目标档位

    // 防抖和锁定
    uint8_t ignore_count; // 忽略计数

    // 定时器
    TimerHandle_t sample_timer; // 采样定时器(10秒周期)
    TimerHandle_t adjust_timer; // 调节定时器(5秒周期)

    // 回调引用
    light_manager_t *light_mgr; // 灯光管理器引用
} constant_light_control_t;

/* 全局实例 */
static constant_light_control_t *cl_ctrl = NULL;

/* brightness_change_source 通过 sensor_control.h 的 extern 声明获取 */

/* 前向声明 */
static void cl_sample_timer_callback(TimerHandle_t xTimer);
static void cl_adjust_timer_callback(TimerHandle_t xTimer);
static float calculate_baseline(const float *samples, uint16_t count);
static bool is_sampling_stable(const float *samples, uint16_t count, float baseline);
static brightness_level_t calculate_target_level(float current_lux,
                                                  float baseline,
                                                  brightness_level_t current_level);
static bool is_any_panel_on(void);

/**
 * @brief 计算采样数据的基准值(中位数)
 */
static float calculate_baseline(const float *samples, uint16_t count)
{
    if (count == 0) {
        return 0.0f;
    }

