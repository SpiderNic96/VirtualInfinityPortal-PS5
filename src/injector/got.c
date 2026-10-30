/* Finding the game's import slots (GOT) for the hooks, for any build of the game.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#define DI_INJECTOR 1
#include "got.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include "imports.h"

void web_log(const char *fmt, ...);
int got_quiet;
int got_reloc_tries = 50;   /* x 200 ms: how long to wait for the loader to finish relocating */   /* retries while the game is still loading: log only the last attempt */
#define LOG(...) do { if (!got_quiet) web_log(__VA_ARGS__); } while (0)

/* ------------------------------------------------------ symbol resolving */

int find_handle(pid_t pid, const char *lib, uint32_t *h) {
    char name[64];
    const char *suffix[] = {".sprx", ".prx", "_web.sprx", "_sys.sprx", 0};
    for (int i = 0; suffix[i]; i++) {
        snprintf(name, sizeof name, "%s%s", lib, suffix[i]);
        if (!kernel_dynlib_handle(pid, name, h)) return 0;
    }
    return -1;
}

intptr_t resolve(pid_t pid, const char *lib, const char *nid) {
    uint32_t h;
    intptr_t a = 0;
    if (!find_handle(pid, lib, &h)) a = kernel_dynlib_resolve(pid, h, nid);
    if (!a && !strcmp(lib, "libkernel")) {
        a = kernel_dynlib_resolve(pid, 0x1, nid);
        if (!a) a = kernel_dynlib_resolve(pid, 0x2001, nid);
    }
    return a;
}

/* ---------------------------------------------------- GOT slot discovery */



const char *SC_NAME[] = {"bad", "bound", "lazy (PLT stub, not called yet)", "bound elsewhere"};
intptr_t g_slot[H_COUNT][MAX_SLOTS];
uint64_t g_slotval[H_COUNT][MAX_SLOTS];
int g_slotcls[H_COUNT][MAX_SLOTS];
int g_nslot[H_COUNT];
intptr_t g_resolved[H_COUNT];
intptr_t g_orig[H_COUNT];          /* what our hook calls through to */
static intptr_t g_img_lo, g_img_hi;       /* the eboot's mapping */

/* game memory read: kernel r/w first, the debugger interface if that is refused */
int rd(pid_t pid, intptr_t a, void *b, size_t n) {
    if (!kernel_proc_copyout(pid, a, b, n)) return 0;
    return mdbg_copyout(pid, a, b, n) ? -1 : 0;
}

static int in_image(uint64_t v) { return v >= (uint64_t)g_img_lo && v < (uint64_t)g_img_hi; }

/* a PS4 eboot's import slot before the loader has bound it: 0xeffffffe'<index> */
static int unbound_marker(uint64_t v) { return (v >> 32) == 0xeffffffeu; }

/* is v inside a module loaded in the game (a system library)? Ranges found are cached per discover */
static pid_t g_pid;
static struct { uint64_t lo, hi; } g_mods[64];
static int g_nmods;
static int in_module(uint64_t v) {
    if (v < 0x10000 || v >= 0x800000000000ull) return 0;
    for (int i = 0; i < g_nmods; i++) if (v >= g_mods[i].lo && v < g_mods[i].hi) return 1;
    unsigned int hnd = 0;
    if (kernel_dynlib_find_handle(g_pid, (unsigned long)v, &hnd)) return 0;
    dynlib_obj_t o;
    if (g_nmods < 64 && !kernel_dynlib_obj(g_pid, hnd, &o) && o.mapsize) {
        g_mods[g_nmods].lo = o.mapbase; g_mods[g_nmods].hi = o.mapbase + o.mapsize; g_nmods++;
    }
    return 1;
}

/* what a GOT slot holds: the bound function, a lazy PLT stub inside the eboot, a function in
 * another loaded module - or anything else (garbage: a wrong base, a slot not bound yet), which
 * is never written */
