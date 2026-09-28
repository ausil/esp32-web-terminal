#include "nvs.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { test_nvs_clear(); return ESP_OK; }

/* Tiny fixed-table store. Namespaces are per-handle: nvs_open returns a slot
 * index into s_ns_names so two handles on different namespaces don't clash. */
#define MAX_ENTRIES 64
#define MAX_VAL 4096
#define MAX_HANDLES 16

typedef struct {
    bool used;
    char ns[16];
    char key[16];
    int type;  /* 1=str 2=u32 3=u8 4=blob */
    size_t len;
    uint8_t val[MAX_VAL];
} entry_t;

static entry_t s_store[MAX_ENTRIES];
static char s_ns_names[MAX_HANDLES][16];
static nvs_handle_t s_next_handle = 1;

static const char *ns_of(nvs_handle_t h)
{
    if (h <= 0 || h > MAX_HANDLES) return "";
    return s_ns_names[h - 1];
}

static entry_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_store[i].used && strcmp(s_store[i].ns, ns) == 0 &&
            strcmp(s_store[i].key, key) == 0) {
            return &s_store[i];
        }
    }
    return NULL;
}

static entry_t *claim(const char *ns, const char *key, int type)
{
    entry_t *e = find(ns, key);
    if (e && e->type == type) return e;
    if (e) return NULL;  /* type mismatch — real NVS rejects it too */
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_store[i].used) {
            e = &s_store[i];
            e->used = true;
            strncpy(e->ns, ns, sizeof(e->ns) - 1);
            e->ns[sizeof(e->ns) - 1] = '\0';
            strncpy(e->key, key, sizeof(e->key) - 1);
            e->key[sizeof(e->key) - 1] = '\0';
            e->type = type;
            e->len = 0;
            return e;
        }
    }
    return NULL;
}

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    nvs_handle_t h = s_next_handle++;
    if (h > MAX_HANDLES) return ESP_ERR_NO_MEM;
    strncpy(s_ns_names[h - 1], namespace_name, sizeof(s_ns_names[0]) - 1);
    s_ns_names[h - 1][sizeof(s_ns_names[0]) - 1] = '\0';
    *out_handle = h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

void test_nvs_clear(void)
{
    memset(s_store, 0, sizeof(s_store));
    s_next_handle = 1;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *value, size_t *length)
{
    entry_t *e = find(ns_of(h), key);
    if (!e || e->type != 1) return ESP_ERR_NVS_NOT_FOUND;
    if (!value) { *length = e->len; return ESP_OK; }
    if (*length < e->len) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(value, e->val, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    entry_t *e = claim(ns_of(h), key, 1);
    if (!e) return ESP_ERR_NO_MEM;
    e->len = strlen(value) + 1;
    if (e->len > MAX_VAL) return ESP_ERR_INVALID_SIZE;
    memcpy(e->val, value, e->len);
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    entry_t *e = find(ns_of(h), key);
    if (!e || e->type != 2) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, e->val, sizeof(uint32_t));
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    entry_t *e = claim(ns_of(h), key, 2);
    if (!e) return ESP_ERR_NO_MEM;
    memcpy(e->val, &value, sizeof(value));
    e->len = sizeof(value);
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    entry_t *e = find(ns_of(h), key);
    if (!e || e->type != 3) return ESP_ERR_NVS_NOT_FOUND;
    *out = e->val[0];
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    entry_t *e = claim(ns_of(h), key, 3);
    if (!e) return ESP_ERR_NO_MEM;
    e->val[0] = value;
    e->len = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length)
{
    entry_t *e = find(ns_of(h), key);
    if (!e || e->type != 4) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *length = e->len; return ESP_OK; }
    if (*length < e->len) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, e->val, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length)
{
    entry_t *e = claim(ns_of(h), key, 4);
    if (!e) return ESP_ERR_NO_MEM;
    if (length > MAX_VAL) return ESP_ERR_INVALID_SIZE;
    memcpy(e->val, value, length);
    e->len = length;
    return ESP_OK;
}
