/* got.c against synthetic eboots: lazy PLT slots, bound slots, 0x400000-based offsets,
 * no sceUsbdGetDeviceList import, libSceUsbd only findable by handle scan. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/injector/got.h"
#include "../src/injector/imports.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
void web_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); if (getenv("V")) { vprintf(fmt, ap); printf("\n"); } va_end(ap); }

#define IMG 0x100000
static uint8_t *img;                 /* the eboot as mapped */
static uint8_t usbd_lib[0x1000];     /* where libSceUsbd's functions "are" */
static dynlib_obj_t OBJ; static dynlib_dynsec_t DS;
static elf_sym SYM[64]; static char STR[4096]; static elf_rela PLT[64], RELA[8];
static int name_lookup_ok = 1;

int kernel_copyout(intptr_t k, void *u, size_t n) { memcpy(u, (void *)k, n); return 0; }
static int in_img(intptr_t a, size_t n) { return a >= (intptr_t)img && a + (intptr_t)n <= (intptr_t)img + IMG; }
int kernel_proc_copyout(pid_t p, intptr_t a, void *b, size_t n) { (void)p; if (!in_img(a, n)) return -1; memcpy(b, (void *)a, n); return 0; }
int kernel_proc_copyin(pid_t p, const void *b, intptr_t a, size_t n) { (void)p; if (!in_img(a, n)) return -1; memcpy((void *)a, b, n); return 0; }
int mdbg_copyout(pid_t p, intptr_t a, void *b, size_t n) { return kernel_proc_copyout(p, a, b, n); }
int kernel_dynlib_obj(int pid, unsigned int h, dynlib_obj_t *o) {
    (void)pid;
    if (h == 7) { memset(o, 0, sizeof *o); o->mapbase = (unsigned long)usbd_lib; o->mapsize = sizeof usbd_lib; return 0; }
    if (h) return -1;
    *o = OBJ; return 0;
}
int kernel_dynlib_handle(pid_t p, const char *n, uint32_t *h) { (void)p; if (name_lookup_ok && !strncmp(n, "libSceUsbd.sprx", 15)) { *h = 7; return 0; } if (!strcmp(n, "libScePad.sprx")) { *h = 9; return 0; } return -1; }
intptr_t kernel_dynlib_mapbase_addr(pid_t p, uint32_t h) { (void)p; return (h == 0 || h == 7 || h == 9 || h == 0x33) ? 0x1000 : 0; }
int kernel_dynlib_path(int p, unsigned h, char *b, unsigned long n) { (void)p; snprintf(b, n, "/system/common/lib/libSceUsbd.sprx#%u", h); return 0; }
/* modules: libSceUsbd's functions live in usbd_lib */
int kernel_dynlib_find_handle(int pid, unsigned long a, unsigned int *h) {
    (void)pid; if (a >= (unsigned long)usbd_lib && a < (unsigned long)usbd_lib + sizeof usbd_lib) { *h = 7; return 0; } return -1;
}
static int idx_of(const char *nid) { for (int h = 0; h < H_COUNT; h++) if (!strcmp(DI_IMPORTS[h].nid, nid)) return h; return -1; }
intptr_t kernel_dynlib_resolve(pid_t p, uint32_t h, const char *nid) {
    (void)p; int i = idx_of(nid);
    if (i < 0) return 0;
    if ((h == 7 || h == 0x33) && !strcmp(DI_IMPORTS[i].lib, "libSceUsbd")) return (intptr_t)usbd_lib + i * 16;
    if (h == 9 && !strcmp(DI_IMPORTS[i].lib, "libScePad")) return (intptr_t)usbd_lib + i * 16;
    return 0;
}

/* build an eboot: `imports` by hook index, offsets relative to `rel_base`, lazy or bound values */
static void build(const int *imports, int n, uint64_t rel_base, int lazy) {
    memset(img, 0, IMG); memset(SYM, 0, sizeof SYM); memset(PLT, 0, sizeof PLT);
    int so = 1, ns = 1, np = 0;
    /* some unrelated imports too, so the base scoring has material */
    for (int k = 0; k < n + 20; k++) {
        char nm[32];
        if (k < n) snprintf(nm, sizeof nm, "%s#Z#Z", DI_IMPORTS[imports[k]].nid);
        else snprintf(nm, sizeof nm, "Other%06d#A#B", k);
        SYM[ns].st_name = (uint32_t)so; strcpy(STR + so, nm); so += (int)strlen(nm) + 1;
        uint64_t got_rva = 0x80000 + (uint64_t)np * 8;
        PLT[np].r_offset = rel_base + got_rva;
        PLT[np].r_info = ((uint64_t)ns << 32) | 7;
        uint64_t val = (lazy || k >= n) ? (uint64_t)(intptr_t)img + 0x1000 + np * 16     /* PLT stub in text */
                                        : (uint64_t)(intptr_t)usbd_lib + imports[k] * 16;
        memcpy(img + got_rva, &val, 8);
        ns++; np++;
    }
    DS.symtab = (unsigned long)SYM; DS.symtabsize = ns * sizeof(elf_sym);
    DS.strtab = (unsigned long)STR; DS.strtabsize = (unsigned long)so;
    DS.pltrela = (unsigned long)PLT; DS.pltrelasize = np * sizeof(elf_rela);
    DS.rela = (unsigned long)RELA; DS.relasize = 0;
    memset(&OBJ, 0, sizeof OBJ);
    OBJ.mapbase = (unsigned long)img; OBJ.mapsize = IMG; OBJ.textsize = 0x80000; OBJ.dynsec = (unsigned long)&DS;
    OBJ.vaddrbase = rel_base;
}

