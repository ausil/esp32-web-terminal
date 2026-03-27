// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "web_server.h"
#include "auth.h"
#include "config.h"
#include "uart_bridge.h"
#include "gpio_control.h"
#include "wifi_manager.h"
#include "ota_github.h"
#include "esp_https_server.h"
#include "esp_ota_ops.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// NTP status from main.c
extern const char *ntp_get_server(void);
extern bool ntp_is_synced(void);

static const char *TAG = "web_srv";

static httpd_handle_t s_server = NULL;

/* Embedded files:
 * - index.html via EMBED_TXTFILES (adds null terminator, use strlen)
 * - xterm.min.js.gz via EMBED_FILES (raw binary, no null terminator)
 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t xterm_gz_start[]   asm("_binary_xterm_min_js_gz_start");
extern const uint8_t xterm_gz_end[]     asm("_binary_xterm_min_js_gz_end");
extern const uint8_t server_crt_start[] asm("_binary_server_crt_start");
extern const uint8_t server_crt_end[]   asm("_binary_server_crt_end");
extern const uint8_t server_key_start[] asm("_binary_server_key_start");
extern const uint8_t server_key_end[]   asm("_binary_server_key_end");

// --- Persistent TLS certs (survive OTA updates via NVS) ---
#define NVS_CERTS_NAMESPACE "tls_certs"
#define MAX_CERT_SIZE 4096

static char *s_cert_pem = NULL;
static char *s_key_pem = NULL;

// Load certs from NVS, or save embedded certs to NVS on first boot
static void load_or_save_certs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_CERTS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for certs: %s", esp_err_to_name(err));
        return;
    }

    size_t cert_len = 0, key_len = 0;
    err = nvs_get_blob(nvs, "cert", NULL, &cert_len);

    if (err == ESP_OK && cert_len > 0) {
        // Certs exist in NVS — load them
        s_cert_pem = malloc(cert_len);
        s_key_pem = malloc(MAX_CERT_SIZE);
        if (s_cert_pem && s_key_pem) {
            nvs_get_blob(nvs, "cert", s_cert_pem, &cert_len);
            key_len = MAX_CERT_SIZE;
            nvs_get_blob(nvs, "key", s_key_pem, &key_len);
            ESP_LOGI(TAG, "TLS certs loaded from NVS (cert=%d key=%d bytes)", cert_len, key_len);
        }
    } else {
        // First boot or NVS cleared — save embedded certs
        cert_len = strlen((const char *)server_crt_start) + 1;
        key_len = strlen((const char *)server_key_start) + 1;
        nvs_set_blob(nvs, "cert", server_crt_start, cert_len);
        nvs_set_blob(nvs, "key", server_key_start, key_len);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "TLS certs saved to NVS from firmware (cert=%d key=%d bytes)", cert_len, key_len);

        // Use embedded certs directly this boot
        s_cert_pem = malloc(cert_len);
        s_key_pem = malloc(key_len);
        if (s_cert_pem && s_key_pem) {
            memcpy(s_cert_pem, server_crt_start, cert_len);
            memcpy(s_key_pem, server_key_start, key_len);
        }
    }

    nvs_close(nvs);
}

// --- WebSocket client tracking ---
#define MAX_WS_CLIENTS 4

typedef struct {
    int fd;
    bool active;
} ws_client_t;

static ws_client_t s_ws_clients[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

static void ws_add_client(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!s_ws_clients[i].active) {
            s_ws_clients[i].fd = fd;
            s_ws_clients[i].active = true;
            xSemaphoreGive(s_ws_mutex);
            ESP_LOGI(TAG, "WS client added: fd=%d slot=%d", fd, i);
            return;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    ESP_LOGW(TAG, "WS client slots full, rejecting fd=%d", fd);
}

static void ws_remove_client(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i].active && s_ws_clients[i].fd == fd) {
            s_ws_clients[i].active = false;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

void web_server_ws_broadcast(const uint8_t *data, size_t len)
{
    if (!s_server) return;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i].active) {
            esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_clients[i].fd, &ws_pkt);
            if (err != ESP_OK) {
                s_ws_clients[i].active = false;
            }
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

// --- Helpers ---

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "null");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

static esp_err_t send_json_error(httpd_req_t *req, int status, const char *message)
{
    httpd_resp_set_status(req, status == 401 ? "401 Unauthorized" :
                                status == 429 ? "429 Too Many Requests" :
                                "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    return httpd_resp_send(req, buf, strlen(buf));
}

static bool require_auth(httpd_req_t *req)
{
    if (!auth_check_request(req)) {
        send_json_error(req, 401, "Authentication required");
        return false;
    }
    return true;
}

// --- Static file handlers ---

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* EMBED_TXTFILES adds null terminator; use strlen to get actual content length */
    return httpd_resp_send(req, (const char *)index_html_start, strlen((const char *)index_html_start));
}

