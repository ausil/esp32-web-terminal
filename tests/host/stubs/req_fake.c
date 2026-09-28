/* Fake for the httpd_req_t accessors auth.c uses (cookie / Authorization /
 * query string). Opaque-to-production struct; tests fill it via setters. */
#include "esp_http_server.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cookie;
    const char *authorization;
    const char *query;
} test_req_t;

httpd_req_t *test_req_make(void) { return calloc(1, sizeof(test_req_t)); }
void test_req_free(httpd_req_t *r) { free(r); }
void test_req_set_cookie(httpd_req_t *r, const char *v) { ((test_req_t *)r)->cookie = v; }
void test_req_set_authorization(httpd_req_t *r, const char *v) { ((test_req_t *)r)->authorization = v; }
void test_req_set_query(httpd_req_t *r, const char *v) { ((test_req_t *)r)->query = v; }

static const char *hdr_value(httpd_req_t *r, const char *name)
{
    test_req_t *t = (test_req_t *)r;
    if (strcmp(name, "Cookie") == 0) return t->cookie;
    if (strcmp(name, "Authorization") == 0) return t->authorization;
    return NULL;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name)
{
    const char *v = hdr_value(req, name);
    return v ? strlen(v) + 1 : 0;   /* real API counts the NUL */
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name,
                                      char *out, size_t out_len)
{
    const char *v = hdr_value(req, name);
    if (!v || strlen(v) + 1 > out_len) return ESP_FAIL;
    strcpy(out, v);
    return ESP_OK;
}

size_t httpd_req_get_url_query_len(httpd_req_t *req)
{
    test_req_t *t = (test_req_t *)req;
    return t->query ? strlen(t->query) + 1 : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t len)
{
    test_req_t *t = (test_req_t *)req;
    if (!t->query || strlen(t->query) + 1 > len) return ESP_FAIL;
    strcpy(buf, t->query);
    return ESP_OK;
}
