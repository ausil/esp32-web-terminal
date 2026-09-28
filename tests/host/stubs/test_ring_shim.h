/* Shim for the extracted WS-ring TU (see extract_ring.py). The extraction
 * window starts after ws_client_t/s_ws_clients/s_ws_mutex/s_server are
 * declared in web_server.c, so this header provides them — plus the one
 * external symbol the real ws_revalidate_locked() needs,
 * auth_validate_session(), renamed by a compile define so the ring test can
 * link the real auth.c under its own name in other tests.
 *
 * MAX_WS_CLIENTS / WS_AUTH_RECHECK_US are NOT defined here: extract_ring.py
 * scrapes them from web_server.c and emits them before this include, so a
 * rename upstream breaks the build instead of drifting silently. */
#pragma once
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define AUTH_SESSION_TOKEN_LEN 32

typedef struct {
    int fd;
    bool active;
    int port_index;
    char token[AUTH_SESSION_TOKEN_LEN * 2 + 1];
    int64_t last_auth_us;
} ws_client_t;

static ws_client_t s_ws_clients[MAX_WS_CLIENTS];
/* In production the mutex is created in web_server_init(); the ring's
 * entrypoints bail out while it is NULL, so tests need a non-NULL stand-in
 * (the semphr stubs are no-ops anyway). */
static int s_ws_mutex_dummy;
static SemaphoreHandle_t s_ws_mutex = &s_ws_mutex_dummy;
static httpd_handle_t s_server = (httpd_handle_t)0xC0FFEE;
static const char *TAG = "ws_ring_test";

/* The ring test links the real config/auth code under fake names (compile
 * -D rename). "BAD" tokens fail validation, everything else passes — enough
 * to exercise the eviction-on-push path deterministically. */
bool fake_auth_validate_session(const char *token)
{
    return token && strcmp(token, "BAD") != 0;
}
