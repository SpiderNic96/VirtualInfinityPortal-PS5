/* Disney Infinity virtual base - PS5 injector payload.
 *
 * Waits for a PS4 Disney Infinity game (2.0 or 3.0, any region) to start, injects the hook
 * module (dihook.bin) into the game process and redirects the game's libSceUsbd / libScePad
 * imports to it. The GOT slots are read from the eboot's own relocation tables at runtime, so
 * no per-version offsets are needed. Stays resident: saves figure progress to /data/dimod/tags,
 * serves the phone remote and drives the TV picker in the system UI.
 *
 * Send to an ELF loader (etaHEN, elfldr on port 9021):  socat -u FILE:diinject.elf TCP:<ps5>:9021
 *
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later
 * Process lookup, remote allocation and GOT patching follow the LEGO Dimensions Virtual Toy Pad. */
#define DI_INJECTOR 1
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#include "../hook/dihook.h"
#include "../core/ditag.h"
#include "imports.h"
#include "blob.h"      /* DIHOOK_BIN / DIHOOK_BIN_len, generated from build/dihook.bin */
#include "pt.h"
#include "game.h"
#include "tags.h"
#include "catalog.h"
#include "webremote.h"
#include "cui.h"
#include "favs.h"
#include "../overlay/controller/ovl_controller.h"

#define DATA_DIR "/data/dimod"
#define TAG_DIR  DATA_DIR "/tags"
#define LOG_FILE DATA_DIR "/diinject.log"
#define CATALOG  DATA_DIR "/web/ui/catalog.tsv"

/* confirmed title IDs; other regions are recognised by the game's own title (param.sfo) */
static const char *KNOWN_TITLES[] = {"CUSA00600" /* 2.0 US */, "CUSA01705" /* 3.0 US */, "CUSA01862" /* 3.0 EU */, 0};

/* ---------------------------------------------------------------- utils */

typedef struct { char useless1[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
typedef struct { uint32_t app_id; uint64_t unknown1; uint32_t app_type; char title_id[10]; char unknown2[0x3c]; } app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;
static void logv(const char *fmt, va_list ap) {
    char buf[600];
    vsnprintf(buf, sizeof buf, fmt, ap);
    printf("[diinject] %s\n", buf);
    pthread_mutex_lock(&log_mx);
    struct stat st;
    int big = !stat(LOG_FILE, &st) && st.st_size > 1024 * 1024;
    FILE *f = fopen(LOG_FILE, big ? "w" : "a");
    if (f) {
        time_t t = time(0);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(f, "%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mx);
}
void web_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); logv(fmt, ap); va_end(ap); }
#define LOG(...) web_log(__VA_ARGS__)

static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* the overlay crash-loop guard tripped (ovl_controller) */
static void overlay_guard_notify(const char *msg) { notify("Disney Infinity: %s", msg); }

static uint64_t now_ms(void) { return game_ms(); }

int local_ip(char *out, size_t n) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x08080808);
    socklen_t len = sizeof a;
    int r = -1;
    if (!connect(fd, (struct sockaddr *)&a, sizeof a) && !getsockname(fd, (struct sockaddr *)&a, &len)) {
        inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t)n);
        r = a.sin_addr.s_addr ? 0 : -1;
    }
    close(fd);
    return r;
}

/* ------------------------------------------------------------- config */

static struct {
    uint32_t hold_ms, combo, peek_mask, peek_size, fast_load, overlay, force_fake, restore_pad;
    uint32_t web_port, sniff_log, in_timeouts, tex_mode, opacity, ui_size, theme, background, led_unit;
    char combo_txt[64], peek_txt[64], titles[256];
} cfg = {250, 0x2 | 0x4, 0x100, 2, 1, 1, 0, 1, 8766, 1, 1, 0, 92, 1, 0, 0, LED_UNIT_MS_DEFAULT, "L3+R3", "L2", ""};
static pthread_mutex_t cfg_mx = PTHREAD_MUTEX_INITIALIZER;

#define CONFIG_VERSION 3

