#include "pir_init.h"

#include "board_pins.h"

#define TAG "pir_init"

esp_err_t pir_init(void)
{
    ESP_LOGI(TAG, "初始化PIR人体感应传感器");
    ESP_LOGI(TAG, "PIR传感器GPIO引脚: %d", PIR_IO_NUM);

    gpio_config_t config = {
        .pin_bit_mask = (1ULL << PIR_IO_NUM),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PIR传感器GPIO配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    int init_state = gpio_get_level(PIR_IO_NUM);
    const char* state_desc = (init_state == PIR_NO_MOTION) ? "无动作" : "有动作";

    ESP_LOGI(TAG, "PIR传感器初始化成功");
    ESP_LOGI(TAG, "初始状态: %d (%s)", init_state, state_desc);
    ESP_LOGI(TAG, "传感器状态说明: 0=检测到动作, 1=无动作");

    return ESP_OK;
}

bool pir_get_state(void)
{
    int level = gpio_get_level(PIR_IO_NUM);
    bool no_motion = (level == PIR_NO_MOTION);
    return no_motion;
}
