/* In-game hook module: a virtual Disney Infinity base behind the libSceUsbd imports of the
 * PS4 Disney Infinity games, plus the controller hook for the console picker.
 *
 * Freestanding and position independent. The PS5 injector maps it into the game, fills in the
 * real function addresses and redirects the game's GOT entries for sceUsbd* / scePad* to the
 * h_* functions below. Every hook passes straight through for anything that is not the fake
 * device, so other USB hardware keeps working.
 *
 * The device model mirrors libusb 1.0 (which libSceUsbd is): the fake libusb_device and
 * libusb_device_handle are two static objects identified by address.
 *
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later
 * Device-list / transfer pump structure from the LEGO Dimensions Virtual Toy Pad (same author). */
#include "dihook.h"
#include "grid_input.h"

__attribute__((section(".hdr"), used)) di_header HDR = {
    .magic = DIH_MAGIC,
    .version = DIH_VERSION,
    .opt_hold_ms = 250,
    .opt_combo = 0x2 | 0x4,   /* L3 + R3 */
};

/* ------------------------------------------------------------- utilities */

void *memcpy(void *d, const void *s, unsigned long n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int v, unsigned long n) {
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)v;
    return d;
}

static void lock(void) {
    while (__atomic_exchange_n(&HDR.lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}
static void unlock(void) { __atomic_store_n(&HDR.lock, 0, __ATOMIC_RELEASE); }

typedef int (*usleep_fn)(uint32_t);
static void sleep_us(uint32_t us) {
    if (HDR.fn_usleep) ((usleep_fn)HDR.fn_usleep)(us);
    else for (volatile uint32_t i = 0; i < us * 50; i++) __builtin_ia32_pause();
}

/* coarse millisecond clock: advanced by the pump thread and by blocking waits */
static volatile uint64_t g_ms;
static void tick_ms(uint32_t ms) { __atomic_fetch_add(&g_ms, ms, __ATOMIC_RELAXED); }

#define ORIG(i, type) ((type)HDR.orig[i])
#define HAVE(i) (HDR.orig[i] != 0)
#define COUNT(i) __atomic_fetch_add(&HDR.calls[i], 1, __ATOMIC_RELAXED)

/* libSceUsbd error codes are 0x80240000 - LIBUSB_ERROR_x */
#define E_INVALID   ((int)0x80240002)
#define E_NOT_FOUND ((int)0x80240005)
#define E_TIMEOUT   ((int)0x80240007)
#define E_PIPE      ((int)0x80240009)

enum { XS_COMPLETED = 0, XS_ERROR = 1, XS_TIMED_OUT = 2, XS_CANCELLED = 3, XS_STALL = 4 };
enum { XT_CONTROL = 0, XT_INTERRUPT = 3 };

/* ------------------------------------------------- libusb mirror structs */

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_dev_desc;

typedef struct {
    uint8_t  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval, bRefresh, bSynchAddress;
    const void *extra; int extra_length;
} usb_ep_desc;            /* 32 bytes */

typedef struct {
    uint8_t  bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t  bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
    const usb_ep_desc *endpoint;
    const void *extra; int extra_length;
} usb_if_desc;            /* 40 bytes */

typedef struct { const usb_if_desc *altsetting; int num_altsetting; } usb_if;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const usb_if *interface;
    const void *extra; int extra_length;
} usb_cfg_desc;           /* 40 bytes */

typedef struct usb_xfer usb_xfer;
typedef void (*usb_cb)(usb_xfer *);
struct usb_xfer {
    void    *dev_handle;   /* 0x00 */
    uint8_t  flags;        /* 0x08 */
    uint8_t  endpoint;     /* 0x09 */
    uint8_t  type;         /* 0x0a */
    uint32_t timeout;      /* 0x0c */
    int32_t  status;       /* 0x10 */
    int32_t  length;       /* 0x14 */
    int32_t  actual_length;/* 0x18 */
    usb_cb   callback;     /* 0x20 */
    void    *user_data;    /* 0x28 */
    uint8_t *buffer;       /* 0x30 */
};

typedef struct { long tv_sec, tv_usec; } usb_timeval;

/* the virtual device and its handle: identified by address */
static struct { uint64_t tag; } FAKE_DEV = {0x44494E4642415345ull}, FAKE_HANDLE = {0x44494E4648414E44ull};
#define IS_FAKE_DEV(d) ((const void *)(d) == (const void *)&FAKE_DEV)
#define IS_FAKE_H(h)   ((const void *)(h) == (const void *)&FAKE_HANDLE)

/* raw descriptors (Dolphin / RPCS3 / shadPS4 agree on these) */
static const uint8_t RAW_DEV[18] = {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x20, 0x6F, 0x0E,
                                    0x29, 0x01, 0x00, 0x02, 0x01, 0x02, 0x03, 0x01};
static const uint8_t RAW_CFG[41] = {
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,          /* configuration */
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,          /* interface 0: HID, 2 endpoints */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1D, 0x00,          /* HID 1.11, report descriptor 29 bytes */
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x01,                      /* EP 0x81 IN interrupt 32 */
    0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x01};                     /* EP 0x01 OUT interrupt 32 */
static const uint8_t RAW_HID_REPORT[29] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19, 0x01, 0x29,
                                           0x20, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x20,
                                           0x81, 0x00, 0x19, 0x01, 0x29, 0x20, 0x91, 0x00, 0xC0};
