/* Link between the injected game and the injector's UIs (see game.h).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#define DI_INJECTOR 1
#include "game.h"
#include "catalog.h"
#include "tags.h"
#include "../hook/dihook.h"
#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>

void web_log(const char *fmt, ...);

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;   /* link, snapshot, queue, status */
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_cmd = PTHREAD_MUTEX_INITIALIZER;  /* one figure command at a time */
static pid_t g_pid;
static game_snap g_snap;
static led_anim g_la[3];
static int g_led_unit = LED_UNIT_MS_DEFAULT;
void game_set_led_unit(int ms) { g_led_unit = ms < 5 ? 5 : ms > 500 ? 500 : ms; }
static volatile uint32_t g_combo, g_combo_sent;
void game_set_combo(uint32_t mask) { g_combo = mask; }
static uint32_t g_snap_gen;
static game_ext g_ext;
static struct { int mode, close; } g_ext_req = {-1, 0};
static struct {
    int state;                 /* 0 idle, 1 queued, 2 sent, 3 done */
    uint32_t op, a, b, req;
    int has_stage;
    uint8_t stage[DI_TAG_SIZE];
    uint64_t t0;
    int result;
} g_q;
static char g_status[160];
static uint64_t g_status_t;
static uint32_t g_pad_written[DI_SLOTS];
static int g_pad_valid;

static const char *SLOT_NAMES[DI_SLOTS] = {"the Hexagon", "Hex disc 1", "Hex disc 2",
                                           "the Left pad", "Left disc 1", "Left disc 2",
                                           "the Right pad", "Right disc 1", "Right disc 2"};
const char *game_slot_name(int s) { return (s >= 0 && s < DI_SLOTS) ? SLOT_NAMES[s] : "?"; }

uint64_t game_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void game_status_set(const char *fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_mx);
    memcpy(g_status, b, sizeof g_status);
    g_status_t = game_ms();
    pthread_mutex_unlock(&g_mx);
}

void game_status_get(char *buf, size_t n, uint64_t *when) {
    pthread_mutex_lock(&g_mx);
    snprintf(buf, n, "%s", g_status);
    if (when) *when = g_status_t;
    pthread_mutex_unlock(&g_mx);
}

void game_attach(pid_t pid, intptr_t mem) {
    g_combo_sent = 0;   /* a new game: send the current buttons */
    (void)mem;
    pthread_mutex_lock(&g_mx);
    g_pid = pid;
    memset(&g_snap, 0, sizeof g_snap);
    memset(g_la, 0, sizeof g_la);
    g_ext.valid = 0;
    g_pad_valid = 0;
    pthread_mutex_unlock(&g_mx);
}

void game_detach(void) {
    pthread_mutex_lock(&g_mx);
    g_pid = 0;
    g_snap.linked = 0;
    g_ext.valid = 0;
    if (g_q.state) { g_q.result = -2; g_q.state = 3; pthread_cond_broadcast(&g_cv); }
    pthread_mutex_unlock(&g_mx);
}

#define HOFF(f) offsetof(di_header, f)
#define BOFF(f) (HOFF(base) + offsetof(di_base, f))
#define MB(f) (HOFF(mbox) + offsetof(__typeof__(((di_header *)0)->mbox), f))
#define EXT(f) (HOFF(ext) + offsetof(__typeof__(((di_header *)0)->ext), f))

static void write_pad_file(const di_figure *f) {
    uint32_t now[DI_SLOTS];
    for (int i = 0; i < DI_SLOTS; i++) now[i] = f[i].present ? f[i].fig_id : 0;
    if (g_pad_valid && !memcmp(now, g_pad_written, sizeof now)) return;
    FILE *o = fopen("/data/dimod/pad.txt", "w");
    if (!o) return;
    for (int i = 0; i < DI_SLOTS; i++) if (now[i]) fprintf(o, "%d=%u\n", i, now[i]);
    fclose(o);
    memcpy(g_pad_written, now, sizeof now);
    g_pad_valid = 1;
}

