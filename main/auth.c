// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "auth.h"
#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "auth";

// Monotonic seconds since boot. Session/lockout timing must not use wall-clock
// time(): the first NTP sync jumps the clock decades forward, which would
// instantly expire every session created before the sync.
static time_t mono_now(void)
{
    return (time_t)(esp_timer_get_time() / 1000000);
}

typedef struct {
    char token[AUTH_SESSION_TOKEN_LEN * 2 + 1]; // hex string
    time_t created;
    bool active;
} session_t;

static session_t s_sessions[AUTH_MAX_SESSIONS];

/* Failed-login counters are kept per address. A single global counter let anyone
 * burn the five guesses and lock the real owner out for AUTH_LOCKOUT_S, from
 * anywhere, with no credentials at all — the lockout was a denial-of-service
 * button pointed at the person trying to log in. */
typedef struct {
    bool used;
    char ip[AUTH_IP_LEN];
    int failed_attempts;
    time_t lockout_until;
} host_t;

static host_t s_hosts[AUTH_TRACKED_HOSTS];

/* Looks up an address's counter; claims a slot when create is set. The table is
 * small and fixed on purpose — the server holds four sockets — so when it fills
 * the entry whose lockout lapsed longest ago is recycled. That can drop a count
 * early, which is the direction that costs the attacker's protection rather than
 * an honest user's access. */
static host_t *host_entry(const char *ip, bool create)
{
    const char *key = ip ? ip : "";
    host_t *free_slot = NULL;
    host_t *stalest = NULL;

    for (int i = 0; i < AUTH_TRACKED_HOSTS; i++) {
        host_t *h = &s_hosts[i];
        if (h->used && strcmp(h->ip, key) == 0) return h;
        if (!h->used) {
            if (!free_slot) free_slot = h;
            continue;
        }
        if (!stalest || h->lockout_until < stalest->lockout_until) stalest = h;
    }

    if (!create) return NULL;

    host_t *h = free_slot ? free_slot : stalest;
    h->used = true;
    h->failed_attempts = 0;
    h->lockout_until = 0;
    strncpy(h->ip, key, AUTH_IP_LEN - 1);
    h->ip[AUTH_IP_LEN - 1] = '\0';
    return h;
}

esp_err_t auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_hosts, 0, sizeof(s_hosts));
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

bool auth_is_locked_out(const char *client_ip)
{
    host_t *h = host_entry(client_ip, false);
    if (!h || h->lockout_until == 0) return false;

    if (mono_now() >= h->lockout_until) {
        h->lockout_until = 0;
        h->failed_attempts = 0;
        return false;
    }
    return true;
}

char *auth_login(const char *username, const char *password, const char *client_ip)
{
    if (auth_is_locked_out(client_ip)) {
        ESP_LOGW(TAG, "Login attempt from %s during lockout",
                 client_ip ? client_ip : "unknown address");
        return NULL;
    }

    app_config_t *conf = config_get();

    if (strcmp(username, conf->auth_user) != 0 || !config_check_password(password)) {
        host_t *h = host_entry(client_ip, true);
        if (h) {
            h->failed_attempts++;
            ESP_LOGW(TAG, "Login failed for user '%s' from %s (attempt %d/%d)",
                     username, client_ip ? client_ip : "unknown address",
                     h->failed_attempts, AUTH_MAX_FAILED);
            if (h->failed_attempts >= AUTH_MAX_FAILED) {
                h->lockout_until = mono_now() + AUTH_LOCKOUT_S;
                ESP_LOGW(TAG, "Locked out %s for %d seconds",
                         client_ip ? client_ip : "unknown address", AUTH_LOCKOUT_S);
            }
        }
        return NULL;
    }

    // Success — this address is trustworthy again
    host_t *ok = host_entry(client_ip, false);
    if (ok) {
        ok->failed_attempts = 0;
        ok->lockout_until = 0;
    }

    // Find free session slot (or evict oldest)
    int slot = -1;
    time_t oldest_time = 0;
    int oldest_slot = 0;
    time_t now = mono_now();

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

    time_t now = mono_now();

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

void auth_invalidate_all_sessions(void)
{
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        s_sessions[i].active = false;
    }
    ESP_LOGI(TAG, "All sessions invalidated");
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
