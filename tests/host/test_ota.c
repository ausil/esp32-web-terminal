/* ota_github.c — version comparison and release-asset selection, driven
 * through ota_github_check() with a canned HTTP response. semver_compare is
 * static, so it's covered by behavior: fake app_desc version vs fake
 * tag_name, exactly how the device decides to update. */
#include "unity.h"
#include "test_common.h"
#include "ota_github.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"   /* test_ota_client_reset / _fail_open */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* One real asset entry plus a padding field, shaped like GitHub's JSON. */
static char s_json[24576];

static void serve(const char *app_version, const char *tag, int status)
{
    snprintf(s_json, sizeof(s_json),
             "{\"tag_name\":\"%s\",\"name\":\"rel\","
             "\"assets\":["
             "{\"name\":\"esp32-web-terminal-esp32c6.bin\","
             "\"browser_download_url\":\"https://host/esp32c6.bin\",\"size\":123},"
             "{\"name\":\"esp32-web-terminal-esp32s3.bin\","
             "\"browser_download_url\":\"https://host/esp32s3.bin\"}"
             "],\"upload_date\":\"2026-01-01T00:00:00Z\"}",
             tag);
    test_app_desc_set(app_version);
    test_ota_client_reset(s_json, status);
}

void setUp(void) {}
void tearDown(void) {}

void test_update_newer_with_asset(void)
{
    serve("1.0.0", "v1.1.0", 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_TRUE(r.update_available);
    TEST_ASSERT_EQUAL_STRING("1.1.0", r.latest_version);
    TEST_ASSERT_EQUAL_STRING("https://host/esp32c6.bin", r.asset_url);
}

void test_equal_and_older(void)
{
    serve("1.2.3", "v1.2.3", 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_FALSE(r.update_available);

    serve("2.0.0", "v1.9.9", 200);
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_FALSE(r.update_available);
}

/* Numeric, not lexicographic: 1.10.0 > 1.9.0 — string compare would get
 * this wrong and freeze a whole minor series. */
void test_minor_10_beats_9(void)
{
    serve("1.9.0", "v1.10.0", 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_TRUE(r.update_available);
}

/* Patch likewise: 1.6.10 > 1.6.9 */
void test_patch_10_beats_9(void)
{
    serve("1.6.9", "v1.6.10", 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_TRUE(r.update_available);
}

/* No asset matching this target: report no update rather than handing the
 * user a 404 download (the old releases/latest asset-count confusion). */
void test_missing_asset_for_target(void)
{
    snprintf(s_json, sizeof(s_json),
             "{\"tag_name\":\"v9.0.0\",\"assets\":["
             "{\"name\":\"something-else.bin\",\"browser_download_url\":\"https://host/x.bin\"}]}");
    test_app_desc_set("1.0.0");
    test_ota_client_reset(s_json, 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_FALSE(r.update_available);
    TEST_ASSERT_EQUAL_STRING("", r.asset_url);
}

/* Regression for the buffer-size incident (see CLAUDE.md): releases/latest
 * has measured ~8.2KB since GitHub added per-asset digests. A response
 * around that size must still parse and select the asset. */
void test_large_response_parses(void)
{
    char *p = s_json;
    p += sprintf(p, "{\"tag_name\":\"v9.9.9\",\"assets\":["
                    "{\"name\":\"esp32-web-terminal-esp32c6.bin\","
                    "\"browser_download_url\":\"https://host/c6.bin\"}],\"filler\":\"");
    while ((p - s_json) < 8300) {
        p += sprintf(p, "padding padding padding ");
    }
    p += sprintf(p, "\"}");
    test_app_desc_set("1.0.0");
    test_ota_client_reset(s_json, 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_TRUE(r.update_available);
    TEST_ASSERT_EQUAL_STRING("https://host/c6.bin", r.asset_url);
}

void test_garbage_and_status_fail(void)
{
    test_app_desc_set("1.0.0");

    test_ota_client_reset("this is not json", 200);
    ota_github_check_result_t r;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ota_github_check(&r));

    serve("1.0.0", "v1.1.0", 404);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ota_github_check(&r));

    test_ota_client_reset("{\"nope\":1}", 200);   /* no tag_name */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ota_github_check(&r));
}

void test_open_failure_returns_error(void)
{
    serve("1.0.0", "v1.1.0", 200);
    test_ota_client_fail_open();
    ota_github_check_result_t r;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ota_github_check(&r));
}

/* Current documented behavior: a -beta suffix is ignored by the sscanf parse,
 * so v2.0.0-beta1 compares as 2.0.0. Pins the semantics in case someone
 * later "fixes" prerelease handling. */
void test_prerelease_compares_as_base(void)
{
    serve("1.0.0", "v2.0.0-beta1", 200);
    ota_github_check_result_t r;
    TEST_ESP_OK(ota_github_check(&r));
    TEST_ASSERT_TRUE(r.update_available);
    TEST_ASSERT_EQUAL_STRING("2.0.0-beta1", r.latest_version);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_update_newer_with_asset);
    RUN_TEST(test_equal_and_older);
    RUN_TEST(test_minor_10_beats_9);
    RUN_TEST(test_patch_10_beats_9);
    RUN_TEST(test_missing_asset_for_target);
    RUN_TEST(test_large_response_parses);
    RUN_TEST(test_garbage_and_status_fail);
    RUN_TEST(test_open_failure_returns_error);
    RUN_TEST(test_prerelease_compares_as_base);
    return UNITY_END();
}
