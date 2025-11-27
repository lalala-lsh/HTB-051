/**
 * @file audio_queue.c
 * @brief 音频队列管理器实现
 * @author GYJ
 * @date 2025
 */

#include "audio_queue.h"
#include "mp3_player.h"
#include "tts_list.h"
#include "device_params.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include <string.h>

static const char *TAG = "AUDIO_QUEUE";

/* 队列配置 */
#define AUDIO_QUEUE_SIZE        10      // 音频队列大小
#define AUDIO_TASK_STACK_SIZE   4096    // 音频处理任务堆栈大小
#define AUDIO_TASK_PRIORITY     10       // 音频处理任务优先级

/* 全局变量 */
static QueueHandle_t audio_queue = NULL;           // 音频队列
static TaskHandle_t audio_task_handle = NULL;      // 音频处理任务句柄
static SemaphoreHandle_t audio_mutex = NULL;       // 音频互斥锁
static bool audio_queue_initialized = false;       // 初始化标志
static bool audio_queue_paused = false;            // 暂停标志

/* 音乐/语音启用控制 */
static bool music_enabled = true;                  // 音乐功能启用标志
static bool voice_enabled = true;                  // 语音功能启用标志

/* 音乐恢复相关变量 */
static bool background_music_was_playing = false;  // 背景音乐是否在播放
static char background_music_file[128] = {0};      // 背景音乐文件路径
static bool deferred_background_valid = false;     // 有提示音时延后播放的背景音乐
static char deferred_background_file[128] = {0};
static audio_type_t deferred_background_type = AUDIO_TYPE_MUSIC_CTRL;
static uint8_t deferred_background_priority = AUDIO_PRIORITY_LOW;

/* 防抖记录 */
static uint32_t last_play_time[AUDIO_TYPE_MAX] = {0};
static char last_play_file[AUDIO_TYPE_MAX][128] = {{0}};
static uint32_t prompt_work_count = 0;             // 已入队或正在播放的一次性提示音数量

/* 防抖时间映射 */
static const uint32_t debounce_time_map[AUDIO_TYPE_MAX] = {
    AUDIO_DEBOUNCE_DEFAULT_MS,      // AUDIO_TYPE_SYSTEM
    AUDIO_DEBOUNCE_TIMER_MS,        // AUDIO_TYPE_TIMER
    AUDIO_DEBOUNCE_POWER_MS,        // AUDIO_TYPE_POWER
    AUDIO_DEBOUNCE_MUSIC_CTRL_MS,   // AUDIO_TYPE_MUSIC_CTRL
    AUDIO_DEBOUNCE_DEFAULT_MS,      // AUDIO_TYPE_OTA
    AUDIO_DEBOUNCE_DEFAULT_MS       // AUDIO_TYPE_FUNCTION
};

/* 内部音频请求结构体(用于队列) */
typedef struct {
    char file_path[128];            // 文件路径(复制)
    audio_type_t type;              // 音频类型
    uint8_t priority;               // 优先级
    audio_play_mode_t mode;         // 播放模式
    bool need_resume_music;         // 是否需要恢复背景音乐
} internal_audio_request_t;

/**
 * @brief 获取当前时间(毫秒)
 * @return uint32_t 当前时间
 */
static uint32_t get_current_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool audio_file_exists(const char *path)
{
    struct stat st;
    return (path != NULL && stat(path, &st) == 0);
}

