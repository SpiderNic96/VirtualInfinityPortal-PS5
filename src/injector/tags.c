/* Figure tag files. Every *.bin of 320 bytes in the tag folder is identified by decrypting its
 * block 1, so dumps of real figures and figure files made by Dolphin / RPCS3 / shadPS4 can be
 * dropped in with any file name. Missing figures are created as <id>.bin on first use; the
 * game's write-backs (levels, skills, upgrades) are saved into the same file.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include "tags.h"
#include "catalog.h"
#include "../core/ditag.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void web_log(const char *fmt, ...);

#define MAXTAGS 1024
static struct { uint32_t id; char path[160]; } g_t[MAXTAGS];
static int g_n;
static char g_dir[128] = "/data/dimod/tags";
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static int read_file(const char *path, uint8_t out[320]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, 320);
    close(fd);
    return n == 320 ? 0 : -1;
}

static int write_file(const char *path, const uint8_t d[320]) {
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    ssize_t n = write(fd, d, 320);
    close(fd);
    if (n != 320) { unlink(tmp); return -1; }
    return rename(tmp, path);
}

void tags_init(const char *dir) {
    pthread_mutex_lock(&g_mx);
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    mkdir(g_dir, 0777);
    g_n = 0;
    DIR *d = opendir(g_dir);
    int bad = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_n < MAXTAGS) {
            size_t L = strlen(e->d_name);
            if (L < 5 || strcasecmp(e->d_name + L - 4, ".bin")) continue;
            char path[160];
            snprintf(path, sizeof path, "%s/%s", g_dir, e->d_name);
            uint8_t buf[320];
            if (read_file(path, buf)) { bad++; continue; }
            uint32_t id = ditag_figure_id(buf, 0);
            if (!id) { bad++; continue; }
            int dup = 0;
            for (int i = 0; i < g_n; i++) if (g_t[i].id == id) dup = 1;
            if (dup) continue;   /* first file wins */
            g_t[g_n].id = id;
            snprintf(g_t[g_n].path, sizeof g_t[g_n].path, "%s", path);
            g_n++;
        }
        closedir(d);
    }
    pthread_mutex_unlock(&g_mx);
    web_log("tags: %d figure files in %s%s", g_n, g_dir, bad ? " (some files are not Infinity tags)" : "");
}

int tags_count(void) { return g_n; }

static const char *path_of(uint32_t id) {
    for (int i = 0; i < g_n; i++) if (g_t[i].id == id) return g_t[i].path;
    return 0;
}

static uint32_t rnd(void) {
    static uint32_t s;
    if (!s) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        s = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec * 2654435761u ^ (uint32_t)getpid();
        if (!s) s = 0x9E3779B9u;
    }
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

int tags_get(uint32_t id, uint8_t out[320]) {
    pthread_mutex_lock(&g_mx);
    const char *p = path_of(id);
    if (p && !read_file(p, out) && ditag_figure_id(out, 0) == id) { pthread_mutex_unlock(&g_mx); return 0; }
    /* new figure: NXP-style 7-byte UID (0x04 + 6 random bytes) */
    uint8_t uid[7] = {0x04};
    for (int i = 1; i < 7; i++) uid[i] = (uint8_t)(rnd() >> 8);
    if (ditag_create(out, id, catalog_series_of(id), uid)) { pthread_mutex_unlock(&g_mx); return -1; }
    char path[160];
    snprintf(path, sizeof path, "%s/%u.bin", g_dir, id);
    if (write_file(path, out)) web_log("tags: cannot write %s", path);
    if (!p && g_n < MAXTAGS) {
        g_t[g_n].id = id;
        snprintf(g_t[g_n].path, sizeof g_t[g_n].path, "%s", path);
        g_n++;
    }
    pthread_mutex_unlock(&g_mx);
    web_log("tags: created %s", path);
    return 0;
}

int tags_save(uint32_t id, const uint8_t data[320]) {
    if (!id || ditag_figure_id(data, 0) != id) return -1;   /* never overwrite a file with garbage */
    pthread_mutex_lock(&g_mx);
    const char *p = path_of(id);
    char path[160];
    if (!p) { snprintf(path, sizeof path, "%s/%u.bin", g_dir, id); p = path; }
    int r = write_file(p, data);
    pthread_mutex_unlock(&g_mx);
    return r;
}