static const char STR_MANUFACTURER[] = "PDP Limited. ";
static const char STR_PRODUCT[] = "Disney Infinity Reader";
static const char STR_SERIAL[] = "0000000000";

static usb_ep_desc  EPS[2];
static usb_if_desc  IFD;
static usb_if       IFS;
static usb_cfg_desc CFG;

static void build_descriptors(void) {
    /* filled at runtime: no absolute pointers in initialised data (the image has no relocations) */
    EPS[0] = (usb_ep_desc){7, 5, 0x81, 3, 32, 1, 0, 0, 0, 0};
    EPS[1] = (usb_ep_desc){7, 5, 0x01, 3, 32, 1, 0, 0, RAW_CFG + 18, 9};   /* HID descriptor as extra */
    IFD = (usb_if_desc){9, 4, 0, 0, 2, 3, 0, 0, 0, EPS, 0, 0};
    IFS = (usb_if){&IFD, 1};
    CFG = (usb_cfg_desc){9, 2, 0x29, 1, 1, 0, 0x80, 0xFA, &IFS, 0, 0};
}

/* ------------------------------------------------------- transfer queues */

#define MAXX 64
static usb_xfer *in_q[MAXX];   static uint64_t in_t[MAXX];  static uint32_t in_n;   /* pending IN */
static usb_xfer *done_q[MAXX]; static uint32_t done_h, done_t;                   /* finished, callback due */
static void **saved_list;
static void *my_list[72];
static int initialised, kdrv_detached;
static void start_pump(void);
static void run_mailbox(void);

static void init_once(void) {
    if (initialised) return;
    initialised = 1;
    build_descriptors();
}

static void sniff(int dir, const uint8_t *data, int len) {
    uint32_t i = __atomic_fetch_add(&HDR.sniff_n, 1, __ATOMIC_RELAXED) % DI_SNIFF_MAX;
    uint8_t *e = HDR.sniff[i];
    if (len > 32) len = 32;
    if (len < 0) len = 0;
    e[0] = (uint8_t)dir; e[1] = 0; e[2] = (uint8_t)len; e[3] = 0;
    for (int k = 0; k < len; k++) e[4 + k] = data[k];
}

/* lock held */
static void finish(usb_xfer *t, int status, int actual) {
    t->status = status;
    t->actual_length = actual;
    if (done_t - done_h < MAXX) done_q[done_t++ % MAXX] = t;
}

static void out_report(const uint8_t *d, int len) {   /* lock held */
    sniff(0, d, len);
    di_write(&HDR.base, d, (uint32_t)len);
    HDR.usb_cmds++;
}

static void open_fake(void) {
    lock();
    in_n = 0;
    done_h = done_t = 0;
    HDR.fake_opened++;
    unlock();
    start_pump();
}

/* ---------------------------------------------------- control requests */

static int copy_out(uint8_t *dst, int cap, const void *src, int n) {
    if (n > cap) n = cap;
    if (n > 0 && dst) memcpy(dst, src, (unsigned long)n);
    return n;
}

static int string_desc(uint8_t idx, uint8_t *data, int len) {
    uint8_t buf[64];
    const char *s = idx == 1 ? STR_MANUFACTURER : idx == 2 ? STR_PRODUCT : idx == 3 ? STR_SERIAL : 0;
    if (idx == 0) { buf[0] = 4; buf[1] = 3; buf[2] = 0x09; buf[3] = 0x04; return copy_out(data, len, buf, 4); }
    if (!s) return E_PIPE;
    int n = 0;
    while (s[n] && n < 30) { buf[2 + n * 2] = (uint8_t)s[n]; buf[3 + n * 2] = 0; n++; }
    buf[0] = (uint8_t)(2 + n * 2); buf[1] = 3;
    return copy_out(data, len, buf, buf[0]);
}

static int descriptor(uint8_t type, uint8_t idx, uint8_t *data, int len) {
    switch (type) {
    case 1: return copy_out(data, len, RAW_DEV, sizeof RAW_DEV);
    case 2: return copy_out(data, len, RAW_CFG, sizeof RAW_CFG);
    case 3: return string_desc(idx, data, len);
    case 0x21: return copy_out(data, len, RAW_CFG + 18, 9);
    case 0x22: return copy_out(data, len, RAW_HID_REPORT, sizeof RAW_HID_REPORT);
    default: return E_PIPE;
    }
}