static bool audio_queue_lock(TickType_t timeout_ticks)
{
    if (audio_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(audio_mutex, timeout_ticks) == pdTRUE;
}

static void audio_queue_unlock(void)
{
    if (audio_mutex != NULL) {
        xSemaphoreGive(audio_mutex);
    }
}

static bool is_prompt_request(const internal_audio_request_t *request)
{
    return request != NULL &&
           request->mode == AUDIO_PLAY_MODE_ONCE &&
           request->type != AUDIO_TYPE_MUSIC_CTRL;
}

static void clear_deferred_background_unsafe(void)
{
    deferred_background_valid = false;
    memset(deferred_background_file, 0, sizeof(deferred_background_file));
    deferred_background_type = AUDIO_TYPE_MUSIC_CTRL;
    deferred_background_priority = AUDIO_PRIORITY_LOW;
}

static void enqueue_deferred_background_if_ready_unsafe(void)
{
    if (!deferred_background_valid || prompt_work_count != 0 || audio_queue_paused) {
        return;
    }

    internal_audio_request_t request = {
        .type = deferred_background_type,
        .priority = deferred_background_priority,
        .mode = AUDIO_PLAY_MODE_LOOP,
        .need_resume_music = false
    };

    strncpy(request.file_path, deferred_background_file, sizeof(request.file_path) - 1);
    request.file_path[sizeof(request.file_path) - 1] = '\0';
    clear_deferred_background_unsafe();

    if (xQueueSend(audio_queue, &request, 0) != pdTRUE) {
        ESP_LOGW(TAG, "音频队列已满,丢弃延后背景音乐请求: %s", request.file_path);
    } else {
        ESP_LOGD(TAG, "延后背景音乐请求已加入队列: %s", request.file_path);
    }
}

static void finish_prompt_request_unsafe(void)
{
    if (prompt_work_count > 0) {
        prompt_work_count--;
    }
    enqueue_deferred_background_if_ready_unsafe();
}

/**
 * @brief 检查音频是否在防抖期内
 * @param type 音频类型
 * @return true 在防抖期内,false 不在防抖期内
 */
static bool is_audio_debouncing(audio_type_t type, const char *file_path)
{
    if (type >= AUDIO_TYPE_MAX) {
        return false;
    }
    if (file_path == NULL || strcmp(last_play_file[type], file_path) != 0) {
        return false;
    }

    uint32_t current_time = get_current_time_ms();
    uint32_t elapsed = current_time - last_play_time[type];

    return elapsed < debounce_time_map[type];
}

/**
 * @brief 更新音频类型的最后播放时间
 * @param type 音频类型
 */
static void update_last_play_time(audio_type_t type, const char *file_path)
{
    if (type < AUDIO_TYPE_MAX) {
        last_play_time[type] = get_current_time_ms();
        if (file_path != NULL) {
            strncpy(last_play_file[type], file_path, sizeof(last_play_file[type]) - 1);
            last_play_file[type][sizeof(last_play_file[type]) - 1] = '\0';
        }
    }
}

/**
 * @brief 音频处理任务
 * @param pvParameters 任务参数
 */
static void audio_task(void *pvParameters)
{
    internal_audio_request_t request;

    ESP_LOGI(TAG, "音频队列处理任务启动");

    while (1) {
        // 从队列接收音频请求
        if (xQueueReceive(audio_queue, &request, portMAX_DELAY)) {
            // 检查是否暂停
            if (audio_queue_paused) {
                ESP_LOGD(TAG, "音频队列已暂停,跳过播放: %s", request.file_path);
                if (is_prompt_request(&request) && audio_queue_lock(pdMS_TO_TICKS(1000))) {
                    finish_prompt_request_unsafe();
                    audio_queue_unlock();
                }
                continue;
            }

            // 获取互斥锁
            if (audio_queue_lock(pdMS_TO_TICKS(1000))) {
                ESP_LOGI(TAG, "播放音频: %s, 类型: %d, 优先级: %d, 模式: %d",
                         request.file_path, request.type, request.priority, request.mode);

                esp_err_t ret = ESP_OK;

                if (request.mode == AUDIO_PLAY_MODE_LOOP) {
                    if (!audio_file_exists(request.file_path)) {
                        ESP_LOGW(TAG, "循环播放文件不存在，跳过: %s", request.file_path);
                        background_music_was_playing = false;
                        memset(background_music_file, 0, sizeof(background_music_file));
                        audio_queue_unlock();
                        continue;
                    }
                    // 循环播放模式 - 保存为背景音乐
                    strncpy(background_music_file, request.file_path, sizeof(background_music_file) - 1);
                    background_music_file[sizeof(background_music_file) - 1] = '\0';
                    background_music_was_playing = true;
                    clear_deferred_background_unsafe();

                    ret = mp3_player_play_loop(request.file_path);
                } else {
                    // 单次播放模式 - 检查是否需要中断背景音乐
                    mp3_player_state_t current_state = mp3_player_get_state();
                    if (current_state == MP3_PLAYER_STATE_PLAYING && background_music_was_playing) {
                        ESP_LOGD(TAG, "检测到背景音乐正在播放,准备中断并稍后恢复");
                    }

                    ret = mp3_player_play(request.file_path);

                    if (ret == ESP_OK) {
                        // 等待播放完成
                        esp_err_t wait_ret = mp3_player_wait_for_finish(10000);

                        if (wait_ret == ESP_ERR_TIMEOUT) {
                            ESP_LOGW(TAG, "等待播放完成超时,强制停止播放: %s", request.file_path);
                            background_music_was_playing = false;
                        } else if (wait_ret != ESP_OK) {
                            ESP_LOGE(TAG, "播放过程中发生错误,停止播放: %s", request.file_path);
                            mp3_player_stop();
                            background_music_was_playing = false;
                        } else if (request.need_resume_music && background_music_was_playing) {
                            char resume_file[sizeof(background_music_file)] = {0};
                            strncpy(resume_file, background_music_file, sizeof(resume_file) - 1);
                            resume_file[sizeof(resume_file) - 1] = '\0';
                            ESP_LOGD(TAG, "播放完成,恢复背景音乐: %s", resume_file);

                            if (prompt_work_count > 1) {
                                ESP_LOGD(TAG, "仍有提示音等待播放,延后恢复背景音乐: %s", resume_file);
                            } else if (resume_file[0] != '\0' && audio_file_exists(resume_file)) {
                                // 增加延时时间,确保 I2S 完全释放资源后再恢复音乐(从100ms增加到300ms)
                                vTaskDelay(pdMS_TO_TICKS(300));
                                if (audio_queue_paused) {
                                    ESP_LOGI(TAG, "音频队列已暂停,跳过本次背景音乐恢复: %s", resume_file);
                                    ret = ESP_OK;
                                } else {
                                    ret = mp3_player_play_loop(resume_file);
                                }
                                if (ret == ESP_OK) {
                                    if (!audio_queue_paused) {
                                        ESP_LOGI(TAG, "背景音乐已恢复播放: %s", resume_file);
                                    }
                                } else {
                                    ESP_LOGE(TAG, "恢复背景音乐失败: %s", esp_err_to_name(ret));
                                    background_music_was_playing = false;
                                }
                            } else {
                                ESP_LOGD(TAG, "背景音乐文件不存在或路径为空,不恢复播放");
                                background_music_was_playing = false;
                            }
                        }

                        if (wait_ret != ESP_OK) {
                            ret = wait_ret;
                        }
                    }

                    if (is_prompt_request(&request)) {
                        finish_prompt_request_unsafe();
                    }
                }

                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "播放音频失败: %s, 错误: %s", request.file_path, esp_err_to_name(ret));
                }

                // 更新防抖时间
                update_last_play_time(request.type, request.file_path);

                // 释放互斥锁
                audio_queue_unlock();
            } else {
                ESP_LOGW(TAG, "获取音频互斥锁超时,跳过播放: %s", request.file_path);
            }
        }
    }
}

