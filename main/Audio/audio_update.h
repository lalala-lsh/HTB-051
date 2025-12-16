#ifndef AUDIO_UPDATE_H
#define AUDIO_UPDATE_H

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频更新模块，清理遗留临时文件。
 */
esp_err_t audio_update_init(void);

/**
 * @brief 从 CMD106 的 AUDIO 数组启动异步音频更新任务。
 * @note 仅接受数组元素: {"url":"...", "md5":"...", "size":1234}
 */
esp_err_t audio_update_start_from_json(const cJSON *audio_array);

/**
 * @brief 当前是否有音频更新任务在运行。
 */
bool audio_update_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_UPDATE_H
