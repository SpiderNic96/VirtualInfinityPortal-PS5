/* Favourite figures (see favs.h).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include "favs.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAV_MAX 400
static uint32_t g_f[FAV_MAX];
static int g_n;
static uint32_t g_gen;
static char g_path[160] = "/data/dimod/favorites.txt";
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static void save(void) {
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_n; i++) fprintf(f, "%u\n", g_f[i]);
    fclose(f);
    rename(tmp, g_path);
}

void favs_load(const char *path) {
    pthread_mutex_lock(&g_mx);
    snprintf(g_path, sizeof g_path, "%s", path);
    g_n = 0;
    FILE *f = fopen(g_path, "r");
    if (f) {
        unsigned id;
        while (g_n < FAV_MAX && fscanf(f, "%u", &id) == 1) if (id) g_f[g_n++] = id;
        fclose(f);
    }
    g_gen++;
    pthread_mutex_unlock(&g_mx);
}

static int find(uint32_t id) { for (int i = 0; i < g_n; i++) if (g_f[i] == id) return i; return -1; }

int favs_is(uint32_t id) {
    pthread_mutex_lock(&g_mx);
    int r = find(id) >= 0;
    pthread_mutex_unlock(&g_mx);
    return r;
}

int favs_set(uint32_t id, int on) {
    pthread_mutex_lock(&g_mx);
    int i = find(id);
    if (on && i < 0 && g_n < FAV_MAX) g_f[g_n++] = id;
    else if (!on && i >= 0) { memmove(g_f + i, g_f + i + 1, (size_t)(g_n - i - 1) * sizeof *g_f); g_n--; }
    int r = find(id) >= 0;
    g_gen++;
    save();
    pthread_mutex_unlock(&g_mx);
    return r;
}

int favs_toggle(uint32_t id) { return favs_set(id, !favs_is(id)); }

int favs_list(uint32_t *out, int max) {
    pthread_mutex_lock(&g_mx);
    int n = g_n < max ? g_n : max;
    memcpy(out, g_f, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_mx);
    return n;
}

uint32_t favs_gen(void) { return g_gen; }
