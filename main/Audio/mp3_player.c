/**
 * @file mp3_player.c
 * @brief MP3播放器实现,基于乐鑫ESP-ADF框架
 */

#include "mp3_player.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "spiffs_stream.h"
#include "mp3_decoder.h"

#include "esp_peripherals.h"
#include "periph_spiffs.h"
#include "board.h"
#include "device_params.h"

static const char *TAG = "SPIFFS_MP3";

// 全局MP3播放器实例
static mp3_player_t *g_mp3_player = NULL;

// 任务句柄
static TaskHandle_t mp3_player_task_handle = NULL;

// 线程安全相关
static SemaphoreHandle_t mp3_player_mutex = NULL;
static bool mp3_player_shutting_down = false;

// 循环播放状态标志(需要互斥锁保护)
static bool loop_restart_pending = false;
static TickType_t loop_restart_time = 0;

// 序列播放相关变量(需要互斥锁保护)
static bool sequence_play_pending = false;
static char sequence_second_file[256] = {0};

// I2S 当前时钟参数(用于循环播放时跳过不必要的 i2s_stream_set_clk 调用)
static int cur_i2s_rate = 0;
static int cur_i2s_bits = 0;
static int cur_i2s_ch   = 0;

#define MP3_PLAYER_DMA_DRAIN_MS 200

/**
 * @brief 内部函数声明
 */
static esp_err_t mp3_player_set_volume_internal(int volume, bool save_to_nvs);

/**
 * @brief 内部函数:获取互斥锁
 */
static bool mp3_player_lock(void)
{
    if (!mp3_player_mutex) {
        return false;
    }
    return xSemaphoreTake(mp3_player_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

/**
 * @brief 内部函数:释放互斥锁
 */
static void mp3_player_unlock(void)
{
    if (mp3_player_mutex) {
        xSemaphoreGive(mp3_player_mutex);
    }
}

/**
 * @brief 内部函数:检查播放器是否已初始化(需要在锁保护下调用)
 */
static bool mp3_player_is_initialized_unsafe(void)
{
    return (g_mp3_player != NULL && g_mp3_player->initialized && !mp3_player_shutting_down);
}

/**
 * @brief 内部函数:检查播放器是否已初始化
 */
static bool mp3_player_is_initialized(void)
{
    if (!mp3_player_lock()) {
        return false;
    }
    bool result = mp3_player_is_initialized_unsafe();
    mp3_player_unlock();
    return result;
}

/**
 * @brief 内部函数:检查播放器是否已启用(需要在锁保护下调用)
 */
static bool mp3_player_is_enabled_unsafe(void)
{
    return (mp3_player_is_initialized_unsafe() && g_mp3_player->enabled);
}

/**
 * @brief 内部函数:检查播放器是否已启用
 */
static bool mp3_player_is_enabled(void)
{
    if (!mp3_player_lock()) {
        return false;
    }
    bool result = mp3_player_is_enabled_unsafe();
    mp3_player_unlock();
    return result;
}

static void mp3_player_clear_restart_flags_locked(void)
{
    loop_restart_pending = false;
