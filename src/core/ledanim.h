/* Infinity base LEDs as they look on the real base.
 *
 * The game drives three lights (0 hexagon, 1 left pad, 2 right pad) with three commands:
 *   0x90 colour   pad r g b                     -> the light jumps to the colour
 *   0x92 fade     pad time count r g b          -> `count` fades of `time` units each, alternating
 *                                                  between the colour shown before and the new one
 *   0x93 flash    pad on off count r g b        -> `count` phases, alternating new colour (`on`
 *                                                  units) and the colour shown before (`off` units)
 * An odd count leaves the light on the new colour, an even count back on the old one, 0xFF
 * repeats until the next command. Every animation starts from the colour *shown* at that moment
 * (a fade started half way through a flash starts from wherever the flash was), so this runs on
 * the payload's clock, not in the game. web/index.html has the same model in JavaScript.
 *
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <string.h>
#include "diproto.h"

#define LED_FOREVER 0xFF
#define LED_UNIT_MS_DEFAULT 25   /* one protocol time unit, in ms (led_unit_ms in config.ini) */

typedef struct {
    int      valid;              /* a command was seen */
    uint32_t seq;                /* di_led.seq it was built from */
    uint8_t  mode;               /* DI_LED_* */
    uint8_t  a, b, c;            /* raw parameters */
    uint8_t  from[3], to[3];     /* colour shown when the command arrived, new colour */
    uint64_t t0;                 /* when it arrived (ms) */
} led_anim;

static inline void led_mix(const uint8_t a[3], const uint8_t b[3], float f, uint8_t o[3]) {
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (b[i] - a[i]) * f + 0.5f);
}

/* the colour on the light at `now`; returns 1 while it is still changing */
static inline int led_eval(const led_anim *s, uint64_t now, int unit, uint8_t out[3]) {
    memset(out, 0, 3);
    if (!s->valid || s->mode == DI_LED_OFF) return 0;
    if (unit <= 0) unit = LED_UNIT_MS_DEFAULT;
    uint64_t t = now > s->t0 ? now - s->t0 : 0;
    if (s->mode == DI_LED_FADE) {
        uint64_t dur = (uint64_t)(s->a ? s->a : 1) * (uint64_t)unit;
        uint32_t n = s->b ? s->b : 1;
        uint64_t k = t / dur;
        if (n != LED_FOREVER && k >= n) { memcpy(out, (n & 1) ? s->to : s->from, 3); return 0; }
        float f = (float)(t - k * dur) / (float)dur;
        if (k & 1) f = 1.0f - f;
        led_mix(s->from, s->to, f, out);
        return 1;
    }
    if (s->mode == DI_LED_FLASH) {
        uint64_t on = (uint64_t)(s->a ? s->a : 1) * (uint64_t)unit, off = (uint64_t)(s->b ? s->b : 1) * (uint64_t)unit;
        uint32_t n = s->c ? s->c : 1;
        uint64_t k = t / (on + off), r = t - k * (on + off);
        uint64_t phase = 2 * k + (r >= on);               /* 0 on, 1 off, 2 on ... */
        if (n != LED_FOREVER && phase >= n) { memcpy(out, (n & 1) ? s->to : s->from, 3); return 0; }
        memcpy(out, (phase & 1) ? s->from : s->to, 3);
        return 1;
    }
    memcpy(out, s->to, 3);
    return 0;
}

/* feed the latest LED record read from the game */
static inline void led_update(led_anim *s, const di_led *l, uint64_t now, int unit) {
    if (s->valid && l->seq == s->seq) return;
    if (!l->seq) { memset(s, 0, sizeof *s); return; }   /* no command yet: the light is off */
    uint8_t cur[3];
    led_eval(s, now, unit, cur);
    memcpy(s->from, cur, 3);
    memcpy(s->to, l->rgb, 3);
    /* flashing a colour that is already on: the real base blinks it off */
    if (l->mode == DI_LED_FLASH && !memcmp(s->from, s->to, 3)) memset(s->from, 0, 3);
    s->mode = l->mode; s->a = l->a; s->b = l->b; s->c = l->c;
    s->t0 = now; s->seq = l->seq; s->valid = 1;
}

/* 0..1 brightness of a colour (the brightest channel) */
static inline float led_level(const uint8_t c[3]) {
    int m = c[0] > c[1] ? c[0] : c[1];
    if (c[2] > m) m = c[2];
    return m / 255.0f;
}
