// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "auth.h"
#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "auth";

typedef struct {
    char token[AUTH_SESSION_TOKEN_LEN * 2 + 1]; // hex string
    time_t created;
    bool active;
} session_t;

static session_t s_sessions[AUTH_MAX_SESSIONS];
static int s_failed_attempts = 0;
static time_t s_lockout_until = 0;

esp_err_t auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_failed_attempts = 0;
    s_lockout_until = 0;
    ESP_LOGI(TAG, "Auth initialized");
    return ESP_OK;
}

static void generate_token(char *buf, size_t buf_len)
{
    uint8_t random_bytes[AUTH_SESSION_TOKEN_LEN];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (int i = 0; i < AUTH_SESSION_TOKEN_LEN && (i * 2 + 2) < buf_len; i++) {
        sprintf(buf + i * 2, "%02x", random_bytes[i]);
    }
    buf[AUTH_SESSION_TOKEN_LEN * 2] = '\0';
}

bool auth_is_locked_out(void)
{
    if (s_lockout_until == 0) return false;
    time_t now;
    time(&now);
    if (now >= s_lockout_until) {
        s_lockout_until = 0;
        s_failed_attempts = 0;
        return false;
    }
    return true;
}

char *auth_login(const char *username, const char *password)
{
    if (auth_is_locked_out()) {
        ESP_LOGW(TAG, "Login attempt during lockout");
        return NULL;
    }

    app_config_t *conf = config_get();

    if (strcmp(username, conf->auth_user) != 0 || !config_check_password(password)) {
        s_failed_attempts++;
        ESP_LOGW(TAG, "Login failed for user '%s' (attempt %d/%d)", username, s_failed_attempts, AUTH_MAX_FAILED);
        if (s_failed_attempts >= AUTH_MAX_FAILED) {
            time_t now;
            time(&now);
            s_lockout_until = now + AUTH_LOCKOUT_S;
            ESP_LOGW(TAG, "Account locked out for %d seconds", AUTH_LOCKOUT_S);
        }
        return NULL;
    }

    // Success — reset failed attempts
    s_failed_attempts = 0;

    // Find free session slot (or evict oldest)
    int slot = -1;
    time_t oldest_time = 0;
    int oldest_slot = 0;
    time_t now;
    time(&now);

    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        // Expire old sessions
        if (s_sessions[i].active && (now - s_sessions[i].created) > AUTH_SESSION_TIMEOUT_S) {
            s_sessions[i].active = false;
        }
        if (!s_sessions[i].active) {
            slot = i;
            break;
        }
        if (oldest_time == 0 || s_sessions[i].created < oldest_time) {
            oldest_time = s_sessions[i].created;
            oldest_slot = i;
        }
    }

    if (slot < 0) {
        slot = oldest_slot; // evict oldest
    }

    generate_token(s_sessions[slot].token, sizeof(s_sessions[slot].token));
    s_sessions[slot].created = now;
    s_sessions[slot].active = true;

    char *result = strdup(s_sessions[slot].token);
    ESP_LOGI(TAG, "Login successful for user '%s', session slot %d", username, slot);
    return result;
}

bool auth_validate_session(const char *token)
{
    if (!token || strlen(token) == 0) return false;

    time_t now;
    time(&now);

    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].token, token) == 0) {
            if ((now - s_sessions[i].created) > AUTH_SESSION_TIMEOUT_S) {
                s_sessions[i].active = false;
                return false;
            }
            return true;
        }
    }
    return false;
}

void auth_logout(const char *token)
{
    if (!token) return;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].token, token) == 0) {
            s_sessions[i].active = false;
            ESP_LOGI(TAG, "Session invalidated (slot %d)", i);
            return;
        }
    }
}

char *auth_get_token_from_request(httpd_req_t *req)
{
    // Check cookie first
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len > 0) {
        char *cookie = malloc(cookie_len + 1);
        if (cookie && httpd_req_get_hdr_value_str(req, "Cookie", cookie, cookie_len + 1) == ESP_OK) {
            // Look for session=<token>
            char *session_start = strstr(cookie, "session=");
            if (session_start) {
                session_start += 8; // skip "session="
                char *session_end = strchr(session_start, ';');
                size_t token_len = session_end ? (size_t)(session_end - session_start) : strlen(session_start);
                char *token = malloc(token_len + 1);
                if (token) {
                    memcpy(token, session_start, token_len);
                    token[token_len] = '\0';
                    free(cookie);
                    return token;
                }
            }
        }
        free(cookie);
    }

    // Check Authorization header: Bearer <token>
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len > 0) {
        char *auth = malloc(auth_len + 1);
        if (auth && httpd_req_get_hdr_value_str(req, "Authorization", auth, auth_len + 1) == ESP_OK) {
            if (strncmp(auth, "Bearer ", 7) == 0) {
                char *token = strdup(auth + 7);
                free(auth);
                return token;
            }
        }
        free(auth);
    }

    return NULL;
}

bool auth_check_request(httpd_req_t *req)
{
    char *token = auth_get_token_from_request(req);
    if (!token) return false;
    bool valid = auth_validate_session(token);
    free(token);
    return valid;
}
