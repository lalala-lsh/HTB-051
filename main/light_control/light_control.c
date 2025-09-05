#include "light_control.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

#include "button_manager.h"
#include "ledc_init.h"
#include "light_manager.h"
#include "mqtt_manmager.h"
#include "system_info.h"
#include "settings.h"
#include "Audio/audio_mode.h"
#include "Audio/audio_queue.h"
#include "Audio/tts_list.h"

#define TAG "light_control"

static button_manager_t* g_button_manager = NULL;
static light_manager_t* g_light_manager = NULL;
static bool g_buttons_enabled = false;

/**
 * @brief 恢复出厂设置
 *
 * 清除所有NVS命名空间数据，但保留"SNCode_NVS"命名空间（设备SN码）
 * 包括清除WiFi配置、设备参数等
 * 注意：MAC地址从硬件读取，无需备份
 */
void factory_reset(void)
{
    ESP_LOGW(TAG, "=== Factory Reset Initiated ===");

    // 1. 备份system_info命名空间的SN码（MAC地址从硬件读取，无需备份）
    // 使用栈上缓冲区，无需动态内存分配
    char sn_backup[17] = {0};
    bool has_sn_backup = false;
    
    settings_t* backup_nvs = settings_start("SNCode_NVS", false);
    if (backup_nvs != NULL) {
        esp_err_t err = settings_get_string(backup_nvs, "SNCode", 
                                             sn_backup, sizeof(sn_backup), 
                                             NULL);
        settings_end(backup_nvs);

        if (err == ESP_OK) {
            has_sn_backup = true;
            ESP_LOGI(TAG, "Backed up SN code: %s", sn_backup);
        } else {
            ESP_LOGW(TAG, "No SN code found in NVS");
        }
    }

    // 2. 通知服务器解绑，不等待解绑回复
    mqtt_publish_unbind_message();
    vTaskDelay(pdMS_TO_TICKS(200));

    // 3. 擦除整个NVS分区（包括WiFi配置）
    ESP_LOGI(TAG, "Erasing entire NVS flash...");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "NVS flash erased successfully");

    // 4. 重新初始化NVS
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize NVS: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "NVS reinitialized");

    // 5. 恢复system_info数据（仅SN码）
    if (has_sn_backup) {
        settings_t* restore_nvs = settings_start("SNCode_NVS", true);
        if (restore_nvs != NULL) {
            settings_set_string(restore_nvs, "SNCode", sn_backup);
            settings_end(restore_nvs);
            ESP_LOGI(TAG, "Restored SN code: %s", sn_backup);
        } else {
            ESP_LOGE(TAG, "Failed to create SNCode_NVS namespace for restore");
        }
    }

    ESP_LOGW(TAG, "=== Factory Reset Completed ===");
    ESP_LOGW(TAG, "Device will restart in 2 seconds...");

    // 6. 延时后重启设备
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/**
 * @brief 按键事件回调函数
 *
 * @param button_id 按键ID
 * @param event 事件类型
 * @param arg 用户参数
 */
