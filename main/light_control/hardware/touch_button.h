#ifndef _TOUCH_BUTTON_H_
#define _TOUCH_BUTTON_H_

#include <stdio.h>
#include <stdint.h>

void touch_button_init(void);

uint8_t touch_button_read_flag(void);
uint8_t touch_button_read_id(void);

#endif