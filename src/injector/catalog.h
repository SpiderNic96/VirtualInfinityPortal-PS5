/* Figure catalog (web/ui/catalog.tsv): id, series, kind, name, section (power discs: Vehicles, Skies...),
 * fx ("saber": a Light FX figure whose blade glows with the pad light).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* kinds: what part of the base an item sits on */
enum { K_FIGURE = 'F', K_PIECE = 'P', K_ROUND = 'R', K_HEX = 'H' };

typedef struct { uint32_t id; int series; char kind; char name[64]; char group[48]; uint8_t fx; float foot[3]; char aka[96]; } cat_entry;   /* aka: extra search words */

/* fx: the picture has a light effect layer (g<id>.png) */
enum { FX_NONE, FX_SABER, FX_GLOW };

int  catalog_load(const char *path);       /* returns entries loaded (falls back to id ranges) */
int  catalog_count(void);
const cat_entry *catalog_at(int i);
const cat_entry *catalog_find(uint32_t id);
char catalog_kind_of(uint32_t id);         /* from the catalog, else from the id range */
int  catalog_series_of(uint32_t id);       /* 1..3, 1 if unknown */
const char *catalog_name(uint32_t id, char *buf, int n);
/* 1 if an item of this id may sit on base slot 0..8 */
int  catalog_slot_ok(uint32_t id, int slot);
const char *catalog_kind_label(char kind);
/* the power disc's section ("Vehicles", "Boosts"...), "" for anything else */
const char *catalog_group(uint32_t id);
/* FX_SABER: Light FX blade lit by the pad; FX_GLOW: glow-in-the-dark; else FX_NONE */
int catalog_fx(uint32_t id);
/* where a standing figure / play set piece touches the ground in its badge: centre x, bottom y,
 * base width (fractions of the badge). 0 if the catalog has no foot for it (defaults filled in) */
int catalog_foot(uint32_t id, float *cx, float *by, float *bw);
