#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct httpd_config_type httpd_config_t;
typedef struct httpd_uri httpd_uri_t;
typedef void *httpd_handle_t;
typedef void *httpd_req_t;   /* host tests treat requests as opaque */

/* --- Request fakes (auth.c path) — implemented by test_auth.c --- */
size_t  httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name,
                                      char *out, size_t out_len);
size_t  httpd_req_get_url_query_len(httpd_req_t *req);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t len);

/* --- WebSocket fakes (web_server ring path) --- */
#define HTTPD_WS_TYPE_TEXT   0x01
#define HTTPD_WS_TYPE_BINARY 0x02
#define HTTPD_WS_TYPE_CLOSE  0x08

typedef struct {
    unsigned int final : 1;
    unsigned int fragmented : 1;
    uint8_t type;
    uint8_t *payload;
    size_t len;
} httpd_ws_frame_t;

esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle, int fd, httpd_ws_frame_t *pkt);
esp_err_t httpd_queue_work(httpd_handle_t handle, void (*work)(void *), void *arg);

/* --- Test control (httpd_fake.c) ---
 * The fake "httpd task": queue_work records items the test drains by hand,
 * send_frame_async records frames per fd, but only for fds the test has
 * registered as live sessions — an unregistered fd fails like a real
 * recycled-socket lookup would. */
void test_httpd_reset(void);
void test_httpd_ws_register(int fd);           /* fd is a live session */
int  test_httpd_work_pending(httpd_handle_t handle);
int  test_httpd_run_work(httpd_handle_t handle);   /* -1 when queue empty */
int  test_httpd_send_count(void);
/* Copy out the i-th recorded frame; returns 0 on success. data points into
 * the fake's own storage (valid until the next test_httpd_reset). */
int  test_httpd_send_nth(int i, int *fd, uint8_t *type, size_t *len,
                         uint8_t *data, size_t data_cap);
