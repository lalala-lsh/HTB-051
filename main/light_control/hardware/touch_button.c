#include "touch_button.h"

#include <driver/gpio.h>

#include "board_pins.h"

void touch_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TOUCH_BIN0_IO_NUM) | 
                        (1ULL << TOUCH_BIN1_IO_NUM) | 
                        (1ULL << TOUCH_BIN2_IO_NUM) | 
                        (1ULL << TOUCH_BIN3_IO_NUM),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

uint8_t touch_button_read_flag(void)
{
    return gpio_get_level(TOUCH_BIN3_IO_NUM);
}

uint8_t touch_button_read_id(void)
{
    return (gpio_get_level(TOUCH_BIN2_IO_NUM) << 2) | 
           (gpio_get_level(TOUCH_BIN1_IO_NUM) << 1) | 
           gpio_get_level(TOUCH_BIN0_IO_NUM);    // 计算按键ID
}