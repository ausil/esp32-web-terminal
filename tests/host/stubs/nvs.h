#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* In-memory NVS: open returns a handle into a static table keyed by
 * namespace; get/set behave like the real thing for the types config.c uses
 * (str, u32, u8, blob). */
typedef int nvs_handle_t;

#define NVS_READONLY  0
#define NVS_READWRITE 1

#define ESP_ERR_NVS_NOT_FOUND       0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES   0x1106
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x110A

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *value, size_t *length);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length);

/* Test control: wipe the whole store between tests. */
void test_nvs_clear(void);
