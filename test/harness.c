/* Host integration harness: the real hook blob, game link, tag store, phone remote and TV
 * picker in one process, driven by a simulated Disney Infinity "game".
 *   - game thread: enumerates USB, opens the base, keeps an IN pipe, does the activate / challenge
 *   - pad thread: 60 Hz scePadReadState with buttons scripted through a control file
 *   - monitor thread: game_service() like the payload's monitor loop
 * kernel_proc_copyout/copyin are plain memcpy: the "game memory" is this process.
 * Control file (one command per line, deleted once run):  <hex buttons> <frames>   |  sleep <ms>   |
 *   led <pad 1-3> <r> <g> <b> (hex)   |  write <order>   |  two   |  p2 <hex buttons> <frames>
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/hook/hooks.c"
#include "../src/injector/game.h"
#include "../src/injector/tags.h"
#include "../src/injector/catalog.h"
#include "../src/injector/webremote.h"
#include "../src/injector/cui.h"
#include "../src/injector/favs.h"

void web_log(const char *fmt, ...);
void cui_save_config(const cui_config *c) { if (c->combo) game_set_combo(c->combo); web_log("settings saved: opacity %d ui %d peek %d mask 0x%x fast %d theme %d bg %d combo 0x%x", c->opacity, c->ui_size, c->peek_size, c->peek_mask, c->fast_load, c->theme, c->background, c->combo); }

int kernel_proc_copyout(pid_t pid, intptr_t addr, void *buf, size_t len) { (void)pid; memcpy(buf, (void *)addr, len); return 0; }
int kernel_proc_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len) { (void)pid; memcpy((void *)addr, buf, len); return 0; }

static pthread_mutex_t lmx = PTHREAD_MUTEX_INITIALIZER;
void web_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&lmx);
    fprintf(stderr, "[log] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&lmx);
    va_end(ap);
}

static struct { int x; } REAL_KBD;
static void *real_list[2] = {&REAL_KBD, 0};
static long r_GetDeviceList(void ***l) { *l = real_list; return 1; }
static void r_FreeDeviceList(void **l, int u) { (void)l; (void)u; }
static int r_GetDeviceDescriptor(void *d, usb_dev_desc *o) { (void)d; memset(o, 0, sizeof *o); o->idVendor = 0x054C; return 0; }
static volatile uint32_t pad_buttons, pad2_buttons;
static volatile int two_pads;
static int r_PadReadState(int32_t h, void *data) {
    memset(data, 0, 120); uint32_t b = h ? pad2_buttons : pad_buttons; memcpy(data, &b, 4);
    ((uint8_t *)data)[76] = 1;   /* connected */
    ((uint8_t *)data)[4] = ((uint8_t *)data)[5] = ((uint8_t *)data)[6] = ((uint8_t *)data)[7] = 0x80;
    return 0;
}
static int h_usleep(uint32_t us) { return usleep(us); }
static int h_pcreate(void *thr, const void *attr, void *(*entry)(void *), void *arg, const char *name) {
    (void)attr; (void)name; return pthread_create((pthread_t *)thr, 0, entry, arg);
}

/* ------------------------------------------------------------- the "game" */
static usb_xfer xin, xout;
static uint8_t inbuf[32], outbuf[32];
static volatile int reports, game_seq = 1;
static uint8_t last_in[32];
static void in_cb(usb_xfer *t) {
    if (t->status == XS_COMPLETED) {
        memcpy(last_in, t->buffer, 32);
        reports++;
        if (t->buffer[0] == 0xAB) web_log("game: tag %s pad %d order %d", t->buffer[5] ? "removed" : "added", t->buffer[2], t->buffer[4]);
    }
    if (t->status != XS_CANCELLED) h_SubmitTransfer(t);
}
static void out_cb(usb_xfer *t) { (void)t; }
static pthread_mutex_t cmx = PTHREAD_MUTEX_INITIALIZER;
static void cmd(uint8_t c, const uint8_t *d, int n) {
    pthread_mutex_lock(&cmx);
    memset(outbuf, 0, 32);
    outbuf[0] = 0xFF; outbuf[1] = (uint8_t)(n + 2); outbuf[2] = c; outbuf[3] = (uint8_t)game_seq++;
    if (n) memcpy(outbuf + 4, d, n);
    outbuf[4 + n] = di_checksum(outbuf, 4 + n);
    int r0 = reports;
    h_FillInterruptTransfer(&xout, &FAKE_HANDLE, 1, outbuf, 32, out_cb, 0, 0);
    h_SubmitTransfer(&xout);
    for (int i = 0; i < 200 && reports == r0; i++) usleep(1000);
    pthread_mutex_unlock(&cmx);
}

