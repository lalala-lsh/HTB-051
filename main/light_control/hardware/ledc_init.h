#ifndef _LEDC_INIT_H_
#define _LEDC_INIT_H_

#include <driver/ledc.h>

#define LEDC_NORMAL_TIMER_NUM LEDC_TIMER_0
#define LEDC_40HZ_TIMER_NUM   LEDC_TIMER_1

#define LEDC_NORMAL_FREQ 5000
#define LEDC_40HZ_FREQ   40

#define LEDC_DUTY_RES       LEDC_TIMER_13_BIT
#define LEDC_MAX_DUTY       ((1 << 13) - 1)  // 8191

#define LED_R_CHANNEL          LEDC_CHANNEL_0  /*!< 红色LED*/
#define AMBIENT_LIGHT_CHANNEL  LEDC_CHANNEL_1  /*!< 环境光灯板*/
#define LOWER_LIGHT_CHANNEL    LEDC_CHANNEL_2  /*!< 下光灯板*/
#define UPPER_LIGHT_CHANNEL    LEDC_CHANNEL_3  /*!< 上光灯板*/
#define BUZZER_CHANNEL         LEDC_CHANNEL_4  /*!< 蜂鸣器*/

/**
 * @brief LED工作模式枚举
 */
typedef enum {
    MODE_NORMAL = 0,  ///< 正常模式
    MODE_40HZ = 1,    ///< 40Hz模式
} light_mode_t;

void ledc_init(void);

esp_err_t light_switch_mode(light_mode_t mode);

void led_status_init(void);

esp_err_t light_set_duty(ledc_channel_t channel, uint32_t duty);

esp_err_t light_set_duty_with_time(ledc_channel_t channel, uint32_t duty, uint32_t time_ms);

#endif // _LEDC_INIT_H_