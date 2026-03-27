// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#define AUTH_SESSION_TOKEN_LEN  32
#define AUTH_MAX_SESSIONS       4
#define AUTH_SESSION_TIMEOUT_S  3600  // 1 hour
#define AUTH_MAX_FAILED         5
#define AUTH_LOCKOUT_S          300   // 5 minutes

esp_err_t auth_init(void);

// Returns session token on success, NULL on failure. Caller must free() the token.
char *auth_login(const char *username, const char *password);

// Validate a session token. Returns true if valid.
bool auth_validate_session(const char *token);

// Invalidate a session token (logout).
void auth_logout(const char *token);

// Extract session token from request cookie or Authorization header.
// Returns heap-allocated string or NULL. Caller must free().
char *auth_get_token_from_request(httpd_req_t *req);

// Check if request is authenticated. Returns true if valid session.
bool auth_check_request(httpd_req_t *req);

// Invalidate all active sessions (e.g., after password change).
void auth_invalidate_all_sessions(void);

// Returns true if login is currently locked out due to failed attempts.
bool auth_is_locked_out(void);
