#include "unity.h"
#include "test_common.h"
#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

void setUp(void)
{
    test_nvs_clear();
    test_random_reset();
    TEST_ESP_OK(config_init());
}

void tearDown(void) {}

static int hexeq(const uint8_t *hash, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    char out[CONFIG_AUTH_HASH_LEN * 2 + 1];
    for (int i = 0; i < CONFIG_AUTH_HASH_LEN; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0xF];
    }
    out[CONFIG_AUTH_HASH_LEN * 2] = '\0';
    return strcmp(out, hex) == 0;
}

static void hex_to_hash(const char *hex, uint8_t *out)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < CONFIG_AUTH_HASH_LEN; i++) {
        int hi = strchr(digits, hex[i * 2]) - digits;
        int lo = strchr(digits, hex[i * 2 + 1]) - digits;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

/* The salt the deterministic filler hands out on the first fill of a run is
 * 11..20, so the hash config_init computes for admin/admin is a fixed
 * vector — the whole password path pinned to hashlib. */
void test_default_password_matches_hashlib(void)
{
    app_config_t *c = config_get();
    uint8_t expect_salt[16];
    for (int i = 0; i < 16; i++) expect_salt[i] = (uint8_t)(i + 0x11);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect_salt, c->auth_salt, 16);
    TEST_ASSERT_TRUE(hexeq(c->auth_hash,
        "662dd88e2c6b0c739c677248d2c4b437612dd3e6a3b10b56f75691e3d5c79c23"));
    TEST_ASSERT_TRUE(config_check_password("admin"));
    TEST_ASSERT_FALSE(config_check_password("admin2"));
    TEST_ASSERT_FALSE(config_check_password(""));
}

/* config_set_auth draws the next 16 filler bytes, 22..31. */
void test_set_auth_matches_hashlib(void)
{
    app_config_t *c = config_get();
    TEST_ESP_OK(config_set_auth("admin", "factory-parity-check"));
    uint8_t expect_salt[16];
    for (int i = 0; i < 16; i++) expect_salt[i] = (uint8_t)(i + 0x22);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect_salt, c->auth_salt, 16);
    TEST_ASSERT_TRUE(hexeq(c->auth_hash,
        "fa299db7111d4c7d7c0c8356a2e86ff0048fec9ab6c0e37ff78cac61a1b1055c"));
    TEST_ASSERT_EQUAL_UINT8(1, c->auth_hash_ver);
    TEST_ASSERT_TRUE(c->auth_initialized);
    TEST_ASSERT_TRUE(config_check_password("factory-parity-check"));
}

/* Password > HMAC block size (64) exercises the hash-the-key-first branch of
 * the hand-rolled HMAC — a classic place to go subtly wrong. */
void test_long_password_hashlib(void)
{
    app_config_t *c = config_get();
    const char *pw = "correcthorsebatterystaplecorrecthorsebatterystaple"
                     "01234567890123456789";  /* 70 bytes */
    TEST_ASSERT_EQUAL_UINT(70, strlen(pw));
    TEST_ESP_OK(config_set_auth("admin", pw));
    TEST_ASSERT_TRUE(hexeq(c->auth_hash,
        "2adc23eeb93d553eb94cc69f466556aedcc93406cfc5c37fe83b85d635816f7c"));
    TEST_ASSERT_TRUE(config_check_password(pw));
}

/* hash_ver=0 (pre-PBKDF2 firmware stored SHA-256(salt || password)): devices
 * that never rotated their password must keep authenticating across an OTA. */
void test_legacy_v0_password_migration(void)
{
    app_config_t *c = config_get();
    for (int i = 0; i < 16; i++) c->auth_salt[i] = (uint8_t)(i + 0x11);
    hex_to_hash("eda3937e71316de2092576ab8e0c7bb1144470acaa0f52acd3d9dd60f7f4530e",
                c->auth_hash);  /* SHA-256(salt || "s3cret") */
    c->auth_hash_ver = 0;
    TEST_ASSERT_TRUE(config_check_password("s3cret"));
    TEST_ASSERT_FALSE(config_check_password("s3cret2"));

    /* rotating from a legacy account upgrades to PBKDF2 */
    TEST_ESP_OK(config_set_auth("admin", "brandnewpass1"));
    TEST_ASSERT_EQUAL_UINT8(1, c->auth_hash_ver);
    TEST_ASSERT_TRUE(config_check_password("brandnewpass1"));
    TEST_ASSERT_FALSE(config_check_password("s3cret"));
}

/* NVS round-trip: reload must restore auth, name and per-port baud. */
void test_reload_from_nvs(void)
{
    TEST_ESP_OK(config_set_auth("operator", "passerby123"));
    TEST_ESP_OK(config_set_device_name("rack-pi-01"));
    TEST_ESP_OK(config_set_baud_rate_port(0, 230400));

    TEST_ESP_OK(config_init());
    app_config_t *c = config_get();
    TEST_ASSERT_EQUAL_STRING("operator", c->auth_user);
    TEST_ASSERT_EQUAL_STRING("rack-pi-01", c->device_name);
    TEST_ASSERT_EQUAL_UINT32(230400, c->baud_rate[0]);
    TEST_ASSERT_TRUE(c->auth_initialized);
    TEST_ASSERT_TRUE(config_check_password("passerby123"));
}

/* tools/factory_flash.py --config seeds PBKDF2 with hashlib; firmware must
 * accept it or bricked-looking devices get filed as firmware bugs. Same
 * parameters on both sides — tools/test_factory_flash.py checks the tool
 * actually uses these iteration/salt values. */
void test_factory_flash_parity_vector(void)
{
    app_config_t *c = config_get();
    const uint8_t salt[16] = {0x6b,0x2f,0x8e,0x4a,0x1d,0x3c,0x5b,0x7a,
                              0x9e,0x0f,0x1c,0x2d,0x3e,0x4f,0x50,0x61};
    memcpy(c->auth_salt, salt, 16);
    hex_to_hash("1d814c1ceca29cd19b89fe147ba988c03cd383e0d1dfeeb7c13cbbf73b4c933b",
                c->auth_hash);
    c->auth_hash_ver = 1;
    TEST_ASSERT_TRUE(config_check_password("factory-parity-check"));
}

void test_empty_password_rejected(void)
{
    TEST_ASSERT_FALSE(config_check_password(""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_password_matches_hashlib);
    RUN_TEST(test_set_auth_matches_hashlib);
    RUN_TEST(test_long_password_hashlib);
    RUN_TEST(test_legacy_v0_password_migration);
    RUN_TEST(test_reload_from_nvs);
    RUN_TEST(test_factory_flash_parity_vector);
    RUN_TEST(test_empty_password_rejected);
    return UNITY_END();
}
