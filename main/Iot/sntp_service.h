#ifndef __SNTP_SERVICE_H__
#define __SNTP_SERVICE_H__

#include <esp_err.h>

// SNTP事件组位定义
#define SNTP_SYNCED_BIT BIT0

esp_err_t sntp_service_init(void);

esp_err_t sntp_wait_sync(uint32_t timeout_ms);

#endif
