/*
 * nextOS - tls_crypto.h
 * Cryptographic primitives for TLS 1.2
 *
 * Implements: SHA-256, SHA-1, HMAC-SHA-256, HMAC-SHA-1, AES-128-CBC,
 *             RSA PKCS#1 v1.5, TLS PRF (pseudo-random function)
 */
#ifndef NEXTOS_TLS_CRYPTO_H
#define NEXTOS_TLS_CRYPTO_H

#include <stdint.h>

/* SHA-256 */
#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[SHA256_BLOCK_SIZE];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, int len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256(const uint8_t *data, int len, uint8_t digest[SHA256_DIGEST_SIZE]);

/* SHA-1 */
#define SHA1_BLOCK_SIZE  64
#define SHA1_DIGEST_SIZE 20

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buf[SHA1_BLOCK_SIZE];
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, int len);
void sha1_final(sha1_ctx_t *ctx, uint8_t digest[SHA1_DIGEST_SIZE]);
void sha1(const uint8_t *data, int len, uint8_t digest[SHA1_DIGEST_SIZE]);

/* HMAC-SHA-256 */
void hmac_sha256(const uint8_t *key, int key_len,
                 const uint8_t *data, int data_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/* HMAC-SHA-1 */
void hmac_sha1(const uint8_t *key, int key_len,
               const uint8_t *data, int data_len,
               uint8_t out[SHA1_DIGEST_SIZE]);

/* TLS PRF (SHA-256 based) */
void tls_prf_sha256(const uint8_t *secret, int secret_len,
                    const char *label,
                    const uint8_t *seed, int seed_len,
                    uint8_t *output, int output_len);

/* AES-128 */
#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE   16

typedef struct {
    uint32_t rk[44];  /* Round keys for AES-128 (11 rounds * 4 words) */
} aes128_ctx_t;

void aes128_init(aes128_ctx_t *ctx, const uint8_t key[AES_KEY_SIZE]);
void aes128_encrypt_block(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt_block(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);

/* AES-128-CBC */
int aes128_cbc_encrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *plaintext, int plain_len,
                       uint8_t *ciphertext, int max_out);
int aes128_cbc_decrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *ciphertext, int cipher_len,
                       uint8_t *plaintext, int max_out);

/* RSA public key operations */
#define RSA_MAX_MOD_BYTES 512  /* Support up to 4096-bit keys */

typedef struct {
    uint8_t modulus[RSA_MAX_MOD_BYTES];
    int     mod_len;
    uint32_t exponent;
} rsa_pubkey_t;

/* Extract RSA public key from DER-encoded certificate */
int rsa_extract_pubkey(const uint8_t *cert, int cert_len, rsa_pubkey_t *key);

/* RSA PKCS#1 v1.5 encrypt (for pre-master secret) */
int rsa_pkcs1_encrypt(const rsa_pubkey_t *key,
                      const uint8_t *data, int data_len,
                      uint8_t *output, int max_out);

/* Simple PRNG for TLS */
uint32_t tls_random(void);
void tls_random_bytes(uint8_t *buf, int len);

#endif /* NEXTOS_TLS_CRYPTO_H */
