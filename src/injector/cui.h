/* Disney Infinity virtual base - TV picker drawn in the PS5 system UI (see cui.c).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

typedef struct {
    int enabled;          /* overlay=1 in config.ini */
    uint32_t peek_mask;   /* buttons held during gameplay for the sneak peek (default L2) */
    int peek_size;        /* 0 off, 1 small, 2 medium, 3 large */
    int fast_load;        /* close the picker after placing a figure */
    int tex_mode;         /* 0 auto, 1 pixel upload, 2 PNG files */
    uint32_t combo;       /* open / close buttons (still held when the picker opens) */
    int opacity;          /* 30..100 percent */
    int ui_size;          /* 0 90%, 1 100%, 2 110%, 3 120% */
    int theme;            /* CUI_THEMES: 0 Classic, 1 Clear, 2 Light, 3 Crimson, 4 Neon */
    int background;       /* CUI_BACKGROUNDS: 0 none, 1 Starfield, 2 Aurora, 3 Toy Box, 4 custom photo */
} cui_config;

#define CUI_THEMES 5
#define CUI_BACKGROUNDS 5
#define CUI_BG_CUSTOM 4
#define CUI_THEME_DIR "/data/dimod/themes"
#define CUI_CUSTOM_BG CUI_THEME_DIR "/background.png"   /* PNG, up to 1280x720 (the phone resizes it) */
const char *cui_theme_name(int i);
const char *cui_background_name(int i);
/* the custom background file was replaced or removed: reload it */
void cui_background_changed(void);
int  cui_background_gen(void);

void cui_configure(const cui_config *c);
int  cui_start(void);
void cui_get(cui_config *c);
/* change settings from the phone remote; applied at once and saved */
void cui_update(const cui_config *c);

/* the peek button choices offered in Settings */
int cui_peek_choices(void);
const char *cui_peek_choice(int i, uint32_t *mask);
const char *cui_mask_name(uint32_t mask);   /* "L2", "L1+R1", ... or "custom" */
/* the button combos offered for opening the picker */
int cui_open_choices(void);
const char *cui_open_choice(int i, uint32_t *mask);
const char *cui_open_name(uint32_t mask);

/* implemented by the payload (main.c): persists settings changed in the picker */
void cui_save_config(const cui_config *c);
