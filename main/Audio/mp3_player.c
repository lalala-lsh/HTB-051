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
    loop_restart_time = 0;
    sequence_play_pending = false;
    memset(sequence_second_file, 0, sizeof(sequence_second_file));
}

static void mp3_player_drain_event_queue_locked(void)
{
    if (!g_mp3_player || !g_mp3_player->evt) {
        return;
    }

    audio_event_iface_msg_t msg;
    while (audio_event_iface_listen(g_mp3_player->evt, &msg, 0) == ESP_OK) {
    }
}

static esp_err_t mp3_player_set_i2s_clock_locked(int rate, int bits, int channels)
{
    if (!g_mp3_player || !g_mp3_player->i2s_stream_writer) {
        return ESP_FAIL;
    }

    if (rate <= 0 || bits <= 0 || channels <= 0) {
        ESP_LOGE(TAG, "无效的I2S参数: rate=%d, bits=%d, ch=%d", rate, bits, channels);
        return ESP_ERR_INVALID_ARG;
    }

    if (rate == cur_i2s_rate && bits == cur_i2s_bits && channels == cur_i2s_ch) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_stream_set_clk(g_mp3_player->i2s_stream_writer, rate, bits, channels);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S参数设置失败(rate=%d,bits=%d,ch=%d): %s",
                 rate, bits, channels, esp_err_to_name(ret));
        return ret;
    }

    cur_i2s_rate = rate;
    cur_i2s_bits = bits;
    cur_i2s_ch = channels;
    return ESP_OK;
}

static esp_err_t mp3_player_prepare_i2s_clock_for_file_locked(const char *file_path, bool loop_mode)
{
    if (loop_mode) {
        if (file_path && strcmp(file_path, MUSIC) == 0) {
            return mp3_player_set_i2s_clock_locked(44100, 16, 2);
        }
        if (file_path && strcmp(file_path, MUSIC_40HZ) == 0) {
            return mp3_player_set_i2s_clock_locked(44100, 16, 1);
        }

        ESP_LOGW(TAG, "未知循环音乐文件,使用默认音乐I2S参数: %s", file_path ? file_path : "(null)");
        return mp3_player_set_i2s_clock_locked(44100, 16, 2);
    }

    return mp3_player_set_i2s_clock_locked(16000, 16, 1);
}

static esp_err_t mp3_player_stop_pipeline_locked(bool reset_for_restart)
{
    if (!g_mp3_player || !g_mp3_player->pipeline) {
        return ESP_FAIL;
    }

    mp3_player_clear_restart_flags_locked();

    esp_err_t ret = audio_pipeline_stop(g_mp3_player->pipeline);
    audio_pipeline_wait_for_stop(g_mp3_player->pipeline);
    audio_pipeline_terminate(g_mp3_player->pipeline);

    if (g_mp3_player->mp3_decoder) {
        audio_element_set_ringbuf_done(g_mp3_player->mp3_decoder);
    }
    if (g_mp3_player->i2s_stream_writer) {
        audio_element_set_ringbuf_done(g_mp3_player->i2s_stream_writer);
    }
    if (g_mp3_player->spiffs_stream_reader) {
        audio_element_set_ringbuf_done(g_mp3_player->spiffs_stream_reader);
    }

    vTaskDelay(pdMS_TO_TICKS(MP3_PLAYER_DMA_DRAIN_MS));
    mp3_player_drain_event_queue_locked();

    if (reset_for_restart) {
        if (g_mp3_player->spiffs_stream_reader) {
            audio_element_reset_state(g_mp3_player->spiffs_stream_reader);
        }
        if (g_mp3_player->mp3_decoder) {
            audio_element_reset_state(g_mp3_player->mp3_decoder);
        }
        if (g_mp3_player->i2s_stream_writer) {
            audio_element_reset_state(g_mp3_player->i2s_stream_writer);
        }

        audio_pipeline_reset_ringbuffer(g_mp3_player->pipeline);
        audio_pipeline_reset_elements(g_mp3_player->pipeline);
    }

    return ret;
}

/**
 * @brief 内部函数:创建音频管道
 */
static esp_err_t mp3_player_create_pipeline(void)
{
    if (!g_mp3_player) {
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[3.0] 创建播放音频管道");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    // 增加管道缓冲区大小,减少音频卡顿
    pipeline_cfg.rb_size = 8192;  // 增大环形缓冲区
    g_mp3_player->pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!g_mp3_player->pipeline) {
        ESP_LOGE(TAG, "创建音频管道失败");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[3.1] 创建spiffs流从flash读取数据");
    spiffs_stream_cfg_t flash_cfg = SPIFFS_STREAM_CFG_DEFAULT();
    flash_cfg.type = AUDIO_STREAM_READER;
    // 增加SPIFFS流缓冲区大小,提高读取性能
    flash_cfg.out_rb_size = 8192;  // 增大输出缓冲区
    flash_cfg.task_stack = 4096;   // 增加任务栈大小
