// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include <string.h>

static const char *TAG = "config";
static const char *NVS_NAMESPACE = "webterm";
static app_config_t s_config;

static void compute_hash(const char *password, const uint8_t *salt, uint8_t *hash_out)
{
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md_info, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, salt, CONFIG_AUTH_SALT_LEN);
    mbedtls_md_update(&ctx, (const uint8_t *)password, strlen(password));
    mbedtls_md_finish(&ctx, hash_out);
    mbedtls_md_free(&ctx);
}

static esp_err_t load_string(nvs_handle_t nvs, const char *key, char *buf, size_t max_len, const char *def)
{
    size_t len = max_len;
    esp_err_t err = nvs_get_str(nvs, key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(buf, def, max_len);
        buf[max_len - 1] = '\0';
        return ESP_OK;
    }
    return err;
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, formatting...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot — use defaults
        ESP_LOGI(TAG, "First boot, using default config");
        memset(&s_config, 0, sizeof(s_config));
        strncpy(s_config.ap_ssid, CONFIG_DEFAULT_AP_SSID, sizeof(s_config.ap_ssid) - 1);
        strncpy(s_config.ap_pass, CONFIG_DEFAULT_AP_PASS, sizeof(s_config.ap_pass) - 1);
        s_config.baud_rate = CONFIG_DEFAULT_BAUD_RATE;
        s_config.power_on_default = true;
        s_config.auth_initialized = false;
        strncpy(s_config.device_name, CONFIG_DEFAULT_DEVICE_NAME, sizeof(s_config.device_name) - 1);
        strncpy(s_config.timezone, CONFIG_DEFAULT_TIMEZONE, sizeof(s_config.timezone) - 1);

        // Set default auth credentials
        strncpy(s_config.auth_user, CONFIG_DEFAULT_AUTH_USER, sizeof(s_config.auth_user) - 1);
        esp_fill_random(s_config.auth_salt, CONFIG_AUTH_SALT_LEN);
        compute_hash(CONFIG_DEFAULT_AUTH_PASS, s_config.auth_salt, s_config.auth_hash);

        return ESP_OK;
    }
    ESP_ERROR_CHECK(err);

    load_string(nvs, "sta_ssid", s_config.sta_ssid, sizeof(s_config.sta_ssid), "");
    load_string(nvs, "sta_pass", s_config.sta_pass, sizeof(s_config.sta_pass), "");
    load_string(nvs, "ap_ssid", s_config.ap_ssid, sizeof(s_config.ap_ssid), CONFIG_DEFAULT_AP_SSID);
    load_string(nvs, "ap_pass", s_config.ap_pass, sizeof(s_config.ap_pass), CONFIG_DEFAULT_AP_PASS);
    load_string(nvs, "auth_user", s_config.auth_user, sizeof(s_config.auth_user), CONFIG_DEFAULT_AUTH_USER);
    load_string(nvs, "dev_name", s_config.device_name, sizeof(s_config.device_name), CONFIG_DEFAULT_DEVICE_NAME);
    load_string(nvs, "ntp_srv", s_config.ntp_server, sizeof(s_config.ntp_server), "");
    load_string(nvs, "timezone", s_config.timezone, sizeof(s_config.timezone), CONFIG_DEFAULT_TIMEZONE);

    uint32_t baud = 0;
    if (nvs_get_u32(nvs, "baud_rate", &baud) == ESP_OK) {
        s_config.baud_rate = baud;
    } else {
        s_config.baud_rate = CONFIG_DEFAULT_BAUD_RATE;
    }

    uint8_t power_on = 1;
    if (nvs_get_u8(nvs, "power_on", &power_on) == ESP_OK) {
        s_config.power_on_default = (power_on != 0);
    } else {
        s_config.power_on_default = true;
    }

    size_t hash_len = CONFIG_AUTH_HASH_LEN;
    if (nvs_get_blob(nvs, "auth_hash", s_config.auth_hash, &hash_len) != ESP_OK) {
        // No stored hash — set defaults
        esp_fill_random(s_config.auth_salt, CONFIG_AUTH_SALT_LEN);
        compute_hash(CONFIG_DEFAULT_AUTH_PASS, s_config.auth_salt, s_config.auth_hash);
    } else {
        size_t salt_len = CONFIG_AUTH_SALT_LEN;
        nvs_get_blob(nvs, "auth_salt", s_config.auth_salt, &salt_len);
    }

    uint8_t auth_init = 0;
    nvs_get_u8(nvs, "auth_init", &auth_init);
    s_config.auth_initialized = (auth_init != 0);

    nvs_close(nvs);
    ESP_LOGI(TAG, "Config loaded: baud=%lu, STA SSID='%s'", s_config.baud_rate, s_config.sta_ssid);
    return ESP_OK;
}