static void *game_thread(void *a) {
    (void)a;
    void **list;
    long n = h_GetDeviceList(&list);
    void *dev = 0, *hdl = 0;
    for (long i = 0; i < n; i++) { usb_dev_desc d; if (!h_GetDeviceDescriptor(list[i], &d) && d.idProduct == 0x0129) dev = list[i]; }
    h_Open(dev, &hdl);
    h_FreeDeviceList(list, 1);
    h_FillInterruptTransfer(&xin, hdl, 0x81, inbuf, 32, in_cb, 0, 100);
    h_SubmitTransfer(&xin);
    cmd(0x80, (const uint8_t *)"(c) Disney 2013", 15);
    uint8_t seed[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    cmd(0x81, seed, 8);
    cmd(0x83, 0, 0);
    web_log("game: base activated, polling");
    /* like the game: colour the pads, poll presence now and then */
    uint8_t l1[4] = {1, 0x00, 0x60, 0xFF}, l2[4] = {2, 0xFF, 0x50, 0x00}, l3[6] = {3, 0x20, 0x07, 0x30, 0xFF, 0x60};
    cmd(0x90, l1, 4); cmd(0x90, l2, 4); cmd(0x92, l3, 6);
    /* a light show the way the game uses them: colour, fade, flash (LED_DEMO=0 keeps them still) */
    const char *demo = getenv("LED_DEMO");
    for (int tick = 0;; tick++) {
        cmd(0xA1, 0, 0);
        usleep(500 * 1000);
        if (demo && !strcmp(demo, "0")) continue;
        switch (tick % 16) {
        case 0: { uint8_t f[6] = {2, 0x10, 0x03, 0x00, 0xA0, 0xFF}; cmd(0x92, f, 6); break; }        /* left: fade to blue x3 */
        case 4: { uint8_t f[7] = {3, 0x04, 0x04, 0x07, 0xFF, 0x20, 0x20}; cmd(0x93, f, 7); break; }  /* right: flash red x7 */
        case 8: { uint8_t f[6] = {1, 0x18, 0xFF, 0xFF, 0xC0, 0x30}; cmd(0x92, f, 6); break; }        /* hexagon: pulse gold forever */
        case 12: { uint8_t f[4] = {3, 0xFF, 0x60, 0x00}; cmd(0x90, f, 4); break; }                   /* right: solid orange */
        }
    }
    return 0;
}

static void *pad_thread(void *a) {
    (void)a;
    uint8_t d[120];
    for (;;) { h_PadReadState(0, d); if (two_pads) h_PadReadState(1, d); usleep(16 * 1000); }
    return 0;
}

static void *monitor_thread(void *a) {
    (void)a;
    for (;;) { game_service(1, (intptr_t)&HDR); usleep(25 * 1000); }
    return 0;
}

int main(int argc, char **argv) {
    const char *ctrl = argc > 1 ? argv[1] : "/tmp/di_ctrl.txt";
    const char *tagdir = argc > 2 ? argv[2] : "/tmp/di_tags";
    HDR.orig[H_GetDeviceList] = (uint64_t)r_GetDeviceList;
    HDR.orig[H_FreeDeviceList] = (uint64_t)r_FreeDeviceList;
    HDR.orig[H_GetDeviceDescriptor] = (uint64_t)r_GetDeviceDescriptor;
    HDR.orig[H_PadReadState] = (uint64_t)r_PadReadState;
    HDR.fn_usleep = (uint64_t)h_usleep;
    HDR.fn_pthread_create = (uint64_t)h_pcreate;
    di_init(&HDR.base);
    web_log("catalog %d", catalog_load("/data/dimod/web/ui/catalog.tsv"));
    tags_init(tagdir);
    web_start(18766);
    favs_load("/tmp/di_favs.txt");
    cui_config cc = {1, 0x100, 2, 0, 0, 0x6, 92, 1, 0, 0};
    if (getenv("THEME")) cc.theme = atoi(getenv("THEME"));
    if (getenv("BG")) cc.background = atoi(getenv("BG"));
    cui_configure(&cc);
    cui_start();
    game_attach(1, (intptr_t)&HDR);
    pthread_t t1, t2, t3;
    pthread_create(&t3, 0, monitor_thread, 0);
    pthread_create(&t1, 0, game_thread, 0);
    pthread_create(&t2, 0, pad_thread, 0);
    for (;;) {
        FILE *f = fopen(ctrl, "r");
        if (!f) { usleep(20000); continue; }
        char ln[128];
        while (fgets(ln, sizeof ln, f)) {
            unsigned b, fr, ms;
            unsigned ord;
            if (sscanf(ln, "write %u", &ord) == 1) {   /* the game saves progress into block 1 (file block 4) */
                uint8_t w[19] = {(uint8_t)ord, 1, 0};
                for (int i = 0; i < 16; i++) w[3 + i] = (uint8_t)(0xA0 + i);
                cmd(0xA3, w, 19);
            } else if (!strncmp(ln, "two", 3)) two_pads = 1;
            else if (sscanf(ln, "led %u %x %x %x", &ord, &b, &fr, &ms) == 4) {   /* led <pad 1-3> <r> <g> <b>: set a pad light */
                uint8_t l[4] = {(uint8_t)ord, (uint8_t)b, (uint8_t)fr, (uint8_t)ms};
                cmd(0x90, l, 4);
            }
            else if (sscanf(ln, "p2 %x %u", &b, &fr) == 2) { pad2_buttons = b; usleep(fr * 16 * 1000 + 8000); pad2_buttons = 0; }
            else if (sscanf(ln, "sleep %u", &ms) == 1) usleep(ms * 1000);
            else if (sscanf(ln, "%x %u", &b, &fr) == 2) { pad_buttons = b; usleep(fr * 16 * 1000 + 8000); }
        }
        fclose(f);
        pad_buttons = 0;
        unlink(ctrl);
    }
}
