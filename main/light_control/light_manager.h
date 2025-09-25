#ifndef _LIGHT_MANAGER_H_
#define _LIGHT_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "ledc_init.h"

/**
 * @brief 灯光ID枚举
 */
typedef enum {
    LIGHT_ID_AMBIENT = 0,    // 环境光
    LIGHT_ID_LOWER = 1,      // 下光
    LIGHT_ID_UPPER = 2,      // 上光
    LIGHT_ID_RED = 3,        // 红光
    LIGHT_ID_MAX = 4,        // 灯光总数
} light_id_t;

/**
 * @brief 红光工作模式
 */
typedef enum {
    RED_LIGHT_MODE_OFF = 0,       // 关闭
    RED_LIGHT_MODE_NORMAL = 1,    // 护眼模式（5kHz）
    RED_LIGHT_MODE_THERAPY = 2,   // 专注模式（40Hz + MUSIC_40HZ）
    RED_LIGHT_MODE_SLEEP = 3,     // 助眠模式（40Hz，无蜂鸣器/无音乐，亮度上限20%）
} red_light_mode_t, therapy_state_t;

/**
 * @brief 专注模式音源
 */
typedef enum {
    FOCUS_SOURCE_AUDIO = 0,        // 40Hz音频
    FOCUS_SOURCE_BUZZER = 1,       // 40Hz蜂鸣器
} focus_source_t;

/**
 * @brief 按键5的状态（环境光+下光组合）
 */
typedef enum {
    COMBO_STATE_ALL_OFF = 0,      // 状态0: 全部关闭
    COMBO_STATE_BOTH_ON = 1,      // 状态1: 环境光ON + 下光ON
    COMBO_STATE_AMBIENT_ONLY = 2, // 状态2: 环境光ON + 下光OFF
} combo_state_t;

/**
 * @brief 亮度档位枚举
 */
typedef enum {
    BRIGHTNESS_LEVEL_10 = 0,   // 10%  (按键4)
    BRIGHTNESS_LEVEL_40 = 1,   // 40%  (按键3)
    BRIGHTNESS_LEVEL_60 = 2,   // 60%  (按键2)
    BRIGHTNESS_LEVEL_80 = 3,   // 80%  (按键1)
    BRIGHTNESS_LEVEL_100 = 4,  // 100% (按键0)
    BRIGHTNESS_LEVEL_MAX = 0xFF,
} brightness_level_t;

/**
 * @brief 灯光管理器结构（不透明类型）
 */
typedef struct light_manager light_manager_t;

// =============================================================================
// 生命周期管理
// =============================================================================

/**
 * @brief 创建灯光管理器实例
 *
 * @return light_manager_t* 成功返回管理器指针，失败返回NULL
 */
light_manager_t* light_manager_create(void);

/**
 * @brief 销毁灯光管理器实例
 *
 * @param manager 灯光管理器指针
 */
void light_manager_destroy(light_manager_t* manager);

/**
 * @brief 初始化灯光管理器（从NVS加载状态，预留）
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_init(light_manager_t* manager);

// =============================================================================
// 灯光控制
// =============================================================================

/**
 * @brief 开启指定灯光
 *
 * @param manager 灯光管理器指针
 * @param light_id 灯光ID
 * @param use_fade 是否使用渐变效果
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_turn_on(light_manager_t* manager,
                                 light_id_t light_id,
                                 bool use_fade);

/**
 * @brief 关闭指定灯光
 *
 * @param manager 灯光管理器指针
 * @param light_id 灯光ID
 * @param use_fade 是否使用渐变效果
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_turn_off(light_manager_t* manager,
                                  light_id_t light_id,
                                  bool use_fade);

/**
 * @brief 切换指定灯光开关状态
 *
 * @param manager 灯光管理器指针
 * @param light_id 灯光ID
 * @param use_fade 是否使用渐变效果
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_toggle(light_manager_t* manager,
                                light_id_t light_id,
                                bool use_fade);

// =============================================================================
// 亮度控制
// =============================================================================

/**
 * @brief 设置全局亮度档位（影响所有开启的灯）
 *
 * @param manager 灯光管理器指针
 * @param level 亮度档位
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_set_brightness_level(light_manager_t* manager,
                                               brightness_level_t level);

/**
 * @brief 获取当前全局亮度档位
 *
 * @param manager 灯光管理器指针
 * @return brightness_level_t 当前亮度档位
 */