int main(void) {
    img = aligned_alloc(0x4000, IMG);
    got_reloc_tries = 3;   /* the real payload waits up to 10 s for a loading game */
    intptr_t base = (intptr_t)img;
    int di3[] = {H_OpenDeviceWithVidPid, H_SubmitTransfer, H_FillInterruptTransfer, H_HandleEventsTimeout, H_ClaimInterface, H_PadReadState};

    /* 1: offsets relative to the mapping, lazy (not called yet) -> all hookable, orig = resolved */
    build(di3, 6, 0, 1);
    CHECK(discover(1, base) == 6);
    CHECK(usb_hookable() && !g_nslot[H_GetDeviceList]);
    CHECK(g_slotcls[H_OpenDeviceWithVidPid][0] == SC_LAZY && g_orig[H_OpenDeviceWithVidPid] == (intptr_t)usbd_lib + H_OpenDeviceWithVidPid * 16);
    CHECK(g_slot[H_SubmitTransfer][0] == base + 0x80008);

    /* 2: a fixed-0x400000 eboot (r_offset absolute-style) -> base candidate 3 wins */
    build(di3, 6, 0x400000, 1);
    OBJ.vaddrbase = 0;   /* and the module object does not tell us */
    CHECK(discover(1, base) == 6 && usb_hookable());
    CHECK(g_slot[H_ClaimInterface][0] == base + 0x80020);

    /* 3: bound at load (bind-now) -> classified bound, orig = the game's own pointer */
    build(di3, 6, 0, 0);
    CHECK(discover(1, base) == 6 && g_slotcls[H_SubmitTransfer][0] == SC_BOUND);

    /* 4: libSceUsbd not found by name -> handle scan */
    name_lookup_ok = 0;
    build(di3, 6, 0, 1);
    CHECK(discover(1, base) == 6 && g_resolved[H_SubmitTransfer] != 0 && usb_hookable());
    name_lookup_ok = 1;

    /* 5: garbage offsets (nothing lines up) -> nothing trusted, nothing written */
    build(di3, 6, 0x7777000, 1);
    OBJ.vaddrbase = 0;
    CHECK(discover(1, base) == 0 && !usb_hookable());

    /* 5b: caught mid-load (the user's Disney Infinity 2.0 log): the right formula is not bound yet
     * and the wrong one reads values that only look like pointers -> no wrong formula is trusted,
     * and a slot holding a "pointer" that is in no loaded module is never hooked */
    build(di3, 6, 0, 1);
    for (int k = 0; k < 26; k++) { uint64_t g = 0xeffffffe00000070ull + k; memcpy(img + 0x80000 + k * 8, &g, 8); }   /* not bound yet */
    CHECK(discover(1, base) == 6 && !usb_hookable());          /* found, but nothing hookable yet */
    CHECK(g_slotcls[H_OpenDeviceWithVidPid][0] == SC_BAD);
    {   uint64_t junk = 0x18038f048eull;                       /* "bound elsewhere" garbage from the log */
        memcpy(img + 0x80000, &junk, 8);
        CHECK(classify(H_OpenDeviceWithVidPid, junk) == SC_BAD); }
    build(di3, 6, 0, 0);                                         /* then the loader binds them */
    CHECK(discover(1, base) == 6 && usb_hookable() && g_slotcls[H_OpenDeviceWithVidPid][0] == SC_BOUND);

    /* 6: no USB imports at all (other game) */
    int pad_only[] = {H_PadReadState};
    build(pad_only, 1, 0, 1);
    CHECK(discover(1, base) == 1 && !usb_hookable());

    printf(fails ? "got: %d FAILED\n" : "got: all checks passed\n", fails);
    return fails != 0;
}
