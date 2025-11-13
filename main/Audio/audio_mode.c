/**
 * @file audio_mode.c
 * @brief 音频模式管理器实现
 *
 * 协调本地MP3播放和A2DP蓝牙音频之间的切换
 */

#include "audio_mode.h"
#include "a2dp_sink.h"
#include "mp3_player.h"
#include "audio_queue.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "AUDIO_MODE";

/* 音频模式管理器状态 */
static struct {
    audio_playback_mode_t current_mode;
    SemaphoreHandle_t mutex;
    bool initialized;
} g_audio_mode = {
    .current_mode = AUDIO_PLAYBACK_MODE_LOCAL,
    .mutex = NULL,
    .initialized = false,
};

/**
 * @brief 延迟切换到本地模式的定时器回调（在timer daemon任务上下文执行）
 */
static void deferred_switch_to_local_cb(TimerHandle_t xTimer)
{
    audio_mode_switch_to_local();
    xTimerDelete(xTimer, 0);
}

/**
 * @brief A2DP事件回调 - 用于检测断开连接并自动切换回本地模式
 *
 * 此回调在BTC任务上下文中运行，不能直接执行BT栈操作（会死锁），
 * 必须通过定时器延迟到独立的任务上下文中执行。
 */
static void audio_mode_a2dp_event_cb(a2dp_sink_event_t event, void *param)
{
    switch (event) {
        case A2DP_SINK_EVENT_CONNECTED:
            ESP_LOGI(TAG, "A2DP connected - Bluetooth speaker mode active");
            break;

        case A2DP_SINK_EVENT_DISCONNECTED: {
            ESP_LOGI(TAG, "A2DP disconnected - deferring switch to local mode");
            TimerHandle_t timer = xTimerCreate("a2dp_sw", pdMS_TO_TICKS(100),
                                                pdFALSE, NULL,
                                                deferred_switch_to_local_cb);
            if (timer) {
                xTimerStart(timer, 0);
            } else {
                ESP_LOGE(TAG, "Failed to create deferred switch timer");
            }
            break;
        }

        case A2DP_SINK_EVENT_AUDIO_START:
            ESP_LOGD(TAG, "A2DP audio playback started");
            break;

        case A2DP_SINK_EVENT_AUDIO_STOP:
            ESP_LOGD(TAG, "A2DP audio playback stopped");
            break;

        default:
            break;
    }
}

esp_err_t audio_mode_init(void)
{
