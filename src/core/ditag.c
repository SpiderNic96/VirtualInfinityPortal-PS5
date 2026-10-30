/* Disney Infinity figure tags (see ditag.h). Layout follows Dolphin's / RPCS3's figure creator:
 *   block 0      UID (7 bytes) 89 44 00 C2 ...
 *   block 1      AES-128-ECB( 00 ID ID ID YY MM DD 00 00 SS D1 1F CRC32_BE )
 *   blocks 4, 8, 12, 13   AES(16 zero bytes)
 *   blocks 3, 7, 11, 15, 19 (sector trailers) access bits 17 87 8E / 77 87 88 at +6
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include "ditag.h"
#include <string.h>

/* ------------------------------------------------------------------ SHA-1 */

static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void ditag_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    uint8_t tail[128];
    size_t rem = len - i;
    memset(tail, 0, sizeof tail);
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t tl = (rem + 9 <= 64) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail);
    if (tl == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) {
        out[k * 4] = (uint8_t)(h[k] >> 24); out[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(h[k] >> 8); out[k * 4 + 3] = (uint8_t)h[k];
    }
}

/* ---------------------------------------------------------------- AES-128 */

static const uint8_t SBOX[256] = {
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
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t INV[256];
static int inv_ready;

static uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

static void expand(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4] = {rk[(i - 1) * 4], rk[(i - 1) * 4 + 1], rk[(i - 1) * 4 + 2], rk[(i - 1) * 4 + 3]};
        if (i % 4 == 0) {
            uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon[i / 4 - 1]; t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[u];
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
    }
}

/* state is column-major: s[c*4 + r] */
static void shift_rows(uint8_t s[16], int inv) {
    uint8_t t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            int src = inv ? (c - r + 4) % 4 : (c + r) % 4;
            t[c * 4 + r] = s[src * 4 + r];
        }
    memcpy(s, t, 16);
}

static void mix(uint8_t s[16], int inv) {
    for (int c = 0; c < 4; c++) {
        uint8_t *p = s + c * 4, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        if (!inv) {
            p[0] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
            p[1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
            p[2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
            p[3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
        } else {
            p[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
            p[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
            p[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
            p[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
        }
    }
}

void ditag_aes_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[176], s[16];
    expand(key, rk);
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        shift_rows(s, 0);
        if (r < 10) mix(s, 0);
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

void ditag_aes_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    if (!inv_ready) { for (int i = 0; i < 256; i++) INV[SBOX[i]] = (uint8_t)i; inv_ready = 1; }
    uint8_t rk[176], s[16];
    expand(key, rk);
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        shift_rows(s, 1);
        for (int i = 0; i < 16; i++) s[i] = INV[s[i]];
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
        if (r > 0) mix(s, 1);
    }
    memcpy(out, s, 16);
}

/* --------------------------------------------------------------- CRC / key */

/* reflected CRC-32 (0xEDB88320), initial value 0, no final xor - what the figures use */
uint32_t ditag_crc32(const uint8_t *buf, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

static const uint8_t SALT[31] = {0xAF, 0x62, 0xD2, 0xEC, 0x04, 0x91, 0x96, 0x8C, 0xC5, 0x2A, 0x1A,
                                 0x71, 0x65, 0xF8, 0x65, 0xFE, '(', 'c', ')', ' ', 'D', 'i',
                                 's', 'n', 'e', 'y', ' ', '2', '0', '1', '3'};

void ditag_key(const uint8_t uid[7], uint8_t key[16]) {
    uint8_t in[38], d[20];
    memcpy(in, SALT, 31);
    memcpy(in + 31, uid, 7);
    ditag_sha1(in, sizeof in, d);
    for (int w = 0; w < 4; w++)
        for (int b = 0; b < 4; b++) key[w * 4 + b] = d[w * 4 + 3 - b];
}

int ditag_create(uint8_t out[DITAG_SIZE], uint32_t id, int series, const uint8_t uid7[7]) {
    if (!id || id > 0xFFFFFF) return -1;
    memset(out, 0, DITAG_SIZE);
    /* sector trailers: standard Infinity access bits */
    out[0x36] = 0x17; out[0x37] = 0x87; out[0x38] = 0x8E;
    for (int s = 1; s < 5; s++) { out[s * 0x40 + 0x36] = 0x77; out[s * 0x40 + 0x37] = 0x87; out[s * 0x40 + 0x38] = 0x88; }
    memcpy(out, uid7, 7);
    out[7] = 0x89; out[8] = 0x44; out[9] = 0x00; out[10] = 0xC2;

    uint8_t blk[16] = {0};
    blk[1] = (uint8_t)(id >> 16); blk[2] = (uint8_t)(id >> 8); blk[3] = (uint8_t)id;
    /* manufacture date YY MM DD: the release date of the figure's series */
    if (series == 3)      { blk[4] = 0x0F; blk[5] = 0x08; blk[6] = 0x1C; }
    else if (series == 2) { blk[4] = 0x0E; blk[5] = 0x09; blk[6] = 0x12; }
    else                  { blk[4] = 0x0D; blk[5] = 0x08; blk[6] = 0x12; series = 1; }
    blk[9] = (uint8_t)series; blk[10] = 0xD1; blk[11] = 0x1F;
    uint32_t crc = ditag_crc32(blk, 12);
    blk[12] = (uint8_t)(crc >> 24); blk[13] = (uint8_t)(crc >> 16); blk[14] = (uint8_t)(crc >> 8); blk[15] = (uint8_t)crc;

    uint8_t key[16], zero[16] = {0}, enc_blank[16];
    ditag_key(uid7, key);
    ditag_aes_encrypt(key, blk, out + 16);
    ditag_aes_encrypt(key, zero, enc_blank);
    memcpy(out + 16 * 4, enc_blank, 16);
    memcpy(out + 16 * 8, enc_blank, 16);
    memcpy(out + 16 * 12, enc_blank, 16);
    memcpy(out + 16 * 13, enc_blank, 16);
    return 0;
}

uint32_t ditag_figure_id(const uint8_t tag[DITAG_SIZE], int *series_out) {
    uint8_t key[16], blk[16];
    ditag_key(tag, key);
    ditag_aes_decrypt(key, tag + 16, blk);
    uint32_t crc = ditag_crc32(blk, 12);
    uint32_t want = (uint32_t)blk[12] << 24 | (uint32_t)blk[13] << 16 | (uint32_t)blk[14] << 8 | blk[15];
    if (crc != want) return 0;
    if (series_out) *series_out = blk[9];
    return (uint32_t)blk[1] << 16 | (uint32_t)blk[2] << 8 | blk[3];
}