int classify(int h, uint64_t v) {
    if (!v || unbound_marker(v)) return SC_BAD;
    if (g_resolved[h] && v == (uint64_t)g_resolved[h]) return SC_BOUND;
    if (in_image(v)) return SC_LAZY;
    if (in_module(v)) return SC_FOREIGN;
    return SC_BAD;
}

static void add_slot(pid_t pid, int h, intptr_t a) {
    for (int k = 0; k < g_nslot[h]; k++) if (g_slot[h][k] == a) return;
    if (g_nslot[h] >= MAX_SLOTS) return;
    uint64_t v = 0;
    int cls = rd(pid, a, &v, 8) ? SC_BAD : classify(h, v);
    int k = g_nslot[h]++;
    g_slot[h][k] = a; g_slotval[h][k] = v; g_slotcls[h][k] = cls;
}

static void *kread(unsigned long kaddr, unsigned long len) {
    if (!kaddr || !len || len > (32u << 20)) return 0;
    void *b = malloc(len);
    if (b && kernel_copyout((intptr_t)kaddr, b, len) < 0) { free(b); b = 0; }
    return b;
}

int kernel_dynlib_path(int pid, unsigned int handle, char *path, unsigned long size);

/* libSceUsbd / libScePad by handle scan, when lookup by file name finds nothing */
static uint32_t scan_handle_for(pid_t pid, const char *nid) {
    for (uint32_t h = 1; h < 0x400; h++) {
        if (!kernel_dynlib_mapbase_addr(pid, h)) continue;
        if (kernel_dynlib_resolve(pid, h, nid)) return h;
    }
    return 0;
}

static void resolve_all(pid_t pid) {
    for (int h = 0; h < H_COUNT; h++) g_resolved[h] = resolve(pid, DI_IMPORTS[h].lib, DI_IMPORTS[h].nid);
    static const struct { const char *lib; int anchor; } LIBS[] = {{"libSceUsbd", H_OpenDeviceWithVidPid}, {"libScePad", H_PadReadState}};
    for (int l = 0; l < 2; l++) {
        uint32_t lh;
        int named = !find_handle(pid, LIBS[l].lib, &lh);
        if (named && g_resolved[LIBS[l].anchor]) { LOG("%s: handle 0x%x", LIBS[l].lib, lh); continue; }
        uint32_t sh = scan_handle_for(pid, DI_IMPORTS[LIBS[l].anchor].nid);
        char path[256] = "?";
        if (sh) kernel_dynlib_path(pid, sh, path, sizeof path);
        LOG("%s: name lookup %s, handle scan -> 0x%x (%s)", LIBS[l].lib, named ? "gave no symbols" : "failed", sh, path);
        if (!sh) continue;
        for (int h = 0; h < H_COUNT; h++)
            if (!g_resolved[h] && !strcmp(DI_IMPORTS[h].lib, LIBS[l].lib)) g_resolved[h] = kernel_dynlib_resolve(pid, sh, DI_IMPORTS[h].nid);
    }
    int n = 0;
    for (int h = 0; h < H_COUNT; h++) n += g_resolved[h] != 0;
    LOG("resolved %d of %d hook targets in the game's libraries", n, H_COUNT);
}

/* Relocation offsets are relative to some base; which one depends on how the eboot was linked
 * (PIE, fixed 0x400000, ...). Score every candidate against ALL of the eboot's import slots:
 * the right one puts every slot inside the eboot holding a lazy stub or a bound pointer. */
typedef struct { const char *name; intptr_t add; } base_cand;

