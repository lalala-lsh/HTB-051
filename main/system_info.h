#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <stdio.h>
#include <esp_err.h>

typedef struct system_info system_info_t;

// 初始化系统信息
esp_err_t system_info_init(void);
// 获取系统信息指针
const system_info_t* get_system_info(void);
// 获取设备名称
const char* get_device_name(void);
// 获取设备SN码
const char* get_device_sn(void);
// 获取设备MAC地址
const char* get_device_mac(void);
// 获取固件版本
const char* get_firmware_version(void);
// 设置系统SN
esp_err_t set_system_sncode(const uint8_t *sncode);
// 打印系统信息
void print_system_info(void);
// 打印系统堆状态信息
void print_heap_stats(void);

#endif // _SYSTEM_INFO_H_