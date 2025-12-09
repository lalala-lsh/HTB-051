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

    if (sensor_control_is_pir_dimmed()) {
        ESP_LOGI(TAG, "PIR调暗期间，音频更新后暂不恢复背景音乐");
        return;
    }

    if (bgm_path == NULL || bgm_path[0] == '\0') {
        ESP_LOGD(TAG, "音频更新前无背景音乐，更新后不主动启动");
        return;
    }

    if (strcmp(bgm_path, MUSIC) == 0 && !audio_queue_get_music_enabled()) {
        ESP_LOGD(TAG, "护眼音乐功能已禁用，音频更新后不恢复护眼背景音乐");
        return;
    }

    audio_queue_set_background_music(bgm_path, true);
    if (strcmp(bgm_path, MUSIC_40HZ) == 0) {
        audio_queue_play_loop_force(bgm_path, AUDIO_TYPE_MUSIC_CTRL, AUDIO_PRIORITY_LOW);
    } else {
        audio_queue_play_loop(bgm_path, AUDIO_TYPE_MUSIC_CTRL, AUDIO_PRIORITY_LOW);
    }
    ESP_LOGI(TAG, "音频更新完成，恢复被打断前背景音乐: %s", bgm_path);
}

// #region agent log
static void debug_log(const char *run_id, const char *hypothesis_id, const char *location,
                      const char *message, const char *data_json)
{
    FILE *fp = fopen(DEBUG_LOG_PATH, "a");
    if (fp == NULL) {
        return;
    }
    fprintf(fp,
            "{\"sessionId\":\"%s\",\"runId\":\"%s\",\"hypothesisId\":\"%s\",\"location\":\"%s\","
            "\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
            DEBUG_SESSION_ID, run_id, hypothesis_id, location, message,
            (data_json != NULL) ? data_json : "{}",
            (long long)(esp_timer_get_time() / 1000));
    fclose(fp);
}
// #endregion

static esp_err_t ensure_state_mutex(void)
{
    if (s_state_mutex != NULL) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "创建状态互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static bool set_update_running(bool running)
{
    if (ensure_state_mutex() != ESP_OK) {
        return false;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "获取状态锁超时");
        return false;
    }

    if (running && s_update_running) {
        xSemaphoreGive(s_state_mutex);
        return false;
    }

    s_update_running = running;
    xSemaphoreGive(s_state_mutex);
    return true;
}

bool audio_update_is_running(void)
{
    if (ensure_state_mutex() != ESP_OK) {
        return false;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    bool running = s_update_running;
    xSemaphoreGive(s_state_mutex);
    return running;
}

static bool is_hex_string_32(const char *md5)
{
    if (md5 == NULL || strlen(md5) != AUDIO_UPDATE_MD5_LEN) {
        return false;
    }

    for (int i = 0; i < AUDIO_UPDATE_MD5_LEN; ++i) {
        if (!isxdigit((int)md5[i])) {
            return false;
        }
    }
    return true;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + c - 'A';
    }
    return -1;
}

static bool url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t si = 0;
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_len == 0) {
        return false;
    }

    while (src[si] != '\0') {
        if (di + 1 >= dst_len) {
            return false;
        }

        if (src[si] == '%') {
            int hi = hex_to_int(src[si + 1]);
            int lo = hex_to_int(src[si + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            dst[di++] = (char)((hi << 4) | lo);
            si += 3;
            continue;
        }

        dst[di++] = src[si++];
    }

    dst[di] = '\0';
    return true;
}

static bool ends_with_mp3(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }

    return (tolower((unsigned char)name[len - 4]) == '.') &&
           (tolower((unsigned char)name[len - 3]) == 'm') &&
           (tolower((unsigned char)name[len - 2]) == 'p') &&
           (tolower((unsigned char)name[len - 1]) == '3');
}

static bool validate_file_name(const char *file_name)
{
    size_t i;
    size_t len;

    if (file_name == NULL) {
        return false;
    }

    len = strlen(file_name);
    if (len == 0 || len > AUDIO_UPDATE_MAX_FILE_LEN) {
        return false;
    }

    if (!ends_with_mp3(file_name)) {
        return false;
    }

    if (strstr(file_name, "..") != NULL) {
        return false;
    }

    for (i = 0; i < len; ++i) {
        char c = file_name[i];
        if (c == '/' || c == '\\') {
            return false;
        }
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }

    return true;
}