void game_service(pid_t pid, intptr_t mem) {
    static di_figure figs[DI_SLOTS];
    di_led leds[3];
    struct { uint32_t fake_opened, usb_cmds, pad_calls, pump, delivered; } st;
    uint8_t activated = 0;
    int ok = !kernel_proc_copyout(pid, mem + BOFF(fig), figs, sizeof figs) &&
             !kernel_proc_copyout(pid, mem + BOFF(led), leds, sizeof leds) &&
             !kernel_proc_copyout(pid, mem + HOFF(fake_opened), &st, sizeof st) &&
             !kernel_proc_copyout(pid, mem + BOFF(activated), &activated, 1);

    /* the game wrote to a tag: clear the flag first, then read the data, then save it */
    if (ok) {
        for (int i = 0; i < DI_SLOTS; i++) {
            if (!figs[i].dirty || !figs[i].fig_id) continue;
            uint8_t zero = 0;
            intptr_t fo = mem + BOFF(fig) + (intptr_t)(i * sizeof(di_figure));
            kernel_proc_copyin(pid, &zero, fo + offsetof(di_figure, dirty), 1);
            kernel_proc_copyout(pid, fo + offsetof(di_figure, data), figs[i].data, DI_TAG_SIZE);
            if (tags_save(figs[i].fig_id, figs[i].data)) web_log("tags: could not save figure %u", figs[i].fig_id);
            else web_log("tags: saved progress of figure %u (slot %d)", figs[i].fig_id, i);
        }
        write_pad_file(figs);
    }

    __typeof__(((di_header *)0)->ext) ext;
    int eok = !kernel_proc_copyout(pid, mem + HOFF(ext), &ext, sizeof ext);

    pthread_mutex_lock(&g_mx);
    if (ok) {
        g_snap.linked = 1;
        g_snap.opened = st.fake_opened != 0;
        g_snap.activated = activated;
        g_snap.cmds = st.usb_cmds;
        g_snap.delivered = st.delivered;
        for (int i = 0; i < DI_SLOTS; i++) { g_snap.present[i] = figs[i].present; g_snap.fig[i] = figs[i].fig_id; }
        memcpy(g_snap.led, leds, sizeof leds);
        for (int p = 0; p < 3; p++) led_update(&g_la[p], &leds[p], game_ms(), g_led_unit);
        memcpy(g_snap.la, g_la, sizeof g_la);
        g_snap_gen++;
    }
    int want_mode = g_ext_req.mode, want_close = g_ext_req.close;
    g_ext_req.mode = -1; g_ext_req.close = 0;
    if (eok) {
        g_ext.valid = 1;
        g_ext.mode = ext.mode; g_ext.open = ext.open; g_ext.open_seq = ext.open_seq;
        g_ext.close_req = ext.close_req; g_ext.close_ack = ext.close_ack;
        g_ext.raw = ext.raw; g_ext.wr = ext.wr; g_ext.heartbeat = ext.heartbeat;
        memcpy(g_ext.ring, (const void *)ext.ring, sizeof g_ext.ring);
        g_ext.owner = ext.owner;
        int c = 0;
        for (int k = 0; k < 4; k++) if (ext.seen[k] && st.pad_calls - ext.seen[k] < 400) c++;
        g_ext.connected = c;
    }
    int qs = g_q.state;
    uint32_t op = g_q.op, a = g_q.a, b = g_q.b, req = g_q.req;
    int has_stage = g_q.has_stage;
    static uint8_t stage[DI_TAG_SIZE];
    if (qs == 1 && has_stage) memcpy(stage, g_q.stage, sizeof stage);
    uint64_t t0 = g_q.t0;
    pthread_mutex_unlock(&g_mx);

    uint32_t combo = g_combo;
    if (combo && combo != g_combo_sent && !kernel_proc_copyin(pid, &combo, mem + offsetof(di_header, opt_combo), 4))
        g_combo_sent = combo;   /* the hook reads it every frame: the new buttons work straight away */
    if (eok && want_mode >= 0 && (uint32_t)want_mode != ext.mode) {
        uint32_t m = (uint32_t)want_mode;
        kernel_proc_copyin(pid, &m, mem + EXT(mode), 4);
    }
    if (eok && ext.mode) {   /* keep the hook's overlay lease alive */
        uint32_t l = ext.lease + 1;
        kernel_proc_copyin(pid, &l, mem + EXT(lease), 4);
    }
    if (eok && want_close) {
        uint32_t r = ext.close_req + 1;
        kernel_proc_copyin(pid, &r, mem + EXT(close_req), 4);
    }

    if (qs == 1) {
        uint32_t done = 0, args[3] = {op, a, b};
        kernel_proc_copyout(pid, mem + MB(done), &done, 4);
        if (has_stage) kernel_proc_copyin(pid, stage, mem + MB(stage), sizeof stage);
        kernel_proc_copyin(pid, args, mem + MB(op), sizeof args);
        req = done + 1;
        kernel_proc_copyin(pid, &req, mem + MB(req), 4);
        pthread_mutex_lock(&g_mx);
        g_q.req = req; g_q.t0 = game_ms(); g_q.state = 2;
        pthread_mutex_unlock(&g_mx);
    } else if (qs == 2) {
        uint32_t done = 0;
        int32_t res = -3;
        kernel_proc_copyout(pid, mem + MB(done), &done, 4);
        int finished = done == req;
        if (finished) kernel_proc_copyout(pid, mem + MB(result), &res, 4);
        if (finished || game_ms() - t0 > 3000) {
            pthread_mutex_lock(&g_mx);
            g_q.result = res; g_q.state = 3;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mx);
        }
    }
}

