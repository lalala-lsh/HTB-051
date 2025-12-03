#include "audio_update.h"

#include "audio_mode.h"
#include "audio_queue.h"
#include "light_control.h"
#include "sensor_control.h"
#include "mp3_player.h"
#include "settings.h"
#include "tts_list.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mbedtls/md.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "AUDIO_UPDATE";

#define AUDIO_UPDATE_MAX_URL_LEN      255
#define AUDIO_UPDATE_MAX_FILE_LEN     63
#define AUDIO_UPDATE_MD5_LEN          32
#define AUDIO_UPDATE_BUF_SIZE         4096
#define AUDIO_UPDATE_STACK_SIZE       6144
#define AUDIO_UPDATE_PRIORITY         5
#define AUDIO_UPDATE_SPACE_MARGIN     (16 * 1024)
#define AUDIO_UPDATE_MIN_FREE_HEAP    (40 * 1024)
#define AUDIO_UPDATE_WAIT_PROMPT_MS   30000
#define AUDIO_UPDATE_TMP_PREFIX       ".au_"
#define AUDIO_UPDATE_TMP_SUFFIX       ".tmp"
#define AUDIO_UPDATE_MIGRATION_NS     "device_params"
#define AUDIO_UPDATE_MUSIC_CLEANUP_KEY "mc_once_v1"
#define DEBUG_LOG_PATH                "/home/ubuntu/workspaces/esp-projects/htb-051/.cursor/debug-9ca4f3.log"
#define DEBUG_SESSION_ID              "9ca4f3"

typedef struct {
    char url[AUDIO_UPDATE_MAX_URL_LEN + 1];
    char file_name[AUDIO_UPDATE_MAX_FILE_LEN + 1];
    char md5[AUDIO_UPDATE_MD5_LEN + 1];
    size_t size;
} audio_update_item_t;

typedef struct {
    size_t item_count;
    audio_update_item_t *items;
} audio_update_ctx_t;

static SemaphoreHandle_t s_state_mutex = NULL;
static bool s_update_running = false;

static esp_err_t calculate_file_md5(const char *path, char *md5_out, size_t md5_out_len);
static esp_err_t build_paths(const char *file_name, char *target_path, size_t target_len,
                             char *tmp_path, size_t tmp_len);

static void wait_prompt_audio_before_update(void)
{
    ESP_LOGI(TAG, "等待已入队提示音播放完成，最多%dms", AUDIO_UPDATE_WAIT_PROMPT_MS);
    if (audio_queue_wait_for_prompts_idle(AUDIO_UPDATE_WAIT_PROMPT_MS) == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "等待提示音队列空闲超时，继续执行音频更新");
    }
}

static bool item_needs_update(const audio_update_item_t *item)
{
    char target_path[160];
    char tmp_path[160];
    char local_md5[33];

    if (build_paths(item->file_name, target_path, sizeof(target_path), tmp_path, sizeof(tmp_path)) != ESP_OK) {
        ESP_LOGW(TAG, "预检路径失败，跳过更新项: %s", item->file_name);
        return false;
    }

    if (calculate_file_md5(target_path, local_md5, sizeof(local_md5)) == ESP_OK &&
        strcasecmp(local_md5, item->md5) == 0) {
        ESP_LOGI(TAG, "本地MD5一致，预检跳过: %s", item->file_name);
        return false;
    }

    ESP_LOGI(TAG, "预检需要更新: %s", item->file_name);
    return true;
}

static size_t filter_items_needing_update(audio_update_ctx_t *ctx)
{
    size_t write_idx = 0;

    for (size_t read_idx = 0; read_idx < ctx->item_count; ++read_idx) {
        if (!item_needs_update(&ctx->items[read_idx])) {
            continue;
        }

        if (write_idx != read_idx) {
            ctx->items[write_idx] = ctx->items[read_idx];
        }
        write_idx++;
    }

    ctx->item_count = write_idx;
    return write_idx;
}

static void restore_interrupted_background_music(const char *bgm_path)
{
    if (audio_mode_is_a2dp()) {
        ESP_LOGD(TAG, "当前为A2DP模式，音频更新后不恢复本地背景音乐");
        return;
    }