static esp_err_t handle_xterm_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");

    /* Send in 4KB chunks to avoid overwhelming the single-threaded server */
    const size_t total = xterm_gz_end - xterm_gz_start;
    const size_t chunk_size = 4096;
    size_t sent = 0;

    while (sent < total) {
        size_t to_send = total - sent;
        if (to_send > chunk_size) to_send = chunk_size;
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)xterm_gz_start + sent, to_send);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "xterm chunk send failed at %d/%d", (int)sent, (int)total);
            httpd_resp_send_chunk(req, NULL, 0);
            return err;
        }
        sent += to_send;
    }
    /* End chunked response */
    return httpd_resp_send_chunk(req, NULL, 0);
}

// --- Captive portal 404 ---

static esp_err_t handle_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "https://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// --- API handlers ---

static esp_err_t handle_login(httpd_req_t *req)
{
    if (auth_is_locked_out()) {
        return send_json_error(req, 429, "Too many failed attempts. Try again later.");
    }

    if (req->content_len == 0 || req->content_len >= 256) {
        return send_json_error(req, 400, "Invalid request size");
    }

    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return send_json_error(req, 400, "Empty request body");
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) return send_json_error(req, 400, "Invalid JSON");

    const cJSON *username = cJSON_GetObjectItem(json, "username");
    const cJSON *password = cJSON_GetObjectItem(json, "password");

    if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
        cJSON_Delete(json);
        return send_json_error(req, 400, "Missing username or password");
    }

    char *token = auth_login(username->valuestring, password->valuestring);
    cJSON_Delete(json);

    if (!token) return send_json_error(req, 401, "Invalid credentials");

    char cookie[128];
    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly; Secure; SameSite=Strict", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");

    app_config_t *conf = config_get();
    free(token);
    char resp[64];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"must_change_password\":%s}",
             conf->auth_initialized ? "false" : "true");
    return httpd_resp_send(req, resp, strlen(resp));
}