void game_snapshot(game_snap *s) {
    pthread_mutex_lock(&g_mx);
    *s = g_snap;
    s->led_unit = g_led_unit;
    if (g_pid <= 0) s->linked = 0;
    pthread_mutex_unlock(&g_mx);
}

void game_ext_snapshot(game_ext *e) {
    pthread_mutex_lock(&g_mx);
    *e = g_ext;
    if (g_pid <= 0) e->valid = 0;
    pthread_mutex_unlock(&g_mx);
}

void game_ext_request(int mode, int close) {
    pthread_mutex_lock(&g_mx);
    if (mode >= 0) g_ext_req.mode = mode;
    if (close) g_ext_req.close = 1;
    pthread_mutex_unlock(&g_mx);
}

/* queue one mailbox request and wait for the monitor thread (caller holds g_cmd) */
static int mailbox(uint32_t op, uint32_t a, uint32_t b, const uint8_t *stage) {
    pthread_mutex_lock(&g_mx);
    if (g_pid <= 0) { pthread_mutex_unlock(&g_mx); return -2; }
    g_q.op = op; g_q.a = a; g_q.b = b;
    g_q.has_stage = stage != 0;
    if (stage) memcpy(g_q.stage, stage, DI_TAG_SIZE);
    g_q.state = 1;
    uint64_t t0 = game_ms();
    while (g_q.state != 3 && g_pid > 0 && game_ms() - t0 < 5000) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mx, &ts);
    }
    int r = g_q.state == 3 ? g_q.result : -3;
    g_q.state = 0;
    pthread_mutex_unlock(&g_mx);
    return r;
}

/* wait until the monitor published a snapshot taken after the last command */
static void wait_snapshot(void) {
    pthread_mutex_lock(&g_mx);
    uint32_t gen = g_snap_gen;
    pthread_mutex_unlock(&g_mx);
    for (int i = 0; i < 40; i++) {
        usleep(25 * 1000);
        pthread_mutex_lock(&g_mx);
        int moved = g_snap_gen - gen >= 2 || g_pid <= 0;
        pthread_mutex_unlock(&g_mx);
        if (moved) return;
    }
}

