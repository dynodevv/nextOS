/*
 * nextOS - tls_crypto.c
 * Cryptographic primitives for TLS 1.2
 *
 * SHA-256, HMAC-SHA-256, AES-128-CBC, RSA PKCS#1 v1.5, TLS PRF
 *
 * Note: This is a minimal but correct implementation for use in a
 * freestanding kernel environment. Not optimized for speed.
 */
#include "tls_crypto.h"
#include "../drivers/timer.h"

/* ── Memory helpers ──────────────────────────────────────────────────── */
static void mc(void *d, const void *s, int n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
}
static void mz(void *d, int n) {
    uint8_t *dd = (uint8_t *)d;
    for (int i = 0; i < n; i++) dd[i] = 0;
}
static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ── SHA-256 ─────────────────────────────────────────────────────────── */
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define RR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (RR(x,2)^RR(x,13)^RR(x,22))
#define EP1(x) (RR(x,6)^RR(x,11)^RR(x,25))
#define SIG0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define SIG1(x) (RR(x,17)^RR(x,19)^((x)>>10))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i*4]<<24)|((uint32_t)data[i*4+1]<<16)|
               ((uint32_t)data[i*4+2]<<8)|data[i*4+3];
    for (i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + sha256_k[i] + w[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->count = 0;
    mz(ctx->buf, 64);
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        ctx->buf[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            sha256_transform(ctx, ctx->buf);
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32])
{
    uint64_t bits = ctx->count * 8;
    int idx = (int)(ctx->count % 64);
    ctx->buf[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buf[idx++] = 0;
        sha256_transform(ctx, ctx->buf);
        idx = 0;
    }
    while (idx < 56) ctx->buf[idx++] = 0;
    for (int i = 7; i >= 0; i--)
        ctx->buf[56 + (7-i)] = (uint8_t)(bits >> (i*8));
    sha256_transform(ctx, ctx->buf);
    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (ctx->state[i]>>24)&0xff;
        digest[i*4+1] = (ctx->state[i]>>16)&0xff;
        digest[i*4+2] = (ctx->state[i]>>8)&0xff;
        digest[i*4+3] = ctx->state[i]&0xff;
    }
}

