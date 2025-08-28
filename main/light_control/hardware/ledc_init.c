#include "ledc_init.h"
#include "hal/ledc_types.h"
#include "soc/clk_tree_defs.h"

#include "board_pins.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_err.h>
#include <stdbool.h>

void ledc_init(void)
{
    ledc_timer_config_t tim_normal_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_NORMAL_TIMER_NUM,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_NORMAL_FREQ,
        .deconfigure = false,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tim_normal_cfg));

    ledc_timer_config_t tim_40hz_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_40HZ_TIMER_NUM,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_40HZ_FREQ,
        .deconfigure = false,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tim_40hz_cfg));

    ledc_channel_config_t channel_config = {
        .channel = LED_R_CHANNEL,
        .duty = 0,
        .flags.output_invert = 0,
        .gpio_num = LED_R_IO_NUM,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_NORMAL_TIMER_NUM,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    channel_config.channel = AMBIENT_LIGHT_CHANNEL;
    channel_config.gpio_num = AMBIENT_LIGHT_IO_NUM;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    channel_config.channel = LOWER_LIGHT_CHANNEL;
    channel_config.gpio_num = LOWER_LIGHT_IO_NUM;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    channel_config.channel = UPPER_LIGHT_CHANNEL;
    channel_config.gpio_num = UPPER_LIGHT_IO_NUM;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    channel_config.channel = BUZZER_CHANNEL;
    channel_config.gpio_num = BUZZER_IO_NUM;
    channel_config.timer_sel = LEDC_40HZ_TIMER_NUM;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    ledc_fade_func_install(0);
}

esp_err_t light_switch_mode(light_mode_t mode)
{
    esp_err_t ret;
    ledc_timer_t timer = (mode == MODE_40HZ) ? LEDC_40HZ_TIMER_NUM : LEDC_NORMAL_TIMER_NUM;

    // 切换红光通道定时器
    ret = ledc_bind_channel_timer(LEDC_LOW_SPEED_MODE, LED_R_CHANNEL, timer);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

esp_err_t light_set_duty(ledc_channel_t channel, uint32_t duty)
{
    if (duty > LEDC_MAX_DUTY) {
        duty = LEDC_MAX_DUTY;
    }
    return ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, channel, duty, 0);
}

esp_err_t light_set_duty_with_time(ledc_channel_t channel, uint32_t duty, uint32_t time_ms)
{
    if (duty > LEDC_MAX_DUTY) {
        duty = LEDC_MAX_DUTY;
    }
    return ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, channel, duty, time_ms,
                                        LEDC_FADE_NO_WAIT);
}

void led_status_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED1_IO_NUM) | (1ULL << LED2_IO_NUM) | (1ULL << LED3_IO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(LED1_IO_NUM, 1);
    gpio_set_level(LED2_IO_NUM, 1);
    gpio_set_level(LED3_IO_NUM, 1);
}