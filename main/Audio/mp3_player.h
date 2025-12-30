#ifndef __MP3_PLAYER__
#define __MP3_PLAYER__

#include "esp_err.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "esp_peripherals.h"
#include "tts_list.h"
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MP3播放器状态枚举
 */
typedef enum {
    MP3_PLAYER_STATE_IDLE,          /*!< 空闲状态 */
    MP3_PLAYER_STATE_PLAYING,       /*!< 播放状态 */
    MP3_PLAYER_STATE_PAUSED,        /*!< 暂停状态 */
    MP3_PLAYER_STATE_STOPPED,       /*!< 停止状态 */
    MP3_PLAYER_STATE_ERROR,         /*!< 错误状态 */
    MP3_PLAYER_STATE_DISABLED,      /*!< 禁用状态 */
} mp3_player_state_t;

/**
 * @brief MP3播放模式枚举
 */
typedef enum {
    MP3_PLAYER_MODE_ONCE,           /*!< 单次播放 */
    MP3_PLAYER_MODE_LOOP,           /*!< 循环播放 */
} mp3_player_mode_t;

typedef enum {
    AUDIO_MODE_NONE = 0,        // 全部关闭
    AUDIO_MODE_MUSIC_ONLY,      // 仅音乐开启
    AUDIO_MODE_VOICE_ONLY,      // 仅语音开启
    AUDIO_MODE_MUSIC_VOICE,     // 音乐和语音都开启
    AUDIO_MODE_MAX
} audio_mode_t;

/**
 * @brief MP3播放器句柄结构体
 */
typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t spiffs_stream_reader;
    audio_element_handle_t i2s_stream_writer;
    audio_element_handle_t mp3_decoder;
    audio_event_iface_handle_t evt;
    esp_periph_set_handle_t periph_set;
    esp_periph_handle_t spiffs_handle;
    audio_board_handle_t board_handle;

    mp3_player_state_t state;
    mp3_player_mode_t mode;
    int volume;
    char current_file[256];
    bool initialized;
    bool enabled;
} mp3_player_t;

/**
 * @brief 初始化MP3播放器
 *
 * @param config 播放器配置,如果为NULL则使用默认配置
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t mp3_player_init(void);

/**
 * @brief 播放MP3文件
 *
 * @param file_path 文件路径(SPIFFS文件系统)
 * @return esp_err_t
 *         - ESP_OK 成功
 *         - ESP_FAIL 失败
 */
esp_err_t mp3_player_play(const char *file_path);

/**
 * @brief 循环播放MP3文件
