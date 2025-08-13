#ifndef __PROTOCOL_PARSE_H_
#define __PROTOCOL_PARSE_H_

#include "cJSON.h"
#include "mqtt_client.h"

#include <stdbool.h>


// 响应码定义
#define RES_CODE_SUCCESS "SUCCESS"
#define RES_CODE_FAILED "FAILED"

// 命令码定义 - 设备上报响应
#define CMD_REALTIME_REPORT_RESP 103  // 实时上报回复
#define CMD_UNBIND_RESP 105  // 设备解绑回复
#define CMD_BOOT_SYNC_RESP 106  // 开机同步协议版本回复

// 命令码定义 - A类：服务器发送命令
#define CMD_GET_DEVICE_PARAMS 200  // 获取设备参数
#define CMD_SET_DEVICE_PARAMS 209  // 设置设备参数
#define CMD_SERVER_UNBIND 299      // 服务器发送设备解绑

// 命令码定义 - B类：设备回复
#define CMD_GET_DEVICE_PARAMS_RESP 300  // 获取设备参数回复
#define CMD_SET_DEVICE_PARAMS_RESP 309  // 设置设备参数回复

/**
 * @brief 处理接收到的MQTT消息
 * @param client MQTT客户端
 * @param data 接收到的数据
 * @param data_len 数据长度
 */
void process_received_message(esp_mqtt_client_handle_t client, const char *data, int data_len);




#endif //__PROTOCOL_PARSE_H_