static const char *why(int r) {
    return r == -2 ? "the game is not running" : r == -3 ? "the game did not answer (paused or loading?)" : "refused";
}

/* Where an item goes, by the game's rules (see game.h). */
static int free_of(const game_snap *s, const int *c, int n) {
    for (int i = 0; i < n; i++) if (!s->present[c[i]]) return c[i];
    return -1;
}

int game_route_rules(const game_snap *s, uint32_t id, int want, int side, int players) {
    if (want >= 0 && want < DI_SLOTS && catalog_slot_ok(id, want)) return want;   /* chosen: swap what is there */
    char kind = catalog_kind_of(id);
    /* one controller: you play on the pad your character stands on (the left one unless only the
     * right one has a character) */
    if (players < 2 || side < 0 || side > 1) {
        if (players < 2) side = s->present[6] && !s->present[3] ? 1 : 0;
        else side = -1;   /* two players, nobody named (the phone): fill what is missing */
    }
    static const int L[] = {4, 5}, R[] = {7, 8}, PADS[] = {3, 6}, HEX[] = {1, 2, 0};
    int k;
    switch (kind) {
    case K_FIGURE:   /* one character per player: a new one replaces it */
        if (side >= 0) return side ? 6 : 3;
        k = free_of(s, PADS, 2);
        return k >= 0 ? k : 3;
    case K_ROUND:    /* two power discs under each character */
        if (side >= 0) { k = free_of(s, side ? R : L, 2); return k >= 0 ? k : (side ? 7 : 4); }
        if (s->present[3] && (k = free_of(s, L, 2)) >= 0) return k;
        if (s->present[6] && (k = free_of(s, R, 2)) >= 0) return k;
        if ((k = free_of(s, L, 2)) >= 0 || (k = free_of(s, R, 2)) >= 0) return k;
        return 4;
    case K_PIECE:    /* one play set / Toy Box piece: a new one replaces it */
        return 0;
    case K_HEX:      /* three on the hexagon (the play set's spot too, when it is free) */
        k = free_of(s, HEX, 3);
        return k >= 0 ? k : 1;
    default:
        return want >= 0 && want < DI_SLOTS ? want : 3;
    }
}

int game_route(uint32_t id, int want) {
    game_snap s;
    game_ext e;
    game_snapshot(&s);
    game_ext_snapshot(&e);
    return game_route_rules(&s, id, want, -1, e.valid ? e.connected : 1);
}

int game_place(int slot, uint32_t id, char *msg, size_t n) {
    char name[64];
    catalog_name(id, name, sizeof name);
    pthread_mutex_lock(&g_cmd);
    game_snap s;
    game_snapshot(&s);
    int r;
    if (!s.linked) { snprintf(msg, n, "Disney Infinity isn't running with the virtual base yet."); r = -2; goto out; }
    if (slot < 0 || slot >= DI_SLOTS) { snprintf(msg, n, "Invalid slot."); r = -1; goto out; }
    if (!catalog_slot_ok(id, slot)) {
        snprintf(msg, n, "%s (%s) can't go on %s.", name, catalog_kind_label(catalog_kind_of(id)), game_slot_name(slot));
        r = -1; goto out;
    }
    for (int i = 0; i < DI_SLOTS; i++)
        if (s.present[i] && s.fig[i] == id) {
            if (i == slot) { snprintf(msg, n, "%s is already there.", name); r = 0; goto out; }
            snprintf(msg, n, "%s is already on %s.", name, game_slot_name(i));
            r = -1; goto out;
        }
    uint8_t tag[DI_TAG_SIZE];
    if (tags_get(id, tag)) { snprintf(msg, n, "Could not create a tag for %s.", name); r = -1; goto out; }
    if (s.present[slot] && slot == 0 && catalog_kind_of(id) == K_PIECE && catalog_kind_of(s.fig[0]) == K_HEX) {
        int to = !s.present[1] ? 1 : !s.present[2] ? 2 : -1;
        uint8_t dt[DI_TAG_SIZE];
        if (to > 0 && !tags_get(s.fig[0], dt) && !mailbox(DI_MB_REMOVE, 0, 0, 0)) {
            s.present[0] = 0;   /* lifted off: the play set spot is free */
            usleep(450 * 1000);
            if (mailbox(DI_MB_PLACE, (uint32_t)to, s.fig[0], dt)) web_log("place: could not move hex disc %u to slot %d", s.fig[0], to);
            usleep(300 * 1000);
        }
    }
    if (s.present[slot]) {
        r = mailbox(DI_MB_REMOVE, (uint32_t)slot, 0, 0);
        if (r) { snprintf(msg, n, "Could not lift the figure off %s: %s.", game_slot_name(slot), why(r)); goto out; }
        usleep(450 * 1000);   /* let the game see the empty slot before the new tag arrives */
    }
    r = mailbox(DI_MB_PLACE, (uint32_t)slot, id, tag);
    if (r) snprintf(msg, n, "Could not place %s: %s.", name, why(r));
    else { snprintf(msg, n, "%s placed on %s.", name, game_slot_name(slot)); wait_snapshot(); }
out:
    pthread_mutex_unlock(&g_cmd);
    if (r) web_log("place %u on %d: %s", id, slot, msg);
    game_status_set("%s", msg);
    return r;
}

