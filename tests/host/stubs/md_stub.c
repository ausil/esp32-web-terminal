/* mbedtls md API backed by OpenSSL EVP — see stubs/mbedtls/md.h.

   config.c calls only the plain-hash streaming path (its HMAC and PBKDF2 are
   hand-rolled on top of it), which is exactly what makes this stub useful:
   the hand-rolled code is checked against an independent SHA-256. The
   HMAC-capable setup/start path is deliberately NOT implemented — if
   production code ever reaches for it, setup() returns an error and the
   tests fail loudly rather than silently skipping validation. */
#include "mbedtls/md.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdlib.h>
#include <string.h>

/* Opaque identity token: the type is declared incomplete on purpose (the
 * real mbedtls hides it), so we hand back the address of a private object
 * and only ever compare it for identity. */
static struct md_info_token { int x; } s_sha256_info;

const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t type)
{
    return (type == MBEDTLS_MD_SHA256) ? (const mbedtls_md_info_t *)&s_sha256_info : NULL;
}

void mbedtls_md_init(mbedtls_md_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void mbedtls_md_free(mbedtls_md_context_t *ctx)
{
    if (ctx->evp) {
        EVP_MD_CTX_free(ctx->evp);
        ctx->evp = NULL;
    }
}

int mbedtls_md_setup(mbedtls_md_context_t *ctx, const mbedtls_md_info_t *info, int hmac)
{
    if (info != (const mbedtls_md_info_t *)&s_sha256_info) return -0x1900;  /* BAD_INPUT_DATA */
    if (hmac) return -0x1800;                    /* NOT_SUPPORTED: see header note */
    ctx->evp = EVP_MD_CTX_new();
    return ctx->evp ? 0 : -0x4000;               /* ERR_NO_MEMORY */
}

int mbedtls_md_starts(mbedtls_md_context_t *ctx)
{
    if (!ctx->evp) return -0x1800;
    return EVP_DigestInit_ex(ctx->evp, EVP_sha256(), NULL) == 1 ? 0 : -0x5100;
}

int mbedtls_md_update(mbedtls_md_context_t *ctx, const unsigned char *input, size_t ilen)
{
    if (!ctx->evp) return -0x1800;
    return EVP_DigestUpdate(ctx->evp, input, ilen) == 1 ? 0 : -0x5100;
}

int mbedtls_md_finish(mbedtls_md_context_t *ctx, unsigned char *output)
{
    if (!ctx->evp) return -0x1800;
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(ctx->evp, output, &out_len) != 1 || out_len != 32) return -0x5100;
    return 0;
}

int mbedtls_md_hmac(const mbedtls_md_info_t *info, const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen, unsigned char *output)
{
    if (info != (const mbedtls_md_info_t *)&s_sha256_info) return -0x1900;
    unsigned int out_len = 0;
    if (!HMAC(EVP_sha256(), key, (int)keylen, input, ilen, output, &out_len) || out_len != 32) {
        return -0x5100;
    }
    return 0;
}
