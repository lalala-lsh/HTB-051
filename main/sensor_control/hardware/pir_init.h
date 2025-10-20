#ifndef _PIR_INIT_H_
#define _PIR_INIT_H_

#include <esp_err.h>
#include <esp_log.h>
#include <stdbool.h>

typedef enum {
    PIR_MOTION_DETECTED = 0,
    PIR_NO_MOTION
} pir_state_t;

esp_err_t pir_init(void);

bool pir_get_state(void);

#endif // _PIR_INIT_H_