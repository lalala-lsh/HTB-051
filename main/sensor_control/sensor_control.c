#include "sensor_control.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <i2c_bus.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "audio_queue.h"
#include "board_pins.h"
#include "bh1750.h"
#include "ledc_init.h"
#include <driver/ledc.h>
#include "cJSON.h"
#include "constant_light_control.h"
#include "device_params.h"
#include "light_control.h"
#include "light_manager.h"
#include "mqtt_manmager.h"
#include "pir_init.h"
#include "protocol.h"
#include "tts_list.h"

#define TAG "sensor_control"

/*
1、恒光控制：
使用BH1750传感器检测环境光亮度，用于做恒光控制（在此着重声明！！！！！！！此控制仅仅针对灯板控制与检测，不对红光灯珠做操作与检测）：
 - 当任何一个灯（除红光外）开启时，开启恒光控制
 - 读取5分钟的传感器数据，得到一个相对于稳定的环境光数值，以这个值作为基准±10%
 - 如果超过或者低于基准值，相对应调高或者调低灯板亮度，使其传感器数据回到基准值
 - 如果灯板状态有任何改变（灯板开关、亮度）都重新定义基准值


2、pir检测
 - 创建两个定时器，不启动
    - dim_timeout：回调：降低灯光亮度，开启off_timeout定时器。（20分钟）
    - off_timeout：回调：关闭灯光。（5分钟）
 - 当面板灯光打开或单独护眼模式开启，开启定时器dim_timeout
 - 识别到IO有动作，重置dim_timeout定时器时间
 - 触发dim_timeout定时器回调，开启off_timeout定时器
    - 识别到IO有动作，关闭dim_timeout，开启dim_timeout，恢复灯光亮度
    - 触发回调，关闭灯光，停止定时器
*/

static TimerHandle_t dim_timeout_handle = NULL;
static TimerHandle_t off_timeout_handle = NULL;

static TaskHandle_t sensor_control_handle = NULL;
static brightness_level_t memory_brightness_level = BRIGHTNESS_LEVEL_MAX;
static volatile bool pir_control_flag = false;
static volatile bool is_dimmed = false; /* 标记是否已进入调暗阶段 */

/* PIR功能启用开关(从NVS读取) */
static bool pir_enabled = true;

/* PIR检测锁定机制：亮度变化后忽略前几次PIR检测 */
static volatile uint8_t pir_ignore_count = 0;
#define PIR_IGNORE_CYCLES 5 /* 忽略10次检测周期（约10秒） */

volatile uint8_t brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_EXTERNAL;

/* ========== PIR日志环形缓冲区 ========== */
#define PIR_LOG_BUFFER_SIZE 60
#define PIR_LOG_ENTRY_SIZE 24
static char pir_log_entries[PIR_LOG_BUFFER_SIZE][PIR_LOG_ENTRY_SIZE];
static int pir_log_write_index = 0;
static int pir_log_count = 0;

static void reset_pir_control_state(const char* reason) {
    if (dim_timeout_handle != NULL) {
        xTimerStop(dim_timeout_handle, 100);
    }
    if (off_timeout_handle != NULL) {
        xTimerStop(off_timeout_handle, 100);
    }

    pir_control_flag = false;
    is_dimmed = false;
    memory_brightness_level = BRIGHTNESS_LEVEL_MAX;
    pir_ignore_count = 0;

    ESP_LOGI(TAG, "%s，停止定时休息定时器并重置状态", reason);
}

static void pir_log_record(int gpio_level) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    int year  = (timeinfo.tm_year + 1900) % 10000;
    int month = (timeinfo.tm_mon + 1) % 100;
    int day   = timeinfo.tm_mday % 100;
    int hour  = timeinfo.tm_hour % 100;
    int min   = timeinfo.tm_min  % 100;
    int sec   = timeinfo.tm_sec  % 100;
    snprintf(pir_log_entries[pir_log_write_index], PIR_LOG_ENTRY_SIZE,
             "%04d%02d%02d%02d%02d%02d %d",
             year, month, day, hour, min, sec, gpio_level & 1);
    pir_log_write_index = (pir_log_write_index + 1) % PIR_LOG_BUFFER_SIZE;
    if (pir_log_count < PIR_LOG_BUFFER_SIZE) {
        pir_log_count++;
    }
}

/**
 * @brief 检查是否检测到人体运动
 *
 * 正常阶段: 单次轮询
 * 调暗阶段: 快速多次采样(5次/250ms)，捕获短脉冲
 */
static bool pir_is_motion_detected(bool poll_state) {
    if (poll_state == PIR_MOTION_DETECTED) return true;
    if (!is_dimmed) return false;
    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(PIR_IO_NUM) == 0) return true;
    }
    return false;
}

/**
 * @brief 灯光状态变化回调
 *
 * 当按键触发灯光开关或亮度变化时，重置PIR定时器
 */