static const struct { const char *name; uint32_t bit; } BUTTONS[] = {
    {"L3", 0x2}, {"R3", 0x4}, {"OPTIONS", 0x8}, {"UP", 0x10}, {"RIGHT", 0x20}, {"DOWN", 0x40},
    {"LEFT", 0x80}, {"L2", 0x100}, {"R2", 0x200}, {"L1", 0x400}, {"R1", 0x800},
    {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000}, {"CROSS", 0x4000}, {"SQUARE", 0x8000},
    {"TOUCHPAD", 0x100000}, {0, 0}};

static uint32_t parse_combo(const char *txt) {
    uint32_t mask = 0;
    char tok[16];
    while (*txt) {
        int n = 0;
        while (*txt && *txt != '+' && *txt != '\r' && *txt != '\n' && n < 15) {
            char c = *txt++;
            if (c != ' ') tok[n++] = (char)toupper((unsigned char)c);
        }
        tok[n] = 0;
        if (*txt) txt++;
        if (!n) continue;
        int found = 0;
        for (int i = 0; BUTTONS[i].name; i++) if (!strcmp(tok, BUTTONS[i].name)) { mask |= BUTTONS[i].bit; found = 1; }
        if (!found) { LOG("config: unknown button '%s'", tok); return 0; }
    }
    return mask;
}

static void write_config(void) {
    pthread_mutex_lock(&cfg_mx);
    FILE *f = fopen(DATA_DIR "/config.ini", "w");
    if (!f) { pthread_mutex_unlock(&cfg_mx); return; }
    fprintf(f, "# Disney Infinity - virtual base for PS5\nconfig_version=%d\n\n"
               "# Buttons that open / close the TV picker, joined with '+' (also in the picker's Settings > Open picker).\n"
               "# Names: L1 R1 L2 R2 L3 R3 CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT OPTIONS TOUCHPAD\n"
               "menu_buttons=%s\n# How long they must be held (milliseconds)\nhold_ms=%u\n\n"
               "# TV picker drawn by the PS5 system UI (1/0). The phone remote works either way.\noverlay=%u\n"
               "# Buttons held during gameplay to peek at the base\npeek_buttons=%s\n"
               "# Peek size: 0 off, 1 small, 2 medium, 3 large\npeek_size=%u\n"
               "# Close the picker after placing a figure (1/0)\nfast_load=%u\n"
               "# Picker transparency (30..100 %%) and size (0 90%%, 1 100%%, 2 110%%, 3 120%%)\noverlay_opacity=%u\nui_size=%u\n"
               "# Look: theme 0 Classic, 1 Clear, 2 Light, 3 Crimson, 4 Neon\n"
               "# background 0 none, 1 Starfield, 2 Aurora, 3 Toy Box, 4 your photo (" CUI_CUSTOM_BG ", upload it from the phone)\ntheme=%u\nbackground=%u\n"
               "# Overlay textures: 0 auto, 1 pixel upload, 2 PNG files (try 2 if the art stays blank)\noverlay_textures=%u\n"
               "# Base light timing: milliseconds per step of the game's fade / flash commands\nled_unit_ms=%u\n\n"
               "# Put the figures from the last session back when the game starts (1/0)\nrestore_pad=%u\n"
               "# Emulate even when a real Infinity base is plugged in (1/0)\nforce_fake=%u\n"
               "# Let idle USB reads time out like real hardware (1/0; 0 only for testing)\nusb_timeouts=%u\n"
               "# Phone remote port (http://<ps5-ip>:<port>)\nweb_port=%u\n"
               "# Log every USB report to usb_sniff.log (1/0) - handy for bug reports\nsniff_log=%u\n"
               "# Extra title IDs to treat as Disney Infinity, comma separated (e.g. CUSA00000)\ntitle_ids=%s\n",
            CONFIG_VERSION, cfg.combo_txt, cfg.hold_ms, cfg.overlay, cfg.peek_txt, cfg.peek_size, cfg.fast_load,
            cfg.opacity, cfg.ui_size, cfg.theme, cfg.background, cfg.tex_mode, cfg.led_unit, cfg.restore_pad, cfg.force_fake, cfg.in_timeouts, cfg.web_port, cfg.sniff_log, cfg.titles);
    fclose(f);
    pthread_mutex_unlock(&cfg_mx);
}

