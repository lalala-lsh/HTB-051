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

