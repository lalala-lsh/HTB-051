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
    flash_cfg.task_prio = 8;       // 提高读取任务优先级
    g_mp3_player->spiffs_stream_reader = spiffs_stream_init(&flash_cfg);
    if (!g_mp3_player->spiffs_stream_reader) {
        ESP_LOGE(TAG, "创建spiffs流失败");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[3.2] 创建i2s流向编解码芯片写入数据");
#if defined CONFIG_ESP32_C3_LYRA_V2_BOARD
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_PDM_TX_CFG_DEFAULT();
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
#endif
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    // 优化I2S流配置,减少音频卡顿
    i2s_cfg.task_stack = 4096;               // 增加任务栈大小
    i2s_cfg.task_prio = 9;                   // 提高I2S任务优先级
    i2s_cfg.out_rb_size = 8192;              // 增大输出缓冲区
    g_mp3_player->i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    if (!g_mp3_player->i2s_stream_writer) {
        ESP_LOGE(TAG, "创建i2s流失败");
        return ESP_FAIL;
    }

    // 设置默认音频参数:16000Hz, 1声道, 16位
    // 这是必需的,否则 I2S 启动时会产生杂音
    ESP_LOGD(TAG, "[3.2.1] 设置默认i2s参数: 16000Hz, 16位, 1声道");
    esp_err_t i2s_ret = i2s_stream_set_clk(g_mp3_player->i2s_stream_writer, 16000, 16, 1);
    if (i2s_ret != ESP_OK) {
        ESP_LOGE(TAG, "默认i2s参数设置失败");
        return ESP_FAIL;
    }
    cur_i2s_rate = 16000;
    cur_i2s_bits = 16;
    cur_i2s_ch   = 1;

    ESP_LOGD(TAG, "[3.3] 创建mp3解码器解码mp3文件");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    // 优化MP3解码器配置
    mp3_cfg.task_stack = 4096;    // 增加任务栈大小
    mp3_cfg.task_prio = 8;        // 提高解码任务优先级
    mp3_cfg.out_rb_size = 8192;   // 增大输出缓冲区
    mp3_cfg.task_core = 1;
    g_mp3_player->mp3_decoder = mp3_decoder_init(&mp3_cfg);
    if (!g_mp3_player->mp3_decoder) {
        ESP_LOGE(TAG, "创建mp3解码器失败");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[3.4] 注册所有元素到音频管道");
    audio_pipeline_register(g_mp3_player->pipeline, g_mp3_player->spiffs_stream_reader, "spiffs");
    audio_pipeline_register(g_mp3_player->pipeline, g_mp3_player->mp3_decoder, "mp3");
    audio_pipeline_register(g_mp3_player->pipeline, g_mp3_player->i2s_stream_writer, "i2s");

    ESP_LOGD(TAG, "[3.5] 链接元素 [flash]-->spiffs-->mp3解码器-->i2s流-->[编解码芯片]");
    const char *link_tag[3] = {"spiffs", "mp3", "i2s"};
    audio_pipeline_link(g_mp3_player->pipeline, &link_tag[0], 3);

    return ESP_OK;
}

/**
 * @brief 内部函数:创建事件监听器
 */
static esp_err_t mp3_player_create_event_listener(void)
{
    if (!g_mp3_player) {
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[ 4 ] 设置事件监听器");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    g_mp3_player->evt = audio_event_iface_init(&evt_cfg);
    if (!g_mp3_player->evt) {
        ESP_LOGE(TAG, "创建事件接口失败");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[4.1] 监听管道中所有元素的事件");
    audio_pipeline_set_listener(g_mp3_player->pipeline, g_mp3_player->evt);

    ESP_LOGD(TAG, "[4.2] 监听外设事件");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(g_mp3_player->periph_set), g_mp3_player->evt);

    return ESP_OK;
}

/**
 * @brief 内部函数:销毁音频管道
 */
static void mp3_player_destroy_pipeline(void)
{
    if (!g_mp3_player) {
        return;
    }

    if (g_mp3_player->pipeline) {
        audio_pipeline_stop(g_mp3_player->pipeline);
        audio_pipeline_wait_for_stop(g_mp3_player->pipeline);
        audio_pipeline_terminate(g_mp3_player->pipeline);

        if (g_mp3_player->spiffs_stream_reader) {
            audio_pipeline_unregister(g_mp3_player->pipeline, g_mp3_player->spiffs_stream_reader);
        }
        if (g_mp3_player->i2s_stream_writer) {
            audio_pipeline_unregister(g_mp3_player->pipeline, g_mp3_player->i2s_stream_writer);
        }
        if (g_mp3_player->mp3_decoder) {
            audio_pipeline_unregister(g_mp3_player->pipeline, g_mp3_player->mp3_decoder);
        }

        if (g_mp3_player->evt) {
            audio_pipeline_remove_listener(g_mp3_player->pipeline);
        }

        audio_pipeline_deinit(g_mp3_player->pipeline);
        g_mp3_player->pipeline = NULL;
    }

    if (g_mp3_player->spiffs_stream_reader) {
        audio_element_deinit(g_mp3_player->spiffs_stream_reader);
        g_mp3_player->spiffs_stream_reader = NULL;
    }

    if (g_mp3_player->i2s_stream_writer) {
        audio_element_deinit(g_mp3_player->i2s_stream_writer);
        g_mp3_player->i2s_stream_writer = NULL;
    }

    if (g_mp3_player->mp3_decoder) {
        audio_element_deinit(g_mp3_player->mp3_decoder);
        g_mp3_player->mp3_decoder = NULL;
    }
}

/**
 * @brief 内部函数:处理音频管道事件
 */
static void mp3_player_handle_audio_events(void)
{
    if (!mp3_player_lock()) {
        return;
    }

    if (!mp3_player_is_enabled_unsafe()) {
        mp3_player_unlock();
        return;
    }

    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(g_mp3_player->evt, &msg, 0);

    if (ret != ESP_OK) {
        mp3_player_unlock();
        return;  // 没有事件
    }

    // 处理音乐信息事件 - 动态设置采样率
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)g_mp3_player->mp3_decoder
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t music_info = {0};
        audio_element_getinfo(g_mp3_player->mp3_decoder, &music_info);

        ESP_LOGD(TAG, "音乐信息,采样率=%d,声道=%d,位深=%d",
                 music_info.sample_rates, music_info.channels, music_info.bits);

        if (music_info.sample_rates <= 0 || music_info.bits <= 0 || music_info.channels <= 0) {
            ESP_LOGE(TAG, "MP3音频参数无效,停止当前播放");
            mp3_player_stop_pipeline_locked(true);
            g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
            mp3_player_unlock();
            return;
        }

        // I2S时钟在启动播放前配置。运行中重配可能触发DMA重新分配失败并崩溃。
        audio_element_state_t i2s_state = audio_element_get_state(g_mp3_player->i2s_stream_writer);
        if (i2s_state != AEL_STATE_RUNNING) {
            ESP_LOGW(TAG, "I2S流未运行(状态=%d),跳过音频参数检查", i2s_state);
        } else if (music_info.sample_rates == cur_i2s_rate &&
                   music_info.bits == cur_i2s_bits &&
                   music_info.channels == cur_i2s_ch) {
            // #region agent log (H1d)
            // ESP_LOGI(TAG, "[DBG] I2S参数未变(rate=%d,bits=%d,ch=%d),跳过i2s_stream_set_clk",
            //          cur_i2s_rate, cur_i2s_bits, cur_i2s_ch);
            // #endregion
        } else {
            ESP_LOGE(TAG, "I2S参数不匹配,停止当前播放以避免运行中重配: current=%d/%d/%d, file=%d/%d/%d",
                     cur_i2s_rate, cur_i2s_bits, cur_i2s_ch,
                     music_info.sample_rates, music_info.bits, music_info.channels);
            mp3_player_stop_pipeline_locked(true);
            g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
            mp3_player_unlock();
            return;
        }
    }
    // 处理播放结束事件
    else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
        intptr_t status = (intptr_t)msg.data;

        if (status == AEL_STATUS_STATE_STOPPED || status == AEL_STATUS_STATE_FINISHED) {
            if (msg.source == (void *)g_mp3_player->i2s_stream_writer) {
                ESP_LOGD(TAG, "播放完成");

                //让所有元素脱离阻塞读写
                audio_element_set_ringbuf_done(g_mp3_player->mp3_decoder);
                audio_element_set_ringbuf_done(g_mp3_player->i2s_stream_writer);
                audio_element_set_ringbuf_done(g_mp3_player->spiffs_stream_reader);

                if (g_mp3_player->mode == MP3_PLAYER_MODE_LOOP && g_mp3_player->state == MP3_PLAYER_STATE_PLAYING) {
                    ESP_LOGD(TAG, "循环模式,标记延时重启");

                    // 播放完成即刻静音,防止I2S停止后DMA残留噪音
                    if (g_mp3_player->board_handle && g_mp3_player->board_handle->audio_hal) {
                        audio_hal_set_volume(g_mp3_player->board_handle->audio_hal, 0);
                    }

                    loop_restart_pending = true;
                    loop_restart_time = xTaskGetTickCount() + pdMS_TO_TICKS(50);
                } else if (sequence_play_pending) {
                    ESP_LOGD(TAG, "序列播放,准备播放第二个文件");

                    // 在锁保护下处理序列播放逻辑
                    sequence_play_pending = false;
                    if (sequence_second_file[0] != '\0') {
                        ESP_LOGD(TAG, "标记延时播放第二个文件: %s", sequence_second_file);
                        // 设置循环播放标志,但使用序列播放的第二个文件
                        strncpy(g_mp3_player->current_file, sequence_second_file, sizeof(g_mp3_player->current_file) - 1);
                        g_mp3_player->current_file[sizeof(g_mp3_player->current_file) - 1] = '\0';
                        g_mp3_player->mode = MP3_PLAYER_MODE_LOOP;
                        g_mp3_player->state = MP3_PLAYER_STATE_PLAYING;  // 保持PLAYING状态
                        loop_restart_pending = true;
                        loop_restart_time = xTaskGetTickCount() + pdMS_TO_TICKS(300);
                        // 清除序列播放文件缓存
                        memset(sequence_second_file, 0, sizeof(sequence_second_file));
                    } else {
                        g_mp3_player->state = MP3_PLAYER_STATE_STOPPED;
                    }
                } else {
                    g_mp3_player->state = MP3_PLAYER_STATE_STOPPED;
                }
            }
        } else if (status > AEL_STATUS_NONE && status <= AEL_STATUS_ERROR_UNKNOWN) {
            ESP_LOGE(TAG, "播放链路发生错误,状态=%" PRIiPTR, status);
            loop_restart_pending = false;
            loop_restart_time = 0;
            sequence_play_pending = false;
            memset(sequence_second_file, 0, sizeof(sequence_second_file));
            g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
        }
    }

    mp3_player_unlock();
}