static void on_light_change(light_change_type_t change_type, int light_id,
                            void* arg) {
    // #region agent log (H6: 追踪所有灯光回调)
    if (pir_control_flag) {
        ESP_LOGI(TAG, "[回调] type=%d, light=%d, src=%d, dimmed=%d, ignore=%d",
                 change_type, light_id, brightness_change_source,
                 is_dimmed, pir_ignore_count);
    }
    // #endregion

    /* MQTT同步策略:
     * 1. 本地按键控制 - 总是同步
     * 2. 恒光控制调节亮度 - 总是同步
     * 3. PIR调暗到10% - 不同步
     * 4. PIR超时关灯 - 在pir_control_flag=false时通过LIGHT_CHANGE_OFF同步
     * 5. 远端MQTT控制(CMD 209) - 不同步(服务器已知晓参数)
     */
    bool should_sync_mqtt = false;

    if (brightness_change_source == BRIGHTNESS_CHANGE_SOURCE_EXTERNAL) {
        /* 本地按键控制,总是同步 */
        should_sync_mqtt = true;
    }
    else if (brightness_change_source == BRIGHTNESS_CHANGE_SOURCE_REMOTE) {
        /* 远端MQTT控制(CMD 209),不需要发送CMD 7同步 */
        should_sync_mqtt = false;
    }
    else if (brightness_change_source ==
             BRIGHTNESS_CHANGE_SOURCE_CONSTANT_LIGHT)
    {
        /* 恒光控制调节亮度,总是同步 */
        should_sync_mqtt = true;
    }
    else if (brightness_change_source == BRIGHTNESS_CHANGE_SOURCE_PIR) {
        /* PIR调暗亮度,不同步MQTT */
        should_sync_mqtt = false;
    }
    else if (!pir_control_flag && change_type == LIGHT_CHANGE_OFF) {
        /* PIR超时关灯(pir_control_flag已被重置为false),需要同步 */
        should_sync_mqtt = true;
    }

    if (should_sync_mqtt) {
        mqtt_notify_light_change();
    }

    /* 以下是PIR定时器控制逻辑 */

    /* 仅本地按键和远端MQTT控制需要处理PIR定时器，忽略PIR和恒光内部调用 */
    if (brightness_change_source != BRIGHTNESS_CHANGE_SOURCE_EXTERNAL &&
        brightness_change_source != BRIGHTNESS_CHANGE_SOURCE_REMOTE) {
        return;
    }

    /* PIR控制未激活时不处理PIR逻辑 */
    if (!pir_control_flag) {
        return;
    }

    ESP_LOGI(TAG, "检测到灯光变化(type=%d, light=%d)，重置定时器", change_type,
             light_id);

    /* 处理关灯事件：检查是否所有灯都关闭 */
    if (change_type == LIGHT_CHANGE_OFF) {
        /* 检查是否所有灯都已关闭 */
        if (!light_manager_is_any_on(get_light_manager())) {
            reset_pir_control_state("所有灯已关闭");
            return;
        }
        /* 还有灯开着，继续重置定时器 */
    }

    if (is_dimmed) {
        /* 调暗阶段：恢复亮度，停止off定时器，重启dim定时器 */
        xTimerStop(off_timeout_handle, 100);

        /* 先恢复临时调暗前每盏灯各自保存的亮度 */
        brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_PIR;
        light_manager_restore_saved_brightness(get_light_manager());
        brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_EXTERNAL;

        constant_light_resume();

        is_dimmed = false;
        /* 重新读取亮度 */
        memory_brightness_level =
            light_manager_get_brightness_level(get_light_manager());
        xTimerReset(dim_timeout_handle, 100);
        ESP_LOGI(TAG, "调暗阶段检测到操作，恢复各灯保存亮度并重启dim定时器(level=%d)",
                 memory_brightness_level);
    }
    else {
        /* 正常阶段：只更新亮度记忆，不重置dim定时器（用灯计时，到时提醒休息） */
        if (change_type == LIGHT_CHANGE_BRIGHTNESS) {
            memory_brightness_level =
                light_manager_get_brightness_level(get_light_manager());
        }
        ESP_LOGI(TAG, "正常阶段检测到按键操作，更新亮度记忆(dim定时器继续计时)");
    }

    /* 通知恒光控制外部亮度变化,触发重新采样 */
    constant_light_on_light_change_external();
}

