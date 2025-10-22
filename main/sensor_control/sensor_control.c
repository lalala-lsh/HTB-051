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
