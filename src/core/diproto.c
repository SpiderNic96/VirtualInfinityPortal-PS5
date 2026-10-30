/* Disney Infinity base protocol (see diproto.h).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include "diproto.h"

static void cpy(uint8_t *d, const uint8_t *s, int n) { while (n-- > 0) *d++ = *s++; }
static void zero(void *p, unsigned long n) { uint8_t *d = p; while (n--) *d++ = 0; }

uint8_t di_checksum(const uint8_t *d, int n) {
    unsigned s = 0;
    for (int i = 0; i < n; i++) s += d[i];
    return (uint8_t)s;
}

uint8_t di_pad_of(int slot) { return (slot < 0 || slot >= DI_SLOTS) ? 0 : (uint8_t)(slot / 3 + 1); }

void di_init(di_base *b) {
    zero(b, sizeof *b);
    for (int i = 0; i < DI_SLOTS; i++) b->fig[i].order = 255;
}

/* ------------------------------------------------------------------ queues */

static void push(uint8_t q[DI_QMAX][32], uint32_t *head, uint32_t *tail, const uint8_t r[32]) {
    if (*tail - *head >= DI_QMAX) (*head)++;          /* full: drop the oldest */
    cpy(q[*tail % DI_QMAX], r, 32);
    (*tail)++;
}

int di_pending(const di_base *b) { return b->e_tail != b->e_head || b->q_tail != b->q_head; }

int di_read(di_base *b, uint8_t out[32]) {
    if (b->e_tail != b->e_head) { cpy(out, b->e[b->e_head % DI_QMAX], 32); b->e_head++; return 1; }
    if (b->q_tail != b->q_head) { cpy(out, b->q[b->q_head % DI_QMAX], 32); b->q_head++; return 1; }
    return 0;
}

/* ------------------------------------------------------------- challenge */

static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

/* bob jenkins' small PRNG, as the base runs it */
uint32_t di_next(di_base *b) {
    uint32_t e = b->ra - rotl(b->rb, 27);
    b->ra = b->rb ^ rotl(b->rc, 17);
    b->rb = b->rc + b->rd;
    b->rc = b->rd + e;
    b->rd = e + b->ra;
    return b->rd;
}

void di_seed(di_base *b, uint32_t seed) {
    b->ra = 0xF1EA5EEDu;
    b->rb = b->rc = b->rd = seed;
    for (int i = 0; i < 23; i++) di_next(b);
}

#define DI_MASK 0x8E55AA1B3999E8AAull

uint32_t di_descramble(uint64_t v) {
    uint64_t mask = DI_MASK;
    uint32_t r = 0;
    for (int i = 0; i < 64; i++) {
        if (mask & 0x8000000000000000ull) r = (r << 1) | (uint32_t)(v & 1);
        v >>= 1;
        mask <<= 1;
    }
    return r;
}

uint64_t di_scramble(uint32_t v, uint32_t garbage) {
    uint64_t mask = DI_MASK, r = 0;
    for (int i = 0; i < 64; i++) {
        r <<= 1;
        if (mask & 1) { r |= v & 1; v >>= 1; }
        else { r |= garbage & 1; garbage >>= 1; }
        mask >>= 1;
    }
    return r;
}

/* --------------------------------------------------------------- replies */

static void blank(uint8_t r[32], uint8_t seq) {
    zero(r, 32);
    r[0] = 0xAA; r[1] = 0x01; r[2] = seq;
    r[3] = di_checksum(r, 3);
}

static di_figure *by_order(di_base *b, uint8_t order) {
    for (int i = 0; i < DI_SLOTS; i++)
        if (b->fig[i].order == order && b->fig[i].present) return &b->fig[i];
    for (int i = 0; i < DI_SLOTS; i++)          /* removed but still known */
        if (b->fig[i].order == order) return &b->fig[i];
    return 0;
}

/* block numbers in A2/A3 are 0 -> file block 1, n -> block 4n */
static int file_block(uint8_t block) { return block == 0 ? 1 : block * 4; }

static void led_cmd(di_base *b, uint8_t pad, uint8_t mode, const uint8_t rgb[3], uint8_t a, uint8_t bb, uint8_t c) {
    for (int p = 0; p < 3; p++) {
        if (pad != 0 && pad != p + 1) continue;
        di_led *l = &b->led[p];
        cpy(l->from, l->rgb, 3);
        cpy(l->rgb, rgb, 3);
        l->mode = mode;
        l->a = a; l->b = bb; l->c = c;
        l->seq++;
    }
}