static void dim_timeout_timer_callback(TimerHandle_t xTimer) {
    /* 专注模式不受定时休息控制：面板灯保持原样，音乐不中断 */
    red_light_mode_t dim_red_mode = light_manager_get_red_mode(get_light_manager());
    if (dim_red_mode == RED_LIGHT_MODE_THERAPY) {
        ESP_LOGI(TAG, "专注模式运行中，跳过定时休息调暗，重置dim定时器");
        xTimerReset(dim_timeout_handle, 0);
        return;
    }

    // 记忆当前亮度档位（用于恢复）
    memory_brightness_level =
        light_manager_get_brightness_level(get_light_manager());

    /* 标记为PIR内部调用，避免回调循环 */
    brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_PIR;

    // 使用临时亮度接口（不修改保存的brightness值）
    light_manager_set_temporary_brightness_level(get_light_manager(),
                                                 BRIGHTNESS_LEVEL_10);

    brightness_change_source = BRIGHTNESS_CHANGE_SOURCE_EXTERNAL;

    /* 暂停恒光控制(PIR调暗阶段) */
    constant_light_suspend();

    xTimerStart(off_timeout_handle, 100);

    /* 标记进入调暗阶段 */
    is_dimmed = true;

    /* 设置忽略计数，防止亮度变化触发误检测 */
    pir_ignore_count = PIR_IGNORE_CYCLES;

    audio_queue_stop();
    audio_queue_play(TURN_OFF, AUDIO_TYPE_FUNCTION, AUDIO_PRIORITY_HIGH, false);
    // #region agent log (H1/H3)
    extern int constant_light_get_state(void);
    ESP_LOGI(TAG, "亮度临时调暗到10%%, memory_level=%d, 锁定%d个周期, 恒光state=%d",
             memory_brightness_level, PIR_IGNORE_CYCLES,
             constant_light_get_state());
    // #endregion
}

static void off_timeout_timer_callback(TimerHandle_t xTimer) {
    /* 先重置PIR控制状态，避免回调重启定时器 */
    pir_control_flag = false;
    is_dimmed = false;
    memory_brightness_level = BRIGHTNESS_LEVEL_MAX;
    pir_ignore_count = 0;

    light_manager_t* lm = get_light_manager();
    red_light_mode_t red_mode = light_manager_get_red_mode(lm);

    if (red_mode == RED_LIGHT_MODE_SLEEP) {
        /* 助眠模式运行中：只关闭面板灯，保留助眠红光 */
        light_manager_set_light_state_percent(lm, LIGHT_ID_UPPER, false, 0, true);
        light_manager_set_light_state_percent(lm, LIGHT_ID_LOWER, false, 0, true);
        light_manager_set_light_state_percent(lm, LIGHT_ID_AMBIENT, false, 0, true);
        light_manager_reset_combo_state(lm);
        ESP_LOGI(TAG, "PIR超时，关闭面板灯（保留助眠红光）");
    } else if (red_mode == RED_LIGHT_MODE_THERAPY) {
        /* 专注模式运行中：不关任何灯（由专注模式30分钟定时器统一管理） */
        ESP_LOGI(TAG, "专注模式运行中，跳过PIR关灯");
    } else {
        light_manager_turn_off_all(lm, true);
        ESP_LOGI(TAG, "PIR超时，关闭所有灯光并复位状态");
    }
}

void sensor_control_task(void* pvParameters) {
    pir_state_t current_pir_state;

    while (1) {
        /* 每秒记录PIR GPIO电平到环形缓冲区（无论PIR是否启用） */
        pir_log_record(gpio_get_level(PIR_IO_NUM));

        /* PIR功能禁用时,仅更新恒光控制 */
        if (!pir_enabled) {
            constant_light_update_state();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        light_manager_t* lm = get_light_manager();
        bool panel_on = light_manager_is_any_panel_on(lm);
        red_light_mode_t red_mode = light_manager_get_red_mode(lm);
        bool eye_mode_on = (red_mode == RED_LIGHT_MODE_NORMAL);
        bool rest_timer_should_run = panel_on || eye_mode_on;

        /* 面板灯或护眼模式开启，启动定时休息流程 */
        if (rest_timer_should_run && !pir_control_flag) {
            ESP_LOGI(TAG, "开启dim定时器(panel_on=%d, eye_mode_on=%d)",
                     panel_on, eye_mode_on);
            pir_control_flag = true;
            memory_brightness_level =
                light_manager_get_brightness_level(lm);
            xTimerStart(dim_timeout_handle, 100);
        }
        else if (!rest_timer_should_run && pir_control_flag) {
            reset_pir_control_state("面板灯关闭且未处于护眼模式");
        }

        if (pir_control_flag) {
            current_pir_state = pir_get_state();
            // ESP_LOGI(TAG, "PIR IO:%d, pir_state:%d, is_dimmed:%d",
            //          gpio_get_level(PIR_IO_NUM), current_pir_state, is_dimmed);

            /* 锁定期间递减计数，跳过PIR检测 */
            if (pir_ignore_count > 0) {
                pir_ignore_count--;
                ESP_LOGI(TAG, "PIR检测锁定中，剩余%d个周期", pir_ignore_count);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            /* 检测到运动 */
            if (pir_is_motion_detected(current_pir_state)) {
                if (is_dimmed) {
