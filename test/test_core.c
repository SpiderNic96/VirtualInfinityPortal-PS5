/* Host tests for the protocol + tag code. Prints vectors for test/ref_check.py to cross-check
 * against an independent Python implementation (hashlib / cryptography / RPCS3 algorithms).
 * Build: cc -O1 -Wall -I../src/core test_core.c ../src/core/diproto.c ../src/core/ditag.c */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "diproto.h"
#include "ditag.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void hex(const char *tag, const uint8_t *d, int n) {
    printf("%s ", tag);
    for (int i = 0; i < n; i++) printf("%02x", d[i]);
    printf("\n");
}

static void out_cmd(di_base *b, uint8_t cmd, uint8_t seq, const uint8_t *data, int n) {
    uint8_t p[32] = {0};
    p[0] = 0xFF; p[1] = (uint8_t)(n + 2); p[2] = cmd; p[3] = seq;
    if (n) memcpy(p + 4, data, n);
    p[4 + n] = di_checksum(p, 4 + n);
    di_write(b, p, 32);
}

static int valid_reply(const uint8_t r[32]) {
    int len = r[1];
    if (r[0] == 0xAA) return r[2 + len] == di_checksum(r, 2 + len) || len == 0x15;   /* 0x80 reply is fixed */
    if (r[0] == 0xAB) return r[6] == di_checksum(r, 6);
    return 0;
}