/**
 * @brief 初始化音频队列管理器
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_init(void)
{
    if (audio_queue_initialized) {
        ESP_LOGW(TAG, "音频队列已初始化");
        return ESP_OK;
    }

    // 创建音频队列
    audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(internal_audio_request_t));
    if (audio_queue == NULL) {
        ESP_LOGE(TAG, "创建音频队列失败");
        return ESP_FAIL;
    }

    // 创建互斥锁
    audio_mutex = xSemaphoreCreateMutex();
    if (audio_mutex == NULL) {
        ESP_LOGE(TAG, "创建音频互斥锁失败");
        vQueueDelete(audio_queue);
        return ESP_FAIL;
    }

    // 创建音频处理任务
    BaseType_t ret = xTaskCreate(
        audio_task,
        "audio_queue_task",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        AUDIO_TASK_PRIORITY,
        &audio_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建音频处理任务失败");
        vSemaphoreDelete(audio_mutex);
        vQueueDelete(audio_queue);
        return ESP_FAIL;
    }

    // 初始化防抖记录
    memset(last_play_time, 0, sizeof(last_play_time));
    memset(last_play_file, 0, sizeof(last_play_file));

    // 初始化背景音乐状态
    background_music_was_playing = false;
    memset(background_music_file, 0, sizeof(background_music_file));
    clear_deferred_background_unsafe();
    prompt_work_count = 0;

    // 从NVS读取音乐/语音启用状态
    music_enabled = (device_params_get_music_state() == 1);
    voice_enabled = (device_params_get_voice_state() == 1);

    audio_queue_initialized = true;
    audio_queue_paused = false;

    ESP_LOGI(TAG, "音频队列管理器初始化成功(music=%d, voice=%d)", music_enabled, voice_enabled);
    return ESP_OK;
}

/**
 * @brief 反初始化音频队列管理器
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_deinit(void)
{
    if (!audio_queue_initialized) {
        return ESP_OK;
    }

    // 删除任务
    if (audio_task_handle != NULL) {
        vTaskDelete(audio_task_handle);
        audio_task_handle = NULL;
    }

    // 删除互斥锁
    if (audio_mutex != NULL) {
        vSemaphoreDelete(audio_mutex);
        audio_mutex = NULL;
    }

    // 删除队列
    if (audio_queue != NULL) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
    }

    audio_queue_initialized = false;

    ESP_LOGI(TAG, "音频队列管理器已反初始化");
    return ESP_OK;
}

/**
 * @brief 请求播放音频(带防抖)
 * @param file_path 音频文件路径
 * @param type 音频类型
 * @param priority 优先级(0-3,0最高)
 * @param need_resume_music 播放完成后是否恢复背景音乐
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_play(const char* file_path, audio_type_t type, uint8_t priority, bool need_resume_music)
{
    if (!audio_queue_initialized || file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 检查语音功能是否启用(非音乐类型的音频受voice_enabled控制)
    if (type != AUDIO_TYPE_MUSIC_CTRL && !voice_enabled) {
        ESP_LOGD(TAG, "语音功能已禁用,跳过播放: %s", file_path);
        return ESP_OK;
    }

    // 检查防抖
    if (is_audio_debouncing(type, file_path)) {
        ESP_LOGD(TAG, "音频类型 %d 文件 %s 在防抖期内,跳过播放", type, file_path);
        return ESP_OK; // 防抖期内不是错误,返回成功
    }

    // 构建内部请求
    internal_audio_request_t request = {
        .type = type,
        .priority = priority,
        .mode = AUDIO_PLAY_MODE_ONCE,
        .need_resume_music = need_resume_music
    };

    // 复制文件路径
    strncpy(request.file_path, file_path, sizeof(request.file_path) - 1);
    request.file_path[sizeof(request.file_path) - 1] = '\0';

    bool is_prompt = is_prompt_request(&request);
    if (is_prompt && !audio_queue_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "提示音入队时获取互斥锁超时: %s", file_path);
        return ESP_ERR_TIMEOUT;
    }
    if (is_prompt) {
        prompt_work_count++;
    }

    // 发送到队列
    if (xQueueSend(audio_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (is_prompt) {
            finish_prompt_request_unsafe();
            audio_queue_unlock();
        }
        ESP_LOGW(TAG, "音频队列已满,丢弃请求: %s", file_path);
        return ESP_ERR_NO_MEM;
    }

    if (is_prompt) {
        audio_queue_unlock();
    }

    ESP_LOGD(TAG, "音频请求已加入队列: %s, 类型: %d, 优先级: %d", file_path, type, priority);
    return ESP_OK;
}

/**
 * @brief 请求播放音频循环(背景音乐)
 * @param file_path 音频文件路径
 * @param type 音频类型
 * @param priority 优先级
 * @return esp_err_t 错误码
 */
