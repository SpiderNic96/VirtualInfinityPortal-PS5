/* Host simulation of a game driving the hook blob exactly the way a libusb client would.
 * hooks.c is compiled into this file; the "real" libSceUsbd / libScePad are small fakes here.
 * Build: cc -O1 -pthread -I../src/core -I../src/hook test_hook.c ../src/core/diproto.c ../src/core/ditag.c */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/hook/hooks.c"
#include "ditag.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---------------------------------------------------- fake "real" libraries */
static struct { int x; } REAL_KBD;
static void *real_list[2] = {&REAL_KBD, 0};
static long r_GetDeviceList(void ***l) { *l = real_list; return 1; }
static int r_free_called;
static void r_FreeDeviceList(void **l, int u) { (void)u; if (l == real_list) r_free_called++; }
static int r_GetDeviceDescriptor(void *d, usb_dev_desc *o) {
    (void)d; memset(o, 0, sizeof *o); o->idVendor = 0x054C; o->idProduct = 0x1234; return 0;
}
static uint32_t pad_buttons;
static int r_PadReadState(int32_t h, void *data) {
    (void)h; memset(data, 0, 120); memcpy(data, &pad_buttons, 4); ((uint8_t *)data)[4] = 0x80; ((uint8_t *)data)[5] = 0x80; return 0;
}
static int h_usleep(uint32_t us) { return usleep(us); }
static int h_pcreate(void *thr, const void *attr, void *(*entry)(void *), void *arg, const char *name) {
    (void)attr; (void)name; return pthread_create((pthread_t *)thr, 0, entry, arg);
}

/* ------------------------------------------------------------ game side */
static pthread_mutex_t gmx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t inbox[64][32];
static int in_count, in_status_last, in_timeouts;
static usb_xfer xin, xout;
static uint8_t inbuf[32], outbuf[32];

static void in_cb(usb_xfer *t) {
    pthread_mutex_lock(&gmx);
    in_status_last = t->status;
    if (t->status == XS_COMPLETED && t->actual_length == 32 && in_count < 64) memcpy(inbox[in_count++], t->buffer, 32);
    if (t->status == XS_TIMED_OUT) in_timeouts++;
    pthread_mutex_unlock(&gmx);
    if (t->status != XS_CANCELLED) h_SubmitTransfer(t);   /* resubmit, like the game */
}
static int out_done;
static void out_cb(usb_xfer *t) { (void)t; __atomic_fetch_add(&out_done, 1, __ATOMIC_RELAXED); }

static void send_cmd(uint8_t cmd, uint8_t seq, const uint8_t *data, int n) {
    memset(outbuf, 0, 32);
    outbuf[0] = 0xFF; outbuf[1] = (uint8_t)(n + 2); outbuf[2] = cmd; outbuf[3] = seq;
    if (n) memcpy(outbuf + 4, data, n);
    outbuf[4 + n] = di_checksum(outbuf, 4 + n);
    h_FillInterruptTransfer(&xout, &FAKE_HANDLE, 0x01, outbuf, 32, out_cb, 0, 0);
    h_SubmitTransfer(&xout);
}

/* wait for the next IN report, up to 500 ms */
static int next_report(uint8_t r[32]) {
    static int rd;
    for (int i = 0; i < 500; i++) {
        pthread_mutex_lock(&gmx);
        int ok = rd < in_count;
        if (ok) memcpy(r, inbox[rd++], 32);
        pthread_mutex_unlock(&gmx);
        if (ok) return 1;
        usleep(1000);
    }
    return 0;
}

static void mbox(uint32_t op, uint32_t a, uint32_t b, const uint8_t *stage) {
    if (stage) memcpy(HDR.mbox.stage, stage, DI_TAG_SIZE);
    HDR.mbox.op = op; HDR.mbox.a = a; HDR.mbox.b = b;
    uint32_t want = HDR.mbox.done + 1;
    __atomic_store_n(&HDR.mbox.req, want, __ATOMIC_RELEASE);
    for (int i = 0; i < 500 && HDR.mbox.done != want; i++) usleep(1000);
}

