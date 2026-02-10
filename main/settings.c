#include "settings.h"
#include "esp_err.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Settings"

/**
 * @brief Settings 对象结构体
 *
 * 封装NVS操作的所有状态信息
 * 结构体定义仅在实现文件中可见，对外使用不透明指针
 */
struct settings
{
    char* ns;                /**< NVS命名空间名称 */
    nvs_handle_t nvs_handle; /**< NVS句柄 */
    bool read_write;         /**< 读写模式标志 */
    bool dirty;              /**< 脏数据标志，标识是否有未提交的修改 */
};

/**
 * @brief 使用PSRAM分配内存并复制字符串
 */
static char* strdup_psram(const char* str)
{
    if (str == NULL) {
        return NULL;
    }

    size_t len = strlen(str) + 1;
    char* new_str = (char*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (new_str != NULL) {
        memcpy(new_str, str, len);
    }
    return new_str;
}

settings_t* settings_start(const char* namespace_name, bool read_write)
{
    if (namespace_name == NULL) {
        ESP_LOGE(TAG, "Namespace name cannot be NULL");
        return NULL;
    }

    /* 分配Settings对象内存*/
    settings_t* settings = (settings_t*)heap_caps_calloc(1, sizeof(settings_t), MALLOC_CAP_SPIRAM);
    if (settings == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for settings object");
        return NULL;
    }

    /* 初始化成员变量 */
    settings->ns = strdup_psram(namespace_name);
    if (settings->ns == NULL) {
        ESP_LOGE(TAG, "Failed to duplicate namespace name");
        free(settings);
        return NULL;
    }

    settings->read_write = read_write;
    settings->dirty = false;
    settings->nvs_handle = 0;

    /* 打开NVS */
    esp_err_t ret =
        nvs_open(namespace_name, read_write ? NVS_READWRITE : NVS_READONLY, &settings->nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", namespace_name,
                 esp_err_to_name(ret));
        free(settings->ns);
        free(settings);
        return NULL;
    }

    ESP_LOGD(TAG, "Settings object created for namespace '%s' in %s mode", namespace_name,
             read_write ? "READ/WRITE" : "READ-ONLY");

    return settings;
}

esp_err_t settings_end(settings_t* settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;

    /* 如果有未提交的修改，先提交 */
    if (settings->nvs_handle != 0) {
        if (settings->read_write && settings->dirty) {
            ret = nvs_commit(settings->nvs_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(ret));
            }
            else {
                ESP_LOGD(TAG, "NVS changes committed for namespace '%s'", settings->ns);
            }
            settings->dirty = false;
        }
        nvs_close(settings->nvs_handle);
        settings->nvs_handle = 0;
    }

    /* 释放内存*/
    if (settings->ns != NULL) {
        free(settings->ns);
        settings->ns = NULL;
    }
    free(settings);

    return ret;
}

/**
 * @brief 复制默认值到缓冲区的辅助函数
 */
static esp_err_t copy_default_value(char* buffer, size_t buffer_size, const char* default_value)
{
    if (default_value == NULL) {
        if (buffer_size > 0) {
            buffer[0] = '\0';
        }
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t default_len = strlen(default_value);
    if (default_len >= buffer_size) {
        ESP_LOGE(TAG, "Buffer too small for default value");
        if (buffer_size > 0) {
            buffer[0] = '\0';
        }
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, default_value, default_len + 1);
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t settings_get_string(settings_t* settings, const char* key,
                               char* buffer, size_t buffer_size,
                               const char* default_value)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (settings == NULL || key == NULL) {
        return copy_default_value(buffer, buffer_size, default_value);
    }

    if (settings->nvs_handle == 0) {
        return copy_default_value(buffer, buffer_size, default_value);
    }

    /* 获取字符串长度 */
    size_t length = 0;
    esp_err_t err = nvs_get_str(settings->nvs_handle, key, NULL, &length);
    if (err != ESP_OK) {
        return copy_default_value(buffer, buffer_size, default_value);
    }

    /* 检查缓冲区大小 */
    if (length > buffer_size) {
        ESP_LOGE(TAG, "Buffer too small: need %zu, got %zu", length, buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 读取字符串到用户提供的缓冲区 */
    err = nvs_get_str(settings->nvs_handle, key, buffer, &length);
    if (err != ESP_OK) {
        return copy_default_value(buffer, buffer_size, default_value);
    }

    /* 去除尾部多余的'\0' */
    while (length > 1 && buffer[length - 1] == '\0') {
        length--;
    }
    buffer[length] = '\0';

    return ESP_OK;
}

void settings_set_string(settings_t* settings, const char* key, const char* value)
{
    if (settings == NULL || key == NULL || value == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return;
    }

    if (!settings->read_write) {
        ESP_LOGW(TAG, "Namespace '%s' is not open for writing", settings->ns);
        return;
    }

    esp_err_t ret = nvs_set_str(settings->nvs_handle, key, value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set string '%s': %s", key, esp_err_to_name(ret));
        return;
    }

    settings->dirty = true;
}

int32_t settings_get_int(settings_t* settings, const char* key, int32_t default_value)
{
    if (settings == NULL || key == NULL) {
        return default_value;
    }

