/*
 * Blowfish.dll's COM object, in C.
 *
 * Red Alert 2's MIX archives have Blowfish-encrypted headers, and the game
 * decrypts them through the COM server in Blowfish.dll (Westwood's
 * BlowfishEngine behind a five-method interface). That DLL's code is not
 * lifted, so its object is served here:
 *
 *   3  SetKey(len, key)          up to 56 bytes
 *   4  MaxKeyLength(&n)          56
 *   5  BlockSize(&n)             8
 *   6  Encrypt(len, in, out)     ECB, whole 8-byte blocks; a tail is copied
 *   7  Decrypt(len, in, out)
 *
 * The cipher is standard Blowfish (big-endian halves). Its initial P-array
 * and S-boxes (the digits of pi) are read from the game's own Blowfish.dll,
 * where they sit together, so none are copied into this repository.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

static uint32_t g_init[18 + 1024];
static int g_have_init;

typedef struct { uint32_t p[18], s[4][256]; int keyed; } bf_t;
#define BF(self) ((bf_t *)(uintptr_t)MEM32((self) + 8))

static uint32_t f(const bf_t *b, uint32_t x) {
    return ((b->s[0][x >> 24] + b->s[1][(x >> 16) & 255]) ^ b->s[2][(x >> 8) & 255]) + b->s[3][x & 255];
}
static void enc(const bf_t *b, uint32_t *l, uint32_t *r) {
    uint32_t L = *l, R = *r;
    for (int i = 0; i < 16; i += 2) {
        L ^= b->p[i], R ^= f(b, L);
        R ^= b->p[i + 1], L ^= f(b, R);
    }
    *l = R ^ b->p[17], *r = L ^ b->p[16];
}
static void dec(const bf_t *b, uint32_t *l, uint32_t *r) {
    uint32_t L = *l, R = *r;
    for (int i = 17; i > 1; i -= 2) {
        L ^= b->p[i], R ^= f(b, L);
        R ^= b->p[i - 1], L ^= f(b, R);
    }
    *l = R ^ b->p[0], *r = L ^ b->p[1];
}
static void set_key(bf_t *b, const uint8_t *key, int len) {
    memcpy(b->p, g_init, sizeof b->p);
    memcpy(b->s, g_init + 18, sizeof b->s);
    for (int i = 0, k = 0; i < 18; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++, k = (k + 1) % len) v = v << 8 | key[k];
        b->p[i] ^= v;
    }
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) enc(b, &l, &r), b->p[i] = l, b->p[i + 1] = r;
    for (int s = 0; s < 4; s++)
        for (int i = 0; i < 256; i += 2) enc(b, &l, &r), b->s[s][i] = l, b->s[s][i + 1] = r;
    b->keyed = 1;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

static void process(int decrypt) {                       /* (this, len, in, out) */
    bf_t *b = BF(A32(0));
    int len = (int)A32(1);
    const uint8_t *in = (const uint8_t *)APTR(2);
    uint8_t *out = (uint8_t *)APTR(3);
    if (!in || !out) RET(0x80004003u, 4);
    if (len < 0) RET(0x80070057u, 4);
    if (in != out) memmove(out, in, (size_t)len);
    if (b->keyed)
        for (int k = 0; k + 8 <= len; k += 8) {
            uint32_t l = be32(out + k), r = be32(out + k + 4);
            if (decrypt) dec(b, &l, &r); else enc(b, &l, &r);
            put_be32(out + k, l), put_be32(out + k + 4, r);
        }
    RET(0, 4);
}
static void m_SetKey(void) {                             /* (this, len, key) */
    int len = (int)A32(1);
    if (!A32(2)) RET(0x80004003u, 3);
    if (len < 0 || len > 56) RET(0x80070057u, 3);
    if (len) set_key(BF(A32(0)), (const uint8_t *)APTR(2), len);
    RET(0, 3);
}
static void m_MaxKeyLength(void) { if (!A32(1)) RET(0x80004003u, 2); MEM32(A32(1)) = 56; RET(0, 2); }
static void m_BlockSize(void)    { if (!A32(1)) RET(0x80004003u, 2); MEM32(A32(1)) = 8; RET(0, 2); }
static void m_Encrypt(void) { process(0); }
static void m_Decrypt(void) { process(1); }
static void m_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) { free(BF(self)); free((void *)(uintptr_t)self); }
    RET(n, 1);
}

/* the initial tables: P then S, after the P-array's first two words */
static int load_init(const char *game_dir) {
    char path[1024];
    snprintf(path, sizeof path, "C:\\Blowfish.dll");
    char host[1024];
    if (!hle_host_path(path, host, sizeof host)) { fprintf(stderr, "[blowfish] no Blowfish.dll in %s\n", game_dir); return 0; }
    FILE *fp = fopen(host, "rb");
    if (!fp) return 0;
    static uint8_t d[1 << 20];
    size_t n = fread(d, 1, sizeof d, fp);
    fclose(fp);
    for (size_t i = 0; i + sizeof g_init <= n; i += 4)
        if (*(uint32_t *)(d + i) == 0x243F6A88u && *(uint32_t *)(d + i + 4) == 0x85A308D3u) {
            memcpy(g_init, d + i, sizeof g_init);
            return g_init[18 + 1023] == 0x3AC372E6u;     /* the last S-box word, as the cipher has it */
        }
    return 0;
}

uint32_t ra2_blowfish_create(const uint8_t *iid, uint32_t *out) {
    static uint32_t vt;
    (void)iid;
    if (!g_have_init) g_have_init = load_init("the game folder") ? 1 : -1;
    if (g_have_init < 0) { *out = 0; return 0x80004005u; }
    if (!vt) {
        static const win32hle_shim m[] = {
            { "Blowfish::QueryInterface", hle_com_QueryInterface }, { "Blowfish::AddRef", hle_com_AddRef },
            { "Blowfish::Release", m_Release }, { "Blowfish::SetKey", m_SetKey },
            { "Blowfish::MaxKeyLength", m_MaxKeyLength }, { "Blowfish::BlockSize", m_BlockSize },
            { "Blowfish::Encrypt", m_Encrypt }, { "Blowfish::Decrypt", m_Decrypt }, { 0, 0 } };
        vt = hle_com_vtable(m);
    }
    uint32_t o = hle_com_new(vt, 12);
    MEM32(o + 8) = (uint32_t)(uintptr_t)calloc(1, sizeof(bf_t));
    *out = o;
    return 0;
}
