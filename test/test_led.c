/* LED model: prints the colour of a set of animations over time; test_led.js replays the same
 * cases through the phone remote's JavaScript and both outputs must match.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "../src/core/ledanim.h"

static void check(const char *what, const led_anim *s, uint64_t t, int unit, int r, int g, int b) {
    uint8_t c[3];
    led_eval(s, s->t0 + t, unit, c);
    if (c[0] != r || c[1] != g || c[2] != b) { printf("FAIL %s t=%llu: %d %d %d\n", what, (unsigned long long)t, c[0], c[1], c[2]); assert(0); }
}

int main(void) {
    led_anim s = {0};
    di_led l = {0};
    const int U = 25;
    /* solid */
    l.mode = DI_LED_SOLID; l.rgb[0] = 255; l.seq = 1;
    led_update(&s, &l, 1000, U);
    check("solid", &s, 0, U, 255, 0, 0);
    /* fade red -> blue, 4 units x 3 fades: ends on blue */
    l.mode = DI_LED_FADE; l.rgb[0] = 0; l.rgb[2] = 255; l.a = 4; l.b = 3; l.seq = 2;
    led_update(&s, &l, 2000, U);
    check("fade start", &s, 0, U, 255, 0, 0);
    check("fade half", &s, 50, U, 128, 0, 128);
    check("fade back", &s, 100, U, 0, 0, 255);
    check("fade 2nd half", &s, 150, U, 128, 0, 128);
    check("fade end", &s, 300, U, 0, 0, 255);
    /* even count returns to the old colour */
    l.mode = DI_LED_FADE; l.rgb[1] = 255; l.rgb[2] = 0; l.a = 2; l.b = 2; l.seq = 3;
    led_update(&s, &l, 3000, U);
    check("pulse end", &s, 1000, U, 0, 0, 255);
    /* a command mid-animation starts from the colour on show */
    l.mode = DI_LED_FADE; l.rgb[0] = 255; l.rgb[1] = 255; l.rgb[2] = 255; l.a = 8; l.b = 1; l.seq = 4;
    led_update(&s, &l, 4000, U);
    l.mode = DI_LED_SOLID; l.rgb[0] = 9; l.rgb[1] = 9; l.rgb[2] = 9; l.seq = 5;
    led_update(&s, &l, 4100, U);   /* half way: (0,0,255) -> (255,255,255) */
    check("mid from", &s, 0, U, 9, 9, 9);
    assert(s.from[0] == 128 && s.from[1] == 128 && s.from[2] == 255);
    /* flash on 2 off 2 x5: on off on off on, ends on the new colour; same colour flashes to black */
    l.mode = DI_LED_FLASH; l.rgb[0] = 9; l.rgb[1] = 9; l.rgb[2] = 9; l.a = 2; l.b = 2; l.c = 5; l.seq = 6;
    led_update(&s, &l, 5000, U);
    check("flash on", &s, 10, U, 9, 9, 9);
    check("flash off", &s, 60, U, 0, 0, 0);
    check("flash on2", &s, 110, U, 9, 9, 9);
    check("flash end", &s, 999, U, 9, 9, 9);
    /* forever */
    l.mode = DI_LED_FADE; l.rgb[0] = 200; l.rgb[1] = 0; l.rgb[2] = 0; l.a = 1; l.b = LED_FOREVER; l.seq = 7;
    led_update(&s, &l, 6000, U);
    uint8_t c[3];
    assert(led_eval(&s, 6000 + 100000, U, c) == 1);
    /* table for the JavaScript twin */
    const uint8_t CASES[][9] = {{2, 7, 3, 0, 255, 160, 0, 10, 30}, {2, 16, 255, 0, 0, 200, 255, 50, 0}, {3, 3, 5, 7, 255, 0, 0, 0, 0},
                                {3, 1, 9, 255, 10, 200, 90, 30, 30}, {1, 0, 0, 0, 1, 2, 3, 4, 5}};
    FILE *f = fopen(getenv("LED_OUT") ? getenv("LED_OUT") : "/dev/null", "w");
    for (size_t k = 0; k < sizeof CASES / sizeof *CASES; k++) {
        led_anim a = {1, 1, CASES[k][0], CASES[k][1], CASES[k][2], CASES[k][3], {CASES[k][7], CASES[k][8], 20}, {CASES[k][4], CASES[k][5], CASES[k][6]}, 0};
        for (int t = 0; t <= 3000; t += 7) {
            led_eval(&a, (uint64_t)t, 23, c);
            fprintf(f, "%zu %d %d %d %d\n", k, t, c[0], c[1], c[2]);
        }
    }
    fclose(f);
    printf("led model ok\n");
    return 0;
}