/* the picker's Settings (TV or phone) changed something: keep it in config.ini */
void cui_save_config(const cui_config *c) {
    pthread_mutex_lock(&cfg_mx);
    cfg.opacity = (uint32_t)c->opacity;
    cfg.ui_size = (uint32_t)c->ui_size;
    cfg.peek_size = (uint32_t)c->peek_size;
    cfg.fast_load = (uint32_t)c->fast_load;
    cfg.peek_mask = c->peek_mask;
    cfg.theme = (uint32_t)c->theme;
    cfg.background = (uint32_t)c->background;
    snprintf(cfg.peek_txt, sizeof cfg.peek_txt, "%s", cui_mask_name(c->peek_mask));
    if (c->combo && c->combo != cfg.combo) {
        cfg.combo = c->combo;
        snprintf(cfg.combo_txt, sizeof cfg.combo_txt, "%s", cui_open_name(c->combo));
        game_set_combo(c->combo);
        game_status_set("Open the picker with %s from now on.", cfg.combo_txt);
    }
    pthread_mutex_unlock(&cfg_mx);
    write_config();
}

static void load_config(void) {
    FILE *f = fopen(DATA_DIR "/config.ini", "r");
    if (!f) { write_config(); return; }
    char line[320];
    unsigned ver = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        char txt[256];
        if (line[0] == '#') continue;
        if (sscanf(line, "config_version=%u", &v) == 1) ver = v;
        else if (sscanf(line, "menu_buttons=%63[^\r\n]", txt) == 1) {
            uint32_t m = parse_combo(txt);
            if (m) { cfg.combo = m; snprintf(cfg.combo_txt, sizeof cfg.combo_txt, "%s", txt); }
        } else if (sscanf(line, "peek_buttons=%63[^\r\n]", txt) == 1) {
            uint32_t m = parse_combo(txt);
            if (m) { cfg.peek_mask = m; snprintf(cfg.peek_txt, sizeof cfg.peek_txt, "%s", txt); }
        }
        else if (sscanf(line, "hold_ms=%u", &v) == 1) cfg.hold_ms = v > 3000 ? 3000 : v;
        else if (sscanf(line, "overlay=%u", &v) == 1) cfg.overlay = v != 0;
        else if (sscanf(line, "peek_size=%u", &v) == 1) cfg.peek_size = v > 3 ? 3 : v;
        else if (sscanf(line, "fast_load=%u", &v) == 1) cfg.fast_load = v != 0;
        else if (sscanf(line, "overlay_opacity=%u", &v) == 1) cfg.opacity = v < 30 ? 30 : v > 100 ? 100 : v;
        else if (sscanf(line, "ui_size=%u", &v) == 1) cfg.ui_size = v > 3 ? 1 : v;
        else if (sscanf(line, "overlay_textures=%u", &v) == 1) cfg.tex_mode = v > 2 ? 0 : v;
        else if (sscanf(line, "theme=%u", &v) == 1) cfg.theme = v < CUI_THEMES ? v : 0;
        else if (sscanf(line, "background=%u", &v) == 1) cfg.background = v < CUI_BACKGROUNDS ? v : 0;
        else if (sscanf(line, "led_unit_ms=%u", &v) == 1) cfg.led_unit = v < 5 ? 5 : v > 500 ? 500 : v;
        else if (sscanf(line, "restore_pad=%u", &v) == 1) cfg.restore_pad = v != 0;
        else if (sscanf(line, "force_fake=%u", &v) == 1) cfg.force_fake = v != 0;
        else if (sscanf(line, "usb_timeouts=%u", &v) == 1) cfg.in_timeouts = v != 0;
        else if (sscanf(line, "web_port=%u", &v) == 1) cfg.web_port = (v > 0 && v < 65536) ? v : 8766;
        else if (sscanf(line, "sniff_log=%u", &v) == 1) cfg.sniff_log = v != 0;
        else if (sscanf(line, "title_ids=%255[^\r\n]", txt) == 1) snprintf(cfg.titles, sizeof cfg.titles, "%s", txt);
    }
    fclose(f);
    if (ver != CONFIG_VERSION) write_config();
    game_set_led_unit((int)cfg.led_unit);
}

/* ------------------------------------------------------- process lookup */