static esp_err_t handle_logout(httpd_req_t *req)
{
    char *token = auth_get_token_from_request(req);
    if (token) { auth_logout(token); free(token); }
    httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

static esp_err_t handle_sysinfo(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();

    // Chip info
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddNumberToObject(root, "revision", chip.revision);

    // Firmware
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "firmware_version", app->version);
    cJSON_AddStringToObject(root, "idf_version", app->idf_ver);
    cJSON_AddStringToObject(root, "build_date", app->date);
    cJSON_AddStringToObject(root, "build_time", app->time);

    // OTA partition
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        cJSON_AddStringToObject(root, "ota_partition", running->label);
    }

    // Memory
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());

    // Uptime
    int64_t uptime_us = esp_timer_get_time();
    int uptime_s = (int)(uptime_us / 1000000);
    int days = uptime_s / 86400;
    int hours = (uptime_s % 86400) / 3600;
    int mins = (uptime_s % 3600) / 60;
    int secs = uptime_s % 60;
    char uptime_str[64];
    if (days > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%dd %dh %dm %ds", days, hours, mins, secs);
    } else if (hours > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%dh %dm %ds", hours, mins, secs);
    } else {
        snprintf(uptime_str, sizeof(uptime_str), "%dm %ds", mins, secs);
    }
    cJSON_AddStringToObject(root, "uptime", uptime_str);
    cJSON_AddNumberToObject(root, "uptime_seconds", uptime_s);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    wifi_scan_result_t *results = NULL;
    int count = wifi_manager_scan(&results);

    if (count < 0) {
        httpd_resp_set_type(req, "application/json");
        set_cors_headers(req);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"error\":\"Scan failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(ap, "secure", results[i].authmode != 0);
        cJSON_AddItemToArray(root, ap);
    }
    free(results);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    app_config_t *conf = config_get();
    wifi_manager_status_t wifi = wifi_manager_get_status();
    gpio_status_t gpio = gpio_get_status();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "baud_rate", conf->baud_rate);
    cJSON_AddBoolToObject(root, "power_on", gpio.power_on);
    cJSON_AddBoolToObject(root, "power_on_default", conf->power_on_default);
    cJSON_AddStringToObject(root, "sta_ssid", conf->sta_ssid);
    cJSON_AddBoolToObject(root, "sta_connected", wifi.sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", wifi.sta_ip);
    cJSON_AddStringToObject(root, "ap_ip", wifi.ap_ip);
    cJSON_AddStringToObject(root, "wifi_mode",
                            wifi.mode == WIFI_MGR_MODE_AP ? "ap" :
                            wifi.mode == WIFI_MGR_MODE_STA ? "sta" : "ap+sta");
    cJSON_AddBoolToObject(root, "auth_initialized", conf->auth_initialized);
    cJSON_AddStringToObject(root, "device_name", conf->device_name);
    cJSON_AddStringToObject(root, "ntp_server", conf->ntp_server);
    cJSON_AddStringToObject(root, "ntp_active_server", ntp_get_server());
    cJSON_AddBoolToObject(root, "ntp_synced", ntp_is_synced());
    cJSON_AddStringToObject(root, "timezone", conf->timezone);
    cJSON_AddStringToObject(root, "firmware_version", esp_app_get_description()->version);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len == 0 || req->content_len >= 512) {
        return send_json_error(req, 400, "Invalid request size");
    }

    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return send_json_error(req, 400, "Empty request body");
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) return send_json_error(req, 400, "Invalid JSON");

    const cJSON *baud = cJSON_GetObjectItem(json, "baud_rate");
    if (cJSON_IsNumber(baud)) {
        uint32_t new_baud = (uint32_t)baud->valuedouble;
        static const uint32_t valid_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000};
        bool valid = false;
        for (int i = 0; i < sizeof(valid_bauds) / sizeof(valid_bauds[0]); i++) {
            if (new_baud == valid_bauds[i]) { valid = true; break; }
        }
        if (valid) {
            config_set_baud_rate(new_baud);
            uart_bridge_set_baud_rate(new_baud);
        }
    }

    const cJSON *ssid = cJSON_GetObjectItem(json, "sta_ssid");
    const cJSON *pass = cJSON_GetObjectItem(json, "sta_pass");
    if (cJSON_IsString(ssid) && cJSON_IsString(pass)) {
        wifi_manager_connect_sta(ssid->valuestring, pass->valuestring);
    }

    const cJSON *power_default = cJSON_GetObjectItem(json, "power_on_default");
    if (cJSON_IsBool(power_default)) {
        config_set_power_on_default(cJSON_IsTrue(power_default));
    }

    const cJSON *dev_name = cJSON_GetObjectItem(json, "device_name");
    if (cJSON_IsString(dev_name) && strlen(dev_name->valuestring) > 0) {
        config_set_device_name(dev_name->valuestring);
        wifi_manager_update_hostname(dev_name->valuestring);
    }

    const cJSON *ntp = cJSON_GetObjectItem(json, "ntp_server");
    if (ntp && cJSON_IsString(ntp)) {
        config_set_ntp_server(ntp->valuestring);
    }

    const cJSON *tz = cJSON_GetObjectItem(json, "timezone");
    if (cJSON_IsString(tz)) {
        config_set_timezone(tz->valuestring);
        setenv("TZ", tz->valuestring, 1);
        tzset();
    }

    const cJSON *wifi_disconnect = cJSON_GetObjectItem(json, "wifi_disconnect");
    if (cJSON_IsTrue(wifi_disconnect)) {
        wifi_manager_disconnect_sta();
    }

    const cJSON *new_pass = cJSON_GetObjectItem(json, "new_password");
    if (cJSON_IsString(new_pass)) {
        const cJSON *cur_pass = cJSON_GetObjectItem(json, "current_password");
        if (!cJSON_IsString(cur_pass) || !config_check_password(cur_pass->valuestring)) {
            cJSON_Delete(json);
            return send_json_error(req, 401, "Current password required");
        }
        const char *user = config_get()->auth_user;
        const cJSON *new_user = cJSON_GetObjectItem(json, "username");
        if (cJSON_IsString(new_user)) user = new_user->valuestring;
        config_set_auth(user, new_pass->valuestring);
        auth_invalidate_all_sessions();
    }

    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