/* lock held. Returns bytes transferred or an error. */
static int control(uint8_t rt, uint8_t req, uint16_t value, uint16_t index, uint8_t *data, int len) {
    (void)index;
    if (req == 0x06 && (rt & 0x80)) return descriptor((uint8_t)(value >> 8), (uint8_t)value, data, len);
    if (rt == 0x21 && req == 0x09) { if (data && len > 0) out_report(data, len); return len; }   /* SET_REPORT */
    if (rt == 0xA1 && req == 0x01) {                                                         /* GET_REPORT */
        uint8_t r[32];
        if (!di_read(&HDR.base, r)) return 0;
        HDR.delivered++;
        sniff(1, r, 32);
        return copy_out(data, len, r, 32);
    }
    if (rt == 0x80 && req == 0x00) { uint8_t st[2] = {0, 0}; return copy_out(data, len, st, 2); }  /* GET_STATUS */
    if (rt == 0x80 && req == 0x08) { uint8_t c = 1; return copy_out(data, len, &c, 1); }          /* GET_CONFIGURATION */
    return 0;   /* SET_IDLE, SET_CONFIGURATION, SET_INTERFACE ...: accept */
}

/* --------------------------------------------------------- device list */

typedef long (*fn_list)(void ***);
long h_GetDeviceList(void ***list) {
    COUNT(H_GetDeviceList);
    init_once();
    run_mailbox();
    if (!HAVE(H_GetDeviceList)) return -1;
    long n = ORIG(H_GetDeviceList, fn_list)(list);
    if (n < 0 || !list || !*list) return n;
    if (!(HDR.opt_flags & DIF_FORCE_FAKE) && HAVE(H_GetDeviceDescriptor)) {
        typedef int (*fn_desc)(void *, usb_dev_desc *);
        for (long i = 0; i < n; i++) {
            usb_dev_desc d;
            if (ORIG(H_GetDeviceDescriptor, fn_desc)((*list)[i], &d) == 0 &&
                d.idVendor == DI_VID && d.idProduct == DI_PID) {
                HDR.passthrough = 1;           /* a real base is plugged in: stay out of the way */
                return n;
            }
        }
    }
    HDR.passthrough = 0;
    if (n > 70) return n;
    for (long i = 0; i < n; i++) my_list[i] = (*list)[i];
    my_list[n] = &FAKE_DEV;
    my_list[n + 1] = 0;
    saved_list = *list;
    *list = my_list;
    return n + 1;
}

typedef void (*fn_freelist)(void **, int);
void h_FreeDeviceList(void **list, int unref) {
    COUNT(H_FreeDeviceList);
    if (list == my_list) { list = saved_list; saved_list = 0; }
    if (list && HAVE(H_FreeDeviceList)) ORIG(H_FreeDeviceList, fn_freelist)(list, unref);
}

typedef int (*fn_devdesc)(void *, usb_dev_desc *);
int h_GetDeviceDescriptor(void *dev, usb_dev_desc *d) {
    COUNT(H_GetDeviceDescriptor);
    if (!IS_FAKE_DEV(dev)) return HAVE(H_GetDeviceDescriptor) ? ORIG(H_GetDeviceDescriptor, fn_devdesc)(dev, d) : E_INVALID;
    if (!d) return E_INVALID;
    *d = (usb_dev_desc){0x12, 1, 0x200, 0, 0, 0, 0x20, DI_VID, DI_PID, 0x200, 1, 2, 3, 1};
    return 0;
}

typedef uint8_t (*fn_u8dev)(void *);
uint8_t h_GetBusNumber(void *dev) {
    COUNT(H_GetBusNumber);
    return IS_FAKE_DEV(dev) ? 0x7E : HAVE(H_GetBusNumber) ? ORIG(H_GetBusNumber, fn_u8dev)(dev) : 0;
}
uint8_t h_GetDeviceAddress(void *dev) {
    COUNT(H_GetDeviceAddress);
    return IS_FAKE_DEV(dev) ? 0x2B : HAVE(H_GetDeviceAddress) ? ORIG(H_GetDeviceAddress, fn_u8dev)(dev) : 0;
}
typedef int (*fn_idev)(void *);
int h_GetDeviceSpeed(void *dev) {
    COUNT(H_GetDeviceSpeed);
    return IS_FAKE_DEV(dev) ? 2 /* full speed */ : HAVE(H_GetDeviceSpeed) ? ORIG(H_GetDeviceSpeed, fn_idev)(dev) : 0;
}

typedef void *(*fn_open)(uint16_t, uint16_t);
void *h_OpenDeviceWithVidPid(uint16_t vid, uint16_t pid) {
    COUNT(H_OpenDeviceWithVidPid);
    init_once();
    if (vid == DI_VID && pid == DI_PID && !HDR.passthrough) { open_fake(); return &FAKE_HANDLE; }
    return HAVE(H_OpenDeviceWithVidPid) ? ORIG(H_OpenDeviceWithVidPid, fn_open)(vid, pid) : 0;
}