static void on_button_event(uint8_t button_id, button_event_t event, void* arg)
{
    if (!g_buttons_enabled) {
        ESP_LOGD(TAG, "按键未启用,忽略事件: id=%d event=%d", button_id, event);
        return;
    }

    // 按键0-4：全局亮度控制（反向映射：按键4→10%, 按键3→40%, ...按键0→100%）
    if (button_id <= BUTTON_ID_4) {
        if (event == BUTTON_EVENT_PRESSED) {
            // 反向映射：按键4→档位0(10%), 按键3→档位1(40%), ...按键0→档位4(100%)
            brightness_level_t level = (brightness_level_t)(4 - button_id);
            light_manager_set_brightness_level(g_light_manager, level);
            mqtt_notify_light_change();
        }
        return;
    }

    // 按键5-7：独立处理
    switch (button_id) {
        // case BUTTON_ID_4: // 保持滑动10%亮度 / 长按2秒关闭所有灯光
        //     if (event == BUTTON_EVENT_LONG_PRESS) {
        //         ESP_LOGI(TAG, "Button 4 long pressed for 2s, turning off all lights...");
        //         light_manager_turn_off_all(g_light_manager, true);
        //         mqtt_notify_light_change();
        //     }
        //     break;
        case BUTTON_ID_5: // 环境光+下光组合控制 / 长按2秒关闭所有灯光
            if (event == BUTTON_EVENT_SINGLE_CLICK) {
                light_manager_cycle_combo(g_light_manager);
                mqtt_notify_light_change();
            } else if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "Button 5 long pressed for 2s, turning off all lights...");
                light_manager_turn_off_all(g_light_manager, true);
                mqtt_notify_light_change();
            }
            break;

        case BUTTON_ID_6: // 红光模式切换 / 2秒切换专注音源 / 长按5秒切换A2DP↔本地模式
            if (event == BUTTON_EVENT_SINGLE_CLICK) {
                light_manager_cycle_red_mode(g_light_manager);
                mqtt_notify_light_change();
            } else if (event == BUTTON_EVENT_HOLD_2S) {
                if (light_manager_get_red_mode(g_light_manager) == RED_LIGHT_MODE_THERAPY) {
                    ESP_LOGI(TAG, "Button 6 held for 2s in focus mode, toggling focus source...");
                    light_manager_toggle_focus_source(g_light_manager);
                }
            } else if (event == BUTTON_EVENT_LONG_PRESS) {
                if (audio_mode_is_a2dp()) {
                    ESP_LOGI(TAG, "Button 6 long pressed for 5s, switching back to local mode...");
                    audio_mode_switch_to_local();

                    red_light_mode_t red_mode = light_manager_get_red_mode(g_light_manager);
                    if ((red_mode == RED_LIGHT_MODE_THERAPY &&
                         light_manager_get_focus_source(g_light_manager) == FOCUS_SOURCE_AUDIO) ||
                        (red_mode == RED_LIGHT_MODE_NORMAL && audio_queue_get_music_enabled())) {
                        const char *bgm = (red_mode == RED_LIGHT_MODE_THERAPY) ? MUSIC_40HZ : MUSIC;
                        audio_queue_set_background_music(bgm, true);
                        audio_queue_play(A2DP_CLOSE, AUDIO_TYPE_SYSTEM, AUDIO_PRIORITY_HIGH, true);
                    } else {
                        audio_queue_play(A2DP_CLOSE, AUDIO_TYPE_SYSTEM, AUDIO_PRIORITY_HIGH, false);
                    }
                } else {
                    ESP_LOGI(TAG, "Button 6 long pressed for 5s, switching to A2DP mode...");
                    audio_queue_play(A2DP_OPEN, AUDIO_TYPE_SYSTEM, AUDIO_PRIORITY_HIGH, false);
                    audio_mode_switch_to_a2dp();
                }
            }
            break;

        case BUTTON_ID_7: // 上光开关 / 长按10秒恢复出厂设置
            if (event == BUTTON_EVENT_SINGLE_CLICK) {
                light_manager_toggle(g_light_manager, LIGHT_ID_UPPER, true);
                mqtt_notify_light_change();
            } else if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGW(TAG, "Button 7 long pressed for 10s, triggering factory reset...");
                factory_reset();
            }
            break;

        default:
            break;
    }
}

void light_control_start(void)
{
    // 1. 初始化LEDC
    ledc_init();

    led_status_init();

    // 2. 创建灯光管理器
    g_light_manager = light_manager_create();
    if (g_light_manager == NULL) {
        ESP_LOGE(TAG, "Failed to create light manager");
        return;
    }

    // 3. 初始化灯光管理器（从NVS加载状态）
    esp_err_t ret = light_manager_init(g_light_manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init light manager");
        light_manager_destroy(g_light_manager);
        return;
    }

    // 4. 创建按键管理器
    g_button_manager = button_manager_create();
    if (g_button_manager == NULL) {
        ESP_LOGE(TAG, "Failed to create button manager");
        light_manager_destroy(g_light_manager);
        return;
    }

    // 5. 应用默认配置
    ret = button_manager_apply_default_config(g_button_manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply default config");
        button_manager_destroy(g_button_manager);
        light_manager_destroy(g_light_manager);
        return;
    }

    // 6. 注册回调
    ret = button_manager_register_callback(g_button_manager, on_button_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callback");
        button_manager_destroy(g_button_manager);
        light_manager_destroy(g_light_manager);
        return;
    }

    // 7. 启动按键管理器（增大栈大小以容纳 light_manager 的调用）
    ret = button_manager_start(g_button_manager, 9, 4096);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start button manager");
        button_manager_destroy(g_button_manager);
        light_manager_destroy(g_light_manager);
        return;
    }

    ESP_LOGI(TAG, "Light control started successfully");
    ESP_LOGI(TAG, " - Light manager initialized");
    ESP_LOGI(TAG, " - Button manager started");
}

void light_control_set_buttons_enabled(bool enabled)
{
    g_buttons_enabled = enabled;
    ESP_LOGI(TAG, "本地按键%s", enabled ? "已启用" : "已禁用");
}

light_manager_t* get_light_manager(void)
{
    return g_light_manager;
}

void light_control_stop(void)
{
    ESP_LOGI(TAG, "Stopping light control...");

    // 停止并销毁按键管理器
    if (g_button_manager != NULL) {
        button_manager_stop(g_button_manager);
        button_manager_destroy(g_button_manager);
        g_button_manager = NULL;
        ESP_LOGI(TAG, "Button manager stopped and destroyed");
    }

    // 销毁灯光管理器
    if (g_light_manager != NULL) {
        light_manager_destroy(g_light_manager);
        g_light_manager = NULL;
        ESP_LOGI(TAG, "Light manager destroyed");
    }

    ESP_LOGI(TAG, "Light control stopped");
}