static int score_cand(pid_t pid, intptr_t add, const elf_rela *r, unsigned long n, int *sampled) {
    int ok = 0, tot = 0;
    unsigned long step = n > 800 ? n / 800 : 1;
    for (unsigned long i = 0; i < n; i += step) {
        uint32_t type = (uint32_t)r[i].r_info;
        if (type != 7 && type != 6) continue;
        tot++;
        intptr_t a = add + (intptr_t)r[i].r_offset;
        if (!in_image((uint64_t)a)) continue;
        uint64_t v = 0;
        if (rd(pid, a, &v, 8) || !v) continue;
        /* a stub in the eboot, a pointer, or the loader's not-bound-yet marker: all say "this
         * address really is an import slot" */
        if (in_image(v) || unbound_marker(v) || (v >= 0x10000 && v < 0x800000000000ull)) ok++;
    }
    *sampled = tot;
    return ok;
}

static int slots_from_relocations(pid_t pid, intptr_t base) {
    dynlib_obj_t obj;
    dynlib_dynsec_t ds;
    if (kernel_dynlib_obj(pid, 0, &obj) || !obj.dynsec) { LOG("eboot: no module object"); return -1; }
    if (kernel_copyout((intptr_t)obj.dynsec, &ds, sizeof ds) < 0) { LOG("eboot: dynsec unreadable"); return -1; }
    g_img_lo = base;
    g_img_hi = base + (intptr_t)(obj.mapsize ? obj.mapsize : obj.textsize + obj.datasize + 0x1000000);
    LOG("eboot: mapbase 0x%lx mapsize 0x%lx text 0x%lx data 0x%lx/0x%lx vaddrbase 0x%lx", (long)base, obj.mapsize,
        obj.textsize, obj.database, obj.datasize, obj.vaddrbase);
    elf_sym *sym = kread(ds.symtab, ds.symtabsize);
    char *str = kread(ds.strtab, ds.strtabsize);
    elf_rela *tab[2] = {kread(ds.pltrela, ds.pltrelasize), kread(ds.rela, ds.relasize)};
    unsigned long cnt[2] = {ds.pltrelasize / sizeof(elf_rela), ds.relasize / sizeof(elf_rela)};
    int found = 0;
    if (sym && str && (tab[0] || tab[1])) {
        base_cand c[4] = {{"mapbase + r_offset", base}, {"mapbase + r_offset - vaddrbase", base - (intptr_t)obj.vaddrbase},
                          {"mapbase + r_offset - 0x400000", base - 0x400000}, {"r_offset (absolute)", 0}};
        int best = -1, best_score = 0, sampled = 0;
        /* the loader may still be relocating: only a clear majority is trusted (a game caught
         * mid-load once had the wrong formula "win" with 41% and got hooked in the wrong place) */
        for (int tries = 0; tries < got_reloc_tries && best_score * 2 < sampled + (sampled == 0); tries++) {
            if (tries) usleep(200 * 1000);
            best = -1; best_score = 0; sampled = 0;
            for (int k = 0; k < 4; k++) {
                if (k == 1 && !obj.vaddrbase) continue;
                int s0 = 0, s1 = 0, t0 = 0, t1 = 0;
                if (tab[0]) s0 = score_cand(pid, c[k].add, tab[0], cnt[0], &t0);
                if (tab[1]) s1 = score_cand(pid, c[k].add, tab[1], cnt[1], &t1);
                if (!tries) LOG("base candidate %-32s %d/%d slots plausible", c[k].name, s0 + s1, t0 + t1);
                if (s0 + s1 > best_score) { best_score = s0 + s1; best = k; sampled = t0 + t1; }
            }
        }
        if (best < 0 || best_score * 2 < sampled) {
            LOG("no relocation base fits (best %d of %d): not trusting relocation offsets", best_score, sampled);
        } else {
            LOG("relocation base: %s (%d of %d slots plausible)", c[best].name, best_score, sampled);
            unsigned long nsym = ds.symtabsize / sizeof(elf_sym);
            for (int t = 0; t < 2; t++) {
                if (!tab[t]) continue;
                for (unsigned long i = 0; i < cnt[t]; i++) {
                    uint32_t type = (uint32_t)tab[t][i].r_info, si = (uint32_t)(tab[t][i].r_info >> 32);
                    if ((type != 7 && type != 6 && type != 1) || si >= nsym) continue;
                    if (sym[si].st_name + 12 >= ds.strtabsize) continue;
                    const char *nm = str + sym[si].st_name;
                    intptr_t a = c[best].add + (intptr_t)tab[t][i].r_offset;
                    if (!in_image((uint64_t)a)) continue;
                    for (int h = 0; h < H_COUNT; h++)
                        if (!strncmp(nm, DI_IMPORTS[h].nid, 11) && nm[11] == '#') { add_slot(pid, h, a); found++; }
                }
            }
        }
    } else LOG("eboot: symbol or relocation tables unreadable");
    LOG("relocations: %lu symbols, pltrela %lu, rela %lu bytes -> %d hook slots", ds.symtabsize / sizeof(elf_sym),
        ds.pltrelasize, ds.relasize, found);
    free(sym); free(str); free(tab[0]); free(tab[1]);
    return found;
}