static esp_err_t audio_queue_play_loop_internal(const char* file_path, audio_type_t type,
                                                uint8_t priority, bool respect_music_enabled)
{
    if (!audio_queue_initialized || file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 检查音乐功能是否启用
    if (respect_music_enabled && !music_enabled) {
        ESP_LOGD(TAG, "音乐功能已禁用,跳过循环播放: %s", file_path);
        return ESP_OK;
    }

    // 构建内部请求
    internal_audio_request_t request = {
        .type = type,
        .priority = priority,
        .mode = AUDIO_PLAY_MODE_LOOP,
        .need_resume_music = false
    };

    // 复制文件路径
    strncpy(request.file_path, file_path, sizeof(request.file_path) - 1);
    request.file_path[sizeof(request.file_path) - 1] = '\0';

    if (!audio_queue_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "请求循环播放时获取互斥锁超时: %s", file_path);
        return ESP_ERR_TIMEOUT;
    }

    strncpy(background_music_file, file_path, sizeof(background_music_file) - 1);
    background_music_file[sizeof(background_music_file) - 1] = '\0';
    background_music_was_playing = true;

    if (prompt_work_count > 0) {
        strncpy(deferred_background_file, file_path, sizeof(deferred_background_file) - 1);
        deferred_background_file[sizeof(deferred_background_file) - 1] = '\0';
        deferred_background_type = type;
        deferred_background_priority = priority;
        deferred_background_valid = true;
        audio_queue_unlock();
        ESP_LOGD(TAG, "提示音未处理完,延后背景音乐请求: %s", file_path);
        return ESP_OK;
    }

    xQueueReset(audio_queue);
    audio_queue_unlock();

    // 背景音乐不清空队列,避免吞掉尚未播放的提示音
    mp3_player_state_t current_state = mp3_player_get_state();
    if (current_state == MP3_PLAYER_STATE_PLAYING) {
        ESP_LOGD(TAG, "当前有音频正在播放,准备切换到循环播放");
    }

    if (xQueueSend(audio_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "音频队列已满,丢弃循环播放请求: %s", file_path);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "音频循环播放请求已加入队列: %s, 类型: %d, 优先级: %d", file_path, type, priority);
    return ESP_OK;
}

