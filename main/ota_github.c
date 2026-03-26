// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "ota_github.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_github";

#define GITHUB_API_URL "https://api.github.com/repos/ausil/esp32-web-terminal/releases/latest"
#define RESPONSE_BUFFER_SIZE 8192

// Returns >0 if a > b, 0 if equal, <0 if a < b
static int semver_compare(const char *a, const char *b)
{
    int a_major = 0, a_minor = 0, a_patch = 0;
    int b_major = 0, b_minor = 0, b_patch = 0;

    sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch);
    sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch);

    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

esp_err_t ota_github_check(ota_github_check_result_t *result)
{
    memset(result, 0, sizeof(*result));

    char *buffer = malloc(RESPONSE_BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "User-Agent", "esp32-web-terminal");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        free(buffer);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGE(TAG, "GitHub API returned status %d", status);
        free(buffer);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Read response body
    int total_read = 0;
    int read_len;
    while (total_read < RESPONSE_BUFFER_SIZE - 1) {
        read_len = esp_http_client_read(client, buffer + total_read, RESPONSE_BUFFER_SIZE - 1 - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    buffer[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGE(TAG, "Empty response from GitHub API");
        free(buffer);
        return ESP_FAIL;
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(buffer);
    free(buffer);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse GitHub API response");
        return ESP_FAIL;
    }

    // Get version from tag_name (strip leading "v")
    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    if (!tag || !cJSON_IsString(tag)) {
        ESP_LOGE(TAG, "No tag_name in release");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *tag_str = tag->valuestring;
    if (tag_str[0] == 'v' || tag_str[0] == 'V') {
        tag_str++;
    }
    strncpy(result->latest_version, tag_str, sizeof(result->latest_version) - 1);

    // Compare versions
    const char *current = esp_app_get_description()->version;
    result->update_available = (semver_compare(tag_str, current) > 0);

    ESP_LOGI(TAG, "Current: %s, Latest: %s, Update: %s",
             current, result->latest_version,
             result->update_available ? "yes" : "no");

    // Find the matching asset for this chip
    char expected_name[64];
    snprintf(expected_name, sizeof(expected_name), "esp32-web-terminal-%s.bin", CONFIG_IDF_TARGET);

    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (assets && cJSON_IsArray(assets)) {
        cJSON *asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = cJSON_GetObjectItem(asset, "name");
            if (name && cJSON_IsString(name) && strcmp(name->valuestring, expected_name) == 0) {
                cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
                if (url && cJSON_IsString(url)) {
                    strncpy(result->asset_url, url->valuestring, sizeof(result->asset_url) - 1);
                    ESP_LOGI(TAG, "Asset URL: %s", result->asset_url);
                }
                break;
            }
        }
    }

    if (result->update_available && result->asset_url[0] == '\0') {
        ESP_LOGW(TAG, "Update available but no matching asset for %s", CONFIG_IDF_TARGET);
        result->update_available = false;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ota_github_apply(const char *asset_url)
{
    ESP_LOGI(TAG, "Starting OTA from: %s", asset_url);

    esp_http_client_config_t http_config = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        return err;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update successful");
    return ESP_OK;
}