esp_err_t mp3_player_init(void)
{
    if (g_mp3_player != NULL) {
        ESP_LOGW(TAG, "MP3播放器已经初始化");
        return ESP_OK;
    }

    // 创建互斥锁
    mp3_player_mutex = xSemaphoreCreateMutex();
    if (!mp3_player_mutex) {
        ESP_LOGE(TAG, "创建MP3播放器互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    // 使用静态结构体,避免动态分配,节省内存
    static mp3_player_t mp3_player_static;
    g_mp3_player = &mp3_player_static;
    memset(g_mp3_player, 0, sizeof(mp3_player_t));

    // 初始化关闭标志
    mp3_player_shutting_down = false;

    // 初始化外设管理
    ESP_LOGD(TAG, "[ 1 ] 初始化外设管理");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    g_mp3_player->periph_set = esp_periph_set_init(&periph_cfg);
    if (!g_mp3_player->periph_set) {
        ESP_LOGE(TAG, "初始化外设集失败");
        goto init_fail;
    }

    ESP_LOGD(TAG, "[ 1.1 ] 挂载spiffs");
    // 初始化SPIFFS外设 - 使用spiffs分区
    periph_spiffs_cfg_t spiffs_cfg = {
        .root = "/spiffs",
        .partition_label = "audio",  // 修改为spiffs分区
        .max_files = 5,
        .format_if_mount_failed = false  // 不要格式化spiffs分区,因为里面有预装的音频文件
    };
    g_mp3_player->spiffs_handle = periph_spiffs_init(&spiffs_cfg);
    if (!g_mp3_player->spiffs_handle) {
        ESP_LOGE(TAG, "初始化spiffs外设失败");
        goto init_fail;
    }

    // 启动SPIFFS
    esp_periph_start(g_mp3_player->periph_set, g_mp3_player->spiffs_handle);

    // 等待SPIFFS挂载成功
    int retry_count = 0;
    while (!periph_spiffs_is_mounted(g_mp3_player->spiffs_handle) && retry_count < 10) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        retry_count++;
    }
    if (retry_count >= 10) {
        ESP_LOGE(TAG, "挂载spiffs文件系统失败");
        goto init_fail;
    }

    ESP_LOGD(TAG, "[ 2 ] 启动编解码芯片");
    g_mp3_player->board_handle = audio_board_init();
    if (!g_mp3_player->board_handle) {
        ESP_LOGE(TAG, "初始化音频板失败");
        goto init_fail;
    }
    audio_hal_ctrl_codec(g_mp3_player->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    // 创建音频管道
    if (mp3_player_create_pipeline() != ESP_OK) {
        ESP_LOGE(TAG, "创建音频管道失败");
        goto init_fail;
    }

    // 创建事件监听器
    if (mp3_player_create_event_listener() != ESP_OK) {
        ESP_LOGE(TAG, "创建事件监听器失败");
        goto init_fail;
    }

    // 初始化循环播放状态
    loop_restart_pending = false;
    loop_restart_time = 0;

    // 初始化序列播放状态
    sequence_play_pending = false;
    memset(sequence_second_file, 0, sizeof(sequence_second_file));

    // 初始化状态
    g_mp3_player->state = MP3_PLAYER_STATE_IDLE;
    g_mp3_player->mode = MP3_PLAYER_MODE_ONCE;
    g_mp3_player->initialized = true;
    g_mp3_player->enabled = true;
    memset(g_mp3_player->current_file, 0, sizeof(g_mp3_player->current_file));

    // 设置初始音量为100
    g_mp3_player->volume = device_params_get_music_volume() * 5 + 75;
    mp3_player_set_volume_internal(g_mp3_player->volume, false);

    // 创建播放器任务(适中优先级)
    if (xTaskCreate(mp3_player_task, "mp3_player_task", 4096, NULL, 6, &mp3_player_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "创建mp3播放器任务失败");
        goto init_fail;
    }

    ESP_LOGI(TAG, "MP3播放器初始化成功");
    return ESP_OK;

init_fail:
    // 设置关闭标志,避免其他线程继续访问
    mp3_player_shutting_down = true;
    mp3_player_deinit();
    return ESP_FAIL;
}

esp_err_t mp3_player_play(const char *file_path)
{
    if (!mp3_player_is_enabled()) {
        ESP_LOGE(TAG, "MP3播放器未初始化或已禁用");
        return ESP_FAIL;
    }

    if (!file_path) {
        ESP_LOGE(TAG, "无效的文件路径");
        return ESP_ERR_INVALID_ARG;
    }

    if (!mp3_player_lock()) {
        ESP_LOGE(TAG, "获取MP3播放器锁失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "播放文件: %s", file_path);

    // 无论当前状态如何,都先完全停止和重置管道
    ESP_LOGD(TAG, "强制停止并重置管道");

    esp_err_t stop_ret = mp3_player_stop_pipeline_locked(true);
    if (stop_ret != ESP_OK) {
        ESP_LOGW(TAG, "停止播放管道失败,继续尝试重新播放: %s", esp_err_to_name(stop_ret));
    }

    // 设置播放模式和文件路径
    g_mp3_player->mode = MP3_PLAYER_MODE_ONCE;
    strncpy(g_mp3_player->current_file, file_path, sizeof(g_mp3_player->current_file) - 1);
    g_mp3_player->current_file[sizeof(g_mp3_player->current_file) - 1] = '\0';

    // 在管道启动前配置I2S,避免运行中重配触发DMA分配失败。
    ESP_LOGD(TAG, "配置一次性音频I2S参数: 16000Hz, 16位, 1声道");
    esp_err_t i2s_reset_ret = mp3_player_prepare_i2s_clock_for_file_locked(file_path, false);
    if (i2s_reset_ret != ESP_OK) {
        ESP_LOGE(TAG, "配置一次性音频I2S参数失败,取消播放: %s", esp_err_to_name(i2s_reset_ret));
        g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
        mp3_player_unlock();
        return i2s_reset_ret;
    }

    // 设置文件URI
    audio_element_set_uri(g_mp3_player->spiffs_stream_reader, file_path);

    // 清空 I2S 缓冲区,避免播放残留数据产生爆音
    audio_element_reset_output_ringbuf(g_mp3_player->i2s_stream_writer);

    // 启动播放
    esp_err_t ret = audio_pipeline_run(g_mp3_player->pipeline);
    if (ret == ESP_OK) {
        g_mp3_player->state = MP3_PLAYER_STATE_PLAYING;
        ESP_LOGI(TAG, "播放开始");
    } else {
        ESP_LOGE(TAG, "开始播放失败: %s", esp_err_to_name(ret));
        g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
    }

    mp3_player_unlock();
    return ret;
}

esp_err_t mp3_player_play_loop(const char *file_path)
{
    if (!mp3_player_is_enabled()) {
        ESP_LOGE(TAG, "MP3播放器未初始化或已禁用");
        return ESP_FAIL;
    }

    if (!file_path) {
        ESP_LOGE(TAG, "无效的文件路径");
        return ESP_ERR_INVALID_ARG;
    }

    if (!mp3_player_lock()) {
        ESP_LOGE(TAG, "获取MP3播放器锁失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "循环播放文件: %s", file_path);

    // 无论当前状态如何,都先完全停止和重置管道
    ESP_LOGD(TAG, "强制停止并重置管道");

    esp_err_t stop_ret = mp3_player_stop_pipeline_locked(true);
    if (stop_ret != ESP_OK) {
        ESP_LOGW(TAG, "停止播放管道失败,继续尝试循环播放: %s", esp_err_to_name(stop_ret));
    }

    // 设置播放模式和文件路径
    g_mp3_player->mode = MP3_PLAYER_MODE_LOOP;
    strncpy(g_mp3_player->current_file, file_path, sizeof(g_mp3_player->current_file) - 1);
    g_mp3_player->current_file[sizeof(g_mp3_player->current_file) - 1] = '\0';

    // 在管道启动前配置背景音乐I2S,避免运行中重配触发DMA分配失败。
    esp_err_t i2s_ret = mp3_player_prepare_i2s_clock_for_file_locked(file_path, true);
    if (i2s_ret != ESP_OK) {
        ESP_LOGE(TAG, "配置循环音频I2S参数失败,取消播放: %s", esp_err_to_name(i2s_ret));
        g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
        mp3_player_unlock();
        return i2s_ret;
    }

    // 设置文件URI
    audio_element_set_uri(g_mp3_player->spiffs_stream_reader, file_path);

    // 清空 I2S 缓冲区,避免播放残留数据产生爆音
    audio_element_reset_output_ringbuf(g_mp3_player->i2s_stream_writer);

    // 启动播放
    esp_err_t ret = audio_pipeline_run(g_mp3_player->pipeline);
    if (ret == ESP_OK) {
        g_mp3_player->state = MP3_PLAYER_STATE_PLAYING;
        ESP_LOGI(TAG, "循环播放开始");
    } else {
        ESP_LOGE(TAG, "开始循环播放失败: %s", esp_err_to_name(ret));
        g_mp3_player->state = MP3_PLAYER_STATE_ERROR;
    }

    mp3_player_unlock();
    return ret;
}

esp_err_t mp3_player_stop(void)
{
    if (!mp3_player_is_enabled()) {
        ESP_LOGE(TAG, "MP3播放器未初始化或已禁用");
        return ESP_FAIL;
    }

    if (!mp3_player_lock()) {
        ESP_LOGE(TAG, "获取MP3播放器锁失败");
        return ESP_FAIL;
    }

    if (g_mp3_player->state != MP3_PLAYER_STATE_PLAYING && g_mp3_player->state != MP3_PLAYER_STATE_PAUSED) {
        /* 非播放状态下 stop 属于幂等调用，降级为调试日志避免误报 */
        ESP_LOGD(TAG, "播放器没有在播放或暂停,当前状态: %d", g_mp3_player->state);
        mp3_player_unlock();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "停止播放");

    esp_err_t ret = mp3_player_stop_pipeline_locked(true);
    if (ret == ESP_OK) {
        g_mp3_player->state = MP3_PLAYER_STATE_STOPPED;
        g_mp3_player->mode = MP3_PLAYER_MODE_ONCE;
        memset(g_mp3_player->current_file, 0, sizeof(g_mp3_player->current_file));
        ESP_LOGI(TAG, "播放停止");
    } else {
        ESP_LOGE(TAG, "停止播放失败");
    }

    mp3_player_unlock();
    return ret;
}

esp_err_t mp3_player_set_volume(int volume)
{
    return mp3_player_set_volume_internal(volume, true);
}

static esp_err_t mp3_player_set_volume_internal(int volume, bool save_to_nvs)
{
    (void)save_to_nvs;  // 暂时不使用NVS保存功能

    if (!mp3_player_is_initialized()) {
        ESP_LOGE(TAG, "MP3播放器未初始化");
        return ESP_FAIL;
    }

    if (volume < 0 || volume > 100) {
        ESP_LOGE(TAG, "无效的音量值: %d (0-100)", volume);
        return ESP_ERR_INVALID_ARG;
    }

    g_mp3_player->volume = volume;

    // 设置硬件音量 - 直接使用0-100范围
    esp_err_t ret = ESP_FAIL;
    if (g_mp3_player->board_handle && g_mp3_player->board_handle->audio_hal) {
        ret = audio_hal_set_volume(g_mp3_player->board_handle->audio_hal, volume);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "设置音量失败");
            return ret;
        }
        ESP_LOGI(TAG, "音量已设置: %d", volume);
    } else {
        ESP_LOGE(TAG, "音频硬件未初始化");
        return ESP_FAIL;
    }

    return ret;
}

int mp3_player_get_volume(void)
{
    if (!mp3_player_is_initialized()) {
        ESP_LOGE(TAG, "MP3播放器未初始化");
        return -1;
    }

    return g_mp3_player->volume;
}

mp3_player_state_t mp3_player_get_state(void)
{
    if (!mp3_player_is_initialized()) {
        return MP3_PLAYER_STATE_ERROR;
    }

    return g_mp3_player->state;
}

mp3_player_mode_t mp3_player_get_mode(void)
{
    mp3_player_mode_t mode = MP3_PLAYER_MODE_ONCE;

    if (!mp3_player_lock()) {
        return mode;
    }

    if (mp3_player_is_initialized_unsafe()) {
        mode = g_mp3_player->mode;
    }

    mp3_player_unlock();
    return mode;
}

esp_err_t mp3_player_wait_for_finish(uint32_t timeout_ms)
{
    if (!mp3_player_lock()) {
        ESP_LOGE(TAG, "获取MP3播放器锁失败");
        return ESP_FAIL;
    }

    if (!mp3_player_is_enabled_unsafe()) {
        mp3_player_unlock();
        ESP_LOGE(TAG, "MP3播放器未初始化或已禁用");
        return ESP_FAIL;
    }

    mp3_player_mode_t mode = g_mp3_player->mode;
    mp3_player_state_t state = g_mp3_player->state;
    mp3_player_unlock();

    if (mode == MP3_PLAYER_MODE_LOOP) {
        ESP_LOGW(TAG, "循环播放模式不支持等待完成");
        return ESP_FAIL;
    }

    if (state != MP3_PLAYER_STATE_PLAYING) {
        ESP_LOGW(TAG, "播放器没有在播放,当前状态: %d", state);
        return ESP_OK;  // 没有在播放,认为已经完成
    }

    ESP_LOGD(TAG, "等待播放完成,超时: %" PRIu32 " ms", timeout_ms);

    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    while (state == MP3_PLAYER_STATE_PLAYING) {
        if (timeout_ms > 0) {
            TickType_t elapsed = xTaskGetTickCount() - start_time;
            if (elapsed >= timeout_ticks) {
                ESP_LOGW(TAG, "等待播放完成超时");
                mp3_player_stop();
                return ESP_ERR_TIMEOUT;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // 每50ms检查一次状态

        if (!mp3_player_lock()) {
            ESP_LOGW(TAG, "等待播放完成时获取锁失败");
            return ESP_FAIL;
        }