void sha256(const uint8_t *data, int len, uint8_t digest[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

/* ── HMAC-SHA-256 ────────────────────────────────────────────────────── */
void hmac_sha256(const uint8_t *key, int key_len,
                 const uint8_t *data, int data_len,
                 uint8_t out[32])
{
    uint8_t k_pad[64], tk[32];
    sha256_ctx_t ctx;

    /* If key > 64 bytes, hash it first */
    if (key_len > 64) {
        sha256(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    /* ipad */
    mz(k_pad, 64);
    mc(k_pad, key, key_len);
    for (int i = 0; i < 64; i++) k_pad[i] ^= 0x36;

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, data, data_len);
    uint8_t inner[32];
    sha256_final(&ctx, inner);

    /* opad */
    mz(k_pad, 64);
    mc(k_pad, key, key_len);
    for (int i = 0; i < 64; i++) k_pad[i] ^= 0x5c;

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

/* ── TLS PRF (SHA-256) ───────────────────────────────────────────────── */
/* P_hash(secret, seed) = HMAC(secret, A(1) + seed) + HMAC(secret, A(2) + seed) + ...
 * A(0) = seed, A(i) = HMAC(secret, A(i-1)) */
void tls_prf_sha256(const uint8_t *secret, int secret_len,
                    const char *label,
                    const uint8_t *seed, int seed_len,
                    uint8_t *output, int output_len)
{
    int label_len = sl(label);
    /* Build label + seed */
    uint8_t ls[128];
    int ls_len = 0;
    mc(ls, label, label_len);
    ls_len += label_len;
    if (ls_len + seed_len <= (int)sizeof(ls)) {
        mc(ls + ls_len, seed, seed_len);
        ls_len += seed_len;
    }

    uint8_t a[32];  /* A(i) */
    /* A(1) = HMAC(secret, label + seed) */
    hmac_sha256(secret, secret_len, ls, ls_len, a);

    int done = 0;
    while (done < output_len) {
        /* HMAC(secret, A(i) + label + seed) */
        uint8_t input[32 + 128];
        mc(input, a, 32);
        mc(input + 32, ls, ls_len);
        uint8_t p[32];
        hmac_sha256(secret, secret_len, input, 32 + ls_len, p);

        int take = output_len - done;
        if (take > 32) take = 32;
        mc(output + done, p, take);
        done += take;

        /* A(i+1) = HMAC(secret, A(i)) */
        uint8_t a_next[32];
        hmac_sha256(secret, secret_len, a, 32, a_next);
        mc(a, a_next, 32);
    }
}

/* ── AES-128 ─────────────────────────────────────────────────────────── */
static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint32_t aes_rcon[10] = {
    0x01000000,0x02000000,0x04000000,0x08000000,0x10000000,
    0x20000000,0x40000000,0x80000000,0x1b000000,0x36000000
};

static uint32_t aes_sub_word(uint32_t w)
{
    return ((uint32_t)aes_sbox[(w>>24)&0xff]<<24) |
           ((uint32_t)aes_sbox[(w>>16)&0xff]<<16) |
           ((uint32_t)aes_sbox[(w>>8)&0xff]<<8)   |
           (uint32_t)aes_sbox[w&0xff];
}

static uint32_t aes_rot_word(uint32_t w)
{
    return (w << 8) | (w >> 24);
}

void aes128_init(aes128_ctx_t *ctx, const uint8_t key[16])
{
    for (int i = 0; i < 4; i++)
        ctx->rk[i] = ((uint32_t)key[4*i]<<24)|((uint32_t)key[4*i+1]<<16)|
                     ((uint32_t)key[4*i+2]<<8)|key[4*i+3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = ctx->rk[i-1];
        if (i % 4 == 0)
            t = aes_sub_word(aes_rot_word(t)) ^ aes_rcon[i/4-1];
        ctx->rk[i] = ctx->rk[i-4] ^ t;
    }
}

void aes128_encrypt_block(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    mc(s, in, 16);

    /* AddRoundKey(0) */
    for (int i = 0; i < 16; i++)
        s[i] ^= (ctx->rk[i/4] >> (24 - 8*(i%4))) & 0xff;

    for (int round = 1; round <= 10; round++) {
        /* SubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];

        /* ShiftRows */
        uint8_t t;
        t = s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t = s[2]; s[2]=s[10]; s[10]=t; t = s[6]; s[6]=s[14]; s[14]=t;
        t = s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;

        /* MixColumns (skip in last round) */
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                int ci = c * 4;
                uint8_t a0=s[ci], a1=s[ci+1], a2=s[ci+2], a3=s[ci+3];
                uint8_t x0 = (a0<<1)^((a0&0x80)?0x1b:0);
                uint8_t x1 = (a1<<1)^((a1&0x80)?0x1b:0);
                uint8_t x2 = (a2<<1)^((a2&0x80)?0x1b:0);
                uint8_t x3 = (a3<<1)^((a3&0x80)?0x1b:0);
                s[ci]   = x0 ^ x1 ^ a1 ^ a2 ^ a3;
                s[ci+1] = a0 ^ x1 ^ x2 ^ a2 ^ a3;
                s[ci+2] = a0 ^ a1 ^ x2 ^ x3 ^ a3;
                s[ci+3] = x0 ^ a0 ^ a1 ^ a2 ^ x3;
            }
        }

        /* AddRoundKey */
        for (int i = 0; i < 16; i++)
            s[i] ^= (ctx->rk[round*4 + i/4] >> (24 - 8*(i%4))) & 0xff;
    }
    mc(out, s, 16);
}

void aes128_decrypt_block(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    mc(s, in, 16);

    /* AddRoundKey(10) */
    for (int i = 0; i < 16; i++)
        s[i] ^= (ctx->rk[40 + i/4] >> (24 - 8*(i%4))) & 0xff;

    for (int round = 9; round >= 0; round--) {
        /* InvShiftRows */
        uint8_t t;
        t = s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
        t = s[2]; s[2]=s[10]; s[10]=t; t = s[6]; s[6]=s[14]; s[14]=t;
        t = s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;

        /* InvSubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];

        /* AddRoundKey */
        for (int i = 0; i < 16; i++)
            s[i] ^= (ctx->rk[round*4 + i/4] >> (24 - 8*(i%4))) & 0xff;

        /* InvMixColumns (skip in round 0) */
        if (round > 0) {
            for (int c = 0; c < 4; c++) {
                int ci = c * 4;
                uint8_t a0=s[ci], a1=s[ci+1], a2=s[ci+2], a3=s[ci+3];
                /* GF(2^8) multiply helpers */
                #define xtime(x) (((x)<<1)^(((x)&0x80)?0x1b:0))
                #define mul(x,y) ( \
                    (((y)&1)?(x):0) ^ (((y)&2)?xtime(x):0) ^ \
                    (((y)&4)?xtime(xtime(x)):0) ^ (((y)&8)?xtime(xtime(xtime(x))):0) )
                s[ci]   = mul(a0,14) ^ mul(a1,11) ^ mul(a2,13) ^ mul(a3,9);
                s[ci+1] = mul(a0,9)  ^ mul(a1,14) ^ mul(a2,11) ^ mul(a3,13);
                s[ci+2] = mul(a0,13) ^ mul(a1,9)  ^ mul(a2,14) ^ mul(a3,11);
                s[ci+3] = mul(a0,11) ^ mul(a1,13) ^ mul(a2,9)  ^ mul(a3,14);
                #undef xtime
                #undef mul
            }
        }
    }
    mc(out, s, 16);
}

/* AES-128-CBC encrypt */
int aes128_cbc_encrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *plaintext, int plain_len,
                       uint8_t *ciphertext, int max_out)
{
    aes128_ctx_t ctx;
    aes128_init(&ctx, key);

    /* PKCS#7 padding */
    int pad = 16 - (plain_len % 16);
    int total = plain_len + pad;
    if (total > max_out) return -1;

    uint8_t prev[16];
    mc(prev, iv, 16);

    for (int i = 0; i < total; i += 16) {
        uint8_t block[16];
        for (int j = 0; j < 16; j++) {
            if (i + j < plain_len) block[j] = plaintext[i+j];
            else block[j] = (uint8_t)pad;
        }
        /* XOR with previous ciphertext (or IV) */
        for (int j = 0; j < 16; j++) block[j] ^= prev[j];
        aes128_encrypt_block(&ctx, block, ciphertext + i);
        mc(prev, ciphertext + i, 16);
    }
    return total;
}

