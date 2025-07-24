#ifndef __OTA_H__
#define __OTA_H__

#include <esp_err.h>

typedef enum {
    OTA_STATUS_OFF      = 0x00,         // OTA关闭
    OTA_STATUS_ING      = 0x01,         // OTA进行中
    OTA_STATUS_SUCCESS  = 0x02,         // OTA升级成功
    OTA_STATUS_FAIL     = 0x03,         // OTA升级失败
}OTA_STATUS;

// 公共接口函数
esp_err_t ota_start(const char *url);

OTA_STATUS get_ota_state(void);

#endif