static esp_err_t handle_reset(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    esp_err_t err = gpio_sbc_reset();
    httpd_resp_set_type(req, "application/json");
    return err == ESP_OK ? httpd_resp_send(req, "{\"ok\":true}", 11)
                         : send_json_error(req, 400, "Reset failed");
}

static esp_err_t handle_power(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len >= 128) {
        return send_json_error(req, 400, "Invalid request size");
    }

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    esp_err_t err;

    if (len > 0) {
        buf[len] = '\0';
        cJSON *json = cJSON_Parse(buf);
        if (json) {
            const cJSON *state = cJSON_GetObjectItem(json, "power");
            if (cJSON_IsBool(state)) {
                err = cJSON_IsTrue(state) ? gpio_sbc_power_on() : gpio_sbc_power_off();
                cJSON_Delete(json);
                goto respond;
            }
            cJSON_Delete(json);
        }
    }

    err = gpio_sbc_power_toggle();

respond:
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        gpio_status_t status = gpio_get_status();
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"power_on\":%s}",
                 status.power_on ? "true" : "false");
        return httpd_resp_send(req, resp, strlen(resp));
    } else if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, 429, "Power toggle too fast");
    }
    return send_json_error(req, 400, "Power control failed");
}

// --- OTA firmware update handler ---

static esp_err_t handle_ota(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        return send_json_error(req, 400, "No OTA partition available");
    }

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return send_json_error(req, 400, "OTA begin failed");
    }

    ESP_LOGI(TAG, "OTA started, receiving %d bytes to %s", req->content_len, update_partition->label);

    char buf[4096];
    int total_read = 0;
    bool failed = false;

    while (total_read < req->content_len) {
        int read_len = httpd_req_recv(req, buf, sizeof(buf));
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA recv error at %d/%d", total_read, req->content_len);
            failed = true;
            break;
        }

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }

        total_read += read_len;
    }

    if (failed) {
        esp_ota_abort(ota_handle);
        return send_json_error(req, 400, "OTA write failed");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return send_json_error(req, 400, "OTA validation failed");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        return send_json_error(req, 400, "Failed to set boot partition");
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes), rebooting...", total_read);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"Update complete, rebooting...\"}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// --- ESP reboot handler ---

static esp_err_t handle_esp_reboot(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    ESP_LOGI(TAG, "ESP reboot requested via API");
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// --- TLS certificate upload handler ---

static esp_err_t handle_tls_upload(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len == 0 || req->content_len > 8192) {
        return send_json_error(req, 400, "Invalid content length");
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) return send_json_error(req, 400, "Out of memory");

    int len = httpd_req_recv(req, buf, req->content_len);
    if (len <= 0) {
        free(buf);
        return send_json_error(req, 400, "Failed to read body");
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) return send_json_error(req, 400, "Invalid JSON");

    const cJSON *cert = cJSON_GetObjectItem(json, "cert");
    const cJSON *key  = cJSON_GetObjectItem(json, "key");

    if (!cJSON_IsString(cert) || !cJSON_IsString(key)) {
        cJSON_Delete(json);
        return send_json_error(req, 400, "Missing cert or key");
    }

    size_t cert_len = strlen(cert->valuestring) + 1;
    size_t key_len  = strlen(key->valuestring) + 1;

    // Validate certificate with mbedtls
    mbedtls_x509_crt x509;
    mbedtls_x509_crt_init(&x509);
    int ret = mbedtls_x509_crt_parse(&x509, (const unsigned char *)cert->valuestring, cert_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS cert parse failed: -0x%04x", -ret);
        mbedtls_x509_crt_free(&x509);
        cJSON_Delete(json);
        return send_json_error(req, 400, "Invalid certificate");
    }

    // Validate private key with mbedtls
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
#if defined(MBEDTLS_MAJOR_VERSION) && MBEDTLS_MAJOR_VERSION >= 4
    ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)key->valuestring, key_len, NULL, 0);
