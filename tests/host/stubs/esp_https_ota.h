#pragma once
#include "esp_err.h"
#include "esp_http_client.h"

#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x905

typedef struct {
    const esp_http_client_config_t *http_config;
} esp_https_ota_config_t;

typedef void *esp_https_ota_handle_t;

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *ota_config,
                              esp_https_ota_handle_t *handle);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t https_ota_handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t https_ota_handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t https_ota_handle);
