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

    // 复制数组(避免修改原数据)
    float sorted[CL_SAMPLE_COUNT];
    memcpy(sorted, samples, sizeof(float) * count);

    // 排序(冒泡排序)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                float temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }

    // 返回中位数
    if (count % 2 == 0) {
        return (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0f;
    } else {
        return sorted[count / 2];
    }
}

/**
 * @brief 检查采样数据是否稳定(最后10个点在±10%容差内)
 */
static bool is_sampling_stable(const float *samples, uint16_t count, float baseline)
{
    if (baseline < 1.0f) {
        return false; // 基准值太小,认为不稳定
    }

    float min = baseline * (1.0f - CL_BASELINE_TOLERANCE);
    float max = baseline * (1.0f + CL_BASELINE_TOLERANCE);

    int stable_samples = (count >= 10) ? 10 : count;
    for (int i = count - stable_samples; i < count; i++) {
        if (samples[i] < min || samples[i] > max) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 计算需要调整的亮度档位
 *
 * 策略:
 * - 只使用相对阈值15%判断,适应不同环境光强度
 * - 差异>30%时跳2档,否则跳1档
 */
static brightness_level_t calculate_target_level(float current_lux,
                                                  float baseline,
                                                  brightness_level_t current_level)
{
    float diff = current_lux - baseline;
    float diff_percent = fabs(diff / baseline) * 100.0f;

    // 仅使用相对阈值判断(移除绝对阈值)
    if (diff_percent < CL_RELATIVE_THRESHOLD) {
        return current_level; // 保持不变
    }

    // 环境光过亮 → 降低灯光亮度
    if (current_lux > baseline * (1.0f + CL_BASELINE_TOLERANCE)) {
        if (diff_percent > CL_LARGE_CHANGE_THRESHOLD && current_level > BRIGHTNESS_LEVEL_40) {
            return current_level - 2; // 降2档
        } else if (current_level > BRIGHTNESS_LEVEL_10) {
            return current_level - 1; // 降1档
        }
    }
    // 环境光过暗 → 提高灯光亮度
    else if (current_lux < baseline * (1.0f - CL_BASELINE_TOLERANCE)) {
        if (diff_percent > CL_LARGE_CHANGE_THRESHOLD && current_level < BRIGHTNESS_LEVEL_80) {
            return current_level + 2; // 升2档
        } else if (current_level < BRIGHTNESS_LEVEL_100) {
            return current_level + 1; // 升1档
        }
    }

    return current_level;
}

/**
 * @brief 检查是否有灯板(除红光)开启
 */
static bool is_any_panel_on(void)
{
    if (!cl_ctrl || !cl_ctrl->light_mgr) {
        return false;
    }

    return light_manager_is_on(cl_ctrl->light_mgr, LIGHT_ID_AMBIENT) ||
           light_manager_is_on(cl_ctrl->light_mgr, LIGHT_ID_LOWER) ||
           light_manager_is_on(cl_ctrl->light_mgr, LIGHT_ID_UPPER);
}

/**
 * @brief 采样定时器回调(每10秒读取一次BH1750)
 */
static void cl_sample_timer_callback(TimerHandle_t xTimer)
{
    if (!cl_ctrl || cl_ctrl->state != CL_STATE_SAMPLING) {
        return;
    }

    // 读取BH1750数据
    float lux = 0.0f;
    esp_err_t ret = bh1750_get_data(&lux);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取BH1750失败,跳过本次采样");
        return;
    }

    // 过滤异常值
    if (lux < 0.0f || lux > 10000.0f) {
        ESP_LOGW(TAG, "BH1750数据异常(%.2f lx),跳过", lux);
        return;
    }

    // 存储采样值
    cl_ctrl->sample_buffer[cl_ctrl->sample_index++] = lux;
    // #region agent log (H1: 采样进度)
    ESP_LOGI(TAG, "采样 %d/%d: %.2f lx", cl_ctrl->sample_index, CL_SAMPLE_COUNT, lux);
    // #endregion

    // 检查是否采样完成
    if (cl_ctrl->sample_index >= CL_SAMPLE_COUNT) {
        cl_ctrl->baseline_lux = calculate_baseline(cl_ctrl->sample_buffer, CL_SAMPLE_COUNT);

        if (is_sampling_stable(cl_ctrl->sample_buffer, CL_SAMPLE_COUNT, cl_ctrl->baseline_lux)) {
            // 采样完成,进入ACTIVE状态
            cl_ctrl->state = CL_STATE_ACTIVE;
            cl_ctrl->current_level = light_manager_get_brightness_level(cl_ctrl->light_mgr);
            xTimerStop(cl_ctrl->sample_timer, 100);
            xTimerStart(cl_ctrl->adjust_timer, 100);
            ESP_LOGI(TAG, "基准值建立完成: %.2f lx (容差±%.1f%%)",
                     cl_ctrl->baseline_lux, CL_BASELINE_TOLERANCE * 100.0f);
        } else {
            ESP_LOGW(TAG, "采样数据不稳定(基准%.2f lx),重新采样", cl_ctrl->baseline_lux);
            cl_ctrl->sample_index = 0;
        }
    }
}

/**
 * @brief 调节定时器回调(每5秒检测并调节亮度)
 *
 * 新增连续检测机制:连续3次检测到偏差才执行调节,避免误调节
 */
static void cl_adjust_timer_callback(TimerHandle_t xTimer)
{
    if (!cl_ctrl || cl_ctrl->state != CL_STATE_ACTIVE) {
        return;
    }

    // 忽略计数递减
    if (cl_ctrl->ignore_count > 0) {
        cl_ctrl->ignore_count--;
        ESP_LOGD(TAG, "忽略调节(锁定中,剩余%d周期)", cl_ctrl->ignore_count);
        return;
    }

    // 读取当前光照强度
    float current_lux = 0.0f;
    if (bh1750_get_data(&current_lux) != ESP_OK || current_lux < 0.0f || current_lux > 10000.0f) {
        ESP_LOGW(TAG, "读取BH1750失败或数据异常");
        cl_ctrl->consistent_count = 0; // 读取失败,重置连续计数
        return;
    }

    // 计算目标档位
    brightness_level_t current_level = light_manager_get_brightness_level(cl_ctrl->light_mgr);
    brightness_level_t target_level = calculate_target_level(current_lux, cl_ctrl->baseline_lux, current_level);
    float diff_percent = ((current_lux - cl_ctrl->baseline_lux) / cl_ctrl->baseline_lux) * 100.0f;

    // #region agent log (H1: 恒光调节活动)
    ESP_LOGI(TAG, "[恒光监测] lux=%.1f, 基准=%.1f, 偏差=%+.1f%%, 档位=%d",
             current_lux, cl_ctrl->baseline_lux, diff_percent, current_level);
    // #endregion

    // 检测到偏差(需要调节)
    if (target_level != current_level) {
        // 检查是否与上次目标档位一致
        if (cl_ctrl->pending_target_level == target_level) {
            // 连续检测到相同的偏差
            cl_ctrl->consistent_count++;
            ESP_LOGD(TAG, "[恒光检测] 光照偏差 %.2f lx vs 基准 %.2f lx (%+.1f%%), "
                          "目标档位 %d→%d, 连续计数 %d/%d",
                     current_lux, cl_ctrl->baseline_lux, diff_percent,
                     current_level, target_level,
                     cl_ctrl->consistent_count, CL_CONSISTENT_THRESHOLD);

            // 达到连续阈值,执行调节
            if (cl_ctrl->consistent_count >= CL_CONSISTENT_THRESHOLD) {
                ESP_LOGI(TAG, "[恒光调节] 连续%d次检测到偏差,执行调节 %d→%d",
                         CL_CONSISTENT_THRESHOLD, current_level, target_level);

                // 调节亮度(设置来源标志,在sensor_control.c中声明)
                brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_CONSTANT_LIGHT;
                light_manager_set_brightness_level(cl_ctrl->light_mgr, target_level);
                brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_EXTERNAL;

                cl_ctrl->current_level = target_level;
                cl_ctrl->stable_count = 0;
                cl_ctrl->consistent_count = 0; // 重置连续计数
                cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX; // 重置待定目标
                cl_ctrl->ignore_count = CL_IGNORE_CYCLES; // 锁定30秒
            }
        } else {
            // 目标档位变化,重新开始计数
            cl_ctrl->pending_target_level = target_level;
            cl_ctrl->consistent_count = 1;
            ESP_LOGD(TAG, "[恒光检测] 检测到新的偏差方向,重新计数: %.2f lx vs 基准 %.2f lx (%+.1f%%)",
                     current_lux, cl_ctrl->baseline_lux, diff_percent);
        }
        cl_ctrl->stable_count = 0; // 重置稳定计数
    } else {
        // 在容差范围内,光照稳定
        cl_ctrl->consistent_count = 0; // 重置连续计数
        cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX; // 重置待定目标
        cl_ctrl->stable_count++;
        ESP_LOGD(TAG, "[恒光稳定] 光照稳定在基准值附近 (稳定计数 %d/%d)",
                 cl_ctrl->stable_count, CL_STABLE_THRESHOLD);
    }
}

/**
 * @brief 初始化恒光控制模块
 */
esp_err_t constant_light_init(void)
{
    if (cl_ctrl != NULL) {
        ESP_LOGW(TAG, "恒光控制已初始化");
        return ESP_OK;
    }

    // 分配控制结构(使用heap_caps_malloc从PSRAM分配)
    cl_ctrl = heap_caps_malloc(sizeof(constant_light_control_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cl_ctrl == NULL) {
        ESP_LOGE(TAG, "分配内存失败");
        return ESP_FAIL;
    }

    // 初始化结构
    memset(cl_ctrl, 0, sizeof(constant_light_control_t));
    cl_ctrl->state = CL_STATE_IDLE;
    cl_ctrl->enabled = (device_params_get_constant_light_state() == 1); // 从NVS读取
    cl_ctrl->baseline_tolerance = CL_BASELINE_TOLERANCE;
    cl_ctrl->light_mgr = get_light_manager();
    cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX; // 初始化为无效值

    // 创建采样定时器(10秒周期)
    cl_ctrl->sample_timer = xTimerCreate(
        "cl_sample",                      // 定时器名称
        pdMS_TO_TICKS(CL_SAMPLE_INTERVAL_MS), // 周期
        pdTRUE,                           // 自动重载
        NULL,                             // 定时器ID
        cl_sample_timer_callback          // 回调函数
    );
    if (cl_ctrl->sample_timer == NULL) {
        ESP_LOGE(TAG, "创建采样定时器失败");
        heap_caps_free(cl_ctrl);
        cl_ctrl = NULL;
        return ESP_FAIL;
    }

    // 创建调节定时器(5秒周期)
    cl_ctrl->adjust_timer = xTimerCreate(
        "cl_adjust",                      // 定时器名称
        pdMS_TO_TICKS(CL_ADJUST_INTERVAL_MS), // 周期
        pdTRUE,                           // 自动重载
        NULL,                             // 定时器ID
        cl_adjust_timer_callback          // 回调函数
    );
    if (cl_ctrl->adjust_timer == NULL) {
        ESP_LOGE(TAG, "创建调节定时器失败");
        xTimerDelete(cl_ctrl->sample_timer, 100);
        heap_caps_free(cl_ctrl);
        cl_ctrl = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "恒光控制初始化成功(功能%s)", cl_ctrl->enabled ? "启用" : "禁用");
    return ESP_OK;
}

/**
 * @brief 更新恒光控制状态
 */
void constant_light_update_state(void)
{
    if (!cl_ctrl || !cl_ctrl->enabled) {
        return;
    }

    bool panels_on = is_any_panel_on();

    switch (cl_ctrl->state) {
    case CL_STATE_IDLE:
        // 有灯板开启 → 启动采样
        if (panels_on) {
            cl_ctrl->state = CL_STATE_SAMPLING;
            cl_ctrl->sample_index = 0;
            cl_ctrl->sample_start_time = xTaskGetTickCount();
            xTimerStart(cl_ctrl->sample_timer, 100);
            ESP_LOGI(TAG, "启动恒光控制(采样5分钟建立基准值)");
        }
        break;

    case CL_STATE_SAMPLING:
    case CL_STATE_ACTIVE:
        // 所有灯板关闭 → 回到IDLE
        if (!panels_on) {
            xTimerStop(cl_ctrl->sample_timer, 100);
            xTimerStop(cl_ctrl->adjust_timer, 100);
            cl_ctrl->state = CL_STATE_IDLE;
            cl_ctrl->sample_index = 0;
            cl_ctrl->stable_count = 0;
            cl_ctrl->ignore_count = 0;
            cl_ctrl->consistent_count = 0; // 重置连续检测计数
            cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX; // 重置待定目标
            ESP_LOGI(TAG, "停止恒光控制(所有灯板关闭)");
        }
        break;

    case CL_STATE_SUSPENDED:
        // 所有灯板关闭 → 回到IDLE
        if (!panels_on) {
            xTimerStop(cl_ctrl->adjust_timer, 100);
            cl_ctrl->state = CL_STATE_IDLE;
            cl_ctrl->consistent_count = 0; // 重置连续检测计数
            cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX; // 重置待定目标
            ESP_LOGI(TAG, "停止恒光控制(所有灯板关闭)");
        }
        break;

    default:
        break;
    }
}

/**
 * @brief 外部灯光变化通知
 */
void constant_light_on_light_change_external(void)
{
    if (!cl_ctrl || !cl_ctrl->enabled) {
        return;
    }

    // 如果在ACTIVE状态,重新采样建立基准值(因为外部改变了亮度)
    if (cl_ctrl->state == CL_STATE_ACTIVE) {
        cl_ctrl->state = CL_STATE_SAMPLING;
        cl_ctrl->sample_index = 0;
        cl_ctrl->sample_start_time = xTaskGetTickCount();
        cl_ctrl->consistent_count = 0; // 重置连续检测计数
        cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX; // 重置待定目标
        xTimerStop(cl_ctrl->adjust_timer, 100);
        xTimerStart(cl_ctrl->sample_timer, 100);
        ESP_LOGI(TAG, "外部灯光变化,重新采样建立基准值");
    }
}

/**
 * @brief 暂停恒光控制(PIR调暗时调用)
 */
void constant_light_suspend(void)
{
    if (!cl_ctrl || !cl_ctrl->enabled) {
        return;
    }

    // #region agent log
    ESP_LOGI(TAG, "[suspend] 当前state=%d (0=IDLE,1=SAMPLING,2=ACTIVE,3=SUSPENDED)",
             cl_ctrl->state);
    // #endregion

    if (cl_ctrl->state == CL_STATE_ACTIVE) {
        cl_ctrl->state = CL_STATE_SUSPENDED;
        cl_ctrl->consistent_count = 0;
        cl_ctrl->pending_target_level = BRIGHTNESS_LEVEL_MAX;
