/* Disney Infinity virtual base - TV picker, in the spirit of harrysof's LegoToypad picker.
 *
 * Drawn by the retained-mode renderer the payload loads into SceShellUI (src/overlay, shared
 * with the LEGO Dimensions Virtual Toy Pad): this thread defines widgets, uploads textures and
 * sends per-frame deltas over 127.0.0.1 (ovl_proto.h). Controller input comes from the game
 * hook (di_header.ext), which hides it from the game while the picker is open and reports which
 * controller opened it.
 *
 * Pages (L1 / R1): Base, Play Sets, Characters, Power-Ups, Power Discs, Favourites.
 * Item pages filter by edition with L2 / R2 (All, 1.0, 2.0, 3.0); Square searches everything,
 * Triangle marks a favourite, Cross places. Items follow the game's rules (game_route_rules): one
 * character per player's pad - a new one swaps it out - two discs under it, one play set and up to
 * three pieces on the hexagon; with two controllers they go to the opener's side. OPTIONS opens Settings.
 * Peek (hold the peek button in game): hexagon at the top, the two pads in the bottom corners.
 *
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later
 * Transport / texture / widget layer from the LEGO Dimensions Virtual Toy Pad cui.c. */
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cui.h"
#include "game.h"
#include "catalog.h"
#include "favs.h"
#include "webremote.h"
#if __has_include("build_id.h")
#include "build_id.h"
#else
#define DIMOD_VERSION "0.6"
#define DIMOD_BUILD "dev"
#endif
#include <ctype.h>
#include "../overlay/common/ovl_proto.h"
#ifndef CUI_HOST_TEST
#include "../overlay/controller/ovl_controller.h"
#endif

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

void web_log(const char *fmt, ...);
#define LOGC(...) web_log("overlay: " __VA_ARGS__)

#ifndef UI_DIR
#define UI_DIR "/data/dimod/web/ui"
#define ART_PX 160   /* the size tools/build_data.py makes every badge; anything else is fitted at draw time */
#endif

static cui_config C = {1, 0x100, 2, 1, 0, 0x2 | 0x4, 92, 1};

static uint64_t ms_now(void) { return game_ms(); }

enum {
    B_L3 = 0x2, B_R3 = 0x4, B_OPTIONS = 0x8, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80,
    B_L2 = 0x100, B_R2 = 0x200, B_L1 = 0x400, B_R1 = 0x800, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000,
    B_CROSS = 0x4000, B_SQUARE = 0x8000,
};

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
    fclose(f);
    if (b) { b[n] = 0; if (len) *len = (size_t)n; }
    return b;
}

/* ================================================================ renderer link */

static int g_fd = -1;
static uint32_t g_caps;
static uint64_t g_last_ping, g_next_connect;
static pthread_mutex_t g_send_mx = PTHREAD_MUTEX_INITIALIZER;

static void link_close(const char *why) {
    if (g_fd >= 0) { close(g_fd); LOGC("renderer link closed (%s)", why); }
    g_fd = -1;
}