int game_remove(int slot, char *msg, size_t n) {
    pthread_mutex_lock(&g_cmd);
    game_snap s;
    game_snapshot(&s);
    int r;
    char name[64];
    if (!s.linked) { snprintf(msg, n, "Disney Infinity isn't running with the virtual base yet."); r = -2; }
    else if (slot < 0 || slot >= DI_SLOTS || !s.present[slot]) { snprintf(msg, n, "Nothing on that slot."); r = -1; }
    else {
        catalog_name(s.fig[slot], name, sizeof name);
        r = mailbox(DI_MB_REMOVE, (uint32_t)slot, 0, 0);
        if (r) snprintf(msg, n, "Could not remove %s: %s.", name, why(r));
        else { snprintf(msg, n, "%s removed.", name); wait_snapshot(); }
    }
    pthread_mutex_unlock(&g_cmd);
    game_status_set("%s", msg);
    return r;
}

int game_move(int from, int to, char *msg, size_t n) {
    game_snap s;
    game_snapshot(&s);
    if (!s.linked) { snprintf(msg, n, "Disney Infinity isn't running with the virtual base yet."); return -2; }
    if (from < 0 || from >= DI_SLOTS || to < 0 || to >= DI_SLOTS || !s.present[from]) { snprintf(msg, n, "Nothing to move."); return -1; }
    if (from == to) { snprintf(msg, n, "Already there."); return 0; }
    uint32_t id = s.fig[from];
    char name[64];
    catalog_name(id, name, sizeof name);
    if (!catalog_slot_ok(id, to)) { snprintf(msg, n, "%s can't go on %s.", name, game_slot_name(to)); return -1; }
    if (s.present[to]) { snprintf(msg, n, "%s is taken - clear it first.", game_slot_name(to)); return -1; }
    int r = game_remove(from, msg, n);
    if (r) return r;
    usleep(450 * 1000);
    r = game_place(to, id, msg, n);
    if (!r) { snprintf(msg, n, "%s moved to %s.", name, game_slot_name(to)); game_status_set("%s", msg); }
    return r;
}

int game_clear_all(char *msg, size_t n) {
    pthread_mutex_lock(&g_cmd);
    int r = mailbox(DI_MB_CLEAR_ALL, 0, 0, 0);
    if (r) snprintf(msg, n, "Could not clear the base: %s.", why(r));
    else { snprintf(msg, n, "Base cleared."); wait_snapshot(); }
    pthread_mutex_unlock(&g_cmd);
    game_status_set("%s", msg);
    return r;
}