esp_err_t audio_queue_play_loop(const char* file_path, audio_type_t type, uint8_t priority)
{
    return audio_queue_play_loop_internal(file_path, type, priority, true);
}

esp_err_t audio_queue_play_loop_force(const char* file_path, audio_type_t type, uint8_t priority)
{
    return audio_queue_play_loop_internal(file_path, type, priority, false);
}

/**
 * @brief 停止音频播放
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_stop(void)
{
    if (!audio_queue_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_queue_lock(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "停止音频时获取互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }

    // 清空队列
    xQueueReset(audio_queue);

    // 停止当前播放并重置背景音乐状态
    background_music_was_playing = false;
    memset(background_music_file, 0, sizeof(background_music_file));
    clear_deferred_background_unsafe();
    prompt_work_count = 0;

    esp_err_t ret = mp3_player_stop();
    audio_queue_unlock();
    return ret;
}

/**
 * @brief 检查指定类型音频是否在防抖期内
 * @param type 音频类型
 * @return true 在防抖期内,false 不在防抖期内
 */
bool audio_queue_is_debouncing(audio_type_t type)
{
    if (type >= AUDIO_TYPE_MAX) {
        return false;
    }

    uint32_t current_time = get_current_time_ms();
    uint32_t elapsed = current_time - last_play_time[type];
    return elapsed < debounce_time_map[type];
}

/**
 * @brief 清空音频队列
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_clear(void)
{
    if (!audio_queue_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_queue_lock(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "清空音频队列时获取互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }

    xQueueReset(audio_queue);
    background_music_was_playing = false;
    memset(background_music_file, 0, sizeof(background_music_file));
    clear_deferred_background_unsafe();
    prompt_work_count = 0;
    audio_queue_unlock();
    ESP_LOGI(TAG, "音频队列已清空");
    return ESP_OK;
}

/**
 * @brief 设置音频队列暂停状态
 * @param paused true暂停,false恢复
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_set_paused(bool paused)
{
    audio_queue_paused = paused;
    ESP_LOGI(TAG, "音频队列 %s", paused ? "已暂停" : "已恢复");
    return ESP_OK;
}

esp_err_t audio_queue_wait_for_prompts_idle(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    while (1) {
        bool idle = false;

        if (audio_queue_lock(pdMS_TO_TICKS(100))) {
            idle = (prompt_work_count == 0);
            audio_queue_unlock();
        }

        if (idle) {
            return ESP_OK;
        }

        if (timeout_ms > 0 && (xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief 设置背景音乐状态
 * @param file_path 背景音乐文件路径,NULL表示停止
 * @param is_playing 是否正在播放
 * @return esp_err_t 错误码
 */
esp_err_t audio_queue_set_background_music(const char* file_path, bool is_playing)
{
    if (!audio_queue_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_queue_lock(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "设置背景音乐状态时获取互斥锁超时");