int main(void) {
    static di_base b;
    di_init(&b);
    uint8_t r[32];

    /* --- the fixed activate reply: its last byte really is the checksum of the rest */
    out_cmd(&b, 0x80, 0x01, (const uint8_t *)"(c) Disney 2013", 15);
    CHECK(di_read(&b, r));
    CHECK(r[0] == 0xAA && r[1] == 0x15 && r[23] == di_checksum(r, 23));
    CHECK(b.activated);

    /* --- scramble / descramble round trip and RPCS3 vectors (printed) */
    for (uint32_t v = 1; v < 0xF0000000u; v = v * 7 + 13) CHECK(di_descramble(di_scramble(v, 0x5A5A5A5Au)) == v);
    printf("scr %016llx %016llx\n", (unsigned long long)di_scramble(0x12345678u, 0), (unsigned long long)di_scramble(0xDEADBEEFu, 0xFFFFFFFFu));
    printf("des %08x\n", di_descramble(0x0123456789ABCDEFull));

    /* --- challenge: 0x81 with a known 8-byte value, then three 0x83 */
    uint8_t seedbytes[8] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
    out_cmd(&b, 0x81, 0x02, seedbytes, 8);
    CHECK(di_read(&b, r) && r[0] == 0xAA && r[1] == 1 && r[2] == 2 && valid_reply(r));
    for (int i = 0; i < 3; i++) {
        out_cmd(&b, 0x83, (uint8_t)(3 + i), 0, 0);
        CHECK(di_read(&b, r) && r[1] == 9 && r[2] == 3 + i && valid_reply(r));
        hex("chal", r + 3, 8);
    }

    /* --- tags: create, decode, key, CRC */
    uint8_t uid[7] = {0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6}, tag[320];
    CHECK(ditag_create(tag, 1000001, 1, uid) == 0);
    int series = 0;
    CHECK(ditag_figure_id(tag, &series) == 1000001 && series == 1);
    hex("tag", tag, 320);
    uint8_t key[16];
    ditag_key(uid, key);
    hex("key", key, 16);
    CHECK(ditag_create(tag, 4000233, 3, uid) == 0 && ditag_figure_id(tag, &series) == 4000233 && series == 3);
    tag[20] ^= 1;
    CHECK(ditag_figure_id(tag, 0) == 0);   /* corrupted block 1 is rejected */

    /* --- placement + reads: slot 3 (P1 figure), slot 0 (hexagon), slot 4 (P1 disc) */
    static di_base c;
    di_init(&c);
    uint8_t t1[320], t2[320], t3[320];
    ditag_create(t1, 1000001, 1, uid);
    uid[6]++; ditag_create(t2, 2000001, 1, uid);
    uid[6]++; ditag_create(t3, 3000003, 1, uid);
    CHECK(di_place(&c, 3, t1, 1000001) == 0);
    CHECK(di_place(&c, 3, t1, 1000001) == -2);       /* occupied */
    CHECK(di_place(&c, 0, t2, 2000001) == 0);
    CHECK(di_place(&c, 4, t3, 3000003) == 0);
    /* events: AB 04 pad 09 order 00 */
    CHECK(di_read(&c, r) && r[0] == 0xAB && r[2] == 2 && r[4] == 0 && r[5] == 0 && valid_reply(r));
    CHECK(di_read(&c, r) && r[0] == 0xAB && r[2] == 1 && r[4] == 1 && valid_reply(r));
    CHECK(di_read(&c, r) && r[0] == 0xAB && r[2] == 2 && r[4] == 2 && valid_reply(r));
    CHECK(!di_read(&c, r));
    /* present list: 0x10+1 (hex, order 1), 0x20+0, 0x20+2 */
    out_cmd(&c, 0xA1, 0x10, 0, 0);
    CHECK(di_read(&c, r) && valid_reply(r));
    CHECK(r[1] == 7 && r[3] == 0x11 && r[4] == 9 && r[5] == 0x20 && r[7] == 0x22);
    /* UID by order */
    uint8_t q[3] = {0, 0, 0};
    out_cmd(&c, 0xB4, 0x11, q, 1);
    CHECK(di_read(&c, r) && valid_reply(r) && r[1] == 9 && !memcmp(r + 4, t1, 7));
    /* block 0 -> file block 1, block 1 -> file block 4 */
    q[0] = 0; q[1] = 0;
    out_cmd(&c, 0xA2, 0x12, q, 3);
    CHECK(di_read(&c, r) && valid_reply(r) && r[1] == 0x12 && !memcmp(r + 4, t1 + 16, 16));
    q[1] = 1;
    out_cmd(&c, 0xA2, 0x13, q, 3);
    CHECK(di_read(&c, r) && !memcmp(r + 4, t1 + 64, 16));
    /* write block 2 (file block 8) to order 0, dirty flag, read back */
    uint8_t w[19] = {0, 2, 0};
    for (int i = 0; i < 16; i++) w[3 + i] = (uint8_t)(0xC0 + i);
    out_cmd(&c, 0xA3, 0x14, w, 19);
    CHECK(di_read(&c, r) && valid_reply(r) && r[1] == 2 && r[3] == 0);
    CHECK(c.fig[3].dirty && !memcmp(c.fig[3].data + 128, w + 3, 16));
    /* LEDs: solid on pad 2, fade on all, flash pad 1 */
    uint8_t l1[4] = {2, 0xFF, 0x10, 0x20};
    out_cmd(&c, 0x90, 0x20, l1, 4);
    CHECK(c.led[1].mode == DI_LED_SOLID && c.led[1].rgb[0] == 0xFF && c.led[1].rgb[2] == 0x20);
    uint8_t l2[6] = {0, 0x10, 0x03, 1, 2, 3};
    out_cmd(&c, 0x92, 0x21, l2, 6);
    CHECK(c.led[0].mode == DI_LED_FADE && c.led[2].rgb[2] == 3 && c.led[1].from[0] == 0xFF && c.led[1].a == 0x10);
    uint8_t l3[7] = {1, 5, 6, 7, 9, 8, 7};
    out_cmd(&c, 0x93, 0x22, l3, 7);
    CHECK(c.led[0].mode == DI_LED_FLASH && c.led[0].c == 7 && c.led[0].rgb[0] == 9);
    for (int i = 0; i < 3; i++) CHECK(di_read(&c, r) && r[1] == 1 && valid_reply(r));
    /* remove + re-add keeps the position's order; event says removed */
    CHECK(di_remove(&c, 3) == 0 && di_remove(&c, 3) == -1);
    CHECK(di_read(&c, r) && r[0] == 0xAB && r[2] == 2 && r[4] == 0 && r[5] == 1);
    CHECK(di_place(&c, 3, t1, 1000001) == 0 && di_read(&c, r) && r[4] == 0 && r[5] == 0);
    /* reads of a slot that is empty return zero data */
    di_remove(&c, 0); di_read(&c, r);
    q[0] = 1; q[1] = 0;
    out_cmd(&c, 0xA2, 0x30, q, 3);
    uint8_t z[16] = {0};
    CHECK(di_read(&c, r) && !memcmp(r + 4, z, 16));
    /* unknown command still gets a blank reply */
    out_cmd(&c, 0x77, 0x31, 0, 0);
    CHECK(di_read(&c, r) && r[1] == 1 && r[2] == 0x31 && c.unknown == 1);
    /* overflow drops oldest instead of corrupting */
    for (int i = 0; i < 40; i++) out_cmd(&c, 0xB5, (uint8_t)i, 0, 0);
    int n = 0;
    while (di_read(&c, r)) n++;
    CHECK(n == DI_QMAX);

    printf(fails ? "core: %d FAILED\n" : "core: all checks passed\n", fails);
    return fails != 0;
}