/* "TITLE" from a param.sfo */
static int sfo_title(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static uint8_t b[64 * 1024];
    size_t len = fread(b, 1, sizeof b, f);
    fclose(f);
    if (len < 20 || memcmp(b, "\0PSF", 4)) return -1;
    uint32_t koff, doff, cnt;
    memcpy(&koff, b + 8, 4); memcpy(&doff, b + 12, 4); memcpy(&cnt, b + 16, 4);
    for (uint32_t i = 0; i < cnt && 20 + i * 16 + 16 <= len; i++) {
        const uint8_t *e = b + 20 + i * 16;
        uint16_t ko; uint32_t dl, dofs;
        memcpy(&ko, e, 2); memcpy(&dl, e + 4, 4); memcpy(&dofs, e + 12, 4);
        if (koff + ko >= len || strcmp((const char *)b + koff + ko, "TITLE")) continue;
        if (doff + dofs + dl > len) return -1;
        snprintf(out, n, "%.*s", (int)dl, (const char *)b + doff + dofs);
        return 0;
    }
    return -1;
}

static int title_is_infinity(const char *tid) {
    static struct { char tid[10]; int yes; } cache[16];
    static int nc;
    for (int i = 0; KNOWN_TITLES[i]; i++) if (!strcmp(tid, KNOWN_TITLES[i])) return 1;
    if (cfg.titles[0] && strstr(cfg.titles, tid)) return 1;
    for (int i = 0; i < nc; i++) if (!strcmp(cache[i].tid, tid)) return cache[i].yes;
    static const char *paths[] = {"/user/appmeta/%s/param.sfo", "/system_data/priv/appmeta/%s/param.sfo",
                                  "/user/app/%s/sce_sys/param.sfo", "/mnt/sandbox/pfsmnt/%s-app0/sce_sys/param.sfo", 0};
    int yes = 0, seen = 0;
    char title[256] = "";
    for (int i = 0; paths[i] && !seen; i++) {
        char p[160];
        snprintf(p, sizeof p, paths[i], tid);
        if (!sfo_title(p, title, sizeof title)) seen = 1;
    }
    if (seen) {
        char low[256];
        size_t k = 0;
        for (; title[k] && k < sizeof low - 1; k++) low[k] = (char)tolower((unsigned char)title[k]);
        low[k] = 0;
        yes = strstr(low, "disney") && strstr(low, "infinity");
        if (yes) LOG("%s is \"%s\"", tid, title);
    }
    /* an unreadable sfo is not cached: the mount may simply not be up yet */
    if (seen && nc < 16) { snprintf(cache[nc].tid, sizeof cache[nc].tid, "%s", tid); cache[nc].yes = yes; nc++; }
    return yes;
}

/* the CUSAxxxxx in the app info (its offset differs between firmwares: search the record) */
static int app_title_id(pid_t pid, char tid[10]) {
    app_info_t ai;
    memset(&ai, 0, sizeof ai);
    if (sceKernelGetAppInfo(pid, &ai)) return -1;
    const char *raw = (const char *)&ai;
    for (size_t o = 0; o + 9 <= sizeof ai; o++) {
        if (memcmp(raw + o, "CUSA", 4)) continue;
        int ok = 1;
        for (int k = 4; k < 9; k++) if (!isdigit((unsigned char)raw[o + k])) ok = 0;
        if (!ok) continue;
        memcpy(tid, raw + o, 9);
        tid[9] = 0;
        return 0;
    }
    return -1;
}

static pid_t find_game(char title[10]) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t sz = 0;
    if (sysctl(mib, 4, 0, &sz, 0, 0)) return -1;
    void *buf = malloc(sz);
    if (!buf) return -1;
    pid_t found = -1;
    if (!sysctl(mib, 4, buf, &sz, 0, 0)) {
        for (char *p = buf; p < (char *)buf + sz && found < 0;) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;
            if (strcmp(ki->ki_comm, "eboot.bin")) continue;
            char tid[10];
            if (app_title_id(ki->ki_pid, tid)) continue;
            if (title_is_infinity(tid)) { found = ki->ki_pid; memcpy(title, tid, 10); }
        }
    }
    free(buf);
    return found;
}