static bool parse_file_name_from_url(const char *url, char *file_name, size_t file_name_len)
{
    const char *scan_start;
    const char *path_start;
    const char *path_end;
    const char *base;
    size_t base_len;
    char encoded_name[AUDIO_UPDATE_MAX_FILE_LEN + 1];

    if (url == NULL || file_name == NULL || file_name_len == 0) {
        return false;
    }

    scan_start = strstr(url, "://");
    scan_start = (scan_start != NULL) ? (scan_start + 3) : url;
    path_start = strchr(scan_start, '/');
    if (path_start == NULL) {
        return false;
    }

    path_end = path_start;
    while (*path_end != '\0' && *path_end != '?' && *path_end != '#') {
        ++path_end;
    }

    if (path_end <= path_start + 1) {
        return false;
    }

    base = path_end - 1;
    while (base > path_start && *(base - 1) != '/') {
        --base;
    }

    base_len = (size_t)(path_end - base);
    if (base_len == 0 || base_len > AUDIO_UPDATE_MAX_FILE_LEN) {
        return false;
    }

    memcpy(encoded_name, base, base_len);
    encoded_name[base_len] = '\0';

    if (!url_decode(encoded_name, file_name, file_name_len)) {
        return false;
    }

    return validate_file_name(file_name);
}

static void md5_to_hex(const unsigned char *md5, char *output)
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        output[i * 2] = hex[(md5[i] >> 4) & 0x0F];
        output[i * 2 + 1] = hex[md5[i] & 0x0F];
    }
    output[32] = '\0';
}

static esp_err_t calculate_file_md5(const char *path, char *md5_out, size_t md5_out_len)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    size_t read_len;
    mbedtls_md_context_t ctx;
    unsigned char digest[16];
    const mbedtls_md_info_t *info = NULL;
    esp_err_t ret = ESP_FAIL;

    if (path == NULL || md5_out == NULL || md5_out_len < 33) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    buf = (uint8_t *)heap_caps_malloc(AUDIO_UPDATE_BUF_SIZE, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(fp);
        ESP_LOGE(TAG, "MD5缓冲区分配失败");
        return ESP_ERR_NO_MEM;
    }

    info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (info == NULL) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 0) != 0) {
        ret = ESP_FAIL;
        goto cleanup_md;
    }
    if (mbedtls_md_starts(&ctx) != 0) {
        ret = ESP_FAIL;
        goto cleanup_md_setup;
    }

    while ((read_len = fread(buf, 1, AUDIO_UPDATE_BUF_SIZE, fp)) > 0) {
        if (mbedtls_md_update(&ctx, buf, read_len) != 0) {
            ret = ESP_FAIL;
            goto cleanup_md_setup;
        }
    }

    if (ferror(fp)) {
        ret = ESP_FAIL;
        goto cleanup_md_setup;
    }

    if (mbedtls_md_finish(&ctx, digest) != 0) {
        ret = ESP_FAIL;
        goto cleanup_md_setup;
    }

    md5_to_hex(digest, md5_out);
    ret = ESP_OK;

cleanup_md_setup:
    mbedtls_md_free(&ctx);
cleanup_md:
cleanup:
    free(buf);
    fclose(fp);
    return ret;
}

