#pragma once
#include <stdint.h>

typedef struct {
    const char *project_name;
    const char *version;
    const char *idf_ver;
    const char *date;
    const char *time;
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);
void test_app_desc_set(const char *version);