/* AES-128-CBC decrypt */
int aes128_cbc_decrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *ciphertext, int cipher_len,
                       uint8_t *plaintext, int max_out)
{
    if (cipher_len % 16 != 0 || cipher_len < 16) return -1;
    if (cipher_len > max_out) return -1;

    aes128_ctx_t ctx;
    aes128_init(&ctx, key);

    const uint8_t *prev = iv;
    for (int i = 0; i < cipher_len; i += 16) {
        uint8_t block[16];
        aes128_decrypt_block(&ctx, ciphertext + i, block);
        for (int j = 0; j < 16; j++) block[j] ^= prev[j];
        mc(plaintext + i, block, 16);
        prev = ciphertext + i;
    }

    /* Remove PKCS#7 padding */
    int pad = plaintext[cipher_len - 1];
    if (pad < 1 || pad > 16) return cipher_len;  /* Invalid padding, return all */
    /* Verify padding */
    for (int i = cipher_len - pad; i < cipher_len; i++) {
        if (plaintext[i] != pad) return cipher_len;
    }
    return cipher_len - pad;
}

/* ── RSA Public Key from X.509 DER ───────────────────────────────────── */
/* Parse ASN.1 DER length */
static int asn1_len(const uint8_t *p, int max, int *hlen)
{
    if (max < 1) { *hlen = 0; return 0; }
    if (p[0] < 0x80) { *hlen = 1; return p[0]; }
    int nb = p[0] & 0x7f;
    if (nb > 4 || nb + 1 > max) { *hlen = 0; return 0; }
    int len = 0;
    for (int i = 0; i < nb; i++) len = (len << 8) | p[1+i];
    *hlen = 1 + nb;
    return len;
}

