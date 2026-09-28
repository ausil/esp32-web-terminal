/* Host-test stub of the mbedtls md API, backed by OpenSSL EVP. config.c
 * hand-rolls HMAC-SHA256 and PBKDF2 on top of this API; running them against
 * an independent implementation (and hashlib vectors) is the point. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t mbedtls_md_info_t;

typedef struct {
    void *evp;            /* EVP_MD_CTX * */
    int is_hmac;
    unsigned char hmac_key[64];
    size_t hmac_key_len;
} mbedtls_md_context_t;

const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t type);
void mbedtls_md_init(mbedtls_md_context_t *ctx);
void mbedtls_md_free(mbedtls_md_context_t *ctx);
int mbedtls_md_setup(mbedtls_md_context_t *ctx, const mbedtls_md_info_t *info, int hmac);
int mbedtls_md_starts(mbedtls_md_context_t *ctx);
int mbedtls_md_update(mbedtls_md_context_t *ctx, const unsigned char *input, size_t ilen);
int mbedtls_md_finish(mbedtls_md_context_t *ctx, unsigned char *output);
int mbedtls_md_hmac(const mbedtls_md_info_t *info, const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen, unsigned char *output);
