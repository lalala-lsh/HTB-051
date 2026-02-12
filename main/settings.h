#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <nvs_flash.h>
#include <esp_err.h>

typedef struct settings settings_t;

/**
 * @brief 创建并初始化Settings对象（构造函数）
 *
 * @param namespace_name NVS命名空间名称
 * @param read_write true=读写模式，false=只读模式
 * @return settings_t* 返回Settings对象指针，失败返回NULL
 */
settings_t* settings_start(const char* namespace_name, bool read_write);

/**
 * @brief 销毁Settings对象（析构函数）
 *
 * 如果有未提交的修改会先提交，然后关闭NVS句柄并释放内存
 *
 * @param settings Settings对象指针
 * @return esp_err_t ESP_OK表示成功
 */
esp_err_t settings_end(settings_t* settings);

/**
 * @brief 获取字符串值
 *
 * 将字符串值复制到调用者提供的缓冲区中。
 * 如果键不存在或发生错误，则复制 default_value 到缓冲区。
 *
 * @param settings Settings对象指针
 * @param key 键名
 * @param buffer 用于存储结果的缓冲区（由调用者分配）
 * @param buffer_size 缓冲区大小（包含结尾的'\0'）
 * @param default_value 默认值（当键不存在时使用），可以为NULL
 * @return esp_err_t ESP_OK=成功,
 *                   ESP_ERR_INVALID_ARG=参数无效,
 *                   ESP_ERR_NVS_NOT_FOUND=键不存在(已复制默认值),
 *                   ESP_ERR_INVALID_SIZE=缓冲区太小
 */
esp_err_t settings_get_string(settings_t* settings, const char* key,
                               char* buffer, size_t buffer_size,
                               const char* default_value);

/**
 * @brief 设置字符串值
 *
 * @param settings Settings对象指针
 * @param key 键名
 * @param value 要设置的值
 */
void settings_set_string(settings_t* settings, const char* key, const char* value);

/**
 * @brief 获取整数值
 *
 * @param settings Settings对象指针
 * @param key 键名
 * @param default_value 默认值（当键不存在时返回）
 * @return int32_t 返回整数值
 */
int32_t settings_get_int(settings_t* settings, const char* key, int32_t default_value);

/**
 * @brief 设置整数值
 *
 * @param settings Settings对象指针
 * @param key 键名
 * @param value 要设置的值
 */
void settings_set_int(settings_t* settings, const char* key, int32_t value);

/**
 * @brief 获取布尔值
 *
 * @param settings Settings对象指针
 * @param key 键名
 * @param default_value 默认值（当键不存在时返回）
 * @return bool 返回布尔值
 */
bool settings_get_bool(settings_t* settings, const char* key, bool default_value);

/**
 * @brief 设置布尔值
 *
 * @param settings Settings对象指针
 * @param key 键名
 * @param value 要设置的值
 */
void settings_set_bool(settings_t* settings, const char* key, bool value);

/**
 * @brief 删除指定键
 *
 * @param settings Settings对象指针
 * @param key 要删除的键名
 */
void settings_erase_key(settings_t* settings, const char* key);

/**
 * @brief 删除命名空间中的所有键
 *
 * @param settings Settings对象指针
 */
void settings_erase_all(settings_t* settings);

#endif // _SETTINGS_H_