typedef int (*fn_open2)(void *, void **);
int h_Open(void *dev, void **out) {
    COUNT(H_Open);
    if (!IS_FAKE_DEV(dev)) return HAVE(H_Open) ? ORIG(H_Open, fn_open2)(dev, out) : E_INVALID;
    if (!out) return E_INVALID;
    init_once();
    *out = &FAKE_HANDLE;
    open_fake();
    return 0;
}

typedef void (*fn_vh)(void *);
void h_Close(void *h) {
    COUNT(H_Close);
    if (!IS_FAKE_H(h)) { if (HAVE(H_Close)) ORIG(H_Close, fn_vh)(h); return; }
    lock();
    in_n = 0;     /* libusb: closing with transfers in flight is undefined; just forget them */
    done_h = done_t = 0;
    unlock();
}

typedef void *(*fn_ph)(void *);
void *h_GetDevice(void *h) {
    COUNT(H_GetDevice);
    if (IS_FAKE_H(h)) return &FAKE_DEV;
    return HAVE(H_GetDevice) ? ORIG(H_GetDevice, fn_ph)(h) : 0;
}
void *h_RefDevice(void *dev) {
    COUNT(H_RefDevice);
    if (IS_FAKE_DEV(dev)) return dev;
    return HAVE(H_RefDevice) ? ORIG(H_RefDevice, fn_ph)(dev) : dev;
}
void h_UnrefDevice(void *dev) {
    COUNT(H_UnrefDevice);
    if (!IS_FAKE_DEV(dev) && HAVE(H_UnrefDevice)) ORIG(H_UnrefDevice, fn_vh)(dev);
}

typedef int (*fn_cfg)(void *, uint8_t, usb_cfg_desc **);
int h_GetConfigDescriptor(void *dev, uint8_t idx, usb_cfg_desc **cfg) {
    COUNT(H_GetConfigDescriptor);
    if (!IS_FAKE_DEV(dev)) return HAVE(H_GetConfigDescriptor) ? ORIG(H_GetConfigDescriptor, fn_cfg)(dev, idx, cfg) : E_INVALID;
    if (!cfg) return E_INVALID;
    if (idx != 0) return E_NOT_FOUND;
    init_once();
    *cfg = &CFG;
    return 0;
}
int h_GetConfigDescriptorByValue(void *dev, uint8_t value, usb_cfg_desc **cfg) {
    COUNT(H_GetConfigDescriptorByValue);
    if (!IS_FAKE_DEV(dev))
        return HAVE(H_GetConfigDescriptorByValue) ? ORIG(H_GetConfigDescriptorByValue, fn_cfg)(dev, value, cfg) : E_INVALID;
    if (!cfg) return E_INVALID;
    if (value != 1) return E_NOT_FOUND;
    init_once();
    *cfg = &CFG;
    return 0;
}
typedef int (*fn_acfg)(void *, usb_cfg_desc **);
int h_GetActiveConfigDescriptor(void *dev, usb_cfg_desc **cfg) {
    COUNT(H_GetActiveConfigDescriptor);
    if (!IS_FAKE_DEV(dev))
        return HAVE(H_GetActiveConfigDescriptor) ? ORIG(H_GetActiveConfigDescriptor, fn_acfg)(dev, cfg) : E_INVALID;
    if (!cfg) return E_INVALID;
    init_once();
    *cfg = &CFG;
    return 0;
}

typedef void (*fn_freecfg)(usb_cfg_desc *);
void h_FreeConfigDescriptor(usb_cfg_desc *cfg) {
    COUNT(H_FreeConfigDescriptor);
    if (cfg != &CFG && HAVE(H_FreeConfigDescriptor)) ORIG(H_FreeConfigDescriptor, fn_freecfg)(cfg);
}

typedef int (*fn_hp)(void *, int *);
int h_GetConfiguration(void *h, int *c) {
    COUNT(H_GetConfiguration);
    if (!IS_FAKE_H(h)) return HAVE(H_GetConfiguration) ? ORIG(H_GetConfiguration, fn_hp)(h, c) : E_INVALID;
    if (c) *c = 1;
    return 0;
}