#else
    ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)key->valuestring, key_len, NULL, 0, NULL, NULL);
#endif
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS key parse failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        mbedtls_x509_crt_free(&x509);
        cJSON_Delete(json);
        return send_json_error(req, 400, "Invalid private key");
    }

    // Verify cert and key match
#if defined(MBEDTLS_MAJOR_VERSION) && MBEDTLS_MAJOR_VERSION >= 4
    ret = mbedtls_pk_check_pair(&x509.pk, &pk);
#else
    ret = mbedtls_pk_check_pair(&x509.pk, &pk, NULL, NULL);
#endif
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&x509);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS cert/key mismatch: -0x%04x", -ret);
        cJSON_Delete(json);
        return send_json_error(req, 400, "Certificate and key do not match");
    }
    if (cert_len > MAX_CERT_SIZE || key_len > MAX_CERT_SIZE) {
        cJSON_Delete(json);
        return send_json_error(req, 400, "Cert or key exceeds 4096 bytes");
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_CERTS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return send_json_error(req, 400, "NVS open failed");
    }
    nvs_set_blob(nvs, "cert", cert->valuestring, cert_len);
    nvs_set_blob(nvs, "key",  key->valuestring,  key_len);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    cJSON_Delete(json);

    if (err != ESP_OK) {
        return send_json_error(req, 400, "Failed to save certs");
    }

    ESP_LOGI(TAG, "TLS certs updated via API (cert=%d key=%d bytes), reboot required",
             (int)cert_len, (int)key_len);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    return httpd_resp_send(req, "{\"ok\":true,\"message\":\"TLS certs updated, reboot to apply\"}",
                           HTTPD_RESP_USE_STRLEN);
}

// --- GitHub OTA handlers ---

static esp_err_t handle_ota_check(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    ota_github_check_result_t result;
    esp_err_t err = ota_github_check(&result);
    if (err != ESP_OK) {
        return send_json_error(req, 500, "Failed to check for updates");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "update_available", result.update_available);
    cJSON_AddStringToObject(root, "current_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "latest_version", result.latest_version);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

static esp_err_t handle_ota_github(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    ota_github_check_result_t result;
    esp_err_t err = ota_github_check(&result);
    if (err != ESP_OK) {
        return send_json_error(req, 500, "Failed to check for updates");
    }
    if (!result.update_available) {
        return send_json_error(req, 400, "No update available");
    }

    err = ota_github_apply(result.asset_url);
    if (err != ESP_OK) {
        return send_json_error(req, 500, "OTA update failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"Update complete, rebooting...\"}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// --- WebSocket handler ---

static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — check auth via cookie, header, or query param */
        bool authed = auth_check_request(req);
        if (!authed) {
            size_t qlen = httpd_req_get_url_query_len(req);
            if (qlen > 0) {
                char *query = malloc(qlen + 1);
                if (query && httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK) {
                    char token[128];
                    if (httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK) {
                        authed = auth_validate_session(token);
                    }
                }
                free(query);
            }
        }
        if (!authed) {
            httpd_resp_set_status(req, "401 Unauthorized");
            return httpd_resp_send(req, NULL, 0);
        }
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS client connected: fd=%d", fd);
        return ESP_OK;
    }

    /* Data frame — ensure this client is tracked (in case handshake tracking missed it) */
    int fd = httpd_req_to_sockfd(req);
    bool found = false;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i].active && s_ws_clients[i].fd == fd) { found = true; break; }
    }
    xSemaphoreGive(s_ws_mutex);
    if (!found) {
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS client late-added: fd=%d", fd);
    }

    httpd_ws_frame_t ws_pkt = { .type = HTTPD_WS_TYPE_BINARY };

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK || ws_pkt.len == 0) return ret;

    uint8_t *buf = malloc(ws_pkt.len);
    if (!buf) return ESP_ERR_NO_MEM;

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret == ESP_OK) {
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
            uart_bridge_send(buf, ws_pkt.len);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            ws_remove_client(fd);
        }
    }

    free(buf);
    return ret;
}