int main(void) {
    HDR.orig[H_GetDeviceList] = (uint64_t)r_GetDeviceList;
    HDR.orig[H_FreeDeviceList] = (uint64_t)r_FreeDeviceList;
    HDR.orig[H_GetDeviceDescriptor] = (uint64_t)r_GetDeviceDescriptor;
    HDR.orig[H_PadReadState] = (uint64_t)r_PadReadState;
    HDR.fn_usleep = (uint64_t)h_usleep;
    HDR.fn_pthread_create = (uint64_t)h_pcreate;
    di_init(&HDR.base);
    CHECK(di_ping() == DIH_PING);

    /* --- enumeration: the fake base is appended, the real list is still freed */
    void **list = 0;
    long n = h_GetDeviceList(&list);
    CHECK(n == 2 && list[0] == &REAL_KBD && list[1] == &FAKE_DEV);
    int found = -1;
    for (long i = 0; i < n; i++) {
        usb_dev_desc d;
        if (h_GetDeviceDescriptor(list[i], &d) == 0 && d.idVendor == 0x0E6F && d.idProduct == 0x0129) found = (int)i;
    }
    CHECK(found == 1);
    void *hdl = 0;
    CHECK(h_Open(list[found], &hdl) == 0 && hdl == &FAKE_HANDLE);
    h_FreeDeviceList(list, 1);
    CHECK(r_free_called == 1);
    usb_cfg_desc *cfg = 0;
    CHECK(h_GetActiveConfigDescriptor(&FAKE_DEV, &cfg) == 0 && cfg->bNumInterfaces == 1);
    CHECK(cfg->interface->altsetting->bNumEndpoints == 2 && cfg->interface->altsetting->endpoint[0].bEndpointAddress == 0x81);
    CHECK(h_GetConfigDescriptor(&FAKE_DEV, 1, &cfg) == E_NOT_FOUND);
    CHECK(h_KernelDriverActive(hdl, 0) == 1 && h_DetachKernelDriver(hdl, 0) == 0 && h_KernelDriverActive(hdl, 0) == 0);
    CHECK(h_ClaimInterface(hdl, 0) == 0);
    uint8_t sbuf[64];
    CHECK(h_GetStringDescriptorAscii(hdl, 2, sbuf, 64) > 0 && !strcmp((char *)sbuf, "Disney Infinity Reader"));

    /* --- async IN pipe, like the game's reader thread */
    h_FillInterruptTransfer(&xin, hdl, 0x81, inbuf, 32, in_cb, 0, 0);
    CHECK(h_SubmitTransfer(&xin) == 0);
    usleep(5000);
    CHECK(HDR.pump_running == 1);

    uint8_t r[32];
    send_cmd(0x80, 1, (const uint8_t *)"(c) Disney 2013", 15);
    CHECK(next_report(r) && r[0] == 0xAA && r[1] == 0x15);

    /* challenge vs. a reference base */
    static di_base ref;
    di_init(&ref);
    uint8_t seed[8] = {0x91, 0x02, 0x33, 0x14, 0x05, 0x66, 0x77, 0x08};
    uint64_t sv = 0;
    for (int i = 0; i < 8; i++) sv = (sv << 8) | seed[i];
    di_seed(&ref, di_descramble(sv));
    send_cmd(0x81, 2, seed, 8);
    CHECK(next_report(r) && r[0] == 0xAA && r[1] == 1 && r[2] == 2);
    for (int i = 0; i < 4; i++) {
        send_cmd(0x83, (uint8_t)(3 + i), 0, 0);
        uint64_t want = di_scramble(di_next(&ref), 0), got = 0;
        CHECK(next_report(r) && r[2] == 3 + i);
        for (int k = 0; k < 8; k++) got = (got << 8) | r[3 + k];
        CHECK(got == want);
    }

    /* --- place a figure through the mailbox: the IN pipe sees the "added" event */
    uint8_t uid[7] = {0x04, 1, 2, 3, 4, 5, 6}, tag[320];
    ditag_create(tag, 1000107, 2, uid);   /* Spider-Man */
    mbox(DI_MB_PLACE, 3, 1000107, tag);
    CHECK(HDR.mbox.result == 0);
    CHECK(next_report(r) && r[0] == 0xAB && r[2] == 2 && r[5] == 0);
    uint8_t order = r[4];
    send_cmd(0xA1, 7, 0, 0);
    CHECK(next_report(r) && r[1] == 3 && r[3] == (0x20 | order));
    uint8_t q[3] = {order, 0, 0};
    send_cmd(0xB4, 8, q, 1);
    CHECK(next_report(r) && !memcmp(r + 4, tag, 7));
    send_cmd(0xA2, 9, q, 3);
    CHECK(next_report(r) && !memcmp(r + 4, tag + 16, 16));
    /* the game writes block 3 (file block 12): dirty + state_seq bump for the injector */
    uint32_t seq0 = HDR.state_seq;
    uint8_t w[19] = {order, 3, 0};
    memset(w + 3, 0x5A, 16);
    send_cmd(0xA3, 10, w, 19);
    CHECK(next_report(r) && r[1] == 2);
    CHECK(HDR.base.fig[3].dirty && HDR.base.fig[3].data[12 * 16] == 0x5A);
    (void)seq0;
    /* removal */
    mbox(DI_MB_REMOVE, 3, 0, 0);
    CHECK(HDR.mbox.result == 0 && next_report(r) && r[0] == 0xAB && r[5] == 1);

    /* --- LED command reaches the mirror */
    uint8_t led[4] = {1, 0x10, 0x20, 0x30};
    send_cmd(0x90, 11, led, 4);
    CHECK(next_report(r) && HDR.base.led[0].rgb[2] == 0x30 && HDR.base.led[0].mode == DI_LED_SOLID);

    /* --- an idle IN with a timeout completes as TIMED_OUT and is resubmitted */
    xin.timeout = 30;
    usleep(150 * 1000);
    CHECK(in_timeouts >= 1 && HDR.timeouts >= 1);
    xin.timeout = 0;

    /* --- cancel: the callback sees CANCELLED */
    usleep(50 * 1000);   /* let the resubmitted (timeout 30) transfer cycle */
    in_timeouts = 0;
    xin.timeout = 0;
    int cr = h_CancelTransfer(&xin);
    usleep(20 * 1000);
    CHECK(cr == 0 && in_status_last == XS_CANCELLED);

    /* --- synchronous API */
    int act = 0;
    memset(outbuf, 0, 32);
    outbuf[0] = 0xFF; outbuf[1] = 2; outbuf[2] = 0xB5; outbuf[3] = 0x40; outbuf[4] = di_checksum(outbuf, 4);
    CHECK(h_InterruptTransfer(hdl, 0x01, outbuf, 32, &act, 100) == 0 && act == 32);
    CHECK(h_InterruptTransfer(hdl, 0x81, inbuf, 32, &act, 100) == 0 && act == 32 && inbuf[2] == 0x40);
    CHECK(h_InterruptTransfer(hdl, 0x81, inbuf, 32, &act, 20) == E_TIMEOUT && act == 0);
    /* control SET_REPORT path */
    outbuf[3] = 0x41; outbuf[4] = di_checksum(outbuf, 4);
    CHECK(h_ControlTransfer(hdl, 0x21, 0x09, 0x0200, 0, outbuf, 32, 100) == 32);
    CHECK(h_ControlTransfer(hdl, 0xA1, 0x01, 0x0100, 0, inbuf, 32, 100) == 32 && inbuf[2] == 0x41);
    uint8_t dd[18];
    CHECK(h_ControlTransfer(hdl, 0x80, 0x06, 0x0100, 0, dd, 18, 100) == 18 && dd[8] == 0x6F && dd[10] == 0x29);

    /* --- pad hook: picker combo, input ring, swallowing, release+combo closes */
    HDR.ext.mode = 1;
    uint8_t pd[120];
    pad_buttons = 0x2 | 0x4;                   /* hold L3+R3 */
    for (int i = 0; i < 9; i++) { HDR.ext.lease++; h_PadReadState(1, pd); }
    CHECK(HDR.ext.open == 1);
    uint32_t got_b;
    memcpy(&got_b, pd, 4);
    CHECK(got_b == 0);                         /* swallowed */
    uint32_t wr0 = HDR.ext.wr;
    pad_buttons = 0x4000;                      /* Cross */
    HDR.ext.lease++; h_PadReadState(1, pd);
    CHECK(HDR.ext.wr == wr0 + 1 && HDR.ext.ring[wr0 & 31] == 0x4000);
    memcpy(&got_b, pd, 4);
    CHECK(got_b == 0);
    pad_buttons = 0x6;                         /* combo again after release: closes */
    HDR.ext.lease++; h_PadReadState(1, pd);
    CHECK(HDR.ext.open == 0);
    pad_buttons = 0x4000;                      /* closed: the game gets its buttons again */
    HDR.ext.lease++; h_PadReadState(1, pd);
    memcpy(&got_b, pd, 4);
    CHECK(got_b == 0x4000);
    /* lease expiry: overlay died, hook stops owning the controller */
    for (int i = 0; i < 210; i++) h_PadReadState(1, pd);
    pad_buttons = 0x6;
    for (int i = 0; i < 12; i++) h_PadReadState(1, pd);
    CHECK(HDR.ext.open == 0);

    /* --- passthrough when a real base is present */
    printf("calls: list=%u submit=%u handle=%u cmds=%u delivered=%u timeouts=%u\n", HDR.calls[H_GetDeviceList],
           HDR.calls[H_SubmitTransfer], HDR.calls[H_HandleEvents], HDR.usb_cmds, HDR.delivered, HDR.timeouts);
    printf(fails ? "hook: %d FAILED\n" : "hook: all checks passed\n", fails);
    return fails != 0;
}
