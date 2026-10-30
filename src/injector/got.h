/* Finding the game's import slots (see got.c).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../hook/dihook.h"

/* the SDK's (exported, undeclared) view of a loaded module, from ps5-payload-sdk crt/kernel.h */
typedef struct {
    struct { unsigned long le_next, le_prev; } list_entry;
    unsigned long sysvec;
    unsigned int refcount;
    unsigned long size;
    unsigned long symtab, symtabsize, strtab, strtabsize, pltrela, pltrelasize, rela, relasize;
    unsigned long hash, hashsize, dynamic, dynamicsize, sce_comment, sce_commentsize, sce_dynlib, sce_dynlibsize;
    unsigned long unknown1, unknown1size, buckets, bucketssize;
    unsigned int nbuckets;
    unsigned long chains, chainssize;
    unsigned int nchains;
    unsigned long unknown2[7];
} dynlib_dynsec_t;
typedef struct {
    unsigned long next, path, unknown0[2];
    unsigned int refcount;
    unsigned long handle, mapbase, mapsize, textsize, database, datasize, unknown1, unknown1size;
    unsigned long entry, unknown2, vaddrbase;
    unsigned int tlsindex;
    unsigned long tlsinit, tlsinitsize, tlssize, tlsoffset, tlsalign, pltgot, unknown3[6], init, fini;
    unsigned long eh_frame_hdr, eh_frame_hdr_size, eh_frame, eh_frame_size;
    int status, flags;
    unsigned long unknown4[5], dynsec, unknown5[6];
} dynlib_obj_t;
int kernel_dynlib_obj(int pid, unsigned int handle, dynlib_obj_t *obj);
int kernel_dynlib_find_handle(int pid, unsigned long addr, unsigned int *handle);
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } elf_rela;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } elf_sym;

#define MAX_SLOTS 6
enum { SC_BAD = 0, SC_BOUND = 1, SC_LAZY = 2, SC_FOREIGN = 3 };
extern const char *SC_NAME[];
extern intptr_t g_slot[H_COUNT][MAX_SLOTS];
extern uint64_t g_slotval[H_COUNT][MAX_SLOTS];
extern int g_slotcls[H_COUNT][MAX_SLOTS];
extern int g_nslot[H_COUNT];
extern intptr_t g_resolved[H_COUNT];
extern intptr_t g_orig[H_COUNT];

int find_handle(pid_t pid, const char *lib, uint32_t *h);
intptr_t resolve(pid_t pid, const char *lib, const char *nid);
int rd(pid_t pid, intptr_t a, void *b, size_t n);
int classify(int h, uint64_t v);
int usable(int h);
int usb_hookable(void);
/* resolve the hook targets, find and classify every slot; returns the slot count */
int discover(pid_t pid, intptr_t base);
extern int got_quiet;
extern int got_reloc_tries;
