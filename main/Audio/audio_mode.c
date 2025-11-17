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
    if (g_audio_mode.initialized) {
        return ESP_OK;
    }

    /* 创建互斥锁 */
    g_audio_mode.mutex = xSemaphoreCreateMutex();
    if (g_audio_mode.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    g_audio_mode.current_mode = AUDIO_PLAYBACK_MODE_LOCAL;
    g_audio_mode.initialized = true;

    ESP_LOGI(TAG, "Audio mode manager initialized (mode=LOCAL)");
    return ESP_OK;
}

audio_playback_mode_t audio_mode_get_current(void)
{
    return g_audio_mode.current_mode;
}

esp_err_t audio_mode_switch_to_a2dp(void)
{
    if (!g_audio_mode.initialized) {
        /* 自动初始化 */
        esp_err_t ret = audio_mode_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (xSemaphoreTake(g_audio_mode.mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }

    if (g_audio_mode.current_mode == AUDIO_PLAYBACK_MODE_A2DP) {
        ESP_LOGW(TAG, "Already in A2DP mode");
        xSemaphoreGive(g_audio_mode.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Switching to A2DP mode...");

    /* 等待当前播放的语音提示完成，避免audio_queue仍在wait_for_finish时释放MP3播放器 */
    ESP_LOGI(TAG, "Waiting for current audio to finish...");
    esp_err_t wait_ret = audio_queue_wait_for_prompts_idle(10000);
    if (wait_ret != ESP_OK) {
        ESP_LOGW(TAG, "Timed out waiting for local prompt audio, stopping MP3 before A2DP switch");
        mp3_player_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 步骤1: 暂停 audio_queue (避免新的播放请求) */
    ESP_LOGI(TAG, "Step 1: Pausing audio queue");
    audio_queue_set_paused(true);

    /* 步骤2: 完全反初始化 MP3 播放器，释放 I2S 硬件资源 */
    /* 注意：mp3_player_stop() 只停止播放，不释放 I2S 资源 */
    /* 必须调用 mp3_player_deinit() 才能让 A2DP 使用 I2S */
    ESP_LOGI(TAG, "Step 2: Deinitializing MP3 player to release I2S");
    mp3_player_deinit();

    /* 等待资源完全释放 */
    vTaskDelay(pdMS_TO_TICKS(300));

    /* 步骤3: 初始化 A2DP Sink */
    ESP_LOGI(TAG, "Step 3: Initializing A2DP Sink");
    esp_err_t ret = a2dp_sink_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize A2DP Sink: %s", esp_err_to_name(ret));
        /* 恢复本地模式 */
        audio_queue_set_paused(false);
        xSemaphoreGive(g_audio_mode.mutex);
        return ret;
    }

    /* 步骤4: 注册A2DP事件回调(用于断开时自动切换回本地模式) */
    a2dp_sink_register_callback(audio_mode_a2dp_event_cb);

    /* 步骤5: 启动 A2DP Sink */
    ESP_LOGI(TAG, "Step 4: Starting A2DP Sink");
    ret = a2dp_sink_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start A2DP Sink: %s", esp_err_to_name(ret));
        a2dp_sink_deinit();
        audio_queue_set_paused(false);
        xSemaphoreGive(g_audio_mode.mutex);
        return ret;
    }

    g_audio_mode.current_mode = AUDIO_PLAYBACK_MODE_A2DP;

    ESP_LOGI(TAG, "=== A2DP Mode Active ===");
    ESP_LOGI(TAG, "Device is now discoverable as Bluetooth speaker");
    ESP_LOGI(TAG, "Connect from your phone to stream audio");

    xSemaphoreGive(g_audio_mode.mutex);
    return ESP_OK;
}

esp_err_t audio_mode_switch_to_local(void)
{
    if (!g_audio_mode.initialized) {
        return ESP_OK;
    }

    if (xSemaphoreTake(g_audio_mode.mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }

    if (g_audio_mode.current_mode == AUDIO_PLAYBACK_MODE_LOCAL) {
        ESP_LOGW(TAG, "Already in local mode");
        xSemaphoreGive(g_audio_mode.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Switching to local mode...");

    /* 步骤1: 停止并反初始化 A2DP Sink */
    ESP_LOGI(TAG, "Step 1: Stopping and deinitializing A2DP Sink");
    a2dp_sink_stop();
    a2dp_sink_deinit();

    /* 等待A2DP完全停止 */
    vTaskDelay(pdMS_TO_TICKS(300));

    /* 步骤2: 重新初始化 MP3 播放器 */
    ESP_LOGI(TAG, "Step 2: Reinitializing MP3 player");
    esp_err_t mp3_ret = mp3_player_init();
    if (mp3_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize MP3 player: %s", esp_err_to_name(mp3_ret));
        /* 继续，因为 audio_queue 可能会在后续触发重试 */
    }

    /* 步骤3: 恢复 audio_queue */
    ESP_LOGI(TAG, "Step 3: Resuming audio queue");
    audio_queue_set_paused(false);

    g_audio_mode.current_mode = AUDIO_PLAYBACK_MODE_LOCAL;

    ESP_LOGI(TAG, "=== Local Mode Active ===");
    ESP_LOGI(TAG, "Local MP3 playback restored");

    xSemaphoreGive(g_audio_mode.mutex);
    return ESP_OK;
}

bool audio_mode_is_a2dp(void)
{
    return g_audio_mode.current_mode == AUDIO_PLAYBACK_MODE_A2DP;
}

bool audio_mode_is_local(void)
{
    return g_audio_mode.current_mode == AUDIO_PLAYBACK_MODE_LOCAL;
}