int rsa_extract_pubkey(const uint8_t *cert, int cert_len, rsa_pubkey_t *key)
{
    mz(key, sizeof(*key));
    key->exponent = 65537;

    /*
     * Walk DER to find SubjectPublicKeyInfo -> RSAPublicKey.
     * Look for the BIT STRING (tag 0x03) containing the RSA key.
     * The modulus is the first INTEGER in the nested SEQUENCE.
     */
    const uint8_t *p = cert;
    int rem = cert_len;

    /* Search for BIT STRING tag (0x03) followed by a SEQUENCE */
    for (int i = 0; i < rem - 20; i++) {
        if (p[i] == 0x03 && i > 10) {
            int hlen;
            int blen = asn1_len(p+i+1, rem-i-1, &hlen);
            if (blen < 20 || blen > rem - i) continue;

            const uint8_t *bs = p + i + 1 + hlen;
            /* Skip unused-bits byte */
            if (bs[0] != 0x00) continue;
            bs++; blen--;

            /* Expect SEQUENCE */
            if (bs[0] != 0x30) continue;
            int shlen;
            int slen = asn1_len(bs+1, blen-1, &shlen);
            if (slen < 10) continue;
            const uint8_t *seq = bs + 1 + shlen;

            /* First element: INTEGER = modulus */
            if (seq[0] != 0x02) continue;
            int mhlen;
            int mlen = asn1_len(seq+1, slen-1, &mhlen);
            const uint8_t *mod = seq + 1 + mhlen;

            /* Skip leading zero byte if present */
            if (mlen > 0 && mod[0] == 0x00) { mod++; mlen--; }

            if (mlen > 0 && mlen <= RSA_MAX_MOD_BYTES) {
                mc(key->modulus, mod, mlen);
                key->mod_len = mlen;

                /* Second element: INTEGER = exponent */
                const uint8_t *ep = mod + mlen;
                if (mod + mlen - (cert) < cert_len && ep[0] == 0x02) {
                    int ehlen;
                    int elen = asn1_len(ep+1, 8, &ehlen);
                    if (elen <= 4) {
                        const uint8_t *ev = ep + 1 + ehlen;
                        uint32_t exp = 0;
                        for (int j = 0; j < elen; j++)
                            exp = (exp << 8) | ev[j];
                        key->exponent = exp;
                    }
                }
                return 0;
            }
        }
    }
    return -1;
}

/* ── RSA Modular Exponentiation (big number) ─────────────────────────── */
/* Simple big-number modular exponentiation using byte arrays.
 * result = base^exp mod mod, all as big-endian byte arrays. */

/* Big-number multiply-and-mod for RSA (schoolbook, slow but correct) */
/* Working with arrays of uint32_t for slightly better performance */
#define BN_WORDS ((RSA_MAX_MOD_BYTES/4)+2)

typedef struct {
    uint32_t d[BN_WORDS];
    int n;  /* number of significant words */
} bignum_t;

static void bn_zero(bignum_t *a) { mz(a, sizeof(*a)); }

static void bn_from_bytes(bignum_t *a, const uint8_t *data, int len)
{
    bn_zero(a);
    int words = (len + 3) / 4;
    if (words > BN_WORDS) words = BN_WORDS;
    a->n = words;
    for (int i = 0; i < len && i < BN_WORDS * 4; i++) {
        int wi = (len - 1 - i) / 4;
        int bi = (len - 1 - i) % 4;
        a->d[wi] |= (uint32_t)data[i] << (bi * 8);
    }
    /* Normalize */
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
}

static void bn_to_bytes(const bignum_t *a, uint8_t *out, int len)
{
    mz(out, len);
    for (int i = 0; i < len; i++) {
        int wi = i / 4;
        int bi = i % 4;
        if (wi < BN_WORDS)
            out[len - 1 - i] = (a->d[wi] >> (bi * 8)) & 0xff;
    }
}

/* Compare: returns -1, 0, or 1 */
static int bn_cmp(const bignum_t *a, const bignum_t *b)
{
    int n = a->n > b->n ? a->n : b->n;
    for (int i = n - 1; i >= 0; i--) {
        uint32_t av = (i < a->n) ? a->d[i] : 0;
        uint32_t bv = (i < b->n) ? b->d[i] : 0;
        if (av > bv) return 1;
        if (av < bv) return -1;
    }
    return 0;
}

/* a = a - b (assumes a >= b) */
static void bn_sub(bignum_t *a, const bignum_t *b)
{
    uint64_t borrow = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        uint64_t av = (i < a->n) ? a->d[i] : 0;
        uint64_t bv = (i < b->n) ? b->d[i] : 0;
        uint64_t diff = av - bv - borrow;
        a->d[i] = (uint32_t)diff;
        borrow = (diff >> 63) & 1;
    }
    if (n > a->n) a->n = n;
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
}