/* fallback: scan the eboot for pointers equal to the resolved functions (finds bound slots only) */
static int slots_from_scan(pid_t pid) {
    if (g_img_hi <= g_img_lo || g_img_hi - g_img_lo > (512L << 20)) { LOG("data scan: no usable image range"); return -1; }
    static uint64_t chunk[8192];
    int found = 0, unread = 0;
    for (intptr_t a = g_img_lo & ~7L; a < g_img_hi; a += sizeof chunk) {
        size_t n = (size_t)(g_img_hi - a) < sizeof chunk ? (size_t)(g_img_hi - a) : sizeof chunk;
        if (rd(pid, a, chunk, n)) { unread++; continue; }
        for (size_t i = 0; i < n / 8; i++)
            for (int h = 0; h < H_COUNT; h++)
                if (g_resolved[h] && chunk[i] == (uint64_t)g_resolved[h]) { add_slot(pid, h, a + (intptr_t)(i * 8)); found++; }
    }
    LOG("data scan 0x%lx-0x%lx -> %d slots (%d unreadable chunks)", (long)g_img_lo, (long)g_img_hi, found, unread);
    return found;
}

int usable(int h) {
    int ok = 0;
    for (int k = 0; k < g_nslot[h]; k++) ok |= g_slotcls[h][k] != SC_BAD;
    return ok && g_orig[h];
}

int usb_hookable(void) {
    return usable(H_GetDeviceList) || usable(H_OpenDeviceWithVidPid) || usable(H_Open);
}

int discover(pid_t pid, intptr_t base) {
    memset(g_nslot, 0, sizeof g_nslot);
    g_pid = pid; g_nmods = 0;
    resolve_all(pid);
    slots_from_relocations(pid, base);
    int usb = 0;
    for (int h = 0; h < H_COUNT; h++) if (g_nslot[h] && strcmp(DI_IMPORTS[h].lib, "libScePad")) usb++;
    if (!usb) { LOG("no USB slots from relocations, scanning the eboot"); slots_from_scan(pid); }
    int total = 0;
    for (int h = 0; h < H_COUNT; h++) {
        /* orig: a pointer the game itself was bound to wins; else the resolved export */
        g_orig[h] = g_resolved[h];
        for (int k = 0; k < g_nslot[h]; k++)
            if (g_slotcls[h][k] == SC_BOUND || g_slotcls[h][k] == SC_FOREIGN) { g_orig[h] = (intptr_t)g_slotval[h][k]; break; }
        for (int k = 0; k < g_nslot[h]; k++, total++)
            LOG("import %-36s slot +0x%lx value 0x%lx -> %s (resolved 0x%lx)", DI_IMPORTS[h].name,
                (long)(g_slot[h][k] - base), (long)g_slotval[h][k], SC_NAME[g_slotcls[h][k]], (long)g_resolved[h]);
    }
    LOG("%d hook slots found; USB entry point %s", total, usb_hookable() ? "available" : "MISSING");
    return total;
}

