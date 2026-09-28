/* Fakes for the GitHub OTA path: canned HTTP responses, no-op OTA. */
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include <stdlib.h>
#include <string.h>

static char *s_body;
static int s_body_len;
static int s_served_len;
static int s_status;
static bool s_fail_open;

void test_ota_client_reset(const char *json, int status)
{
    free(s_body);
    s_body = NULL;
    if (json) {
        s_body_len = (int)strlen(json);
        s_body = malloc(s_body_len);
        memcpy(s_body, json, s_body_len);
    }
    s_served_len = 0;
    s_status = status;
    s_fail_open = false;
}

void test_ota_client_fail_open(void)
{
    s_fail_open = true;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    (void)config;
    return (esp_http_client_handle_t)0x1;  /* opaque non-NULL token */
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v)
{ (void)c; (void)k; (void)v; return ESP_OK; }

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)c; (void)write_len;
    return s_fail_open ? ESP_FAIL : ESP_OK;
}

int esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    (void)c;
    return s_body_len;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    (void)c;
    return s_status;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len)
{
    (void)c;
    if (s_served_len >= s_body_len) return 0;
    int n = s_body_len - s_served_len;
    if (n > len) n = len;
    memcpy(buffer, s_body + s_served_len, n);
    s_served_len += n;
    return n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c, esp_https_ota_handle_t *h)
{ (void)c; *h = (esp_https_ota_handle_t)0x2; return ESP_OK; }
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h) { (void)h; return ESP_OK; }
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h) { (void)h; return ESP_OK; }
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h) { (void)h; return ESP_OK; }

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

static esp_app_desc_t s_app_desc = {
    .project_name = "esp32-web-terminal",
    .version = "1.0.0",
    .idf_ver = "test",
    .date = "Jan 1 2026",
    .time = "00:00:00",
};

const esp_app_desc_t *esp_app_get_description(void) { return &s_app_desc; }

void test_app_desc_set(const char *version)
{
    s_app_desc.version = version;
}