/* a = a + b */
static void bn_add(bignum_t *a, const bignum_t *b)
{
    uint64_t carry = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i <= n; i++) {
        uint64_t sum = carry;
        if (i < a->n) sum += a->d[i];
        if (i < b->n) sum += b->d[i];
        a->d[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    a->n = n + 1;
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
}

/* Shift left by 1 bit */
static void bn_shl1(bignum_t *a)
{
    uint32_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint32_t nc = a->d[i] >> 31;
        a->d[i] = (a->d[i] << 1) | carry;
        carry = nc;
    }
    if (carry && a->n < BN_WORDS) {
        a->d[a->n] = carry;
        a->n++;
    }
}

/* c = a * b mod m (using Montgomery-like reduction, but simpler) */
static void bn_mulmod(bignum_t *c, const bignum_t *a, const bignum_t *b, const bignum_t *m)
{
    /* Simple: accumulate a * each bit of b, reduce mod m */
    bignum_t acc, temp;
    bn_zero(&acc);
    mc(&temp, a, sizeof(bignum_t));

    /* Reduce temp mod m first */
    while (bn_cmp(&temp, m) >= 0) bn_sub(&temp, m);

    for (int i = 0; i < b->n * 32; i++) {
        int wi = i / 32;
        int bi = i % 32;
        if (b->d[wi] & (1u << bi)) {
            bn_add(&acc, &temp);
            if (bn_cmp(&acc, m) >= 0) bn_sub(&acc, m);
        }
        bn_shl1(&temp);
        if (bn_cmp(&temp, m) >= 0) bn_sub(&temp, m);
    }
    mc(c, &acc, sizeof(bignum_t));
}

/* c = base^exp mod mod */
static void bn_modexp(bignum_t *result, const bignum_t *base,
                      uint32_t exp, const bignum_t *mod)
{
    bignum_t r, b;
    bn_zero(&r);
    r.d[0] = 1; r.n = 1;
    mc(&b, base, sizeof(bignum_t));
    /* Reduce base mod m */
    while (bn_cmp(&b, mod) >= 0) bn_sub(&b, mod);

    while (exp > 0) {
        if (exp & 1) {
            bn_mulmod(&r, &r, &b, mod);
        }
        bn_mulmod(&b, &b, &b, mod);
        exp >>= 1;
    }
    mc(result, &r, sizeof(bignum_t));
}

/* RSA PKCS#1 v1.5 encrypt: output = (0x00 || 0x02 || PS || 0x00 || data)^e mod n */
int rsa_pkcs1_encrypt(const rsa_pubkey_t *key,
                      const uint8_t *data, int data_len,
                      uint8_t *output, int max_out)
{
    int k = key->mod_len;
    if (k > max_out) return -1;
    if (data_len > k - 11) return -1;  /* Too long for PKCS#1 */

    /* Build PKCS#1 v1.5 type 2 block */
    uint8_t em[RSA_MAX_MOD_BYTES];
    mz(em, k);
    em[0] = 0x00;
    em[1] = 0x02;
    /* PS: non-zero random padding */
    int ps_len = k - data_len - 3;
    for (int i = 0; i < ps_len; i++) {
        uint8_t r;
        do { r = (uint8_t)(tls_random() & 0xFF); } while (r == 0);
        em[2 + i] = r;
    }
    em[2 + ps_len] = 0x00;
    mc(em + 3 + ps_len, data, data_len);

    /* RSA: result = em^e mod n */
    bignum_t base_bn, mod_bn, result_bn;
    bn_from_bytes(&base_bn, em, k);
    bn_from_bytes(&mod_bn, key->modulus, k);
    bn_modexp(&result_bn, &base_bn, key->exponent, &mod_bn);
    bn_to_bytes(&result_bn, output, k);

    return k;
}

/* ── PRNG ────────────────────────────────────────────────────────────── */
static uint32_t prng_state = 0x5A5A5A5A;

uint32_t tls_random(void)
{
    prng_state ^= (uint32_t)timer_get_ticks();
    prng_state = prng_state * 1103515245 + 12345;
    prng_state ^= prng_state >> 16;
    return prng_state;
}

void tls_random_bytes(uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++)
        buf[i] = (uint8_t)(tls_random() & 0xFF);
}
