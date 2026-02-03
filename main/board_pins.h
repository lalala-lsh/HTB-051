#ifndef _BOARD_PIN_H_
#define _BOARD_PIN_H_

#include <driver/gpio.h>

/* PIR传感器 */
#define PIR_IO_NUM              GPIO_NUM_36

/* 光照传感器 */
#define LTR_SCL_IO_NUM          GPIO_NUM_23
#define LTR_SDA_IO_NUM          GPIO_NUM_18

/* ES8311 音频芯片 */
#define ES8311_SCL_IO_NUM       GPIO_NUM_23
#define ES8311_SDA_IO_NUM       GPIO_NUM_18
#define ES8311_MCLK_IO_NUM      GPIO_NUM_17
#define ES8311_SCLK_IO_NUM      GPIO_NUM_5
#define ES8311_DOUT_IO_NUM      -1
#define ES8311_DIN_IO_NUM       GPIO_NUM_26
#define ES8311_LRCK_IO_NUM      GPIO_NUM_25

/* 功放控制和蜂鸣器 */
#define PA_CTRL_IO_NUM          GPIO_NUM_21
#define BUZZER_IO_NUM           GPIO_NUM_4

/* LED 指示灯 */
#define LED1_IO_NUM             GPIO_NUM_27
#define LED2_IO_NUM             GPIO_NUM_14
#define LED3_IO_NUM             GPIO_NUM_12

/* 触摸传感器 */
#define TOUCH_BIN0_IO_NUM       GPIO_NUM_35
#define TOUCH_BIN1_IO_NUM       GPIO_NUM_22
#define TOUCH_BIN2_IO_NUM       GPIO_NUM_34
#define TOUCH_BIN3_IO_NUM       GPIO_NUM_39

/* 灯光控制 */
#define AMBIENT_LIGHT_IO_NUM    GPIO_NUM_15   /*!< 环境光灯板 */
#define LOWER_LIGHT_IO_NUM      GPIO_NUM_2    /*!< 下光灯板 */
#define UPPER_LIGHT_IO_NUM      GPIO_NUM_19   /*!< 上光灯板 */
#define LED_R_IO_NUM            GPIO_NUM_13

#endif // _BOARD_PIN_H_