#pragma once
#include "esp_err.h"
#include <stddef.h>

typedef void *esp_http_client_handle_t;

typedef struct {
    const char *url;
    void *crt_bundle_attach;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    bool keep_alive_enable;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key,
                                     const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

/* Test control: serve `json` (len bytes, or strlen if 0) at `status`,
 * optionally failing open(). Reset by test_all_reset. */
void test_ota_client_reset(const char *json, int status);
void test_ota_client_fail_open(void);
