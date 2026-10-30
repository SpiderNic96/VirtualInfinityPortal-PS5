/* Shared layout between the in-game hook module (blob) and the PS5 injector.
 * The blob is a flat, position independent image that starts with di_header.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include "../core/diproto.h"

#if defined(__clang__) && !defined(DI_INJECTOR)
#pragma GCC visibility push(hidden)   /* blob: every symbol is local, no GOT */
#endif

#define DIH_MAGIC   0x444F4D49u /* "IMOD" */
#define DIH_VERSION 2
#define DIH_PING    0x1D1BA5Eu

/* Hooked imports, in this order (index into hook_off[] / orig[]). Keep in sync with
 * tools/gen_imports.py (injector name/NID table) and src/hook/mkblob.py. */
#define DI_HOOK_LIST(X) \
    X(GetDeviceList) X(FreeDeviceList) X(GetDeviceDescriptor) X(GetBusNumber) X(GetDeviceAddress) \
    X(GetDeviceSpeed) X(OpenDeviceWithVidPid) X(Open) X(Close) X(GetDevice) X(RefDevice) X(UnrefDevice) \
    X(GetConfigDescriptor) X(GetActiveConfigDescriptor) X(GetConfigDescriptorByValue) X(FreeConfigDescriptor) \
    X(GetConfiguration) X(SetConfiguration) X(KernelDriverActive) X(DetachKernelDriver) X(AttachKernelDriver) \
    X(ClaimInterface) X(ReleaseInterface) X(SetInterfaceAltSetting) X(ClearHalt) X(ResetDevice) \
    X(CheckConnected) X(GetMaxPacketSize) X(FillInterruptTransfer) X(FillControlTransfer) X(SubmitTransfer) \
    X(CancelTransfer) X(HandleEvents) X(HandleEventsTimeout) X(HandleEventsLocked) X(InterruptTransfer) \
    X(ControlTransfer) X(GetDescriptor) X(GetStringDescriptor) X(GetStringDescriptorAscii) \
    X(PadReadState) X(PadRead)

#define DI_ENUM(n) H_##n,
enum { DI_HOOK_LIST(DI_ENUM) H_COUNT };
#undef DI_ENUM

/* opt_flags */
#define DIF_FORCE_FAKE   2u   /* emulate even when a real Infinity base is plugged in */
#define DIF_NO_TIMEOUTS  4u   /* never time out pending IN transfers (debug) */

#define DI_SNIFF_MAX 1024

typedef struct {
    uint32_t magic, version;
    uint32_t size;               /* total image size incl. bss */
    uint32_t hook_off[H_COUNT];  /* entry points, relative to image start */
    uint32_t ping_off;           /* di_ping(): returns DIH_PING, proves the image is executable */
    uint32_t pad0_;
    uint64_t orig[H_COUNT];      /* real libSceUsbd / libScePad functions (filled by the injector) */
    uint64_t fn_usleep;          /* sceKernelUsleep */
    uint64_t fn_pthread_create;  /* scePthreadCreate */
    /* options (injector may override) */
    uint32_t opt_hold_ms;        /* how long the combo must be held to open the picker */
    uint32_t opt_combo;          /* ScePad button mask that opens / closes the picker (0 = L3+R3) */
    uint32_t opt_flags;          /* DIF_* */
    uint32_t pad1_;
    /* live state, read by the injector */
    volatile uint32_t state_seq;     /* bumped whenever tags change (placement, game writes) */
    volatile uint32_t passthrough;   /* 1 = real base detected, emulation off */
    volatile uint32_t fake_opened;   /* game opened the virtual base */
    volatile uint32_t usb_cmds;      /* OUT reports received */
    volatile uint32_t pad_calls;     /* pad reads seen */
    volatile uint32_t pump_running;  /* delivery thread started */
    volatile uint32_t delivered;     /* IN reports delivered to the game */
    volatile uint32_t timeouts;      /* IN transfers completed as TIMED_OUT */
    volatile uint32_t calls[H_COUNT];/* per-import call counters (diagnostics) */
    di_base  base;
    uint32_t lock;
    /* USB sniffer ring: [0]=dir 0 out/1 in, [1]=1 real device, [2]=len, [4..35]=data */
    volatile uint32_t sniff_n;
    uint8_t  sniff[DI_SNIFF_MAX][36];
    /* command mailbox: the injector fills op/a/b (and stage for PLACE), then bumps req; the hook
     * handles it on its pump thread or next pad read and sets done = req. */
    struct {
        volatile uint32_t req, done;
        volatile uint32_t op, a, b;
        volatile int32_t result;
        uint8_t stage[DI_TAG_SIZE];
    } mbox;
    /* console overlay (same contract as the LEGO Virtual Toy Pad): while ext.mode != 0 the hook
     * owns the open/close combo and pushes every change of the owner's buttons (plus the left
     * stick as D-pad bits) into ring[] while the picker is open. */
    struct {
        volatile uint32_t mode;        /* injector: 1 = overlay UI active */
        volatile uint32_t open;        /* hook: 1 while the overlay owns the controller */
        volatile uint32_t open_seq;    /* hook: bumped on every open */
        volatile uint32_t close_req;   /* injector: bump to ask the hook to close */
        volatile uint32_t close_ack;   /* hook: = close_req once handled */
        volatile uint32_t raw;         /* hook: player 1's live buttons while closed (sneak peek) */
        volatile uint32_t wr;          /* hook: ring write counter */
        volatile uint32_t heartbeat;   /* hook: pad reads seen in ext mode */
        volatile uint32_t lease;       /* injector: bumped while alive; stale -> hook stops swallowing */
        volatile uint32_t ring[32];    /* hook: buttons | stick-as-dpad on each change while open */
        volatile uint32_t owner;       /* hook: controller index (order first seen) that opened the picker */
        volatile uint32_t seen[4];     /* hook: pad_calls when each controller last read as connected */
    } ext;
} di_header;

enum { DI_MB_PLACE = 1, DI_MB_REMOVE = 2, DI_MB_CLEAR_ALL = 3 };