typedef int (*fn_hi)(void *, int);
#define PASS_HI(name, fake_ret) \
    int h_##name(void *h, int i) { \
        COUNT(H_##name); \
        if (!IS_FAKE_H(h)) return HAVE(H_##name) ? ORIG(H_##name, fn_hi)(h, i) : E_INVALID; \
        return fake_ret; \
    }
PASS_HI(SetConfiguration, 0)
PASS_HI(ClaimInterface, 0)
PASS_HI(ReleaseInterface, 0)
PASS_HI(KernelDriverActive, !kdrv_detached)
int h_DetachKernelDriver(void *h, int i) {
    COUNT(H_DetachKernelDriver);
    if (!IS_FAKE_H(h)) return HAVE(H_DetachKernelDriver) ? ORIG(H_DetachKernelDriver, fn_hi)(h, i) : E_INVALID;
    kdrv_detached = 1;
    return 0;
}
int h_AttachKernelDriver(void *h, int i) {
    COUNT(H_AttachKernelDriver);
    if (!IS_FAKE_H(h)) return HAVE(H_AttachKernelDriver) ? ORIG(H_AttachKernelDriver, fn_hi)(h, i) : E_INVALID;
    kdrv_detached = 0;
    return 0;
}
typedef int (*fn_hii)(void *, int, int);
int h_SetInterfaceAltSetting(void *h, int i, int alt) {
    COUNT(H_SetInterfaceAltSetting);
    if (!IS_FAKE_H(h)) return HAVE(H_SetInterfaceAltSetting) ? ORIG(H_SetInterfaceAltSetting, fn_hii)(h, i, alt) : E_INVALID;
    return 0;
}
typedef int (*fn_hu8)(void *, uint8_t);
int h_ClearHalt(void *h, uint8_t ep) {
    COUNT(H_ClearHalt);
    if (!IS_FAKE_H(h)) return HAVE(H_ClearHalt) ? ORIG(H_ClearHalt, fn_hu8)(h, ep) : E_INVALID;
    return 0;
}
typedef int (*fn_h)(void *);
int h_ResetDevice(void *h) {
    COUNT(H_ResetDevice);
    if (!IS_FAKE_H(h)) return HAVE(H_ResetDevice) ? ORIG(H_ResetDevice, fn_h)(h) : E_INVALID;
    return 0;
}
int h_CheckConnected(void *h) {
    COUNT(H_CheckConnected);
    if (!IS_FAKE_H(h)) return HAVE(H_CheckConnected) ? ORIG(H_CheckConnected, fn_h)(h) : E_INVALID;
    return 0;
}
int h_GetMaxPacketSize(void *dev, uint8_t ep) {
    COUNT(H_GetMaxPacketSize);
    if (!IS_FAKE_DEV(dev)) return HAVE(H_GetMaxPacketSize) ? ORIG(H_GetMaxPacketSize, fn_hu8)(dev, ep) : E_INVALID;
    return 32;
}

/* --------------------------------------------------------- transfers */

/* Sony's fill helpers may refuse our fake handle: fill the libusb struct ourselves */
typedef void (*fn_fill)(usb_xfer *, void *, uint8_t, uint8_t *, int, usb_cb, void *, uint32_t);
void h_FillInterruptTransfer(usb_xfer *t, void *h, uint8_t ep, uint8_t *buf, int len, usb_cb cb, void *ud, uint32_t timeout) {
    COUNT(H_FillInterruptTransfer);
    if (!IS_FAKE_H(h)) { if (HAVE(H_FillInterruptTransfer)) ORIG(H_FillInterruptTransfer, fn_fill)(t, h, ep, buf, len, cb, ud, timeout); return; }
    if (!t) return;
    t->dev_handle = h; t->endpoint = ep; t->type = XT_INTERRUPT; t->timeout = timeout;
    t->buffer = buf; t->length = len; t->user_data = ud; t->callback = cb;
}

typedef void (*fn_fillc)(usb_xfer *, void *, uint8_t *, usb_cb, void *, uint32_t);
void h_FillControlTransfer(usb_xfer *t, void *h, uint8_t *buf, usb_cb cb, void *ud, uint32_t timeout) {
    COUNT(H_FillControlTransfer);
    if (!IS_FAKE_H(h)) { if (HAVE(H_FillControlTransfer)) ORIG(H_FillControlTransfer, fn_fillc)(t, h, buf, cb, ud, timeout); return; }
    if (!t) return;
    t->dev_handle = h; t->endpoint = 0; t->type = XT_CONTROL; t->timeout = timeout;
    t->buffer = buf; t->length = buf ? 8 + (buf[6] | (buf[7] << 8)) : 0;
    t->user_data = ud; t->callback = cb;
}

typedef int (*fn_xfer)(usb_xfer *);
int h_SubmitTransfer(usb_xfer *t) {
    COUNT(H_SubmitTransfer);
    if (!t || !IS_FAKE_H(t->dev_handle)) return HAVE(H_SubmitTransfer) ? ORIG(H_SubmitTransfer, fn_xfer)(t) : E_INVALID;
    lock();
    if (t->type == XT_CONTROL) {
        uint8_t *s = t->buffer;
        int r = s ? control(s[0], s[1], (uint16_t)(s[2] | s[3] << 8), (uint16_t)(s[4] | s[5] << 8), s + 8,
                            s[6] | s[7] << 8) : E_INVALID;
        finish(t, r < 0 ? XS_STALL : XS_COMPLETED, r < 0 ? 0 : r);
    } else if (t->endpoint & 0x80) {
        if (in_n < MAXX) { in_t[in_n] = g_ms; in_q[in_n++] = t; }
        else finish(t, XS_ERROR, 0);
    } else {
        if (t->buffer && t->length > 0) out_report(t->buffer, t->length);
        finish(t, XS_COMPLETED, t->length);
    }
    unlock();
    return 0;
}

int h_CancelTransfer(usb_xfer *t) {
    COUNT(H_CancelTransfer);
    if (!t || !IS_FAKE_H(t->dev_handle)) return HAVE(H_CancelTransfer) ? ORIG(H_CancelTransfer, fn_xfer)(t) : E_INVALID;
    int found = 0;
    lock();
    for (uint32_t i = 0; i < in_n; i++) {
        if (in_q[i] != t) continue;
        for (uint32_t k = i + 1; k < in_n; k++) { in_q[k - 1] = in_q[k]; in_t[k - 1] = in_t[k]; }
        in_n--;
        finish(t, XS_CANCELLED, 0);
        found = 1;
        break;
    }
    unlock();
    return found ? 0 : E_NOT_FOUND;
}

/* Completes virtual transfers. Runs on our own pump thread: the game's USB event thread may sit
 * forever inside the real sceUsbdHandleEvents (no real device -> no event). Returns 1 if busy. */
static uint32_t pump_busy;
static int deliver(void) {
    if (__atomic_exchange_n(&pump_busy, 1, __ATOMIC_ACQUIRE)) return 0;   /* one deliverer at a time */
    int did = 0;
    for (;;) {
        usb_xfer *t = 0;
        lock();
        if (done_t != done_h) {
            t = done_q[done_h++ % MAXX];
        } else if (in_n && di_pending(&HDR.base)) {
            t = in_q[0];
            for (uint32_t i = 1; i < in_n; i++) { in_q[i - 1] = in_q[i]; in_t[i - 1] = in_t[i]; }
            in_n--;
            uint8_t r[32];
            di_read(&HDR.base, r);
            int n = t->length < 32 ? t->length : 32;
            if (t->buffer && n > 0) memcpy(t->buffer, r, (unsigned long)n);
            t->status = XS_COMPLETED; t->actual_length = n;
            sniff(1, r, 32);
            HDR.delivered++;
        } else if (!(HDR.opt_flags & DIF_NO_TIMEOUTS)) {
            /* a real interrupt IN with nothing to say times out after its timeout */
            for (uint32_t i = 0; i < in_n; i++) {
                if (!in_q[i]->timeout || g_ms - in_t[i] < in_q[i]->timeout) continue;
                t = in_q[i];
                for (uint32_t k = i + 1; k < in_n; k++) { in_q[k - 1] = in_q[k]; in_t[k - 1] = in_t[k]; }
                in_n--;
                t->status = XS_TIMED_OUT; t->actual_length = 0;
                HDR.timeouts++;
                break;
            }
        }
        unlock();
        if (!t) break;
        did = 1;
        if (t->callback) t->callback(t);   /* the game copies the data and usually resubmits */
    }
    __atomic_store_n(&pump_busy, 0, __ATOMIC_RELEASE);
    return did;
}

static void *pump(void *arg) {
    (void)arg;
    for (;;) {
        run_mailbox();
        if (!HDR.passthrough && HDR.fake_opened) {
            if (!deliver()) { sleep_us(1000); tick_ms(1); }
        } else { sleep_us(20000); tick_ms(20); }
    }
    return 0;
}

typedef int (*fn_pcreate)(void *thr, const void *attr, void *(*entry)(void *), void *arg, const char *name);
static void start_pump(void) {
    if (__atomic_exchange_n(&HDR.pump_running, 1, __ATOMIC_ACQ_REL)) return;
    static uint64_t thr;
    if (!HDR.fn_pthread_create || ((fn_pcreate)HDR.fn_pthread_create)(&thr, 0, pump, 0, "DIBasePump") != 0)
        HDR.pump_running = 0;
}

/* the game's own event loop: deliver from it too when the pump thread could not start */
static int fake_events(uint32_t max_sleep_us) {
    if (!HDR.pump_running) deliver();
    if (max_sleep_us) { uint32_t s = max_sleep_us > 2000 ? 2000 : max_sleep_us; sleep_us(s); if (!HDR.pump_running) tick_ms(s / 1000 + 1); }
    return 0;
}

typedef int (*fn_void)(void);
int h_HandleEvents(void) {
    COUNT(H_HandleEvents);
    if (HDR.passthrough || !HDR.fake_opened) return HAVE(H_HandleEvents) ? ORIG(H_HandleEvents, fn_void)() : 0;
    return fake_events(2000);
}
typedef int (*fn_tv)(usb_timeval *);
static uint32_t tv_us(const usb_timeval *tv) {
    if (!tv) return 2000;
    long us = tv->tv_sec * 1000000L + tv->tv_usec;
    return us <= 0 ? 0 : us > 2000 ? 2000 : (uint32_t)us;
}
int h_HandleEventsTimeout(usb_timeval *tv) {
    COUNT(H_HandleEventsTimeout);
    if (HDR.passthrough || !HDR.fake_opened) return HAVE(H_HandleEventsTimeout) ? ORIG(H_HandleEventsTimeout, fn_tv)(tv) : 0;
    return fake_events(tv_us(tv));
}
int h_HandleEventsLocked(usb_timeval *tv) {
    COUNT(H_HandleEventsLocked);
    if (HDR.passthrough || !HDR.fake_opened) return HAVE(H_HandleEventsLocked) ? ORIG(H_HandleEventsLocked, fn_tv)(tv) : 0;
    return fake_events(tv_us(tv));
}

/* synchronous interrupt transfer (libusb_interrupt_transfer) */
typedef int (*fn_sync)(void *, uint8_t, uint8_t *, int, int *, uint32_t);
int h_InterruptTransfer(void *h, uint8_t ep, uint8_t *data, int len, int *actual, uint32_t timeout) {
    COUNT(H_InterruptTransfer);
    if (!IS_FAKE_H(h)) return HAVE(H_InterruptTransfer) ? ORIG(H_InterruptTransfer, fn_sync)(h, ep, data, len, actual, timeout) : E_INVALID;
    if (actual) *actual = 0;
    if (!data || len <= 0) return E_INVALID;
    if (!(ep & 0x80)) {
        lock(); out_report(data, len); unlock();
        if (actual) *actual = len;
        return 0;
    }
    for (uint32_t waited = 0;; waited++) {
        run_mailbox();
        lock();
        uint8_t r[32];
        int got = di_read(&HDR.base, r);
        if (got) { HDR.delivered++; sniff(1, r, 32); }
        unlock();
        if (got) {
            int n = len < 32 ? len : 32;
            memcpy(data, r, (unsigned long)n);
            if (actual) *actual = n;
            return 0;
        }
        if (timeout && waited >= timeout) { HDR.timeouts++; return E_TIMEOUT; }
        sleep_us(1000);
        if (!HDR.pump_running) tick_ms(1);
    }
}

typedef int (*fn_ctl)(void *, uint8_t, uint8_t, uint16_t, uint16_t, uint8_t *, int, uint32_t);
int h_ControlTransfer(void *h, uint8_t rt, uint8_t req, uint16_t value, uint16_t index, uint8_t *data, int len, uint32_t timeout) {
    COUNT(H_ControlTransfer);
    if (!IS_FAKE_H(h)) return HAVE(H_ControlTransfer) ? ORIG(H_ControlTransfer, fn_ctl)(h, rt, req, value, index, data, len, timeout) : E_INVALID;
    lock();
    int r = control(rt, req, value, index, data, len);
    unlock();
    return r;
}

typedef int (*fn_gdesc)(void *, uint8_t, uint8_t, uint8_t *, int);
int h_GetDescriptor(void *h, uint8_t type, uint8_t idx, uint8_t *data, int len) {
    COUNT(H_GetDescriptor);
    if (!IS_FAKE_H(h)) return HAVE(H_GetDescriptor) ? ORIG(H_GetDescriptor, fn_gdesc)(h, type, idx, data, len) : E_INVALID;
    return descriptor(type, idx, data, len);
}
typedef int (*fn_gstr)(void *, uint8_t, uint16_t, uint8_t *, int);
int h_GetStringDescriptor(void *h, uint8_t idx, uint16_t lang, uint8_t *data, int len) {
    COUNT(H_GetStringDescriptor);
    if (!IS_FAKE_H(h)) return HAVE(H_GetStringDescriptor) ? ORIG(H_GetStringDescriptor, fn_gstr)(h, idx, lang, data, len) : E_INVALID;
    return string_desc(idx, data, len);
}
typedef int (*fn_gstra)(void *, uint8_t, uint8_t *, int);
int h_GetStringDescriptorAscii(void *h, uint8_t idx, uint8_t *data, int len) {
    COUNT(H_GetStringDescriptorAscii);
    if (!IS_FAKE_H(h)) return HAVE(H_GetStringDescriptorAscii) ? ORIG(H_GetStringDescriptorAscii, fn_gstra)(h, idx, data, len) : E_INVALID;
    const char *s = idx == 1 ? STR_MANUFACTURER : idx == 2 ? STR_PRODUCT : idx == 3 ? STR_SERIAL : 0;
    if (!s || !data || len <= 0) return E_PIPE;
    int n = 0;
    while (s[n] && n < len - 1) { data[n] = (uint8_t)s[n]; n++; }
    data[n] = 0;
    return n;
}

/* ------------------------------------------------------------- mailbox */

static uint32_t mb_busy;
static void run_mailbox(void) {
    uint32_t r = HDR.mbox.req;
    if (r == HDR.mbox.done) return;
    if (__atomic_exchange_n(&mb_busy, 1, __ATOMIC_ACQUIRE)) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (r != HDR.mbox.done) {
        uint32_t op = HDR.mbox.op, a = HDR.mbox.a, b = HDR.mbox.b;
        int32_t res = -1;
        lock();
        if (op == DI_MB_PLACE && a < DI_SLOTS) res = di_place(&HDR.base, (int)a, HDR.mbox.stage, b);
        else if (op == DI_MB_REMOVE && a < DI_SLOTS) res = di_remove(&HDR.base, (int)a);
        else if (op == DI_MB_CLEAR_ALL) { for (int i = 0; i < DI_SLOTS; i++) di_remove(&HDR.base, i); res = 0; }
        HDR.state_seq++;
        unlock();
        HDR.mbox.result = res;
        __atomic_store_n(&HDR.mbox.done, r, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&mb_busy, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------- pad hook */

#define PAD_STRIDE 120   /* sizeof(ScePadData) */

static int32_t h_id[4] = {-1, -1, -1, -1};
static uint32_t h_hold[4];
static int32_t ext_owner = -1;
static uint32_t ext_last = 0xFFFFFFFFu;
static int ext_armed;
static uint32_t lease_seen, lease_frames;

static void swallow(uint8_t *d) {
    uint32_t zero = 0;
    memcpy(d, &zero, 4);
    d[4] = d[5] = d[6] = d[7] = 0x80;   /* sticks centred */
    d[8] = d[9] = 0;                    /* analog L2 / R2 */
}

/* the overlay keeps renewing its lease; if it dies, stop owning the controller */
static int ext_alive(void) {
    if (!HDR.ext.mode) return 0;
    uint32_t l = HDR.ext.lease;
    if (l != lease_seen) { lease_seen = l; lease_frames = 0; return 1; }
    return ++lease_frames < 200;
}

static void pad_frame(int32_t handle, uint8_t *d) {
    run_mailbox();
    if (!ext_alive()) {
        if (HDR.ext.open) __atomic_store_n(&HDR.ext.open, 0, __ATOMIC_RELEASE);
        return;
    }
    int k = 0;
    while (k < 4 && h_id[k] != handle && h_id[k] != -1) k++;
    if (k == 4) k = 3;
    h_id[k] = handle;
    if (d[76]) HDR.ext.seen[k] = HDR.pad_calls;   /* ScePadData.connected */
    uint32_t b;
    memcpy(&b, d, 4);
    uint32_t combo = HDR.opt_combo ? HDR.opt_combo : 0x6;
    HDR.ext.heartbeat++;
    if (!HDR.ext.open) {
        if (k == 0) HDR.ext.raw = b;
        uint32_t hold_frames = HDR.opt_hold_ms / 33 + 1;
        if ((b & combo) == combo) {
            if (++h_hold[k] >= hold_frames) {
                h_hold[k] = 0;
                ext_owner = handle; ext_armed = 0; ext_last = 0xFFFFFFFFu;
                HDR.ext.owner = (uint32_t)k;
                HDR.ext.raw = 0;
                HDR.ext.close_ack = HDR.ext.close_req;
                HDR.ext.open_seq++;
                __atomic_store_n(&HDR.ext.open, 1, __ATOMIC_RELEASE);
                swallow(d);
            }
        } else h_hold[k] = 0;
        return;
    }
    if (HDR.ext.close_req != HDR.ext.close_ack) {
        HDR.ext.close_ack = HDR.ext.close_req;
        __atomic_store_n(&HDR.ext.open, 0, __ATOMIC_RELEASE);
        swallow(d);
        return;
    }
    if (handle != ext_owner) { swallow(d); return; }   /* other players are paused too */
    /* the same combo closes the picker once it was released after opening */
    if ((b & combo) != combo) ext_armed = 1;
    else if (ext_armed) { __atomic_store_n(&HDR.ext.open, 0, __ATOMIC_RELEASE); swallow(d); return; }
    uint32_t nav = b | ld_grid_stick_direction(d[4], d[5]);
    if (nav != ext_last) {
        uint32_t w = HDR.ext.wr;
        HDR.ext.ring[w & 31] = nav;
        __atomic_store_n(&HDR.ext.wr, w + 1, __ATOMIC_RELEASE);
        ext_last = nav;
    }
    swallow(d);
}

typedef int (*fn_pad)(int32_t, void *);
int h_PadReadState(int32_t handle, void *data) {
    COUNT(H_PadReadState);
    int r = HAVE(H_PadReadState) ? ORIG(H_PadReadState, fn_pad)(handle, data) : -1;
    HDR.pad_calls++;
    if (r == 0 && data) pad_frame(handle, (uint8_t *)data);
    return r;
}

typedef int (*fn_padn)(int32_t, void *, int32_t);
int h_PadRead(int32_t handle, void *data, int32_t num) {
    COUNT(H_PadRead);
    int r = HAVE(H_PadRead) ? ORIG(H_PadRead, fn_padn)(handle, data, num) : -1;
    HDR.pad_calls++;
    if (r > 0 && data) {
        uint8_t *d = (uint8_t *)data;
        pad_frame(handle, d + (r - 1) * PAD_STRIDE);     /* the newest sample drives the picker */
        if (HDR.ext.open) for (int i = 0; i < r - 1; i++) swallow(d + i * PAD_STRIDE);
    }
    return r;
}

uint32_t di_ping(void) { return DIH_PING; }
