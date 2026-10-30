/* Figure tag files in /data/dimod/tags (see tags.c).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

void tags_init(const char *dir);
/* the saved tag for figure `id`, created (and written) on first use; 0 = ok */
int  tags_get(uint32_t id, uint8_t out[320]);
/* write the game's updated tag back to the figure's file; 0 = ok */
int  tags_save(uint32_t id, const uint8_t data[320]);
int  tags_count(void);