app_config_t *config_get(void)
{
    return &s_config;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));

    nvs_set_str(nvs, "sta_ssid", s_config.sta_ssid);
    nvs_set_str(nvs, "sta_pass", s_config.sta_pass);
    nvs_set_str(nvs, "ap_ssid", s_config.ap_ssid);
    nvs_set_str(nvs, "ap_pass", s_config.ap_pass);
    nvs_set_str(nvs, "auth_user", s_config.auth_user);
    nvs_set_u32(nvs, "baud_rate", s_config.baud_rate);
    nvs_set_u8(nvs, "power_on", s_config.power_on_default ? 1 : 0);
    nvs_set_blob(nvs, "auth_hash", s_config.auth_hash, CONFIG_AUTH_HASH_LEN);
    nvs_set_blob(nvs, "auth_salt", s_config.auth_salt, CONFIG_AUTH_SALT_LEN);
    nvs_set_u8(nvs, "auth_init", s_config.auth_initialized ? 1 : 0);
    nvs_set_str(nvs, "dev_name", s_config.device_name);
    nvs_set_str(nvs, "ntp_srv", s_config.ntp_server);
    nvs_set_str(nvs, "timezone", s_config.timezone);

    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t config_set_wifi_sta(const char *ssid, const char *password)
{
    strncpy(s_config.sta_ssid, ssid, sizeof(s_config.sta_ssid) - 1);
    s_config.sta_ssid[sizeof(s_config.sta_ssid) - 1] = '\0';
    strncpy(s_config.sta_pass, password, sizeof(s_config.sta_pass) - 1);
    s_config.sta_pass[sizeof(s_config.sta_pass) - 1] = '\0';
    ESP_LOGI(TAG, "WiFi STA config updated: SSID='%s'", ssid);
    return save_to_nvs();
}

esp_err_t config_set_wifi_ap(const char *ssid, const char *password)
{
    strncpy(s_config.ap_ssid, ssid, sizeof(s_config.ap_ssid) - 1);
    s_config.ap_ssid[sizeof(s_config.ap_ssid) - 1] = '\0';
    strncpy(s_config.ap_pass, password, sizeof(s_config.ap_pass) - 1);
    s_config.ap_pass[sizeof(s_config.ap_pass) - 1] = '\0';
    ESP_LOGI(TAG, "WiFi AP config updated: SSID='%s'", ssid);
    return save_to_nvs();
}

esp_err_t config_set_baud_rate(uint32_t baud_rate)
{
    s_config.baud_rate = baud_rate;
    ESP_LOGI(TAG, "Baud rate updated: %lu", baud_rate);
    return save_to_nvs();
}

esp_err_t config_set_power_on_default(bool power_on)
{
    s_config.power_on_default = power_on;
    return save_to_nvs();
}

esp_err_t config_set_device_name(const char *name)
{
    strncpy(s_config.device_name, name, sizeof(s_config.device_name) - 1);
    s_config.device_name[sizeof(s_config.device_name) - 1] = '\0';
    ESP_LOGI(TAG, "Device name updated: '%s'", s_config.device_name);
    return save_to_nvs();
}

esp_err_t config_set_ntp_server(const char *server)
{
    strncpy(s_config.ntp_server, server, sizeof(s_config.ntp_server) - 1);
    s_config.ntp_server[sizeof(s_config.ntp_server) - 1] = '\0';
    ESP_LOGI(TAG, "NTP server updated: '%s'", strlen(s_config.ntp_server) ? s_config.ntp_server : "(DHCP)");
    return save_to_nvs();
}

esp_err_t config_set_timezone(const char *tz)
{
    strncpy(s_config.timezone, tz, sizeof(s_config.timezone) - 1);
    s_config.timezone[sizeof(s_config.timezone) - 1] = '\0';
    ESP_LOGI(TAG, "Timezone updated: '%s'", s_config.timezone);
    return save_to_nvs();
}

esp_err_t config_set_auth(const char *username, const char *password)
{
    strncpy(s_config.auth_user, username, sizeof(s_config.auth_user) - 1);
    s_config.auth_user[sizeof(s_config.auth_user) - 1] = '\0';

    esp_fill_random(s_config.auth_salt, CONFIG_AUTH_SALT_LEN);
    compute_hash(password, s_config.auth_salt, s_config.auth_hash);
    s_config.auth_initialized = true;

    ESP_LOGI(TAG, "Auth credentials updated for user '%s'", username);
    return save_to_nvs();
}

bool config_check_password(const char *password)
{
    uint8_t hash[CONFIG_AUTH_HASH_LEN];
    compute_hash(password, s_config.auth_salt, hash);
    return memcmp(hash, s_config.auth_hash, CONFIG_AUTH_HASH_LEN) == 0;
}
