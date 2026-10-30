/* Favourite figures, shared by the TV picker and the phone remote (/data/dimod/favorites.txt).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
void favs_load(const char *path);
int  favs_is(uint32_t id);
int  favs_toggle(uint32_t id);           /* returns 1 if it is now a favourite */
int  favs_set(uint32_t id, int on);
int  favs_list(uint32_t *out, int max);  /* in the order they were added */
uint32_t favs_gen(void);                 /* bumped on every change */