static int proc_alive_fast(pid_t pid) { return kernel_get_proc(pid) != 0; }
static int proc_alive(pid_t pid) {
    char title[10];
    return kernel_get_proc(pid) != 0 && find_game(title) == pid;
}

#include "got.h"
#include "build_id.h"

/* ------------------------------------------------------------ injection */

static int write_got(pid_t pid, intptr_t addr, uint64_t val) {
    intptr_t page = addr & ~0x3FFFL;
    int prot = kernel_get_vmem_protection(pid, page, 0x4000);
    if (kernel_mprotect(pid, page, 0x4000, PROT_READ | PROT_WRITE)) LOG("mprotect GOT RW failed");
    int r = kernel_proc_copyin(pid, &val, addr, sizeof val);
    if (prot > 0) kernel_mprotect(pid, page, 0x4000, prot);
    uint64_t chk = 0;
    kernel_proc_copyout(pid, addr, &chk, sizeof chk);
    return (r || chk != val) ? -1 : 0;
}

long pt_call(pid_t pid, intptr_t addr, ...);

/* mmap RWX in the target (must be attached); raises its privileges like elfldr, then restores them */
static intptr_t alloc_exec(pid_t pid, size_t size) {
    static const uint8_t allcaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t caps[16];
    uint64_t authid = kernel_get_ucred_authid(pid);
    kernel_get_ucred_caps(pid, caps);
    kernel_set_ucred_caps(pid, allcaps);
    kernel_set_ucred_authid(pid, 0x4800000000010003L);
    const int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
    intptr_t mem = pt_mmap(pid, 0, size, rwx, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == -1 || !mem) {
        LOG("mmap RWX failed (errno %d), trying RW + mprotect", pt_errno(pid));
        mem = pt_mmap(pid, 0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (mem != -1 && mem && pt_mprotect(pid, mem, size, rwx)) {
            LOG("mprotect RWX failed (errno %d), trying kernel vm_entry", pt_errno(pid));
            kernel_mprotect(pid, mem, size, rwx);
        }
    }
    kernel_set_ucred_authid(pid, authid);
    kernel_set_ucred_caps(pid, caps);
    if (mem == -1 || !mem) { LOG("remote mmap failed"); return 0; }
    int prot = kernel_get_vmem_protection(pid, mem, size);
    for (int i = 0; i < 5 && !(prot & PROT_EXEC); i++) {   /* PS4 processes may drop PROT_EXEC */
        kernel_mprotect(pid, mem, size, rwx);
        prot = kernel_get_vmem_protection(pid, mem, size);
    }
    if (prot >= 0 && !(prot & PROT_EXEC)) {
        LOG("memory at 0x%lx is not executable (prot=%d)", (long)mem, prot);
        pt_munmap(pid, mem, size);
        return 0;
    }
    return mem;
}

/* figures from the last session, placed in the image before it goes live */
static void restore_pad(di_header *h) {
    FILE *f = fopen(DATA_DIR "/pad.txt", "r");
    if (!f) return;
    int slot, n = 0;
    unsigned id;
    while (fscanf(f, "%d=%u", &slot, &id) == 2) {
        if (slot < 0 || slot >= DI_SLOTS || !id || !catalog_slot_ok(id, slot)) continue;
        uint8_t tag[DI_TAG_SIZE];
        if (tags_get(id, tag)) continue;
        if (!di_place(&h->base, slot, tag, id)) n++;
    }
    fclose(f);
    LOG("restored %d figures from the last session", n);
}

static intptr_t inject(pid_t pid) {
    const di_header *src = (const di_header *)DIHOOK_BIN;
    if (src->magic != DIH_MAGIC || src->version != DIH_VERSION) { LOG("bad blob"); return 0; }
    size_t size = (src->size + 0x3FFF) & ~0x3FFFUL;
    uint8_t *img = calloc(1, size);
    if (!img) return 0;
    memcpy(img, DIHOOK_BIN, DIHOOK_BIN_len);
    di_header *h = (di_header *)img;
    for (int i = 0; i < H_COUNT; i++) h->orig[i] = (uint64_t)g_orig[i];
    h->fn_usleep = (uint64_t)resolve(pid, DI_USLEEP.lib, DI_USLEEP.nid);
    h->fn_pthread_create = (uint64_t)resolve(pid, DI_PTHREAD_CREATE.lib, DI_PTHREAD_CREATE.nid);
    LOG("scePthreadCreate=0x%lx sceKernelUsleep=0x%lx", (long)h->fn_pthread_create, (long)h->fn_usleep);
    h->opt_hold_ms = cfg.hold_ms;
    h->opt_combo = cfg.combo;
    h->opt_flags = (cfg.force_fake ? DIF_FORCE_FAKE : 0) | (cfg.in_timeouts ? 0 : DIF_NO_TIMEOUTS);
    di_init(&h->base);
    if (cfg.restore_pad) restore_pad(h);

    if (pt_attach(pid)) { LOG("pt_attach failed"); free(img); return 0; }
    intptr_t mem = alloc_exec(pid, size);
    int ok = 0;
    if (mem) {
        if (kernel_proc_copyin(pid, img, mem, size)) LOG("copyin blob failed");
        else {
            long r = pt_call(pid, mem + h->ping_off);
            LOG("blob at 0x%lx (%zu bytes) ping=0x%lx", (long)mem, size, r);
            ok = (r == DIH_PING);
        }
        if (!ok) pt_munmap(pid, mem, size);
    }
    if (!ok) { pt_detach(pid, 0); LOG("aborting: blob is not executable in the game process (game left untouched)"); free(img); return 0; }

    /* the game stays stopped (still attached) while its import slots are rewritten: it never runs
     * with half of its USB calls hooked and half not */

    int patched = 0, failed = 0, skipped = 0;
    for (int i = 0; i < H_COUNT; i++)
        for (int k = 0; k < g_nslot[i]; k++) {
            uint64_t cur = 0;
            int cls = rd(pid, g_slot[i][k], &cur, 8) ? SC_BAD : classify(i, cur);   /* re-read: lazy slots bind on first call */
            if (cls == SC_BAD || !g_orig[i]) {
                skipped++;   /* never write a slot we could not validate */
                LOG("not hooking %s at 0x%lx (holds 0x%lx)", DI_IMPORTS[i].name, (long)g_slot[i][k], (long)cur);
                continue;
            }
            if (write_got(pid, g_slot[i][k], (uint64_t)(mem + h->hook_off[i]))) { failed++; LOG("GOT patch failed: %s", DI_IMPORTS[i].name); }
            else patched++;
        }
    pt_detach(pid, 0);
    LOG("hooked %d import slots (%d failed, %d skipped)", patched, failed, skipped);
    free(img);
    return mem;
}

/* already injected (payload re-sent while the game runs)? 0 = no, -1 = other version */
static intptr_t find_existing(pid_t pid) {
    static const int ANCHORS[] = {H_GetDeviceList, H_OpenDeviceWithVidPid, H_Open, H_SubmitTransfer, H_PadReadState, H_PadRead};
    const di_header *h = (const di_header *)DIHOOK_BIN;
    for (size_t a = 0; a < sizeof ANCHORS / sizeof *ANCHORS; a++) {
        int i = ANCHORS[a];
        for (int k = 0; k < g_nslot[i]; k++) {
            if (g_slotcls[i][k] != SC_FOREIGN) continue;
            intptr_t mem = (intptr_t)g_slotval[i][k] - h->hook_off[i];
            uint32_t hdr[2] = {0, 0};
            if (rd(pid, mem, hdr, sizeof hdr) || hdr[0] != DIH_MAGIC) continue;
            return hdr[1] == DIH_VERSION ? mem : -1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------- monitoring */

static void drain_sniff(pid_t pid, intptr_t mem, uint32_t *done) {
    uint32_t sn = 0;
    kernel_proc_copyout(pid, mem + offsetof(di_header, sniff_n), &sn, 4);
    if (sn == *done) return;
    if (!cfg.sniff_log) { *done = sn; return; }
    struct stat sst;
    int big = !stat(DATA_DIR "/usb_sniff.log", &sst) && sst.st_size > 2 * 1024 * 1024;
    FILE *f = fopen(DATA_DIR "/usb_sniff.log", big ? "w" : "a");
    if (sn - *done > DI_SNIFF_MAX) {
        if (f) fprintf(f, "... %u reports not logged\n", sn - *done - DI_SNIFF_MAX);
        *done = sn - DI_SNIFF_MAX;
    }
    uint64_t t = now_ms();
    for (; *done != sn; (*done)++) {
        uint8_t e[36];
        kernel_proc_copyout(pid, mem + offsetof(di_header, sniff) + (*done % DI_SNIFF_MAX) * 36, e, 36);
        if (!f) continue;
        fprintf(f, "%llu %s ", (unsigned long long)t, e[0] ? "IN " : "OUT");
        for (int k = 0; k < e[2] && k < 32; k++) fprintf(f, "%02x", e[4 + k]);
        fprintf(f, "\n");
    }
    if (f) fclose(f);
}

static void monitor(pid_t pid, intptr_t mem) {
    uint64_t last_status = 0;
    int reported = 0, reported_pt = 0;
    uint32_t sniff_done = 0, calls_seen[H_COUNT];
    memset(calls_seen, 0, sizeof calls_seen);
    kernel_proc_copyout(pid, mem + offsetof(di_header, sniff_n), &sniff_done, 4);
    while (proc_alive(pid)) {
        struct { uint32_t seq, passthrough, opened, cmds, pad_calls, pump, delivered, timeouts; } s;
        uint32_t calls[H_COUNT];
        if (kernel_proc_copyout(pid, mem + offsetof(di_header, state_seq), &s, sizeof s)) break;
        kernel_proc_copyout(pid, mem + offsetof(di_header, calls), calls, sizeof calls);
        uint64_t t = now_ms();
        /* which imports the game really uses: logged the first time each one is called */
        for (int i = 0; i < H_COUNT; i++)
            if (calls[i] && !calls_seen[i]) { LOG("game called %s", DI_IMPORTS[i].name); calls_seen[i] = 1; }
        if (t - last_status > 15000) {
            last_status = t;
            LOG("status: opened=%u cmds=%u delivered=%u timeouts=%u pump=%u pad_calls=%u passthrough=%u",
                s.opened, s.cmds, s.delivered, s.timeouts, s.pump, s.pad_calls, s.passthrough);
        }
        if (!reported && s.cmds > 0) {
            reported = 1;
            char ip[32] = "";
            LOG("the game is talking to the virtual base");
            if (!local_ip(ip, sizeof ip))
                notify("Disney Infinity: virtual base connected!\nHold %s for the picker\nPhone: http://%s:%u", cfg.combo_txt, ip, cfg.web_port);
            else notify("Disney Infinity: virtual base connected!\nHold %s for the picker.", cfg.combo_txt);
        }
        if (s.passthrough && !reported_pt) {
            reported_pt = 1;
            LOG("real Infinity base detected: emulation off (force_fake=1 to override)");
            notify("Disney Infinity: a real Infinity base is plugged in - the virtual base stays off.");
        }
        drain_sniff(pid, mem, &sniff_done);
        for (int i = 0; i < 4 && proc_alive_fast(pid); i++) {
            game_service(pid, mem);
            ovl_ctl_tick();
            usleep(25 * 1000);
        }
    }
    LOG("game exited");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mkdir(DATA_DIR, 0777);
    mkdir(TAG_DIR, 0777);
    mkdir(CUI_THEME_DIR, 0777);
    load_config();
    {   /* one instance. The newer build always wins: an older copy started later (an autoloader,
         * a payload sent from an old folder) stops itself instead of replacing the newer one */
        FILE *pf = fopen(DATA_DIR "/diinject.pid", "r");
        int old = 0;
        char old_build[32] = "";
        if (pf) { if (fscanf(pf, "%d %31s", &old, old_build) < 1) old = 0; fclose(pf); }
        if (old > 0 && old != getpid() && kernel_get_proc(old)) {
            if (old_build[0] && strcmp(old_build, DIMOD_BUILD) > 0) {
                LOG("v%s build %s: a newer build %s is already running (pid %d) - this copy stops", DIMOD_VERSION, DIMOD_BUILD,
                    old_build, old);
                notify("Disney Infinity: a newer virtual base (build %s) is already running.\nThis older copy (build %s) stopped.",
                       old_build, DIMOD_BUILD);
                return 0;
            }
            LOG("stopping previous injector pid %d (build %s)", old, old_build[0] ? old_build : "older than 0.6");
            kill(old, SIGKILL);
        }
        pf = fopen(DATA_DIR "/diinject.pid", "w");
        if (pf) { fprintf(pf, "%d %s\n", getpid(), DIMOD_BUILD); fclose(pf); }
    }
    LOG("started v%s build %s (blob %u bytes, %d hooks)", DIMOD_VERSION, DIMOD_BUILD, (unsigned)DIHOOK_BIN_len, H_COUNT);
    int nc = catalog_load(CATALOG);
    LOG("catalog: %d figures%s", nc, nc ? "" : " (missing " CATALOG " - copy the web folder)");
    tags_init(TAG_DIR);
    favs_load(DATA_DIR "/favorites.txt");
    web_start((int)cfg.web_port);
    if (web_build_mismatch()) {
        LOG("web folder is build %s, this payload is %s: replace /data/dimod/web", web_build(), DIMOD_BUILD);
        notify("Disney Infinity: the web folder is from another build (%s).\nReplace /data/dimod/web with the one from this zip (%s).",
               web_build(), DIMOD_BUILD);
    }
    {
        cui_config cc = {(int)cfg.overlay, cfg.peek_mask, (int)cfg.peek_size, (int)cfg.fast_load, (int)cfg.tex_mode, cfg.combo,
                         (int)cfg.opacity, (int)cfg.ui_size, (int)cfg.theme, (int)cfg.background};
        game_set_led_unit((int)cfg.led_unit);
        game_set_combo(cfg.combo);
        cui_configure(&cc);
        ovl_ctl_set_guard(DATA_DIR "/overlay_guard.txt", overlay_guard_notify);
        ovl_ctl_enable((int)cfg.overlay);
        if (cfg.overlay) cui_start();
    }
    notify("Disney Infinity: virtual base v%s is active (build %s).\nStart Disney Infinity 2.0 or 3.0.", DIMOD_VERSION, DIMOD_BUILD);
    for (;;) {
        char title[10];
        pid_t pid = find_game(title);
        if (pid <= 0) { ovl_ctl_tick(); usleep(250 * 1000); continue; }
        LOG("found %s pid %d", title, pid);
        intptr_t base = 0;
        for (int i = 0; i < 100 && !(base = kernel_dynlib_mapbase_addr(pid, 0)); i++) usleep(100 * 1000);
        if (!base) { LOG("eboot base not found"); while (proc_alive(pid)) sleep(1); continue; }
        /* on a cold start the game loads its USB library a little after the eboot is up: keep
         * looking (quietly) for up to 90 s before deciding this version has no USB base */
        discover(pid, base);
        for (int tries = 0; !usb_hookable() && tries < 90 && proc_alive(pid); tries++) {
            if (!tries) LOG("%s: USB library not loaded yet - waiting for the game to finish starting", title);
            sleep(1);
            got_quiet = tries < 89;
            discover(pid, base);
            got_quiet = 0;
            if (usb_hookable()) LOG("USB entry point found after %d s", tries + 1);
        }
        if (!proc_alive(pid)) { LOG("game exited while starting"); continue; }
        if (!usb_hookable()) {
            LOG("%s: no usable sceUsbd entry point (GetDeviceList / OpenDeviceWithVidPid / Open) - see the import lines above", title);
            notify("Disney Infinity: this game version does not use the USB base (see %s).", LOG_FILE);
            while (proc_alive(pid)) sleep(1);
            continue;
        }
        load_config();
        intptr_t mem = find_existing(pid);
        if (mem < 0) {
            LOG("an incompatible virtual base is already active in this game");
            notify("Disney Infinity: close and restart the game to update the virtual base.");
            while (proc_alive(pid)) sleep(1);
            continue;
        }
        if (mem) LOG("already injected at 0x%lx, monitoring", (long)mem);
        else mem = inject(pid);
        if (!mem) {
            notify("Disney Infinity: failed to inject the virtual base (see %s)", LOG_FILE);
            while (proc_alive(pid)) sleep(1);
            continue;
        }
        game_attach(pid, mem);
        monitor(pid, mem);
        game_detach();
    }
    return 0;
}