brightness_level_t light_manager_get_brightness_level(light_manager_t* manager);

/**
 * @brief 设置临时亮度（不影响保存的亮度值）
 *
 * 用于PIR调暗等临时调节场景，只修改PWM输出，不修改brightness字段
 *
 * @param manager 灯光管理器指针
 * @param level 临时亮度档位
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_set_temporary_brightness_level(light_manager_t* manager,
                                                        brightness_level_t level);

/**
 * @brief 恢复所有已开启灯光到各自保存的亮度
 *
 * 用于PIR临时调暗后的恢复。该接口只恢复PWM输出，不覆盖每盏灯保存的brightness值。
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_restore_saved_brightness(light_manager_t* manager);

// =============================================================================
// 模式控制
// =============================================================================

/**
 * @brief 按键5：环境光+下光组合控制（3态循环）
 *
 * 状态转换：
 * 状态0（全关） -> 状态1（环境光ON + 下光ON） ->
 * 状态2（环境光ON + 下光OFF） -> 状态0（全关）
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_cycle_combo(light_manager_t* manager);

/**
 * @brief 重置按键5组合状态为全关
 *
 * 用于外部流程直接关闭下光/环境光后，同步组合按键状态机。
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_reset_combo_state(light_manager_t* manager);

/**
 * @brief 按键6：红光模式切换（3态循环）
 *
 * 状态转换：
 * OFF -> NORMAL（5kHz） -> THERAPY（40Hz + MUSIC_40HZ） -> OFF
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_cycle_red_mode(light_manager_t* manager);

/**
 * @brief 获取当前红光模式
 *
 * @param manager 灯光管理器指针
 * @return red_light_mode_t 当前红光模式
 */
red_light_mode_t light_manager_get_red_mode(light_manager_t* manager);

/**
 * @brief 获取光疗模式的亮度配置
 *
 * @param manager 灯光管理器指针
 * @param mode 0=护眼, 1=专注, 2=助眠
 * @return uint8_t 亮度百分比 (0-100)
 */
uint8_t light_manager_get_therapy_brightness(light_manager_t* manager, int mode);

/**
 * @brief 获取专注模式当前音源偏好
 *
 * @param manager 灯光管理器指针
 * @return focus_source_t 专注模式音源
 */
focus_source_t light_manager_get_focus_source(light_manager_t* manager);

/**
 * @brief 专注模式下切换音源
 *
 * 仅在RED_LIGHT_MODE_THERAPY下生效，不重置专注开始时间和30分钟自动关闭定时器。
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_toggle_focus_source(light_manager_t* manager);

/**
 * @brief 获取当前组合状态
 *
 * @param manager 灯光管理器指针
 * @return combo_state_t 当前组合状态
 */
combo_state_t light_manager_get_combo_state(light_manager_t* manager);

// =============================================================================
// 状态变化回调
// =============================================================================

/**
 * @brief 灯光状态变化类型
 */
typedef enum {
    LIGHT_CHANGE_ON,         // 灯光开启
    LIGHT_CHANGE_OFF,        // 灯光关闭
    LIGHT_CHANGE_BRIGHTNESS, // 亮度变化
} light_change_type_t;

/**
 * @brief 灯光状态变化回调函数类型
 *
 * @param change_type 变化类型
 * @param light_id 灯光ID（亮度变化时为-1表示全局）