void di_write(di_base *b, const uint8_t *buf, uint32_t len) {
    if (len < 4 || buf[0] != 0xFF) return;
    uint8_t cmd = buf[2], seq = buf[3];
    uint8_t r[32];
    blank(r, seq);
    b->cmds++;

    switch (cmd) {
    case 0x80: {   /* activate: constant reply (byte 2 is 0 on real bases, not the sequence) */
        static const uint8_t act[24] = {0xAA, 0x15, 0x00, 0x00, 0x0F, 0x01, 0x00, 0x03, 0x02, 0x09, 0x09, 0x43,
                                        0x20, 0x32, 0x62, 0x36, 0x36, 0x4B, 0x34, 0x99, 0x67, 0x31, 0x93, 0x8C};
        zero(r, 32);
        cpy(r, act, 24);
        b->activated = 1;
        break;
    }
    case 0x81: {   /* seed the challenge PRNG with the descrambled 64-bit value */
        if (len < 12) break;
        uint64_t v = 0;
        for (int i = 4; i < 12; i++) v = (v << 8) | buf[i];
        di_seed(b, di_descramble(v));
        break;
    }
    case 0x83: {   /* next challenge value, scrambled into 8 big-endian bytes */
        uint64_t v = di_scramble(di_next(b), 0);
        zero(r, 32);
        r[0] = 0xAA; r[1] = 0x09; r[2] = seq;
        for (int i = 0; i < 8; i++) r[3 + i] = (uint8_t)(v >> (56 - 8 * i));
        r[11] = di_checksum(r, 11);
        break;
    }
    case 0x90:     /* LED colour: pad, r, g, b */
        if (len >= 8) led_cmd(b, buf[4], DI_LED_SOLID, buf + 5, 0, 0, 0);
        break;
    case 0x92:     /* LED fade: pad, speed, count, r, g, b */
        if (len >= 10) led_cmd(b, buf[4], DI_LED_FADE, buf + 7, buf[5], buf[6], 0);
        break;
    case 0x93:     /* LED flash: pad, on, off, count, r, g, b */
        if (len >= 11) led_cmd(b, buf[4], DI_LED_FLASH, buf + 8, buf[5], buf[6], buf[7]);
        break;
    case 0x95: case 0x96: case 0xB5:   /* colour sequences / status: blank reply */
        break;
    case 0xA1: {   /* present tags: (pad << 4 | order), 09 for each */
        zero(r, 32);
        int x = 3;
        for (int i = 0; i < DI_SLOTS && x < 29; i++) {
            if (!b->fig[i].present) continue;
            r[x] = (uint8_t)((di_pad_of(i) << 4) + (b->fig[i].order & 0x0F));
            r[x + 1] = 0x09;
            x += 2;
        }
        r[0] = 0xAA; r[1] = (uint8_t)(x - 2); r[2] = seq;
        r[x] = di_checksum(r, x);
        break;
    }
    case 0xA2: {   /* read block: order, block, ? */
        zero(r, 32);
        r[0] = 0xAA; r[1] = 0x12; r[2] = seq; r[3] = 0x00;
        di_figure *f = len >= 6 ? by_order(b, buf[4]) : 0;
        int fb = len >= 6 ? file_block(buf[5]) : 99;
        if (f && f->present && fb < 20) cpy(r + 4, f->data + 16 * fb, 16);
        r[20] = di_checksum(r, 20);
        break;
    }
    case 0xA3: {   /* write block: order, block, ?, 16 bytes */
        zero(r, 32);
        r[0] = 0xAA; r[1] = 0x02; r[2] = seq; r[3] = 0x00;
        di_figure *f = len >= 23 ? by_order(b, buf[4]) : 0;
        int fb = len >= 23 ? file_block(buf[5]) : 99;
        if (f && f->present && fb < 20) { cpy(f->data + 16 * fb, buf + 7, 16); f->dirty = 1; }
        r[4] = di_checksum(r, 4);
        break;
    }
    case 0xB4: {   /* tag UID: order */
        zero(r, 32);
        r[0] = 0xAA; r[1] = 0x09; r[2] = seq; r[3] = 0x00;
        di_figure *f = len >= 5 ? by_order(b, buf[4]) : 0;
        if (f && f->present) cpy(r + 4, f->data, 7);
        r[11] = di_checksum(r, 11);
        break;
    }
    default:
        b->unknown++;
        b->last_unknown = cmd;
        break;
    }
    push(b->q, &b->q_head, &b->q_tail, r);
}

/* ------------------------------------------------------------ placement */

static void event(di_base *b, int slot, uint8_t removed) {
    uint8_t r[32];
    zero(r, 32);
    r[0] = 0xAB; r[1] = 0x04; r[2] = di_pad_of(slot); r[3] = 0x09;
    r[4] = b->fig[slot].order; r[5] = removed;
    r[6] = di_checksum(r, 6);
    push(b->e, &b->e_head, &b->e_tail, r);
}

int di_place(di_base *b, int slot, const uint8_t data[DI_TAG_SIZE], uint32_t fig_id) {
    if (slot < 0 || slot >= DI_SLOTS) return -1;
    di_figure *f = &b->fig[slot];
    if (f->present) return -2;
    cpy(f->data, data, DI_TAG_SIZE);
    f->fig_id = fig_id;
    f->present = 1;
    f->dirty = 0;
    if (f->order == 255) f->order = b->next_order++;
    event(b, slot, 0);
    return 0;
}

int di_remove(di_base *b, int slot) {
    if (slot < 0 || slot >= DI_SLOTS) return -1;
    di_figure *f = &b->fig[slot];
    if (!f->present) return -1;
    f->present = 0;
    event(b, slot, 1);
    return 0;
}
