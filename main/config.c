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

// HMAC-SHA256 using basic mbedtls md API (works across v3 and v4)
#define SHA256_BLOCK_SIZE 64
#define SHA256_HASH_SIZE  32
#define PBKDF2_ITERATIONS 10000

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len, uint8_t *out)
{
    uint8_t k_ipad[SHA256_BLOCK_SIZE], k_opad[SHA256_BLOCK_SIZE];
    uint8_t tk[SHA256_HASH_SIZE];

    // If key > block size, hash it first
    if (key_len > SHA256_BLOCK_SIZE) {
        mbedtls_md_context_t ctx;
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, md, 0);
        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, key, key_len);
        mbedtls_md_finish(&ctx, tk);
        mbedtls_md_free(&ctx);
        key = tk;
        key_len = SHA256_HASH_SIZE;
    }

    memset(k_ipad, 0x36, SHA256_BLOCK_SIZE);
    memset(k_opad, 0x5c, SHA256_BLOCK_SIZE);
    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    // inner: SHA256(k_ipad || data)
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t inner[SHA256_HASH_SIZE];
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, k_ipad, SHA256_BLOCK_SIZE);
    mbedtls_md_update(&ctx, data, data_len);
    mbedtls_md_finish(&ctx, inner);

    // outer: SHA256(k_opad || inner)
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, k_opad, SHA256_BLOCK_SIZE);
    mbedtls_md_update(&ctx, inner, SHA256_HASH_SIZE);
    mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

static void pbkdf2_hmac_sha256(const char *password, const uint8_t *salt, size_t salt_len,
                                int iterations, uint8_t *out, size_t out_len)
{
    uint8_t U[SHA256_HASH_SIZE], T[SHA256_HASH_SIZE];
    size_t password_len = strlen(password);
    uint32_t block = 1;
    size_t offset = 0;

    uint8_t *salt_block = malloc(salt_len + 4);
    if (!salt_block) return;
    memcpy(salt_block, salt, salt_len);

    while (offset < out_len) {
        salt_block[salt_len + 0] = (uint8_t)(block >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block);

        // U1 = HMAC(password, salt || INT_32_BE(block))
        hmac_sha256((const uint8_t *)password, password_len, salt_block, salt_len + 4, U);
        memcpy(T, U, SHA256_HASH_SIZE);

        // U2..Uc
        for (int i = 1; i < iterations; i++) {
            hmac_sha256((const uint8_t *)password, password_len, U, SHA256_HASH_SIZE, U);
            for (int j = 0; j < SHA256_HASH_SIZE; j++) T[j] ^= U[j];
        }

        size_t to_copy = out_len - offset;
        if (to_copy > SHA256_HASH_SIZE) to_copy = SHA256_HASH_SIZE;
        memcpy(out + offset, T, to_copy);
        offset += to_copy;
        block++;
    }
    free(salt_block);
}

static void compute_hash(const char *password, const uint8_t *salt, uint8_t *hash_out)
{
    pbkdf2_hmac_sha256(password, salt, CONFIG_AUTH_SALT_LEN, PBKDF2_ITERATIONS, hash_out, CONFIG_AUTH_HASH_LEN);
}

// Legacy single SHA-256 hash for migration from pre-PBKDF2 firmware
static void compute_hash_legacy(const char *password, const uint8_t *salt, uint8_t *hash_out)
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

        // Set default auth credentials (PBKDF2)
        strncpy(s_config.auth_user, CONFIG_DEFAULT_AUTH_USER, sizeof(s_config.auth_user) - 1);
        esp_fill_random(s_config.auth_salt, CONFIG_AUTH_SALT_LEN);
        compute_hash(CONFIG_DEFAULT_AUTH_PASS, s_config.auth_salt, s_config.auth_hash);
        s_config.auth_hash_ver = 1;

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
        // No stored hash — set defaults with PBKDF2
        esp_fill_random(s_config.auth_salt, CONFIG_AUTH_SALT_LEN);
        compute_hash(CONFIG_DEFAULT_AUTH_PASS, s_config.auth_salt, s_config.auth_hash);
        s_config.auth_hash_ver = 1;
    } else {
        size_t salt_len = CONFIG_AUTH_SALT_LEN;
        nvs_get_blob(nvs, "auth_salt", s_config.auth_salt, &salt_len);
        uint8_t hash_ver = 0;
        nvs_get_u8(nvs, "hash_ver", &hash_ver);
        s_config.auth_hash_ver = hash_ver;
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
    nvs_set_u8(nvs, "hash_ver", s_config.auth_hash_ver);
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
    s_config.auth_hash_ver = 1;  // Always upgrade to PBKDF2
    s_config.auth_initialized = true;

    ESP_LOGI(TAG, "Auth credentials updated for user '%s'", username);
    return save_to_nvs();
}

static int constant_time_compare(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

bool config_check_password(const char *password)
{
    uint8_t hash[CONFIG_AUTH_HASH_LEN];
    if (s_config.auth_hash_ver == 0) {
        // Legacy single SHA-256 hash from pre-PBKDF2 firmware
        compute_hash_legacy(password, s_config.auth_salt, hash);
    } else {
        compute_hash(password, s_config.auth_salt, hash);
    }
    return constant_time_compare(hash, s_config.auth_hash, CONFIG_AUTH_HASH_LEN);
}