static int send_all(const void *d, size_t n) {
    const uint8_t *p = d;
    while (n) {
        ssize_t k = send(g_fd, p, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int send_msg2(uint16_t type, const void *a, uint32_t na, const void *b, uint32_t nb) {
    if (g_fd < 0) return -1;
    ovl_msg h = {OVL_MAGIC, type, OVL_VERSION, na + nb};
    pthread_mutex_lock(&g_send_mx);
    int r = send_all(&h, sizeof h) || (na && send_all(a, na)) || (nb && send_all(b, nb));
    pthread_mutex_unlock(&g_send_mx);
    if (r) link_close("send failed");
    return r ? -1 : 0;
}

static int recv_full(int fd, void *b, size_t n, int timeout_ms) {
    uint8_t *p = b;
    uint64_t t0 = ms_now();
    while (n) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = {0, 50 * 1000};
        if (select(fd + 1, &rs, 0, 0, &tv) <= 0) {
            if ((int)(ms_now() - t0) > timeout_ms) return -1;
            continue;
        }
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}

static void reset_sent_state(void);

static int link_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(OVL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ovl_msg h;
    ovl_hello hello;
    if (recv_full(fd, &h, sizeof h, 3000) || h.magic != OVL_MAGIC || h.type != OVL_HELLO || h.len != sizeof hello ||
        recv_full(fd, &hello, sizeof hello, 3000)) {
        close(fd);
        LOGC("renderer did not say hello");
        return -1;
    }
    g_fd = fd;
    g_caps = hello.caps;
    LOGC("renderer link up: pid %d caps 0x%x scene %.0fx%.0f", hello.pid, hello.caps, (double)hello.scene_w, (double)hello.scene_h);
    ovl_config cfg = {(uint32_t)C.tex_mode, {0, 0, 0}};
    send_msg2(OVL_CONFIG, &cfg, sizeof cfg, 0, 0);
    reset_sent_state();
    return 0;
}

static void link_poll(void) {
    while (g_fd >= 0) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(g_fd, &rs);
        struct timeval tv = {0, 0};
        if (select(g_fd + 1, &rs, 0, 0, &tv) <= 0) return;
        ovl_msg h;
        if (recv_full(g_fd, &h, sizeof h, 1000) || h.magic != OVL_MAGIC || h.len > 4096) { link_close("bad read"); return; }
        char body[4097];
        if (h.len && recv_full(g_fd, body, h.len, 1000)) { link_close("bad read"); return; }
        body[h.len] = 0;
        if (h.type == OVL_STATUS) LOGC("renderer: %s", body);
    }
}

/* ================================================================ textures */

typedef struct { char name[24]; uint32_t id; int sent, dynamic; uint64_t used; int bytes, w, h; } texent;
#define MAXT 700
static texent T[MAXT];
static int NT;
static int g_tex_budget;

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h ? h : 1;
}

static texent *tex_find(const char *name) {
    for (int i = 0; i < NT; i++) if (!strcmp(T[i].name, name)) return &T[i];
    if (NT >= MAXT) return 0;
    texent *t = &T[NT++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->id = name_hash(name);
    t->dynamic = (name[0] == 'p' && name[1] >= '0' && name[1] <= '9') || name[0] == '@' || !strncmp(name, "plq_", 4);   /* badges, tints, plaques */
    return t;
}

static void tex_rebind(uint32_t id);

/* "@<mask>#rrggbb": a white mask recoloured here for renderers that cannot tint (no OVL_CAP_FILTER) */
typedef struct { char name[24]; int w, h; uint8_t *a; } maskc;
#define NMASK 16
static maskc g_masks[NMASK];

static const maskc *mask_alpha(const char *name) {
    for (int i = 0; i < NMASK; i++) if (g_masks[i].a && !strcmp(g_masks[i].name, name)) return &g_masks[i];
    char path[160];
    snprintf(path, sizeof path, UI_DIR "/%s.png", name);
    size_t n = 0;
    uint8_t *png = (uint8_t *)slurp(path, &n);
    if (!png) return 0;
    int w = 0, h = 0, comp = 0;
    uint8_t *px = stbi_load_from_memory(png, (int)n, &w, &h, &comp, 4);
    free(png);
    if (!px) return 0;
    int slot = 0;
    for (int i = 0; i < NMASK; i++) if (!g_masks[i].a) { slot = i; break; }
    maskc *m = &g_masks[slot];
    free(m->a);
    m->a = px;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->w = w; m->h = h;
    return m;
}

static int tex_upload_tinted(texent *t) {
    char base[24];
    const char *hash = strchr(t->name, '#');
    if (!hash || hash - t->name - 1 >= (long)sizeof base) { t->sent = -1; return -1; }
    snprintf(base, sizeof base, "%.*s", (int)(hash - t->name - 1), t->name + 1);
    unsigned rgb = (unsigned)strtoul(hash + 1, 0, 16);
    const maskc *m = mask_alpha(base);
    if (!m) { t->sent = -1; return -1; }
    size_t n = (size_t)m->w * m->h;
    uint8_t *px = malloc(n * 4);
    if (!px) return -1;
    unsigned cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
    for (size_t i = 0; i < n; i++) {
        px[i * 4] = (uint8_t)(m->a[i * 4] * cr / 255); px[i * 4 + 1] = (uint8_t)(m->a[i * 4 + 1] * cg / 255);
        px[i * 4 + 2] = (uint8_t)(m->a[i * 4 + 2] * cb / 255); px[i * 4 + 3] = m->a[i * 4 + 3];
    }
    uint8_t hdr[8];
    uint16_t w16 = (uint16_t)m->w, h16 = (uint16_t)m->h;
    memcpy(hdr, &t->id, 4); memcpy(hdr + 4, &w16, 2); memcpy(hdr + 6, &h16, 2);
    int r = send_msg2(OVL_TEX, hdr, 8, px, (uint32_t)(n * 4));
    free(px);
    t->bytes = (int)(n * 4);
    t->w = m->w; t->h = m->h;
    if (!r) { t->sent = 1; tex_rebind(t->id); }
    return r;
}

static int tex_upload(texent *t) {
    if (t->name[0] == '@') return tex_upload_tinted(t);
    char path[160];
    if (!strncmp(t->name, "bgc", 3)) snprintf(path, sizeof path, "%s", CUI_CUSTOM_BG);   /* the uploaded photo */
    else snprintf(path, sizeof path, UI_DIR "/%s.png", t->name);
    size_t n = 0;
    uint8_t *png = (uint8_t *)slurp(path, &n);
    if (!png) { static int warned; if (t->name[0] != 'b' && warned++ < 8) LOGC("missing %s", path); t->sent = -1; return -1; }
    int r, w = 0, h = 0, comp = 0;
    uint8_t hdr[8];
    if ((g_caps & OVL_CAP_PIXELS) && C.tex_mode != 2) {
        uint8_t *px = stbi_load_from_memory(png, (int)n, &w, &h, &comp, 4);
        if (!px) { free(png); t->sent = -1; return -1; }
        /* a big photo: halve it until it fits a message; a badge (p<id>): halve big art down to at
         * most 2x the 160 px it is made for, so a 2048 px render costs no more than a normal badge */
        int badge_art = t->name[0] == 'p' && t->name[1] >= '0' && t->name[1] <= '9';
        while ((((size_t)w * h * 4 > (3u << 20)) || (badge_art && (w > 2 * ART_PX || h > 2 * ART_PX))) && w > 1 && h > 1) {
            int nw = w / 2, nh = h / 2;
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++)
                    for (int k = 0; k < 4; k++) {
                        const uint8_t *q = px + ((size_t)(2 * y) * w + 2 * x) * 4 + k;
                        px[((size_t)y * nw + x) * 4 + k] = (uint8_t)((q[0] + q[4] + q[(size_t)w * 4] + q[(size_t)w * 4 + 4] + 2) / 4);
                    }
            w = nw; h = nh;
        }
        uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
        memcpy(hdr, &t->id, 4); memcpy(hdr + 4, &w16, 2); memcpy(hdr + 6, &h16, 2);
        r = send_msg2(OVL_TEX, hdr, 8, px, (uint32_t)(w * h * 4));
        stbi_image_free(px);
    } else {
        stbi_info_from_memory(png, (int)n, &w, &h, &comp);
        uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
        memcpy(hdr, &t->id, 4); memcpy(hdr + 4, &w16, 2); memcpy(hdr + 6, &h16, 2);
        r = send_msg2(OVL_TEXPNG, hdr, 8, png, (uint32_t)n);
    }
    t->bytes = w * h * 4;
    t->w = w; t->h = h;
    free(png);
    if (!r) { t->sent = 1; tex_rebind(t->id); }
    return r;
}

/* the size of a texture once it has been loaded (0 x 0 before) */
static void tex_dims(const char *name, int *w, int *h) {
    texent *t = tex_find(name);
    *w = t && t->sent == 1 ? t->w : 0;
    *h = t && t->sent == 1 ? t->h : 0;
}

static uint32_t tex(const char *name) {
    texent *t = tex_find(name);
    if (!t) return 0;
    t->used = ms_now();
    if (!t->sent && g_fd >= 0 && g_tex_budget > 0) { g_tex_budget--; tex_upload(t); }
    if (t->sent < 0) return 0;   /* no such file: show nothing rather than what the widget had before */
    return t->id;   /* the renderer waits for late textures */
}

#define DYN_CAP 64
#define DYN_BYTES (8 << 20)
static void tex_evict(void) {
    int live = 0;
    long bytes = 0;
    for (int i = 0; i < NT; i++) if (T[i].dynamic && T[i].sent == 1) { live++; bytes += T[i].bytes; }
    uint64_t now = ms_now();
    while (live > DYN_CAP || bytes > DYN_BYTES) {
        int lru = -1;
        for (int i = 0; i < NT; i++)
            if (T[i].dynamic && T[i].sent == 1 && now - T[i].used > 500 && (lru < 0 || T[i].used < T[lru].used)) lru = i;
        if (lru < 0) break;
        send_msg2(OVL_TEXFREE, &T[lru].id, 4, 0, 0);
        T[lru].sent = 0;
        tex_rebind(T[lru].id);
        live--;
        bytes -= T[lru].bytes;
    }
}

/* ================================================================ widgets */

/* ================================================================ widgets */

#define GCOLS 5
#define GROWS 3
#define GCELLS (GCOLS * GROWS)
#define NKEYS 40
#define NSET 10

enum {
    W_DIM, W_BGIMG, W_WIN, W_TITLE, W_SUB,
    W_TAB0, W_TABL0 = W_TAB0 + 6, W_ED0 = W_TABL0 + 6, W_EDL0 = W_ED0 + 4,
    W_BASE = W_EDL0 + 4, W_GLOW0, W_LIT0 = W_GLOW0 + 3, W_SLOT0 = W_LIT0 + 3, W_ART0 = W_SLOT0 + 9,
    W_GHOST0 = W_ART0 + 12, W_UP0 = W_GHOST0 + 9, W_FLASH0 = W_UP0 + 3, W_SAB0 = W_FLASH0 + 3, W_FOCUS = W_SAB0 + 9, W_TGT, W_PTAG0, W_PTAGL0 = W_PTAG0 + 2,
    W_OWNER = W_PTAGL0 + 2, W_TARGET,
    /* Base page: one column per pad (header, LED, top card, "stacked under", two disc cards) */
    W_COLH0, W_COLHL0 = W_COLH0 + 3, W_COLLEDBG0 = W_COLHL0 + 3, W_COLLED0 = W_COLLEDBG0 + 3, W_CAP0 = W_COLLED0 + 3,
    W_SCARD0 = W_CAP0 + 3, W_SART0 = W_SCARD0 + 9, W_SNAME0 = W_SART0 + 9, W_SSUB0 = W_SNAME0 + 9, W_SSAB0 = W_SSUB0 + 9,
    W_INFO = W_SSAB0 + 3,
    /* item grid: the shelf / album behind it, then per cell its card, slot, shadow, art, name, star */
    W_GRIDBG, W_PLANK0, W_CELL0 = W_PLANK0 + 4, W_CSLOT0 = W_CELL0 + GCELLS, W_CSH0 = W_CSLOT0 + GCELLS,
    W_CART0 = W_CSH0 + GCELLS, W_CLAB0 = W_CART0 + GCELLS, W_CSTAR0 = W_CLAB0 + GCELLS,
    W_CSUB0 = W_CSTAR0 + GCELLS, W_SECL = W_CSUB0 + GCELLS,
    W_FSPOT, W_FSH, W_FART,   /* the highlighted item, drawn last: it comes forward / rises out */
    W_DOOR0, W_DOOR1, W_TURN, W_TURNSH,   /* the case's glass doors swinging open, an album page turning */
    W_RFOCUS, W_RNAME, W_COUNTL, W_SCROLLBG, W_SCROLL, W_EMPTY,
    W_QBOX, W_QTEXT, W_KEY0, W_KEYL0 = W_KEY0 + NKEYS, W_KFOCUS = W_KEYL0 + NKEYS,
    W_SROWBG0, W_SLAB0 = W_SROWBG0 + NSET, W_SVAL0 = W_SLAB0 + NSET,
    W_HINT = W_SVAL0 + NSET, W_STATUS,
    /* sneak peek: per pad its plate (cut from the tilted base), the light spill, the lit rim, then the stack */
    W_PKPLATE0, W_PKSPILL0 = W_PKPLATE0 + 3, W_PKLIT0 = W_PKSPILL0 + 3, W_PKART0 = W_PKLIT0 + 3,
    W_PKGHOST0 = W_PKART0 + 12, W_PKUP0 = W_PKGHOST0 + 9, W_PKFLASH0 = W_PKUP0 + 3, W_PKSAB0 = W_PKFLASH0 + 3, W_PKTAG0 = W_PKSAB0 + 9, W_PKTAGL0 = W_PKTAG0 + 2,
    W_COUNT = W_PKTAGL0 + 2
};

typedef struct {
    float x, y, w, h;
    uint32_t vis;
    float alpha;
    uint8_t c[4];
    uint32_t tex;
    char text[120];
    uint16_t font;
    uint8_t align, lines;
} wstate;

static uint8_t g_types[W_COUNT];
static wstate g_cur[W_COUNT], g_sent[W_COUNT];
static int g_sent_valid;

static void tex_rebind(uint32_t id) {
    for (int i = 0; i < W_COUNT; i++) if (g_sent[i].tex == id) g_sent[i].tex = 0xFFFFFFFFu;
}

static void define_types(void) {
    for (int i = 0; i < W_COUNT; i++) g_types[i] = OVL_W_IMAGE;
    int panels[] = {W_DIM, W_WIN, W_SCROLLBG, W_SCROLL, W_QBOX, W_PTAG0, W_PTAG0 + 1, W_PKTAG0, W_PKTAG0 + 1};
    for (size_t i = 0; i < sizeof panels / sizeof *panels; i++) g_types[panels[i]] = OVL_W_PANEL;
    int labels[] = {W_TITLE, W_SUB, W_OWNER, W_TARGET, W_INFO, W_RNAME, W_COUNTL, W_EMPTY, W_QTEXT, W_HINT, W_STATUS, W_SECL};
    for (size_t i = 0; i < sizeof labels / sizeof *labels; i++) g_types[labels[i]] = OVL_W_LABEL;
    for (int i = 0; i < 6; i++) { g_types[W_TAB0 + i] = OVL_W_PANEL; g_types[W_TABL0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < 4; i++) { g_types[W_ED0 + i] = OVL_W_PANEL; g_types[W_EDL0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < 2; i++) { g_types[W_PTAGL0 + i] = OVL_W_LABEL; g_types[W_PKTAGL0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < 3; i++) { g_types[W_COLH0 + i] = OVL_W_PANEL; g_types[W_COLHL0 + i] = g_types[W_CAP0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < 9; i++) { g_types[W_SCARD0 + i] = OVL_W_PANEL; g_types[W_SNAME0 + i] = g_types[W_SSUB0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < GCELLS; i++) { g_types[W_CELL0 + i] = OVL_W_PANEL; g_types[W_CLAB0 + i] = g_types[W_CSUB0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < NKEYS; i++) { g_types[W_KEY0 + i] = OVL_W_PANEL; g_types[W_KEYL0 + i] = OVL_W_LABEL; }
    for (int i = 0; i < NSET; i++) { g_types[W_SROWBG0 + i] = OVL_W_PANEL; g_types[W_SLAB0 + i] = g_types[W_SVAL0 + i] = OVL_W_LABEL; }
}

static void reset_sent_state(void) {
    g_sent_valid = 0;
    for (int i = 0; i < NT; i++) if (T[i].sent == 1) T[i].sent = 0;
}

static void frame_begin(void) {
    for (int i = 0; i < W_COUNT; i++) { g_cur[i] = g_sent[i]; g_cur[i].vis = 0; }
}

/* the picker is laid out on a 1920x1080 canvas and scaled about its centre (UI size). Window
 * opacity fades the window and the panels only: text and artwork stay solid, so a see-through
 * window over a bright game scene never washes the words out */
static float XS = 1.0f, XO = 1.0f;
static void xf(float *x, float *y, float *w, float *h) {
    *x = 960 + (*x - 960) * XS; *y = 540 + (*y - 540) * XS; *w *= XS; *h *= XS;
}

static void img(int id, const char *texname, float x, float y, float w, float h, float alpha, const uint8_t *tint) {
    wstate *s = &g_cur[id];
    xf(&x, &y, &w, &h);
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->vis = 1; s->alpha = id == W_BGIMG ? alpha * XO : alpha;   /* the background picture is part of the window */
    static const uint8_t white[4] = {255, 255, 255, 255};
    memcpy(s->c, tint ? tint : white, 4);
    s->tex = tex(texname);
}

static void tinted(int id, const char *mask, float x, float y, float w, float h, float alpha, const uint8_t rgb[3]) {
    if (g_caps & OVL_CAP_FILTER) {
        uint8_t t4[4] = {rgb[0], rgb[1], rgb[2], 255};
        img(id, mask, x, y, w, h, alpha, t4);
        return;
    }
    char name[24];
    unsigned q[3];
    for (int k = 0; k < 3; k++) q[k] = (unsigned)((rgb[k] * 5 + 127) / 255) * 51;
    snprintf(name, sizeof name, "@%s#%02x%02x%02x", mask, q[0], q[1], q[2]);
    img(id, name, x, y, w, h, alpha, 0);
}

static void panel(int id, float x, float y, float w, float h, const uint8_t rgba[4]) {
    wstate *s = &g_cur[id];
    xf(&x, &y, &w, &h);
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->vis = 1; s->alpha = XO;
    memcpy(s->c, rgba, 4);
}

static void label(int id, const char *text, float x, float y, float w, int font, int align, int lines,
                  const uint8_t rgba[4], float alpha) {
    wstate *s = &g_cur[id];
    float h = (float)font;
    xf(&x, &y, &w, &h);
    font = (int)(font * XS + 0.5f);
    s->x = x; s->y = y; s->w = w; s->h = (float)font;
    s->vis = 1; s->alpha = alpha;
    memcpy(s->c, rgba, 4);
    s->font = (uint16_t)font; s->align = (uint8_t)align; s->lines = (uint8_t)lines;
    if (!(g_caps & OVL_CAP_TRIM)) {
        int max = w > 0 ? (int)(w / (font * 0.52f)) * (lines ? lines : 1) : 200;
        int n = (int)strlen(text);
        if (n > max && max > 3) { snprintf(s->text, sizeof s->text, "%.*s...", max - 3, text); return; }
    }
    snprintf(s->text, sizeof s->text, "%s", text);
}

static uint8_t g_buf[128 * 1024];

static void frame_send(void) {
    if (g_fd < 0) return;
    if (!g_sent_valid) {
        uint8_t def[2 + W_COUNT];
        uint16_t n = W_COUNT;
        memcpy(def, &n, 2);
        memcpy(def + 2, g_types, W_COUNT);
        if (send_msg2(OVL_DEFINE, def, sizeof def, 0, 0)) return;
        memset(g_sent, 0, sizeof g_sent);
        for (int i = 0; i < W_COUNT; i++) { g_sent[i].alpha = -1; g_sent[i].tex = 0xFFFFFFFFu; g_sent[i].font = 0xFFFF; }
    }
    size_t off = 2;
    uint16_t count = 0;
    for (int i = 0; i < W_COUNT; i++) {
        wstate *a = &g_cur[i], *b = &g_sent[i];
        uint16_t mask = 0;
        if (!g_sent_valid) mask |= OVL_F_VIS;
        if (a->vis != b->vis) mask |= OVL_F_VIS;
        if (a->vis) {
            if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h) mask |= OVL_F_GEOM;
            if (a->alpha != b->alpha) mask |= OVL_F_ALPHA;
            if (memcmp(a->c, b->c, 4)) mask |= OVL_F_COLOR;
            if (g_types[i] == OVL_W_IMAGE && a->tex != b->tex) mask |= OVL_F_TEX;
            if (g_types[i] == OVL_W_LABEL && (strcmp(a->text, b->text) || a->font != b->font || a->align != b->align ||
                                             a->lines != b->lines)) mask |= OVL_F_TEXT;
        }
        if (!mask) continue;
        if (off + 220 > sizeof g_buf) break;
        uint16_t id = (uint16_t)i;
        memcpy(g_buf + off, &id, 2); memcpy(g_buf + off + 2, &mask, 2); off += 4;
        if (mask & OVL_F_GEOM) { memcpy(g_buf + off, &a->x, 4); memcpy(g_buf + off + 4, &a->y, 4);
                                 memcpy(g_buf + off + 8, &a->w, 4); memcpy(g_buf + off + 12, &a->h, 4); off += 16; }
        if (mask & OVL_F_VIS) { memcpy(g_buf + off, &a->vis, 4); off += 4; }
        if (mask & OVL_F_ALPHA) { memcpy(g_buf + off, &a->alpha, 4); off += 4; }
        if (mask & OVL_F_COLOR) { memcpy(g_buf + off, a->c, 4); off += 4; }
        if (mask & OVL_F_TEX) { memcpy(g_buf + off, &a->tex, 4); off += 4; }
        if (mask & OVL_F_TEXT) {
            uint16_t len = (uint16_t)strlen(a->text);
            memcpy(g_buf + off, &a->font, 2); g_buf[off + 2] = a->align; g_buf[off + 3] = a->lines;
            memcpy(g_buf + off + 4, &len, 2); off += 6;
            memcpy(g_buf + off, a->text, len); off += len;
            while (len & 3) { g_buf[off++] = 0; len++; }
        }
        *b = *a;
        count++;
    }
    if (!count && g_sent_valid) return;
    memcpy(g_buf, &count, 2);
    if (send_msg2(OVL_FRAME, g_buf, (uint32_t)off, 0, 0) == 0) g_sent_valid = 1;
}

/* ================================================================ base geometry */

/* draw order inside a pad: disc 2, disc 1, then what stands on top */
static const int ZIDX[9] = {2, 1, 0, 5, 4, 3, 8, 7, 6};
/* the item widgets of a stack view: per pad disc 2, disc 1, the shadow of what stands on top, then
 * what stands on top (back pad first) */
#define AIDX(s) ((s) / 3 * 4 + ((s) % 3 == 2 ? 0 : (s) % 3 == 1 ? 1 : 3))
#define SIDX(p) ((p) * 4 + 2)

/* D-pad grid on the Base page: columns left pad / hexagon / right pad, rows top / disc / disc */
static const int NAV[3][3] = {{3, 0, 6}, {4, 1, 7}, {5, 2, 8}};

/* ================================================================ themes */

typedef struct {
    const char *name;
    uint8_t dim[4], win[4], text[4], muted[4], accent[4], dark[4], row[4], row_sel[4], cell[4], cell_on[4];
    uint8_t good[4], bad[4], gold[4], p1[4], p2[4], hexc[4];
    uint8_t win_on_bg;        /* window fill alpha over a background picture */
} theme;

static const theme THEMES[CUI_THEMES] = {
    {"Classic", {0, 0, 0, 110}, {12, 18, 34, 240}, {236, 241, 248, 255}, {148, 163, 184, 255}, {56, 189, 248, 255},
     {12, 18, 34, 255}, {23, 32, 54, 230}, {30, 58, 112, 245}, {23, 32, 54, 240}, {20, 26, 40, 200},
     {134, 239, 172, 255}, {252, 165, 165, 255}, {250, 204, 21, 255}, {59, 130, 246, 255}, {249, 115, 22, 255},
     {250, 204, 21, 255}, 150},
    /* see-through glass: the game stays visible behind everything */
    {"Clear", {0, 0, 0, 0}, {6, 10, 20, 110}, {255, 255, 255, 255}, {214, 224, 236, 255}, {125, 211, 252, 255},
     {6, 10, 20, 255}, {6, 10, 20, 130}, {14, 64, 104, 190}, {6, 10, 20, 120}, {0, 0, 0, 70},
     {134, 239, 172, 255}, {254, 202, 202, 255}, {253, 224, 71, 255}, {96, 165, 250, 255}, {251, 146, 60, 255},
     {253, 224, 71, 255}, 40},
    {"Light", {0, 0, 0, 90}, {241, 245, 249, 245}, {15, 23, 42, 255}, {71, 85, 105, 255}, {2, 132, 199, 255},
     {255, 255, 255, 255}, {226, 232, 240, 240}, {186, 230, 253, 250}, {255, 255, 255, 245}, {226, 232, 240, 200},
     {21, 128, 61, 255}, {185, 28, 28, 255}, {161, 98, 7, 255}, {37, 99, 235, 255}, {234, 88, 12, 255},
     {161, 98, 7, 255}, 175},
    {"Crimson", {0, 0, 0, 110}, {26, 8, 12, 240}, {254, 242, 242, 255}, {252, 165, 165, 255}, {239, 68, 68, 255},
     {26, 8, 12, 255}, {56, 18, 26, 230}, {127, 29, 29, 245}, {48, 14, 20, 240}, {30, 10, 14, 200},
     {134, 239, 172, 255}, {254, 202, 202, 255}, {250, 204, 21, 255}, {96, 165, 250, 255}, {251, 146, 60, 255},
     {250, 204, 21, 255}, 150},
    {"Neon", {0, 0, 0, 110}, {13, 7, 28, 240}, {245, 243, 255, 255}, {196, 181, 253, 255}, {217, 70, 239, 255},
     {13, 7, 28, 255}, {33, 19, 61, 230}, {91, 33, 182, 245}, {30, 17, 56, 240}, {18, 10, 34, 200},
     {110, 231, 183, 255}, {253, 164, 175, 255}, {250, 204, 21, 255}, {34, 211, 238, 255}, {251, 146, 60, 255},
     {250, 204, 21, 255}, 150},
};
static const char *BACKGROUNDS[CUI_BACKGROUNDS] = {"None", "Starfield", "Aurora", "Toy Box", "Custom photo"};
const char *cui_theme_name(int i) { return i >= 0 && i < CUI_THEMES ? THEMES[i].name : 0; }
const char *cui_background_name(int i) { return i >= 0 && i < CUI_BACKGROUNDS ? BACKGROUNDS[i] : 0; }
static volatile int g_bg_gen;
void cui_background_changed(void) { __atomic_fetch_add(&g_bg_gen, 1, __ATOMIC_RELAXED); }
int cui_background_gen(void) { return __atomic_load_n(&g_bg_gen, __ATOMIC_RELAXED); }

static const theme *TH = &THEMES[0];
#define COL_WIN TH->win
#define COL_DIM TH->dim
#define COL_TEXT TH->text
#define COL_MUTED TH->muted
#define COL_ACCENT TH->accent
#define COL_DARK TH->dark
#define COL_ROW TH->row
#define COL_ROW_SEL TH->row_sel
#define COL_CELL TH->cell
#define COL_CELL_ON TH->cell_on
#define COL_GOOD TH->good
#define COL_BAD TH->bad
#define COL_GOLD TH->gold
static const uint8_t ED_COL[4][3] = {{56, 189, 248}, {56, 189, 248}, {239, 68, 68}, {168, 85, 247}};

/* ================================================================ state */

enum { PG_BASE, PG_PLAYSETS, PG_CHARS, PG_POWERUPS, PG_DISCS, PG_FAVS, NPAGES };
static const char *PAGE_NAMES[NPAGES] = {"Base", "Play Sets", "Characters", "Power-Ups", "Power Discs", "Favourites"};
static const char PAGE_KIND[NPAGES] = {0, K_PIECE, K_FIGURE, K_ROUND, K_HEX, 0};
enum { SCR_MAIN, SCR_SEARCH, SCR_SETTINGS };

#define MAX_SECTIONS 64   /* the character display has 26 shelves; room to grow */
static struct {
    int open, screen, page, ed;          /* ed: 0 all, 1..3 */
    int spot, target;                    /* Base page focus; target spot (-1 = automatic) */
    uint32_t open_seq, ring_rd, nav;
    int held_since_dir;
    uint64_t repeat_at;
    uint32_t items[512];                 /* grid cells: an id, or a section header / gap (see IS_ITEM) */
    int n, sel, top, real;               /* real = items without headers and gaps */
    char groups[MAX_SECTIONS][48];       /* section names of this list, for the header cells */
    uint64_t close_at;                   /* fast loading: close once the drop-on animation has played */
    uint32_t foc_id, foc_prev, pick_id;  /* grid highlight animation: what is / was highlighted, what was just picked */
    uint64_t foc_t, pick_t;
    uint64_t door_t, turn_t;             /* display animations: doors opening, a page turning */
    int turn_dir, disp_key, disp_sec;
    char query[32];
    int key, kb;                         /* search: key focus, 1 = keyboard has focus */
    int set_sel;
    uint64_t clear_armed, reset_armed;
    int owner, connected;
    int peek_held;
    game_snap snap;
} S;

static const char *SPOT_NAMES[9] = {"Hexagon", "Hex disc 1", "Hex disc 2", "Left pad", "Left disc 1", "Left disc 2",
                                    "Right pad", "Right disc 1", "Right disc 2"};

/* ------------------------------------------------------------------ settings */

static const struct { const char *name; uint32_t mask; } PEEK_CHOICES[] = {
    {"L2", B_L2}, {"R2", B_R2}, {"L1", B_L1}, {"R1", B_R1}, {"L2+R2", B_L2 | B_R2}, {"L1+R1", B_L1 | B_R1},
    {"L3", B_L3}, {"R3", B_R3}, {"TOUCHPAD", 0x100000}};
#define NPEEK ((int)(sizeof PEEK_CHOICES / sizeof *PEEK_CHOICES))
static pthread_mutex_t g_cfg_mx = PTHREAD_MUTEX_INITIALIZER;

int cui_peek_choices(void) { return NPEEK; }
const char *cui_peek_choice(int i, uint32_t *mask) {
    if (i < 0 || i >= NPEEK) return 0;
    if (mask) *mask = PEEK_CHOICES[i].mask;
    return PEEK_CHOICES[i].name;
}
const char *cui_mask_name(uint32_t mask) {
    for (int i = 0; i < NPEEK; i++) if (PEEK_CHOICES[i].mask == mask) return PEEK_CHOICES[i].name;
    return "custom";
}
/* the button combos that can open / close the picker (Settings > Open picker); two buttons held
 * together, so a normal press in game never opens it by accident */
static const struct { const char *name; uint32_t mask; } OPEN_CHOICES[] = {
    {"L3+R3", B_L3 | B_R3}, {"L1+R1", B_L1 | B_R1}, {"L2+R2", B_L2 | B_R2}, {"L1+L2", B_L1 | B_L2},
    {"R1+R2", B_R1 | B_R2}, {"L3+TOUCHPAD", B_L3 | 0x100000}, {"R3+TOUCHPAD", B_R3 | 0x100000}};
#define NOPEN ((int)(sizeof OPEN_CHOICES / sizeof *OPEN_CHOICES))
int cui_open_choices(void) { return NOPEN; }
const char *cui_open_choice(int i, uint32_t *mask) {
    if (i < 0 || i >= NOPEN) return 0;
    if (mask) *mask = OPEN_CHOICES[i].mask;
    return OPEN_CHOICES[i].name;
}
const char *cui_open_name(uint32_t mask) {
    for (int i = 0; i < NOPEN; i++) if (OPEN_CHOICES[i].mask == mask) return OPEN_CHOICES[i].name;
    return "custom (config.ini)";
}
static int open_index(void) {
    for (int i = 0; i < NOPEN; i++) if (OPEN_CHOICES[i].mask == C.combo) return i;
    return -1;
}

static int peek_index(void) {
    for (int i = 0; i < NPEEK; i++) if (PEEK_CHOICES[i].mask == C.peek_mask) return i;
    return 0;
}
static const float UI_SCALES[4] = {0.9f, 1.0f, 1.1f, 1.2f};

static void clamp_cfg(cui_config *c) {
    if (c->opacity < 30) c->opacity = 30;
    if (c->opacity > 100) c->opacity = 100;
    if (c->ui_size < 0 || c->ui_size > 3) c->ui_size = 1;
    if (c->peek_size < 0 || c->peek_size > 3) c->peek_size = 2;
    if (!c->peek_mask) c->peek_mask = B_L2;
    if (c->theme < 0 || c->theme >= CUI_THEMES) c->theme = 0;
    if (c->background < 0 || c->background >= CUI_BACKGROUNDS) c->background = 0;
}

void cui_configure(const cui_config *c) { C = *c; clamp_cfg(&C); }
void cui_get(cui_config *c) { pthread_mutex_lock(&g_cfg_mx); *c = C; pthread_mutex_unlock(&g_cfg_mx); }
void cui_update(const cui_config *c) {
    pthread_mutex_lock(&g_cfg_mx);
    C.opacity = c->opacity; C.ui_size = c->ui_size; C.peek_size = c->peek_size;
    C.peek_mask = c->peek_mask; C.fast_load = c->fast_load;
    C.theme = c->theme; C.background = c->background;
    if (c->combo) C.combo = c->combo;
    clamp_cfg(&C);
    cui_config copy = C;
    pthread_mutex_unlock(&g_cfg_mx);
    cui_save_config(&copy);
}
static void settings_changed(void) {
    clamp_cfg(&C);
    cui_config copy = C;
    cui_save_config(&copy);
}

/* ------------------------------------------------------------------ actions */

/* swap animation: what leaves a spot rises off and fades, what arrives drops on with a small bounce */
#define LIFT_MS 450
#define SWAP_GAP_MS 300   /* the new piece waits this long when it replaces another */
#define DROP_MS 500

enum { ACT_NONE, ACT_PLACE, ACT_REMOVE, ACT_CLEAR };
static pthread_mutex_t g_act_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_act_cv = PTHREAD_COND_INITIALIZER;
static struct { int kind, slot, busy, ready, rc; uint32_t id; } g_act;

static void *action_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_act_mx);
        while (!g_act.kind) pthread_cond_wait(&g_act_cv, &g_act_mx);
        int kind = g_act.kind, slot = g_act.slot;
        uint32_t id = g_act.id;
        g_act.kind = ACT_NONE;
        pthread_mutex_unlock(&g_act_mx);
        char msg[200];
        int rc = kind == ACT_PLACE ? game_place(slot, id, msg, sizeof msg)
               : kind == ACT_REMOVE ? game_remove(slot, msg, sizeof msg) : game_clear_all(msg, sizeof msg);
        pthread_mutex_lock(&g_act_mx);
        g_act.rc = rc; g_act.ready = kind; g_act.busy = 0;
        pthread_mutex_unlock(&g_act_mx);
    }
    return 0;
}

static int act_start(int kind, int slot, uint32_t id) {
    pthread_mutex_lock(&g_act_mx);
    int ok = !g_act.busy;
    if (ok) { g_act.kind = kind; g_act.slot = slot; g_act.id = id; g_act.busy = 1; g_act.ready = 0; pthread_cond_signal(&g_act_cv); }
    pthread_mutex_unlock(&g_act_mx);
    if (!ok) game_status_set("Still busy with the last change...");
    return ok ? 0 : -1;
}

static void picker_close(int tell_hook) {
    S.open = 0;
    S.close_at = 0;
    if (tell_hook) game_ext_request(-1, 1);
}

static void act_collect(void) {
    pthread_mutex_lock(&g_act_mx);
    int kind = g_act.ready, rc = g_act.rc;
    g_act.ready = 0;
    pthread_mutex_unlock(&g_act_mx);
    if (kind == ACT_PLACE && rc == 0) {
        S.target = -1;
        if (C.fast_load) S.close_at = ms_now() + LIFT_MS + SWAP_GAP_MS + DROP_MS + 60;   /* let the drop-on play */
    }
}

/* ------------------------------------------------------------------ routing */

static int multi(void) { return S.connected >= 2; }
static int me(void) { return S.owner & 1; }   /* 0 = left pad (player 1), 1 = right pad (player 2) */

/* where an item goes: the chosen spot if it fits, else by the game's rules for the player picking
 * (game_route_rules: one character per pad, two discs under it, one play set on the hexagon) */
static int route(uint32_t id) {
    if (S.target >= 0 && catalog_slot_ok(id, S.target)) return S.target;
    return game_route_rules(&S.snap, id, -1, multi() ? me() : -1, multi() ? S.connected : 1);
}

static int page_for_spot(int s) {
    return s == 0 ? PG_PLAYSETS : s <= 2 ? PG_DISCS : (s == 3 || s == 6) ? PG_CHARS : PG_POWERUPS;
}

/* ------------------------------------------------------------------ item lists */

static int ci_contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* grid cells that are not items: a section header (low bits = section) or an empty gap */
#define CELL_HEAD 0xFF000000u
#define CELL_GAP  0xFE000000u
#define IS_ITEM(v) ((v) && (v) < CELL_GAP)
#define IS_HEAD(v) (((v) & 0xFF000000u) == CELL_HEAD)

/* the focus never rests on a header or gap: move right within the row, else left */
static void sel_fix(void) {
    if (!S.n || IS_ITEM(S.items[S.sel])) return;
    int row = S.sel / GCOLS;
    for (int i = S.sel; i < S.n && i / GCOLS == row; i++) if (IS_ITEM(S.items[i])) { S.sel = i; return; }
    for (int i = S.sel; i >= 0; i--) if (IS_ITEM(S.items[i])) { S.sel = i; return; }
    for (int i = 0; i < S.n; i++) if (IS_ITEM(S.items[i])) { S.sel = i; return; }
}

/* the position of the focused item among the real items (1-based) */
static int sel_ordinal(void) {
    int k = 0;
    for (int i = 0; i <= S.sel && i < S.n; i++) k += IS_ITEM(S.items[i]);
    return k;
}

static void build_items(void) {
    S.n = 0;
    if (S.screen == SCR_SEARCH) {
        if (S.query[0])
            for (int i = 0; i < catalog_count() && S.n < 512; i++) {
                const cat_entry *e = catalog_at(i);
                if (ci_contains(e->name, S.query) || ci_contains(e->aka, S.query)) S.items[S.n++] = e->id;
            }
    } else if (S.page == PG_FAVS) {
        uint32_t f[400];
        int nf = favs_list(f, 400);
        for (int i = 0; i < nf && S.n < 512; i++)
            if (!S.ed || catalog_series_of(f[i]) == S.ed) S.items[S.n++] = f[i];
    } else if (S.page != PG_BASE) {
        /* the disc pages come in sections (the catalog is in section order): every section starts
         * a new row with a header cell, the rest of the row before it is left empty */
        int ng = 0, sectioned = PAGE_KIND[S.page] == K_ROUND || PAGE_KIND[S.page] == K_HEX || PAGE_KIND[S.page] == K_FIGURE;
        const char *last = 0;
        for (int i = 0; i < catalog_count() && S.n < 500; i++) {
            const cat_entry *e = catalog_at(i);
            if (e->kind != PAGE_KIND[S.page] || (S.ed && e->series != S.ed)) continue;
            if (sectioned && e->group[0] && (!last || strcmp(last, e->group)) && ng < MAX_SECTIONS) {
                while (S.n % GCOLS) S.items[S.n++] = CELL_GAP;
                snprintf(S.groups[ng], sizeof S.groups[ng], "%s", e->group);
                S.items[S.n++] = CELL_HEAD | (uint32_t)ng++;
                last = e->group;
            }
            S.items[S.n++] = e->id;
        }
        if (ng == 1) {   /* one section only (an edition filter): no header needed */
            int k = 0;
            for (int i = 0; i < S.n; i++) if (IS_ITEM(S.items[i])) S.items[k++] = S.items[i];
            S.n = k;
        }
    }
    S.real = 0;
    for (int i = 0; i < S.n; i++) S.real += IS_ITEM(S.items[i]);
    if (S.sel >= S.n) S.sel = S.n ? S.n - 1 : 0;
    if (S.sel < 0) S.sel = 0;
    sel_fix();
}

static int grid_rows(void) { return S.screen == SCR_SEARCH ? 2 : GROWS; }

static void grid_scroll(void) {
    int row = S.sel / GCOLS, rows = grid_rows();
    if (row < S.top) S.top = row;
    if (row >= S.top + rows) S.top = row - rows + 1;
}

/* returns 1 when moving up/down left the grid (search: to the keyboard) */
static int grid_move(uint32_t p) {
    if (!S.real) return (p & B_DOWN) != 0;
    int sel = S.sel, out = 0;
    if ((p & B_LEFT) && sel % GCOLS && IS_ITEM(S.items[sel - 1])) sel--;
    if ((p & B_RIGHT) && sel % GCOLS < GCOLS - 1 && sel + 1 < S.n && IS_ITEM(S.items[sel + 1])) sel++;
    if (p & B_UP) {
        if (sel >= GCOLS) sel -= GCOLS;   /* onto a header or a gap: sel_fix() moves along the row */
    }
    if (p & B_DOWN) {
        if (sel + GCOLS < S.n) sel += GCOLS;
        else if (sel / GCOLS < (S.n - 1) / GCOLS) sel = S.n - 1;
        else out = 1;
    }
    S.sel = sel;
    sel_fix();
    grid_scroll();
    return out;
}

static void place_selected(void) {
    if (!S.n || !IS_ITEM(S.items[S.sel])) return;
    uint32_t id = S.items[S.sel];
    S.pick_id = id; S.pick_t = ms_now();   /* it is lifted out of the shelf / album */
    act_start(ACT_PLACE, route(id), id);
}

/* ------------------------------------------------------------------ input */

static const char *KEYS[NKEYS] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
                                  "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
                                  "U", "V", "W", "X", "Y", "Z", "'", "-", "SPACE", "DEL",
                                  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};

static void query_back(void) { size_t n = strlen(S.query); if (n) S.query[n - 1] = 0; S.sel = 0; S.top = 0; }
static void query_add(const char *k) {
    size_t n = strlen(S.query);
    if (!strcmp(k, "DEL")) { query_back(); return; }
    if (n + 1 >= sizeof S.query) return;
    S.query[n] = !strcmp(k, "SPACE") ? ' ' : k[0];
    S.query[n + 1] = 0;
    S.sel = 0; S.top = 0;
}

static void open_search(void) { S.screen = SCR_SEARCH; S.kb = 1; S.key = 0; S.sel = 0; S.top = 0; }

static void set_page(int p) {
    S.page = (p + NPAGES) % NPAGES;
    S.sel = 0; S.top = 0;
    if (S.target >= 0 && PAGE_KIND[S.page] && page_for_spot(S.target) != S.page) S.target = -1;
}

static void settings_adjust(int dir) {
    switch (S.set_sel) {
    case 0: C.opacity = C.opacity + dir * 10; break;
    case 1: C.ui_size = (C.ui_size + dir + 4) % 4; break;
    case 2: C.theme = (C.theme + dir + CUI_THEMES) % CUI_THEMES; break;
    case 3: C.background = (C.background + dir + CUI_BACKGROUNDS) % CUI_BACKGROUNDS; break;
    case 4: C.peek_size = (C.peek_size + dir + 4) % 4; break;
    case 5: C.peek_mask = PEEK_CHOICES[(peek_index() + dir + NPEEK) % NPEEK].mask; break;
    case 6: { int k = open_index(); C.combo = OPEN_CHOICES[((k < 0 ? 0 : k) + (k < 0 ? 0 : dir) + NOPEN) % NOPEN].mask; break; }
    case 7: C.fast_load = !C.fast_load; break;
    default: return;
    }
    settings_changed();
}

static void press(uint32_t p) {
    uint64_t now = ms_now();
    if (S.screen == SCR_SETTINGS) {
        if (p & (B_CIRCLE | B_OPTIONS)) { S.screen = SCR_MAIN; return; }
        if (p & B_UP) S.set_sel = (S.set_sel + NSET - 1) % NSET;
        if (p & B_DOWN) S.set_sel = (S.set_sel + 1) % NSET;
        if (p & (B_LEFT | B_RIGHT)) settings_adjust((p & B_RIGHT) ? 1 : -1);
        if (p & B_CROSS) {
            if (S.set_sel <= 7) settings_adjust(1);
            else if (S.set_sel == 8) {
                if (S.clear_armed && now - S.clear_armed < 2500) { S.clear_armed = 0; act_start(ACT_CLEAR, 0, 0); }
                else { S.clear_armed = now; game_status_set("Press Cross again to clear the whole base."); }
            } else {
                if (S.reset_armed && now - S.reset_armed < 2500) {
                    S.reset_armed = 0;
                    C.opacity = 92; C.ui_size = 1; C.peek_size = 2; C.peek_mask = B_L2; C.fast_load = 1;
                    C.combo = B_L3 | B_R3;
                    C.theme = 0; C.background = 0;
                    settings_changed();
                    game_status_set("Settings reset.");
                } else { S.reset_armed = now; game_status_set("Press Cross again to reset all settings."); }
            }
        }
        return;
    }
    if (p & B_OPTIONS) { S.screen = SCR_SETTINGS; S.set_sel = 0; return; }

    if (S.screen == SCR_SEARCH) {
        if (p & B_CIRCLE) { S.screen = SCR_MAIN; S.sel = 0; S.top = 0; return; }
        if (p & B_SQUARE) { query_back(); return; }
        if (S.kb) {
            int r = S.key / 10, c = S.key % 10;
            if (p & B_LEFT) c = (c + 9) % 10;
            if (p & B_RIGHT) c = (c + 1) % 10;
            if (p & B_DOWN) r = (r + 1) % 4;
            if (p & B_UP) { if (r == 0) { if (S.n) { S.kb = 0; S.sel = S.top * GCOLS; } } else r--; }
            S.key = r * 10 + c;
            if (p & B_CROSS) query_add(KEYS[S.key]);
        } else {
            if (p & B_TRIANGLE) { if (S.n && IS_ITEM(S.items[S.sel])) favs_toggle(S.items[S.sel]); return; }
            if (p & B_CROSS) { place_selected(); return; }
            if (grid_move(p)) S.kb = 1;
        }
        return;
    }

    /* main screen */
    if (p & (B_L1 | B_R1)) { set_page(S.page + ((p & B_R1) ? 1 : -1)); return; }
    if (p & (B_L2 | B_R2)) { S.ed = (S.ed + ((p & B_R2) ? 1 : 3)) % 4; S.sel = 0; S.top = 0; return; }
    if (S.page == PG_BASE) {
        int r = 0, c = 0;
        for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) if (NAV[y][x] == S.spot) { r = y; c = x; }
        if (p & B_LEFT) c = (c + 2) % 3;
        if (p & B_RIGHT) c = (c + 1) % 3;
        if (p & B_UP) r = (r + 2) % 3;
        if (p & B_DOWN) r = (r + 1) % 3;
        S.spot = NAV[r][c];
        if (p & B_CROSS) { int t = S.spot; set_page(page_for_spot(t)); S.target = t; }
        else if (p & B_SQUARE) { if (S.snap.present[S.spot]) act_start(ACT_REMOVE, S.spot, 0); }
        else if (p & B_TRIANGLE) {
            if (S.clear_armed && now - S.clear_armed < 2500) { S.clear_armed = 0; act_start(ACT_CLEAR, 0, 0); }
            else { S.clear_armed = now; game_status_set("Press Triangle again to clear the whole base."); }
        } else if (p & B_CIRCLE) picker_close(1);
        return;
    }
    if (p & B_CIRCLE) { S.target = -1; set_page(PG_BASE); return; }
    if (p & B_SQUARE) { open_search(); return; }
    if (p & B_TRIANGLE) { if (S.n && IS_ITEM(S.items[S.sel])) favs_toggle(S.items[S.sel]); return; }
    if (p & B_CROSS) { place_selected(); return; }
    grid_move(p);
}

static void input_tick(const game_ext *e) {
    uint64_t now = ms_now();
    if (!e->valid) return;
    S.connected = e->connected;
    if (e->open && e->open_seq != S.open_seq) {           /* the hook just opened the picker */
        S.open_seq = e->open_seq;
        S.open = 1;
        S.screen = SCR_MAIN;
        S.owner = (int)e->owner;
        S.target = -1;
        S.ring_rd = e->wr;
        S.nav = C.combo;   /* the opening combo is still held: its release is not a press */
        S.clear_armed = S.reset_armed = 0;
        S.close_at = 0;
        if (S.page == PG_BASE) S.spot = multi() && me() ? 6 : 3;
        build_items();
    }
    if (!e->open && S.open) S.open = 0;
    if (!S.open) return;
    for (; S.ring_rd != e->wr; S.ring_rd++) {
        if (e->wr - S.ring_rd > 32) S.ring_rd = e->wr - 32;
        uint32_t nav = e->ring[S.ring_rd & 31];
        uint32_t pressed = nav & ~S.nav;
        S.nav = nav;
        if (pressed & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) { S.held_since_dir = 1; S.repeat_at = now + 380; }
        if (pressed) { press(pressed); build_items(); }
        if (!S.open) return;
    }
    uint32_t dirs = S.nav & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
    if (!dirs) S.held_since_dir = 0;
    else if (S.held_since_dir && now >= S.repeat_at) { S.repeat_at = now + 110; press(dirs); }
}

/* ================================================================ LEDs */

static int g_led_busy;   /* an animation is running: keep drawing at full rate */
static void led_now(int p, uint8_t out[3], float *bright) {
    if (led_eval(&S.snap.la[p], ms_now(), S.snap.led_unit, out)) g_led_busy = 1;
    *bright = led_level(out);
}

/* ================================================================ drawing */

/* a badge inside a d x d box, centred: art of another size or shape is fitted, never stretched */
static void badge(int id, uint32_t fid, float cx, float cy, float d, float alpha) {
    char name[24];
    snprintf(name, sizeof name, "p%u", fid);
    int tw, th;
    tex_dims(name, &tw, &th);
    float w = d, h = d;
    if (tw > 0 && th > 0 && tw != th) { if (tw > th) h = d * th / tw; else w = d * tw / th; }
    img(id, name, cx - w / 2, cy - h / 2, w, h, alpha, 0);
}

/* light effects over a figure's art, g<id>.png in the badge's own frame:
 *  FX_SABER  Light FX figures: the blade glows in its own colour, as bright as the light of the pad
 *            the figure stands on right now (fades and flashes included), the way the real figures
 *            carry the base's light up into the saber. Off when the pad is dark.
 *  FX_GLOW   glow-in-the-dark (Glowing Buzz): charges while its pad is lit, glows when the pad goes
 *            dark and fades slowly like phosphorescent plastic; only faintly visible in the light.
 *            Put on fully charged, like a figure picked up from a lit room. */
#define GLOW_CHARGE_MS 900.0f    /* lit pad: charged in about a second */
#define GLOW_DECAY_MS 20000.0f   /* dark pad: the afterglow fades over ~20 s */
static struct { uint32_t fid; float charge; uint64_t t; } GL[9];

static float glow_level(int slot, uint32_t fid, float br) {
    uint64_t now = ms_now();
    if (GL[slot].fid != fid) { GL[slot].fid = fid; GL[slot].charge = 1.0f; GL[slot].t = now; }
    float dt = (float)(now - GL[slot].t);
    GL[slot].t = now;
    float ch = GL[slot].charge;
    if (br > 0.05f) ch += (1.0f - ch) * (dt * br / GLOW_CHARGE_MS > 1.0f ? 1.0f : dt * br / GLOW_CHARGE_MS);
    ch *= expf(-dt / GLOW_DECAY_MS * (1.0f - br));   /* it gives its light off while the pad is dim */
    GL[slot].charge = ch;
    float dark = 1.0f - (br > 1.0f ? 1.0f : br);
    return ch * (0.16f + 0.74f * dark * dark);
}

static void saber(int id, uint32_t fid, int slot, float cx, float cy, float d, float alpha) {
    int fx = catalog_fx(fid);
    if (id < 0 || fx == FX_NONE || slot < 0 || slot > 8) return;
    uint8_t c[3];
    float br;
    led_now(slot / 3, c, &br);
    float level = fx == FX_GLOW ? glow_level(slot, fid, br) : (br * 1.25f > 1.0f ? 1.0f : br * 1.25f);
    float a = alpha * level;
    if (fx == FX_GLOW) g_led_busy = 1;   /* the afterglow keeps changing: keep drawing */
    if (a < 0.03f) return;
    char name[24];
    snprintf(name, sizeof name, "g%u", fid);
    img(id, name, cx - d / 2, cy - d / 2, d, d, a, 0);
}

static struct { uint32_t id, old; uint64_t t; int primed; } A[9];

/* notice what changed on each spot since the last frame */
static void anim_track(void) {
    uint64_t now = ms_now();

    for (int s = 0; s < 9; s++) {
        uint32_t cur = S.snap.linked && S.snap.present[s] ? S.snap.fig[s] : 0;
        if (!A[s].primed) { A[s].id = cur; A[s].primed = 1; continue; }
        if (cur != A[s].id) { A[s].old = A[s].id; A[s].id = cur; A[s].t = now; }

    }
}

static const uint8_t *player_col(int pad) { return pad == 0 ? TH->hexc : pad == 1 ? TH->p1 : TH->p2; }

/* ================================================================ the base on a table
 * base3d.png (tools/base3d.py): the base tilted back the way it sits in front of you, with masks
 * for its lights that line up with it and base3d.txt saying where everything goes. Figures and
 * play set pieces stand on their pads by their own bases, power discs lie flat under them,
 * stacked the way they are on the real base (disc 2 on the pad, disc 1 on it, the figure on top).
 * The lights are the real base's: the rim of a pad glows, the light spills onto the white
 * plastic and comes up into the clear base of what stands on it. */
static struct { float aspect, pad[3][4], ring[3][4], spill[3][4], layer, plate[3][4]; } G3 = {
    0.66f,
    {{0.50208f, 0.18723f, 0.12358f, 0.07224f}, {0.30400f, 0.36667f, 0.14576f, 0.08679f}, {0.70901f, 0.35894f, 0.14511f, 0.08708f}},
    {{0.372f, 0.115f, 0.262f, 0.152f}, {0.149f, 0.280f, 0.309f, 0.184f}, {0.556f, 0.272f, 0.308f, 0.185f}},
    {{0.277f, 0.072f, 0.451f, 0.251f}, {0.047f, 0.224f, 0.514f, 0.315f}, {0.453f, 0.216f, 0.504f, 0.316f}},
    0.01389f,
    {{0.277f, 0.072f, 0.451f, 0.251f}, {0.047f, 0.224f, 0.514f, 0.315f}, {0.453f, 0.216f, 0.504f, 0.316f}}};

static void g3_load(void) {
    char *t = slurp(UI_DIR "/base3d.txt", 0);
    if (!t) { LOGC("no base3d.txt: built-in base geometry"); return; }
    char *save = 0;
    for (char *ln = strtok_r(t, "\n", &save); ln; ln = strtok_r(0, "\n", &save)) {
        int p;
        float v[4];
        if (sscanf(ln, "aspect %f", &v[0]) == 1) G3.aspect = v[0];
        else if (sscanf(ln, "layer %f", &v[0]) == 1) G3.layer = v[0];
        else if (sscanf(ln, "pad %d %f %f %f %f", &p, &v[0], &v[1], &v[2], &v[3]) == 5 && p >= 0 && p < 3) memcpy(G3.pad[p], v, sizeof v);
        else if (sscanf(ln, "ring %d %f %f %f %f", &p, &v[0], &v[1], &v[2], &v[3]) == 5 && p >= 0 && p < 3) memcpy(G3.ring[p], v, sizeof v);
        else if (sscanf(ln, "spill %d %f %f %f %f", &p, &v[0], &v[1], &v[2], &v[3]) == 5 && p >= 0 && p < 3) memcpy(G3.spill[p], v, sizeof v);
        else if (sscanf(ln, "plate %d %f %f %f %f", &p, &v[0], &v[1], &v[2], &v[3]) == 5 && p >= 0 && p < 3) memcpy(G3.plate[p], v, sizeof v);
    }
    free(t);
}

static const char *RING3[3] = {"l3d_ring0", "l3d_ring1", "l3d_ring2"}, *SPILL3[3] = {"l3d_spill0", "l3d_spill1", "l3d_spill2"};

/* one item on spot s, layer = how many discs lie under it; lift / scale / alpha animate it */
static void place3d(int widget, int sab, int up, int shadow, uint32_t fid, int s, float bx, float by, float bw, int layer,
                    float lift, float scale, float alpha, float lit, const uint8_t lc[3]) {
    int p = s / 3;
    const float *pd = G3.pad[p];
    float rx = pd[2] * bw, ry = pd[3] * bw;
    float pcx = bx + pd[0] * bw, pcy = by + pd[1] * bw - layer * G3.layer * bw - lift;
    char k = catalog_kind_of(fid);
    char name[24];
    snprintf(name, sizeof name, "p%u", fid);
    if (k == K_FIGURE || k == K_PIECE) {   /* stands on its own base */
        float fx, fy, fw;
        catalog_foot(fid, &fx, &fy, &fw);
        float D;
        if (k == K_PIECE) D = rx * 2 * 1.2f;   /* play set pieces: about as tall as the hexagon is wide */
        else {   /* figures: their base fills most of the pad */
            D = rx * 2 * 0.82f / (fw > 0.2f ? fw : 0.2f);
            float maxD = rx * 2 * 2.0f;   /* a figure on a tiny base doesn't become a giant */
            if (D > maxD) D = maxD;
        }
        D *= scale;
        float left = pcx - fx * D, top = pcy + ry * 0.32f - fy * D;
        if (shadow >= 0) {   /* a soft shadow where its base meets the pad; it firms up as it comes down */
            float near = 1.0f - lift / (bw * 0.25f);
            if (near > 0.0f) {
                float gx = pcx, gy = pcy + lift + ry * 0.32f;
                float sw = fw * D / scale * (1.25f + 0.35f * (1.0f - near)), sh = sw * ry / rx * 0.95f;
                static const uint8_t blk[3] = {0, 0, 0};
                tinted(shadow, "l3d_shadow", gx - sw / 2 + sw * 0.05f, gy - sh * 0.62f, sw, sh, 0.5f * near * near * alpha, blk);
            }
        }
        img(widget, name, left, top, D, D, alpha, 0);
        saber(sab, fid, s, left + D / 2, top + D / 2, D, alpha);
        if (up >= 0 && lit > 0.02f) {   /* the pad light coming up into its clear base */
            float uw = fw * D * 1.1f, uh = uw * 0.8f;
            tinted(up, "l3d_up", left + fx * D - uw / 2, top + fy * D - uh * 0.8f, uw, uh, 0.38f * lit * alpha, lc);
        }
    } else {   /* a power disc lies flat */
        float w = rx * 2 * (p == 0 ? 0.9f : 0.86f) * scale, h = w * ry / rx;
        img(widget, name, pcx - w / 2, pcy - h / 2, w, h, alpha, 0);
    }
}

/* the widgets one view draws a stack with: items by ZIDX, the light up into a figure base by pad */
typedef struct { int art, ghost, sab, up, flash; } wset;
static const wset WS_PICK = {W_ART0, W_GHOST0, W_SAB0, W_UP0, W_FLASH0}, WS_PEEK = {W_PKART0, W_PKGHOST0, W_PKSAB0, W_PKUP0, W_PKFLASH0};

/* the moment something lands on a pad, a ring of light runs out from its rim */
#define RIPPLE_MS 520.0f
static void ripple(const wset *W, int s, float bx, float by, float bw, int layer, float g, float lit, const uint8_t lc[3]) {
    int p = s / 3;
    const float *rg = G3.ring[p];
    float grow = (s % 3 ? 0.92f : 1.02f) * (1.0f + 0.32f * g);
    float rw = rg[2] * bw * grow, rh = rg[3] * bw * grow;
    float rcx = bx + (rg[0] + rg[2] / 2) * bw, rcy = by + (rg[1] + rg[3] / 2) * bw - layer * G3.layer * bw;
    static const uint8_t cool[3] = {225, 238, 255};
    uint8_t c[3];
    for (int k = 0; k < 3; k++) c[k] = lit > 0.05f ? (uint8_t)(lc[k] + (255 - lc[k]) * 0.45f) : cool[k];
    float a = (1.0f - g) * (1.0f - g) * 0.95f;
    tinted(W->flash + p, RING3[p], rcx - rw / 2, rcy - rh / 2, rw, rh, a, c);
}

/* spot s with its swap animation: the old piece lifts off and fades, the new one drops on */
static void piece3d(const wset *W, int s, float bx, float by, float bw, int layer, float lit, const uint8_t lc[3]) {
    int up = s % 3 == 0 ? W->up + s / 3 : -1;
    float dt = A[s].t ? (float)(ms_now() - A[s].t) : 1e9f;
    if (A[s].old && dt < LIFT_MS) {
        float g = dt / LIFT_MS;
        place3d(W->ghost + ZIDX[s], -1, -1, -1, A[s].old, s, bx, by, bw, layer, g * g * bw * 0.2f, 1.0f + 0.2f * g, 1.0f - g, 0, lc);
    }
    if (!S.snap.present[s] || !S.snap.linked) return;
    int sh = s % 3 == 0 ? W->art + SIDX(s / 3) : -1;
    float land = (A[s].old ? SWAP_GAP_MS : 0) + DROP_MS * 0.55f;   /* when the drop first touches down */
    if (A[s].t && dt >= land && dt < land + RIPPLE_MS) ripple(W, s, bx, by, bw, layer, (dt - land) / RIPPLE_MS, lit, lc);
    float f = (dt - (A[s].old ? SWAP_GAP_MS : 0)) / DROP_MS;
    if (f >= 1.0f) { place3d(W->art + AIDX(s), W->sab + ZIDX[s], up, sh, S.snap.fig[s], s, bx, by, bw, layer, 0, 1, 1, lit, lc); return; }
    if (f <= 0.0f) return;
    float u = f - 1.0f, e = 1.0f + 2.70158f * u * u * u + 1.70158f * u * u;   /* ease out, small overshoot */
    place3d(W->art + AIDX(s), W->sab + ZIDX[s], up, sh, S.snap.fig[s], s, bx, by, bw, layer, (1.0f - e) * bw * 0.25f,
            1.12f - 0.12f * e, f < 0.6f ? f / 0.6f : 1.0f, lit * f, lc);
}

/* how many discs lie under spot s */
static int stack_layer(int s) {
    int p = s / 3, k = s % 3;
    return k == 2 ? 0 : k == 1 ? S.snap.present[p * 3 + 2] : S.snap.present[p * 3 + 1] + S.snap.present[p * 3 + 2];
}

/* the base on a table in the box x, y, w, h (it sits at the bottom, what stands on it fills the
 * room above); mark = spot to ring (-1 none) */
static void draw_base(float x, float y, float w, float h, int mark, int mark_is_target) {
    float bw = w, bh = bw * G3.aspect, bx = x, by = y + h - bh;
    img(W_BASE, "base3d", bx, by, bw, bh, 1.0f, 0);
    uint8_t lc[3][3];
    float br[3];
    for (int p = 0; p < 3; p++) {
        led_now(p, lc[p], &br[p]);
        if (br[p] <= 0.02f) continue;
        const float *sp = G3.spill[p], *rg = G3.ring[p];
        tinted(W_GLOW0 + p, SPILL3[p], bx + sp[0] * bw, by + sp[1] * bw, sp[2] * bw, sp[3] * bw, 0.18f + 0.55f * br[p], lc[p]);
        tinted(W_LIT0 + p, RING3[p], bx + rg[0] * bw, by + rg[1] * bw, rg[2] * bw, rg[3] * bw, 0.3f + 0.7f * br[p], lc[p]);
    }
    for (int s = 0; s < 9; s++) {
        int p = s / 3, k = s % 3;
        int layer = stack_layer(s);
        (void)k;
        piece3d(&WS_PICK, s, bx, by, bw, layer, br[p], lc[p]);
        if (s == mark) {   /* the rim of that spot, at the height it is in the stack */
            const float *rg = G3.ring[p];
            float lift = layer * G3.layer * bw, grow = k ? 0.9f : 1.04f;
            float rw = rg[2] * bw * grow, rh = rg[3] * bw * grow, rcx = bx + (rg[0] + rg[2] / 2) * bw, rcy = by + (rg[1] + rg[3] / 2) * bw - lift;
            tinted(mark_is_target ? W_TGT : W_FOCUS, RING3[p], rcx - rw / 2, rcy - rh / 2, rw, rh, 1.0f,
                   mark_is_target ? COL_GOLD : COL_ACCENT);
        }
    }
    /* which side is whose: tags at the front outer edge of each pad */
    static const char *TAG[2] = {"P1", "P2"};
    for (int k = 0; k < 2; k++) {
        const float *pd = G3.pad[k + 1];
        float tw = bw * 0.1f, th = bw * 0.055f;
        float tx = bx + (pd[0] + (k ? 1 : -1) * pd[2] * 0.95f) * bw - tw / 2, ty = by + (pd[1] + pd[3] * 1.15f) * bw - th / 2;
        panel(W_PTAG0 + k, tx, ty, tw, th, player_col(k + 1));
        label(W_PTAGL0 + k, TAG[k], tx, ty + th * 0.16f, tw, (int)(th * 0.62f), 1, 1, COL_DARK, 1);
    }
}

static void draw_tabs(float x0, float y) {
    for (int t = 0; t < NPAGES; t++) {
        float x = x0 + t * 172;
        panel(W_TAB0 + t, x, y, 164, 48, t == S.page ? COL_ACCENT : COL_ROW);
        label(W_TABL0 + t, PAGE_NAMES[t], x, y + 12, 164, 22, 1, 1, t == S.page ? COL_DARK : COL_TEXT, 1);
    }
}

static void draw_editions(float x0, float y) {
    static const char *ED[4] = {"All", "1.0", "2.0", "3.0"};
    for (int i = 0; i < 4; i++) {
        float x = x0 + i * 112;
        uint8_t on[4] = {ED_COL[i][0], ED_COL[i][1], ED_COL[i][2], 255};
        panel(W_ED0 + i, x, y, 104, 40, i == S.ed ? on : COL_ROW);
        label(W_EDL0 + i, ED[i], x, y + 9, 104, 21, 1, 1, i == S.ed ? COL_DARK : COL_TEXT, 1);
    }
}

/* ---------------------------------------------------------------- the display: shelf and album
 * Characters and play set pieces stand on the shelves of a white display case (by franchise); power
 * discs sit in the cut-outs of a collector's album. The highlighted figure steps forward off its
 * shelf, the highlighted disc rises out of its slot; picking one lifts it out. */
static const uint8_t INK[4] = {38, 40, 48, 255}, INK2[4] = {104, 108, 120, 255}, CARD[4] = {252, 251, 248, 255};
#define FOCUS_MS 190.0f
#define PICK_MS 380.0f
#define DOOR_MS 620.0f   /* the display case's glass doors swing open */
#define TURN_MS 460.0f   /* an album page turns */

static float ease_out(float f) { f = f < 0 ? 0 : f > 1 ? 1 : f; return 1.0f - (1.0f - f) * (1.0f - f) * (1.0f - f); }

/* a standing item on a shelf: foot on the board at (cx, fy); grow / forward animate it */
static void shelf_item(int w, int shw, uint32_t id, float cx, float fy, float D, float grow, float fwd, float alpha) {
    float fx, ffy, fw;
    catalog_foot(id, &fx, &ffy, &fw);
    if (catalog_kind_of(id) == K_ROUND || catalog_kind_of(id) == K_HEX) { fx = 0.5f; ffy = 0.97f; fw = 0.5f; D *= 0.8f; }
    float d = D * grow, foot = fy + fwd;
    if (shw >= 0) {   /* its shadow on the board */
        static const uint8_t blk[3] = {0, 0, 0};
        float sw = fw * d * 1.35f, sh = sw * 0.22f;
        tinted(shw, "l3d_shadow", cx - sw / 2, foot - sh * 0.6f, sw, sh, (0.3f + 0.25f * (grow - 1.0f) * 6) * alpha, blk);
    }
    char name[24];
    snprintf(name, sizeof name, "p%u", id);
    img(w, name, cx - fx * d, foot - ffy * d, d, d, alpha, 0);
}

static void draw_grid(float gx, float gy, int rows, int focus) {
    const float CW = 194, CH = 190, GAP = 10;
    char k = S.screen == SCR_SEARCH ? 0 : PAGE_KIND[S.page];
    int album = k == K_ROUND || k == K_HEX;
    uint64_t now = ms_now();
    uint32_t fid = focus && S.n && IS_ITEM(S.items[S.sel]) ? S.items[S.sel] : 0;
    if (fid != S.foc_id) { S.foc_prev = S.foc_id; S.foc_id = fid; S.foc_t = now; }
    float fe = ease_out((now - S.foc_t) / FOCUS_MS), pe = 1.0f - fe;   /* in for the new one, out for the old */
    float pg = S.pick_t && now - S.pick_t < PICK_MS ? (now - S.pick_t) / PICK_MS : -1;
    float th = rows * (CH + GAP), GW = GCOLS * (CW + GAP) + 14;
    img(W_GRIDBG, album ? "album_page" : "shelf_back", gx - 12, gy - 12, GW, th + 14, 1.0f, 0);
    /* what starts the doors / a page turn: opening the picker, another page or edition, another section */
    int key = (int)(S.open_seq * 64 + S.page * 8 + S.ed * 2 + (S.screen == SCR_SEARCH));
    int sec = -1;
    for (int q = S.sel; q >= 0 && q < S.n; q--) if (IS_HEAD(S.items[q])) { sec = (int)(S.items[q] & 0xFF); break; }
    if (key != S.disp_key) {
        S.disp_key = key; S.disp_sec = sec;
        if (S.screen != SCR_SEARCH) { if (album) { S.turn_t = now; S.turn_dir = 1; } else S.door_t = now; }
    } else if (album && sec != S.disp_sec) {
        S.turn_dir = sec > S.disp_sec ? 1 : -1; S.disp_sec = sec; S.turn_t = now;
    }
    if (!album)
        for (int r = 0; r < rows; r++) img(W_PLANK0 + r, "shelf_plank", gx - 12, gy + r * (CH + GAP) + 148, GCOLS * (CW + GAP) + 14, 48, 1.0f, 0);
    for (int i = 0; i < GCOLS * rows; i++) {
        int idx = (S.top + i / GCOLS) * GCOLS + i % GCOLS;
        if (idx >= S.n) continue;
        uint32_t id = S.items[idx];
        float x = gx + (i % GCOLS) * (CW + GAP), y = gy + (i / GCOLS) * (CH + GAP), cx = x + CW / 2;
        if (IS_HEAD(id)) {   /* the shelf's / section's plaque (franchise logo, tools/display.py) and how many */
            int g = (int)(id & 0xFF), cnt = 0;
            for (int q = idx + 1; q < S.n && !IS_HEAD(S.items[q]); q++) cnt += IS_ITEM(S.items[q]);
            char pn[24];
            snprintf(pn, sizeof pn, "plq_%08x", name_hash(S.groups[g]));
            img(W_CSLOT0 + i, pn, x + 2, y + (album ? 12 : 22), CW - 4, (CW - 4) * 232.0f / 360.0f, 1.0f, 0);
            texent *pt = tex_find(pn);
            if (pt && pt->sent < 0)   /* no plaque made for this section (yet): its name instead */
                label(W_CLAB0 + i, S.groups[g], x + 10, y + (album ? 50 : 60), CW - 20, 20, 1, 3, INK, 1);
            char nb[24];
            snprintf(nb, sizeof nb, "%d %s%s", cnt, album ? "disc" : "figure", cnt == 1 ? "" : "s");
            if (!album) panel(W_CELL0 + i, cx - 88, y + 162, 176, 25, CARD);   /* on the shelf's edge, like a name card */
            label(W_CSUB0 + i, nb, cx - 84, y + (album ? 152 : 165), 168, 17, 1, 1, INK2, 1);
            continue;
        }
        if (!IS_ITEM(id)) continue;
        int on_base = 0;
        for (int s = 0; s < 9; s++) if (S.snap.present[s] && S.snap.fig[s] == id) on_base = 1;
        float al = on_base ? 0.45f : 1.0f;
        float lift = id == S.foc_id ? fe : id == S.foc_prev ? pe : 0;   /* highlight, eased */
        int top = id == S.foc_id;   /* the highlighted one is drawn last, over its neighbours */
        float pa = 1.0f, pl = 0;
        if (pg >= 0 && id == S.pick_id) { pa = 1.0f - pg; pl = pg; }   /* just picked: it lifts away */
        char nm[64];
        catalog_name(id, nm, sizeof nm);
        if (album) {
            float sy = y + 78;
            img(W_CSLOT0 + i, k == K_HEX ? "album_slot_h" : "album_slot_r", cx - 76, sy - 76, 152, 152, 1.0f, 0);
            float rise = 20 * lift + 60 * pl, sz = 128 * (1 + 0.1f * lift + 0.15f * pl);
            if (lift > 0.01f || pl > 0) {   /* its shadow on the page as it rises out */
                static const uint8_t blk[3] = {0, 0, 0};
                tinted(top ? W_FSH : W_CSH0 + i, "l3d_shadow", cx - 62, sy + 34, 124, 36, 0.4f * lift * pa, blk);
            }
            badge(top ? W_FART : W_CART0 + i, id, cx, sy - rise, sz, al * pa);
            label(W_CLAB0 + i, nm, x + 4, y + 156, CW - 8, 17, 1, 2, on_base ? INK2 : INK, 1);
        } else {
            float fy = y + 152;
            if (top && lift > 0.01f) {   /* a soft light behind the one that steps forward */
                static const uint8_t warm[3] = {255, 250, 236};
                tinted(W_FSPOT, "glow", cx - 110, fy - 190, 220, 200, 0.55f * lift, warm);
            }
            shelf_item(top ? W_FART : W_CART0 + i, top ? W_FSH : W_CSH0 + i, id, cx, fy - 46 * pl, 150,
                       1.0f + 0.14f * lift + 0.1f * pl, 7 * lift, al * pa);
            panel(W_CELL0 + i, cx - 88, y + 162, 176, 25, CARD);   /* its name card on the shelf's edge */
            label(W_CLAB0 + i, nm, cx - 84, y + 165, 168, 18, 1, 1, on_base ? INK2 : INK, 1);
        }
        if (favs_is(id)) img(W_CSTAR0 + i, "star", x + CW - 36, y + 6, 30, 30, 1.0f, 0);
    }
    /* the glass doors of the case swing open on their hinges (outer edges), the glass catching the light */
    if (!album && S.door_t && now - S.door_t < DOOR_MS) {
        float g = ease_out((now - S.door_t) / DOOR_MS), half = GW / 2, w = half * (1.0f - 0.9f * g), a = 1.0f - 0.55f * g;
        img(W_DOOR0, "glass_door_l", gx - 12, gy - 12, w, th + 14, a, 0);
        img(W_DOOR1, "glass_door_r", gx - 12 + GW - w, gy - 12, w, th + 14, a, 0);
    }
    /* an album page turns over the spine: a blank page folds away and shows the new discs */
    if (album && S.turn_t && now - S.turn_t < TURN_MS) {
        float g = ease_out((now - S.turn_t) / TURN_MS), w = GW * (1.0f - g);
        float x = S.turn_dir > 0 ? gx - 12 : gx - 12 + GW - w;   /* forward: the page folds to the left */
        img(W_TURN, "album_page", x, gy - 12, w, th + 14, 1.0f, 0);
        float sw = 40 + 50 * (1.0f - g), sx = S.turn_dir > 0 ? x + w : x - sw;
        static const uint8_t wh[4] = {255, 255, 255, 255};
        img(W_TURNSH, S.turn_dir > 0 ? "page_shade" : "page_shade_r", sx, gy - 12, sw, th + 14, 0.9f * (1.0f - g), wh);   /* dark at the fold */
    }
    /* the section the focus is in, next to the edition buttons */
    if (focus && S.screen == SCR_MAIN && S.n)
        for (int k = S.sel; k >= 0; k--)
            if (IS_HEAD(S.items[k])) {
                char sb[80];
                snprintf(sb, sizeof sb, "Section: %s", S.groups[S.items[k] & 0xFF]);
                label(W_SECL, sb, gx + 470, gy - 50, 360, 21, 0, 1, COL_ACCENT, 1);
                break;
            }
    int total_rows = (S.n + GCOLS - 1) / GCOLS;
    if (total_rows > rows) {
        float th = rows * (CH + GAP) - GAP, h = th * rows / total_rows, y = gy + th * S.top / total_rows;
        static const uint8_t sb[4] = {40, 52, 80, 255};
        panel(W_SCROLLBG, gx + GCOLS * (CW + GAP) + 2, gy, 8, th, sb);
        panel(W_SCROLL, gx + GCOLS * (CW + GAP) + 2, y, 8, h, COL_ACCENT);
    }
}

static void focused_name(float x, float y) {
    if (!S.n || !IS_ITEM(S.items[S.sel])) return;
    char nm[64], buf[180];
    uint32_t id = S.items[S.sel];
    catalog_name(id, nm, sizeof nm);
    int spot = route(id);
    const char *g = catalog_group(id);
    snprintf(buf, sizeof buf, "%s  -  %s%s%s %d.0  ->  %s", nm, g, *g ? " / " : "", catalog_kind_label(catalog_kind_of(id)),
             catalog_series_of(id), SPOT_NAMES[spot]);
    label(W_RNAME, buf, x, y, 1030, 24, 0, 1, COL_TEXT, 1);
}

/* Base page: the three pads as columns (left pad / hexagon / right pad, the D-pad grid), each
 * with what stands on it and the discs stacked underneath, its owner and its live LED */
static void draw_stacks(float x0, float y0) {
    static const int COLS[3] = {3, 0, 6};
    static const char *HEAD[3] = {"PLAYER 1 - LEFT", "HEXAGON - PLAY SET", "PLAYER 2 - RIGHT"};
    static const char *SUB_TOP[3] = {"Character", "Play set / Toy Box", "Character"};
    static const char *SUB_DISC[3] = {"Power-up disc", "Hex power disc", "Power-up disc"};
    static const int LEDP[3] = {1, 0, 2};
    const float CW = 330, GAP = 20;
    char buf[96];
    for (int k = 0; k < 3; k++) {
        float x = x0 + k * (CW + GAP);
        int pad = LEDP[k], top = COLS[k];
        const uint8_t *pc = player_col(pad);
        uint8_t hc[4] = {pc[0], pc[1], pc[2], 255};
        panel(W_COLH0 + k, x, y0, CW, 50, hc);
        int mine = multi() && pad && pad - 1 == me();
        snprintf(buf, sizeof buf, "%s%s", HEAD[k], mine ? "  (you)" : "");
        label(W_COLHL0 + k, buf, x + 14, y0 + 14, CW - 64, 19, 0, 1, COL_DARK, 1);
        /* the pad's light, as the base shows it right now */
        uint8_t c[3];
        float br;
        led_now(pad, c, &br);
        static const uint8_t off[3] = {40, 46, 60};
        tinted(W_COLLEDBG0 + k, pad == 0 ? "hexfill" : "circfill", x + CW - 48, y0 + 5, 40, 40, 1.0f, COL_DARK);
        tinted(W_COLLED0 + k, pad == 0 ? "hexfill" : "circfill", x + CW - 44, y0 + 9, 32, 32, 1.0f, br > 0.02f ? c : off);
        for (int r = 0; r < 3; r++) {
            int sl = top + r;
            int big = r == 0;
            float y = big ? y0 + 62 : y0 + 330 + (r - 1) * 132, h = big ? 226 : 122;
            int sel = S.spot == sl;
            panel(W_SCARD0 + sl, x, y, CW, h, sel ? COL_ROW_SEL : S.snap.present[sl] ? COL_CELL : COL_ROW);
            char nm[64] = "";
            if (S.snap.present[sl]) catalog_name(S.snap.fig[sl], nm, sizeof nm);
            if (big) {
                if (S.snap.present[sl]) {
                    badge(W_SART0 + sl, S.snap.fig[sl], x + CW / 2, y + 78, 128, 1.0f);
                    saber(W_SSAB0 + k, S.snap.fig[sl], sl, x + CW / 2, y + 78, 128, 1.0f);
                }
                else tinted(W_SART0 + sl, sl == 0 ? "hexring" : "ring", x + CW / 2 - 56, y + 22, 112, 112, 0.7f, COL_MUTED);
                label(W_SNAME0 + sl, S.snap.present[sl] ? nm : "empty", x + 10, y + 150, CW - 20, 22, 1, 2,
                      S.snap.present[sl] ? COL_TEXT : COL_MUTED, 1);
                label(W_SSUB0 + sl, SUB_TOP[k], x + 10, y + 198, CW - 20, 18, 1, 1, COL_MUTED, 1);
            } else {
                if (S.snap.present[sl]) badge(W_SART0 + sl, S.snap.fig[sl], x + 58, y + h / 2, 92, 1.0f);
                else tinted(W_SART0 + sl, sl < 3 ? "hexring" : "ring", x + 18, y + h / 2 - 40, 80, 80, 0.75f, pc);
                snprintf(buf, sizeof buf, "Disc %d  -  %s", r, SUB_DISC[k]);
                label(W_SSUB0 + sl, buf, x + 114, y + 16, CW - 124, 17, 0, 1, COL_MUTED, 1);
                label(W_SNAME0 + sl, S.snap.present[sl] ? nm : "empty", x + 114, y + 44, CW - 124, 20, 0, 2,
                      S.snap.present[sl] ? COL_TEXT : COL_MUTED, 1);
            }
            if (sel) tinted(W_RFOCUS, "frame", x - 6, y - 6, CW + 12, h + 12, 1.0f, COL_ACCENT);
        }
        label(W_CAP0 + k, "stacked underneath:", x + 4, y0 + 298, CW, 17, 0, 1, COL_MUTED, 1);
    }
}

/* the chosen picture behind the picker window */
static int g_bg_sent_gen = -1;
static char g_bg_name[24];
static const char *bg_texture(void) {
    if (C.background <= 0 || C.background >= CUI_BACKGROUNDS) return 0;
    if (C.background != CUI_BG_CUSTOM) { snprintf(g_bg_name, sizeof g_bg_name, "bg%d", C.background); return g_bg_name; }
    int gen = cui_background_gen();
    if (gen != g_bg_sent_gen) {   /* a new photo: let go of the old texture */
        texent *old = g_bg_sent_gen >= 0 ? tex_find(g_bg_name) : 0;
        if (old && old->sent == 1 && g_fd >= 0) { send_msg2(OVL_TEXFREE, &old->id, 4, 0, 0); old->sent = 0; tex_rebind(old->id); }
        g_bg_sent_gen = gen;
        snprintf(g_bg_name, sizeof g_bg_name, "bgc%d", gen);
    }
    texent *t = tex_find(g_bg_name);
    return t && t->sent < 0 ? 0 : g_bg_name;   /* no photo uploaded (yet) */
}

static void draw_picker(void) {
    char buf[200];
    XS = UI_SCALES[C.ui_size]; XO = C.opacity / 100.0f;
    TH = &THEMES[C.theme];
    if (COL_DIM[3]) panel(W_DIM, -400, -400, 2720, 1880, COL_DIM);
    const float WX = 160, WY = 90, WW = 1600, WH = 900, RX = 700;
    const char *bg = bg_texture();
    if (bg) {
        img(W_BGIMG, bg, WX, WY, WW, WH, 1.0f, 0);
        uint8_t w[4] = {COL_WIN[0], COL_WIN[1], COL_WIN[2], TH->win_on_bg};
        panel(W_WIN, WX, WY, WW, WH, w);
    } else panel(W_WIN, WX, WY, WW, WH, COL_WIN);
    label(W_TITLE, "DISNEY INFINITY", WX + 34, WY + 26, 480, 32, 0, 1, COL_TEXT, 1);
    const char *sub = !S.snap.linked ? "game not linked" : !S.snap.opened ? "waiting for the game..." : "base connected";
    if (web_build_mismatch())   /* the web folder (art, catalog, phone page) is from another build */
        snprintf(buf, sizeof buf, "web folder is build %s: replace it", web_build());
    else snprintf(buf, sizeof buf, "%s  |  v%s (%s)", sub, DIMOD_VERSION, DIMOD_BUILD);
    label(W_SUB, buf, WX + 34, WY + 66, 490, 18, 0, 1, web_build_mismatch() ? COL_BAD : S.snap.activated ? COL_GOOD : COL_MUTED, 1);

    /* who is placing, and where */
    if (multi()) snprintf(buf, sizeof buf, "Player %d picking  -  %d controllers", me() + 1, S.connected);
    else snprintf(buf, sizeof buf, "One controller  -  you play on the %s pad", S.snap.present[6] && !S.snap.present[3] ? "right" : "left");
    label(W_OWNER, buf, WX + 34, WY + 640, 500, 21, 0, 1, COL_MUTED, 1);
    int mark = -1, mark_target = 0;
    if (S.screen == SCR_MAIN && S.page == PG_BASE) mark = S.spot;
    else if (S.screen != SCR_SETTINGS && S.n && IS_ITEM(S.items[S.sel])) { mark = route(S.items[S.sel]); mark_target = 1; }
    draw_base(WX + 30, WY + 96, 490, 530, mark, mark_target);
    if (S.target >= 0 && S.screen != SCR_SETTINGS) {
        snprintf(buf, sizeof buf, "Placing on: %s", SPOT_NAMES[S.target]);
        label(W_TARGET, buf, WX + 34, WY + 674, 500, 21, 0, 1, COL_GOLD, 1);
    }

    const char *hint = "";
    if (S.screen == SCR_SETTINGS) {
        label(W_INFO, "SETTINGS", RX, WY + 26, 600, 30, 0, 1, COL_TEXT, 1);
        static const char *ROWS[NSET] = {"Window opacity", "UI size", "Theme", "Background", "Sneak peek size",
                                         "Sneak peek button", "Open picker", "Fast loading", "Clear the base", "Reset all settings"};
        static const char *SIZES[4] = {"90%", "100%", "110%", "120%"}, *PEEKS[4] = {"Off", "Small", "Medium", "Large"};
        for (int i = 0; i < NSET; i++) {
            float y = WY + 96 + i * 66;
            panel(W_SROWBG0 + i, RX, y, 1030, 58, i == S.set_sel ? COL_ROW_SEL : COL_ROW);
            label(W_SLAB0 + i, ROWS[i], RX + 22, y + 16, 520, 24, 0, 1, COL_TEXT, 1);
            char v[64] = "";
            if (i == 0) snprintf(v, sizeof v, "<  %d%%  >", C.opacity);
            else if (i == 1) snprintf(v, sizeof v, "<  %s  >", SIZES[C.ui_size]);
            else if (i == 2) snprintf(v, sizeof v, "<  %s  >", THEMES[C.theme].name);
            else if (i == 3) snprintf(v, sizeof v, "<  %s%s  >", BACKGROUNDS[C.background],
                                      C.background == CUI_BG_CUSTOM && !bg_texture() ? " (upload one from the phone)" : "");
            else if (i == 4) snprintf(v, sizeof v, "<  %s  >", PEEKS[C.peek_size]);
            else if (i == 5) snprintf(v, sizeof v, "<  hold %s  >", cui_mask_name(C.peek_mask));
            else if (i == 6) snprintf(v, sizeof v, "<  hold %s  >", cui_open_name(C.combo));
            else if (i == 7) snprintf(v, sizeof v, "<  %s  >", C.fast_load ? "On" : "Off");
            else snprintf(v, sizeof v, "press X");
            label(W_SVAL0 + i, v, RX + 400, y + 16, 610, 24, 2, 1, i == S.set_sel ? COL_ACCENT : COL_MUTED, 1);
        }
        hint = "Up/Down choose   Left/Right change   X confirm   O back";
    } else if (S.screen == SCR_SEARCH) {
        label(W_INFO, "SEARCH", RX, WY + 26, 300, 30, 0, 1, COL_TEXT, 1);
        static const uint8_t qb[4] = {30, 41, 66, 255};
        panel(W_QBOX, RX, WY + 80, 1030, 54, qb);
        snprintf(buf, sizeof buf, "%s%s", S.query, S.kb ? "_" : "");
        label(W_QTEXT, S.query[0] || S.kb ? buf : "type a name...", RX + 18, WY + 93, 900, 26, 0, 1, S.query[0] ? COL_TEXT : COL_MUTED, 1);
        snprintf(buf, sizeof buf, "%d found", S.n);
        label(W_COUNTL, buf, RX + 830, WY + 34, 200, 22, 2, 1, COL_MUTED, 1);
        draw_grid(RX, WY + 150, 2, !S.kb);
        if (!S.n && S.query[0]) label(W_EMPTY, "Nothing matches", RX, WY + 300, 1030, 26, 1, 1, COL_MUTED, 1);
        for (int k = 0; k < NKEYS; k++) {
            float x = RX + (k % 10) * 103, y = WY + 570 + (k / 10) * 58;
            int foc = S.kb && k == S.key;
            panel(W_KEY0 + k, x, y, 97, 52, foc ? COL_ACCENT : COL_ROW);
            label(W_KEYL0 + k, KEYS[k], x, y + 14, 97, strlen(KEYS[k]) > 1 ? 17 : 23, 1, 1, foc ? COL_DARK : COL_TEXT, 1);
        }
        if (!S.kb) focused_name(RX, WY + 548 - 30);
        hint = S.kb ? "X type   [] delete   Up results   O back" : "X place   /\\ favourite   [] delete   Down keyboard   O back";
    } else if (S.page == PG_BASE) {
        draw_tabs(RX - 10, WY + 26);
        draw_stacks(RX, WY + 96);
        hint = "D-pad spot   X choose for it   [] remove   /\\ x2 clear   L1/R1 pages   OPTIONS settings   O close";
    } else {
        draw_tabs(RX - 10, WY + 26);
        draw_editions(RX, WY + 92);
        snprintf(buf, sizeof buf, "%d", S.real);
        if (S.real) snprintf(buf, sizeof buf, "%d / %d", sel_ordinal(), S.real);
        label(W_COUNTL, buf, RX + 830, WY + 100, 200, 22, 2, 1, COL_MUTED, 1);
        draw_grid(RX, WY + 150, GROWS, 1);
        if (!S.real) label(W_EMPTY, S.page == PG_FAVS ? "No favourites yet - press Triangle on any item" : "Nothing in this edition",
                        RX, WY + 380, 1030, 26, 1, 1, COL_MUTED, 1);
        focused_name(RX, WY + 760);
        hint = "X place   /\\ favourite   [] search   L1/R1 pages   L2/R2 edition   OPTIONS settings   O back";
    }
    label(W_HINT, hint, WX + 34, WY + WH - 52, WW - 68, 21, 0, 1, COL_MUTED, 1);
    char st[160];
    uint64_t when = 0;
    game_status_get(st, sizeof st, &when);
    if (st[0] && ms_now() - when < 4000) {
        int bad = strstr(st, "an't") || strstr(st, "ould not") || strstr(st, "already") || strstr(st, "taken") || strstr(st, "busy");
        label(W_STATUS, st, WX + 34, WY + 712, 480, 21, 0, 3, bad ? COL_BAD : COL_GOOD, 1);
    }
}

/* sneak peek: the three pads on their own, the hexagon at the top of the screen and the two pads
 * in the bottom corners, each the way it looks on the tilted base: its plate, the real lights,
 * what stands on it and the discs stacked under it, with the same swap animation. */
static void draw_peek(void) {
    static const float R[4] = {0, 54, 74, 96};   /* pad radius on screen, by peek size */
    float r = R[C.peek_size < 0 || C.peek_size > 3 ? 2 : C.peek_size];
    if (r <= 0) return;
    XS = 1.0f; XO = C.opacity / 100.0f;
    TH = &THEMES[C.theme];
    const float m = 30;
    for (int p = 0; p < 3; p++) {
        const float *pd = G3.pad[p];
        float bw = r / pd[2];   /* this pad as if the whole base were drawn at width bw */
        float ry = pd[3] * bw;
        float cx = p == 0 ? 960 : p == 1 ? m + r * 1.25f : 1920 - m - r * 1.25f;
        float cy = p == 0 ? m + r * 2.45f : 1080 - m - ry * 1.55f;   /* the hexagon's piece stands up from the top */
        float bx = cx - pd[0] * bw, by = cy - pd[1] * bw;
        uint8_t c[3];
        float br;
        led_now(p, c, &br);
        const float *pl = G3.plate[p], *sp = G3.spill[p], *rg = G3.ring[p];
        static const char *PLATE[3] = {"l3d_plate0", "l3d_plate1", "l3d_plate2"};
        img(W_PKPLATE0 + p, PLATE[p], bx + pl[0] * bw, by + pl[1] * bw, pl[2] * bw, pl[3] * bw, 0.96f, 0);
        if (br > 0.02f) {
            tinted(W_PKSPILL0 + p, SPILL3[p], bx + sp[0] * bw, by + sp[1] * bw, sp[2] * bw, sp[3] * bw, 0.18f + 0.55f * br, c);
            tinted(W_PKLIT0 + p, RING3[p], bx + rg[0] * bw, by + rg[1] * bw, rg[2] * bw, rg[3] * bw, 0.3f + 0.7f * br, c);
        }
        for (int k = 2; k >= 0; k--) piece3d(&WS_PEEK, p * 3 + k, bx, by, bw, stack_layer(p * 3 + k), br, c);
        if (p) {   /* P1 / P2 at the front outer edge of the pad */
            float w = r * 0.56f, h = r * 0.32f;
            float x = cx + (p == 1 ? -1 : 1) * r * 0.98f - w / 2, y = cy + ry * 1.05f - h / 2;
            panel(W_PKTAG0 + p - 1, x, y, w, h, player_col(p));
            label(W_PKTAGL0 + p - 1, p == 1 ? "P1" : "P2", x, y + h * 0.14f, w, (int)(h * 0.66f), 1, 1, COL_DARK, 1);
        }
    }
}

static void layout_once(void) {
    g_led_busy = 0;
    frame_begin();
    if (S.open) draw_picker();
    else if (S.peek_held) draw_peek();
    frame_send();
}

/* ================================================================ thread */

static void *cui_thread(void *arg) {
    (void)arg;
    game_ext_request(0, 0);
    int mode_on = 0;
    uint64_t last_frame = 0;
    uint32_t fav_gen = 0;
    S.spot = 3; S.target = -1; S.page = PG_BASE;
    for (;;) {
        uint64_t now = ms_now();
        game_ext e;
        game_ext_snapshot(&e);
        game_snapshot(&S.snap);
        if (g_fd < 0 && now >= g_next_connect) {
            g_next_connect = now + 2000;
#ifndef CUI_HOST_TEST
            if (ovl_ctl_online())
#endif
                if (!link_open()) g_last_ping = now;
        }
        if (g_fd >= 0) {
            link_poll();
            if (g_fd >= 0 && now - g_last_ping > 1000) { g_last_ping = now; send_msg2(OVL_PING, 0, 0, 0, 0); }
        }
        int want = g_fd >= 0 && e.valid;
        if (want != mode_on || (e.valid && e.mode != (uint32_t)want && now - last_frame > 500)) {
            game_ext_request(want, 0);
            if (want != mode_on) LOGC("TV picker %s", want ? "ready (hold the menu buttons in game)" : "inactive");
            mode_on = want;
        }
        if (g_fd >= 0) {
            g_tex_budget = 6;
            pthread_mutex_lock(&g_cfg_mx);
            input_tick(&e);
            S.peek_held = C.peek_size > 0 && e.valid && !S.open && C.peek_mask && (e.raw & C.peek_mask) == C.peek_mask;
            act_collect();
            anim_track();
            if (S.open && S.close_at && now >= S.close_at) picker_close(1);
            if (S.open && (S.page == PG_FAVS || favs_gen() != fav_gen)) { fav_gen = favs_gen(); build_items(); }
            if (S.open || S.peek_held || now - last_frame > 250) { layout_once(); last_frame = now; }
            pthread_mutex_unlock(&g_cfg_mx);
            tex_evict();
        } else if (S.open) {
            S.open = 0;
        }
        usleep(16 * 1000);
    }
    return 0;
}

int cui_start(void) {
    if (!C.enabled) { LOGC("disabled in config.ini (overlay=0)"); return -1; }
    define_types();
    g3_load();
    pthread_t t, a;
    if (pthread_create(&a, 0, action_thread, 0)) return -1;
    return pthread_create(&t, 0, cui_thread, 0);
}
