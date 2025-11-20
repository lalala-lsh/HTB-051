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
