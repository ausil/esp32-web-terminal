#pragma once
#include "esp_http_server.h"
#include <stddef.h>

httpd_req_t *test_req_make(void);
void test_req_free(httpd_req_t *r);
void test_req_set_cookie(httpd_req_t *r, const char *v);
void test_req_set_authorization(httpd_req_t *r, const char *v);
void test_req_set_query(httpd_req_t *r, const char *v);
