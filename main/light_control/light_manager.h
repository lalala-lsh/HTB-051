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
 * @param arg 用户参数
 */
typedef void (*light_change_callback_t)(light_change_type_t change_type, int light_id, void* arg);

/**
 * @brief 注册灯光状态变化回调
 *
 * @param manager 灯光管理器指针
 * @param callback 回调函数
 * @param arg 用户参数
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_register_change_callback(light_manager_t* manager,
                                                  light_change_callback_t callback,
                                                  void* arg);

/**
 * @brief 按当前灯光状态同步面板指示灯
 *
 * 指示灯低电平点亮，高电平熄灭：
 * LED1 跟随上灯板，LED3 跟随下灯板或环境灯，LED2 跟随红光模式。
 *
 * @param manager 灯光管理器指针
 */
void light_manager_sync_indicator_leds(light_manager_t* manager);

// =============================================================================
// 状态查询
// =============================================================================

/**
 * @brief 强制关闭所有灯光并重置状态
 *
 * 用于外部模块（如PIR超时）需要关闭所有灯光的场景，
 * 会同步重置combo_state和red_mode等内部状态
 *
 * @param manager 灯光管理器指针
 * @param use_fade 是否使用渐变效果
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_turn_off_all(light_manager_t* manager, bool use_fade);

/**
 * @brief OTA升级前临时关闭硬件输出
 *
 * 仅关闭PWM输出、蜂鸣器和音频播放，不修改灯光开关状态、
 * 红光模式、组合状态、NVS或MQTT状态。
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_suspend_outputs_for_ota(light_manager_t* manager);

/**
 * @brief 判断是否有任何灯开启（用于NVS生命周期管理）
 *
 * @param manager 灯光管理器指针
 * @return true 有灯开启
 * @return false 所有灯关闭
 */
bool light_manager_is_any_on(light_manager_t* manager);

/**
 * @brief 查询单个灯是否开启
 *
 * @param manager 灯光管理器指针
 * @param light_id 灯光ID
 * @return true 灯开启
 * @return false 灯关闭
 */
bool light_manager_is_on(light_manager_t* manager, light_id_t light_id);

/**
 * @brief 获取单个灯的当前亮度百分比
 *
 * @param manager 灯光管理器指针
 * @param light_id 灯光ID
 * @return uint8_t 亮度百分比 (0-100)
 */
uint8_t light_manager_get_light_brightness(light_manager_t* manager, light_id_t light_id);

// =============================================================================
// MQTT协议接口 - 直接设置百分比亮度
// =============================================================================

/**
 * @brief 设置指定灯光的百分比亮度并开关（MQTT协议使用）
 *
 * @param manager 灯光管理器指针
 * @param light_id 灯光ID
 * @param on 是否开启 (true/false)
 * @param brightness_percent 亮度百分比 (1-100)，仅在on=true时生效
 * @param use_fade 是否使用渐变
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_set_light_state_percent(light_manager_t* manager,
                                                 light_id_t light_id,
                                                 bool on,
                                                 uint8_t brightness_percent,
                                                 bool use_fade);

/**
 * @brief 设置光疗红光模式（MQTT协议使用）
 *
 * @param manager 灯光管理器指针
 * @param therapy_state 光疗状态: 0=关闭、1=护眼、2=专注、3=助眠
 * @param brightness_normal 护眼模式亮度 (1-100)
 * @param brightness_therapy 专注模式亮度 (1-100)
 * @param brightness_sleep 助眠模式亮度 (1-100, 内部按20%上限缩放)
 * @param use_fade 是否使用渐变
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_set_therapy_state(light_manager_t* manager,
                                           therapy_state_t therapy_state,
                                           uint8_t brightness_normal,
                                           uint8_t brightness_therapy,
                                           uint8_t brightness_sleep,
                                           bool use_fade);

/**
 * @brief 判断是否有面板灯（不含红光）开启
 *
 * 用于PIR判断是否需要激活，助眠模式单独开时不应激活PIR
 *
 * @param manager 灯光管理器指针
 * @return true 有面板灯开启
 * @return false 所有面板灯关闭
 */
bool light_manager_is_any_panel_on(light_manager_t* manager);

// =============================================================================
// 红光渐变闪烁功能（OTA升级视觉反馈）
// =============================================================================

/**
 * @brief 启动红光渐变闪烁（呼吸效果）
 *
 * 用于OTA升级等需要视觉反馈的场景。
 * 红光会以约2秒周期进行平滑呼吸式闪烁（使用正弦函数实现）。
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 *
 * @note 闪烁期间会忽略其他红光控制命令
 * @note 使用正常模式（5kHz PWM），不影响光疗设置
 * @note 自动关闭蜂鸣器（如果开启）
 */
esp_err_t light_manager_start_red_blink(light_manager_t* manager);

/**
 * @brief 停止红光渐变闪烁
 *
 * 停止闪烁并关闭红光。
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_stop_red_blink(light_manager_t* manager);

// =============================================================================
// NVS存储（预留接口）
// =============================================================================

/**
 * @brief 保存当前状态到NVS（预留，待用户提供键值对表）
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_save_state(light_manager_t* manager);

/**
 * @brief 从NVS加载状态（预留，待用户提供键值对表）
 *
 * @param manager 灯光管理器指针
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t light_manager_load_state(light_manager_t* manager);

#endif // _LIGHT_MANAGER_H_