// --- Close callback ---

static void on_close(httpd_handle_t hd, int sockfd)
{
    ws_remove_client(sockfd);
    close(sockfd);
}

// --- Server lifecycle ---

esp_err_t web_server_init(void)
{
    memset(s_ws_clients, 0, sizeof(s_ws_clients));
    s_ws_mutex = xSemaphoreCreateMutex();
    assert(s_ws_mutex);
    load_or_save_certs();
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.max_open_sockets = 4;
    config.httpd.max_uri_handlers = 20;
    config.httpd.stack_size = 10240;
    config.httpd.lru_purge_enable = true;
    config.httpd.close_fn = on_close;

    /* Use persistent certs from NVS (survive OTA), fall back to embedded */
    const uint8_t *cert = s_cert_pem ? (const uint8_t *)s_cert_pem : server_crt_start;
    const uint8_t *key = s_key_pem ? (const uint8_t *)s_key_pem : server_key_start;
    config.servercert = cert;
    config.servercert_len = strlen((const char *)cert) + 1;
    config.prvtkey_pem = key;
    config.prvtkey_len = strlen((const char *)key) + 1;

    esp_err_t err = httpd_ssl_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(err));
        return err;
    }

    /* Two static file routes */
    const httpd_uri_t route_root = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
    const httpd_uri_t route_xterm = { .uri = "/lib/xterm.min.js", .method = HTTP_GET, .handler = handle_xterm_js };

    /* API routes */
    const httpd_uri_t route_login = { .uri = "/api/login", .method = HTTP_POST, .handler = handle_login };
    const httpd_uri_t route_logout = { .uri = "/api/logout", .method = HTTP_POST, .handler = handle_logout };
    const httpd_uri_t route_config_get = { .uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get };
    const httpd_uri_t route_config_post = { .uri = "/api/config", .method = HTTP_POST, .handler = handle_config_post };
    const httpd_uri_t route_reset = { .uri = "/api/reset", .method = HTTP_POST, .handler = handle_reset };
    const httpd_uri_t route_power = { .uri = "/api/power", .method = HTTP_POST, .handler = handle_power };
    const httpd_uri_t route_ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota };
    const httpd_uri_t route_sysinfo = { .uri = "/api/sysinfo", .method = HTTP_GET, .handler = handle_sysinfo };
    const httpd_uri_t route_wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
    const httpd_uri_t route_ota_check = { .uri = "/api/ota/check", .method = HTTP_GET, .handler = handle_ota_check };
    const httpd_uri_t route_ota_github = { .uri = "/api/ota/github", .method = HTTP_POST, .handler = handle_ota_github };
    const httpd_uri_t route_tls = { .uri = "/api/tls", .method = HTTP_POST, .handler = handle_tls_upload };
    const httpd_uri_t route_reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_esp_reboot };

    /* WebSocket */
    const httpd_uri_t route_ws = { .uri = "/ws", .method = HTTP_GET, .handler = handle_ws, .is_websocket = true };

    httpd_register_uri_handler(s_server, &route_root);
    httpd_register_uri_handler(s_server, &route_xterm);
    httpd_register_uri_handler(s_server, &route_login);
    httpd_register_uri_handler(s_server, &route_logout);
    httpd_register_uri_handler(s_server, &route_config_get);
    httpd_register_uri_handler(s_server, &route_config_post);
    httpd_register_uri_handler(s_server, &route_reset);
    httpd_register_uri_handler(s_server, &route_power);
    httpd_register_uri_handler(s_server, &route_ota);
    httpd_register_uri_handler(s_server, &route_sysinfo);
    httpd_register_uri_handler(s_server, &route_wifi_scan);
    httpd_register_uri_handler(s_server, &route_ota_check);
    httpd_register_uri_handler(s_server, &route_ota_github);
    httpd_register_uri_handler(s_server, &route_tls);
    httpd_register_uri_handler(s_server, &route_reboot);
    httpd_register_uri_handler(s_server, &route_ws);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_404);

    ESP_LOGI(TAG, "HTTPS server started on port 443");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_ssl_stop(s_server);
        s_server = NULL;
    }
}
