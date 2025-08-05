#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <cJSON.h>

#include "system_info.h"
#include <time.h>

#include "device_params.h"

// cJSON协议结构体
typedef struct
{
    cJSON* protocol_js; // cJSON对象
    char* cmd;          // 命令字符串
    char* version;      // 版本号
    char* device_type;  // 设备类型
    char* device_code;  // 设备码
    time_t device_time; // 设备时间
    char* rid;          // 请求ID
} cjson_protocol_t;

// 工作记录结构体
typedef struct
{
    time_t start_time; // 开始时间
    time_t end_time;   // 结束时间
    int work_time;     // 工作时长(秒)
    int mode;          // 模式
} work_record_t;

#define WILL_MESSAGE_CMD           "999"
#define HEART_BEAT_CMD             "0"
#define REALTIME_REPORT_CMD        "3"
#define UNBIND_CMD                 "5"
#define BOOT_SYNC_CMD              "6"
#define PARAMS_SYNC_CMD            "7"
#define GET_DEVICE_PARAMS_CMD      "300"
#define SET_DEVICE_PARAMS_RESP_CMD "309"

/**
 * @brief 构建遗嘱消息
 * @return 遗嘱消息JSON字符串，使用后需要free
 */
char* build_will_message(void);

/**
 * @brief 构建心跳包
 * @return 心跳包JSON字符串，使用后需要free
 */
char* build_heartbeat_message(void);

/**
 * @brief 构建实时上报消息
 * @param record 当前工作记录
 * @return 实时上报JSON字符串，使用后需要free
 */
char* build_realtime_report_message(const work_record_t* record);

/**
 * @brief 构建设备解绑消息
 * @return 设备解绑JSON字符串，使用后需要free
 */
char* build_unbind_message(void);

/**
 * @brief 构建开机同步协议版本消息
 * @param params 设备参数
 * @return 开机同步JSON字符串，使用后需要free
 */
char* build_boot_sync_message(void);

/**
 * @brief 构建设备参数同步消息
 * @param params 设备参数
 * @return 参数同步JSON字符串，使用后需要free
 */
char* build_params_sync_message(void);

/**
 * @brief 构建获取设备参数回复消息
 * @param request_rid 请求ID
 * @param params 设备参数
 * @param success 是否成功
 * @return 回复JSON字符串，使用后需要free
 */
char* build_get_device_params_response(const char* request_rid, bool success);

char* build_set_device_params_response(const char* request_rid, const cJSON* key,
                                       const cJSON* value, bool success);

#endif // _PROTOCOL_H_