static esp_err_t build_paths(const char *file_name, char *target_path, size_t target_len,
                             char *tmp_path, size_t tmp_len)
{
    int target_written;
    int tmp_written;

    target_written = snprintf(target_path, target_len, "/spiffs/%s", file_name);
    if (target_written <= 0 || (size_t)target_written >= target_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    tmp_written = snprintf(tmp_path, tmp_len, "/spiffs/%s%s%s", AUDIO_UPDATE_TMP_PREFIX, file_name,
                           AUDIO_UPDATE_TMP_SUFFIX);
    if (tmp_written <= 0 || (size_t)tmp_written >= tmp_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return (path != NULL && stat(path, &st) == 0);
}

static esp_err_t copy_file(const char *src, const char *dst)
{
    FILE *in = NULL;
    FILE *out = NULL;
    uint8_t *buf = NULL;
    size_t n;
    esp_err_t ret = ESP_FAIL;

    in = fopen(src, "rb");
    if (in == NULL) {
        return ESP_FAIL;
    }

    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return ESP_FAIL;
    }

    buf = (uint8_t *)heap_caps_malloc(AUDIO_UPDATE_BUF_SIZE, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(in);
        fclose(out);
        return ESP_ERR_NO_MEM;
    }

    while ((n = fread(buf, 1, AUDIO_UPDATE_BUF_SIZE, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            goto cleanup;
        }
    }

    if (ferror(in) || fflush(out) != 0) {
        goto cleanup;
    }

    ret = ESP_OK;

cleanup:
    free(buf);
    fclose(in);
    fclose(out);
    return ret;
}

static bool has_enough_space(size_t expected_size)
{
    size_t total = 0;
    size_t used = 0;
    size_t free_space;
    size_t required = expected_size + AUDIO_UPDATE_SPACE_MARGIN;

    if (esp_spiffs_info("audio", &total, &used) != ESP_OK) {
        ESP_LOGW(TAG, "获取SPIFFS空间信息失败，默认视为不足");
        return false;
    }

    free_space = (total > used) ? (total - used) : 0;
    ESP_LOGI(TAG, "SPIFFS容量检查: total=%u used=%u free=%u required=%u",
             (unsigned int)total, (unsigned int)used, (unsigned int)free_space,
             (unsigned int)required);
    return free_space >= required;
}

static bool is_music_file(const char *file_name)
{
    return (file_name != NULL && strcasecmp(file_name, "music.mp3") == 0);
}

static bool is_music_cleanup_done(void)
{
    settings_t *settings = settings_start(AUDIO_UPDATE_MIGRATION_NS, true);
    bool done = false;

    if (settings == NULL) {
        ESP_LOGW(TAG, "读取music一次性标记失败，默认按未执行处理");
        return false;
    }

    done = settings_get_bool(settings, AUDIO_UPDATE_MUSIC_CLEANUP_KEY, false);
    settings_end(settings);
    return done;
}

static void mark_music_cleanup_done(void)
{
    settings_t *settings = settings_start(AUDIO_UPDATE_MIGRATION_NS, true);

    if (settings == NULL) {
        ESP_LOGW(TAG, "写入music一次性标记失败");
        return;
    }

    settings_set_bool(settings, AUDIO_UPDATE_MUSIC_CLEANUP_KEY, true);
    settings_end(settings);
}

/**
 * @brief 一次性迁移：删除旧 music.mp3 释放空间（仅执行一次）
 * @note 仅做删除动作，不在此处写“已完成”标记。标记由下载并替换成功后再写入。
 */
static bool run_one_time_music_cleanup_if_needed(const char *target_path)
{
    if (target_path != NULL && file_exists(target_path)) {
        if (unlink(target_path) == 0) {
            ESP_LOGI(TAG, "一次性music清理：已删除旧music文件: %s", target_path);
            return true;
        }
        ESP_LOGW(TAG, "一次性music清理：删除旧文件失败: %s errno=%d", target_path, errno);
        return false;
    }
    ESP_LOGI(TAG, "一次性music清理：旧music文件不存在，视为已清理");
    return true;
}

static esp_err_t replace_target_file(const char *tmp_path, const char *target_path)
{
    char backup_path[180];
    int backup_written;
    int rename_err = 0;
    bool target_exists = false;
    bool tmp_exists = false;

    if (rename(tmp_path, target_path) == 0) {
        return ESP_OK;
    }
    rename_err = errno;
    target_exists = file_exists(target_path);
    tmp_exists = file_exists(tmp_path);

    if (!tmp_exists) {
        ESP_LOGE(TAG, "替换失败，临时文件不存在: %s", tmp_path);
        return ESP_FAIL;
    }

    /* 新增文件场景：目标不存在时直接复制落盘 */
    if (!target_exists) {
        if (copy_file(tmp_path, target_path) == ESP_OK) {
            unlink(tmp_path);
            ESP_LOGI(TAG, "rename失败已恢复(copy写入): target=%s errno=%d", target_path, rename_err);
            return ESP_OK;
        }
