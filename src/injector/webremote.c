/* Phone remote: a tiny HTTP server for /data/dimod/web plus a JSON API.
 *   GET  /api/state                     base, LEDs, slots, status
 *   GET  /api/roster                    web/roster.json
 *   POST /api/place?slot=N&id=ID        slot -1 = pick the right free slot
 *   POST /api/remove?slot=N   /api/move?from=A&to=B   /api/clear
 *   GET  /api/favorites   POST /api/fav?id=ID&on=0|1   (shared with the TV picker)
 *   GET  /api/settings    POST /api/settings?opacity=&ui_size=&peek_size=&peek=&open=&fast_load=&theme=&background=
 *   GET  /api/background  POST /api/background (PNG body: the custom picker background)  ?remove=1
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include "webremote.h"
#include "game.h"
#include "catalog.h"
#include "favs.h"
#include "cui.h"
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <strings.h>

void web_log(const char *fmt, ...);

#define WEB_DIR "/data/dimod/web"
static volatile int g_clients;

static int send_all(int fd, const void *d, size_t n) {
    const char *p = d;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static void reply(int fd, const char *status, const char *type, const void *body, size_t len, int cache) {
    char h[384];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     status, type, len, cache ? "public, max-age=86400" : "no-store");
    if (send_all(fd, h, (size_t)n) == 0 && body && len) send_all(fd, body, len);
}

static void reply_json(int fd, int code, const char *json) {
    reply(fd, code == 200 ? "200 OK" : code == 409 ? "409 Conflict" : "400 Bad Request", "application/json", json, strlen(json), 0);
}

static const char *mime(const char *p) {
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".js")) return "text/javascript";
    if (!strcmp(e, ".css")) return "text/css";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".jpg") || !strcmp(e, ".jpeg")) return "image/jpeg";
    if (!strcmp(e, ".svg")) return "image/svg+xml";
    if (!strcmp(e, ".woff2")) return "font/woff2";
    if (!strcmp(e, ".tsv") || !strcmp(e, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void serve_file(int fd, const char *rel, int cache) {
    char path[256];
    snprintf(path, sizeof path, WEB_DIR "%s", rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *m = strcmp(rel, "/index.html") ? "404 Not Found" :
                        "Web remote files missing: copy the release's data/dimod/web folder to /data/dimod/web";
        reply(fd, "404 Not Found", "text/plain; charset=utf-8", m, strlen(m), 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char h[384];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: %s\r\nConnection: close\r\n\r\n",
                     mime(rel), sz, cache ? "public, max-age=86400" : "no-cache");
    if (send_all(fd, h, (size_t)n) == 0) {
        char buf[16384];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) if (send_all(fd, buf, k)) break;
    }
    fclose(f);
}

static long qint(const char *q, const char *key, long def) {
    if (!q) return def;
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') return strtol(p + kl + 1, 0, 10);
        p = strchr(p, '&');
        if (p) p++;
    }
    return def;
}

static void json_str(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

#if __has_include("build_id.h")
#include "build_id.h"
#else
#define DIMOD_VERSION "0.6"
#define DIMOD_BUILD "dev"
#endif

static char g_web_build[32];
const char *web_build(void) {
    if (!g_web_build[0]) {
        FILE *f = fopen(WEB_DIR "/build.txt", "r");
        if (!f || fscanf(f, "%31s", g_web_build) != 1) snprintf(g_web_build, sizeof g_web_build, "missing");
        if (f) fclose(f);
    }
    return g_web_build;
}
int web_build_mismatch(void) { return strcmp(web_build(), DIMOD_BUILD) != 0; }

static void state_json(int fd) {
    game_snap s;
    game_snapshot(&s);
    char st[160], est[200];
    uint64_t when = 0;
    game_status_get(st, sizeof st, &when);
    json_str(est, sizeof est, st);
    char b[8192];
    size_t o = 0;
    o += (size_t)snprintf(b + o, sizeof b - o,
                          "{\"version\":\"" DIMOD_VERSION "\",\"build\":\"" DIMOD_BUILD "\",\"linked\":%s,\"opened\":%s,\"activated\":%s,\"cmds\":%u,\"status\":\"%s\",\"status_age\":%llu,\"leds\":[",
                          s.linked ? "true" : "false", s.opened ? "true" : "false", s.activated ? "true" : "false", s.cmds, est,
                          (unsigned long long)(when ? game_ms() - when : 999999));
    /* the lights as ledanim.h models them: the phone replays the same animation from `age` */
    uint64_t now = game_ms();
    for (int p = 0; p < 3; p++) {
        const led_anim *l = &s.la[p];
        o += (size_t)snprintf(b + o, sizeof b - o,
                              "%s{\"valid\":%d,\"mode\":%u,\"from\":[%u,%u,%u],\"to\":[%u,%u,%u],\"a\":%u,\"b\":%u,\"c\":%u,\"seq\":%u,\"age\":%llu}",
                              p ? "," : "", l->valid, l->mode, l->from[0], l->from[1], l->from[2], l->to[0], l->to[1], l->to[2],
                              l->a, l->b, l->c, l->seq, (unsigned long long)(l->valid && now > l->t0 ? now - l->t0 : 0));
    }
    o += (size_t)snprintf(b + o, sizeof b - o, "],\"led_unit\":%d,\"bg_gen\":%d,\"slots\":[", s.led_unit, cui_background_gen());
    for (int i = 0; i < DI_SLOTS; i++) {
        char nm[64], en[128];
        int pres = s.linked && s.present[i];
        catalog_name(s.fig[i], nm, sizeof nm);
        json_str(en, sizeof en, nm);
        o += (size_t)snprintf(b + o, sizeof b - o, "%s{\"present\":%s,\"id\":%u,\"name\":\"%s\",\"kind\":\"%c\"}",
                              i ? "," : "", pres ? "true" : "false", pres ? s.fig[i] : 0, pres ? en : "",
                              pres && catalog_kind_of(s.fig[i]) ? catalog_kind_of(s.fig[i]) : '-');
    }
    snprintf(b + o, sizeof b - o, "]}");
    reply_json(fd, 200, b);
}

static void result_json(int fd, int r, const char *msg) {
    char e[300], b[360];
    json_str(e, sizeof e, msg);
    snprintf(b, sizeof b, "{\"ok\":%s,\"msg\":\"%s\"}", r ? "false" : "true", e);
    reply_json(fd, r ? 409 : 200, b);
}

static void settings_json(int fd) {
    cui_config c;
    cui_get(&c);
    char b[1024];
    struct stat st;
    int have_bg = !stat(CUI_CUSTOM_BG, &st) && st.st_size > 0;
    size_t o = (size_t)snprintf(b, sizeof b, "{\"opacity\":%d,\"ui_size\":%d,\"peek_size\":%d,\"fast_load\":%d,\"theme\":%d,\"background\":%d,"
                                "\"custom_bg\":%s,\"bg_gen\":%d,\"themes\":[",
                                c.opacity, c.ui_size, c.peek_size, c.fast_load, c.theme, c.background, have_bg ? "true" : "false", cui_background_gen());
    for (int i = 0; i < CUI_THEMES; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s\"%s\"", i ? "," : "", cui_theme_name(i));
    o += (size_t)snprintf(b + o, sizeof b - o, "],\"backgrounds\":[");
    for (int i = 0; i < CUI_BACKGROUNDS; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s\"%s\"", i ? "," : "", cui_background_name(i));
    o += (size_t)snprintf(b + o, sizeof b - o, "],\"peek\":\"%s\",\"peek_choices\":[", cui_mask_name(c.peek_mask));
    for (int i = 0; i < cui_peek_choices(); i++)
        o += (size_t)snprintf(b + o, sizeof b - o, "%s\"%s\"", i ? "," : "", cui_peek_choice(i, 0));
    o += (size_t)snprintf(b + o, sizeof b - o, "],\"open\":\"%s\",\"open_choices\":[", cui_open_name(c.combo));
    for (int i = 0; i < cui_open_choices(); i++)
        o += (size_t)snprintf(b + o, sizeof b - o, "%s\"%s\"", i ? "," : "", cui_open_choice(i, 0));
    snprintf(b + o, sizeof b - o, "]}");
    reply_json(fd, 200, b);
}

static void favorites_json(int fd) {
    uint32_t f[400];
    int n = favs_list(f, 400);
    char b[4800];
    size_t o = (size_t)snprintf(b, sizeof b, "{\"ids\":[");
    for (int i = 0; i < n && o + 16 < sizeof b; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s%u", i ? "," : "", f[i]);
    snprintf(b + o, sizeof b - o, "]}");
    reply_json(fd, 200, b);
}

/* the request body (after the headers already read into req[0..n)); caller frees */
static uint8_t *read_body(int fd, const char *req, size_t n, size_t max, size_t *len) {
    const char *end = strstr(req, "\r\n\r\n");
    if (!end) return 0;
    size_t hdr = (size_t)(end - req) + 4, want = 0;
    for (const char *p = req; p < end; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : end) {
        if (!strncasecmp(p, "Content-Length:", 15)) { want = (size_t)strtoul(p + 15, 0, 10); break; }
    }
    if (!want || want > max) return 0;
    uint8_t *b = malloc(want);
    if (!b) return 0;
    size_t have = n > hdr ? n - hdr : 0;
    if (have > want) have = want;
    memcpy(b, req + hdr, have);
    while (have < want) {
        ssize_t k = recv(fd, b + have, want - have, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; free(b); return 0; }
        have += (size_t)k;
    }
    *len = want;
    return b;
}

static void background_post(int fd, const char *q, const char *req, size_t n) {
    if (qint(q, "remove", 0)) {
        unlink(CUI_CUSTOM_BG);
        cui_background_changed();
        result_json(fd, 0, "Background photo removed.");
        return;
    }
    size_t len = 0;
    uint8_t *body = read_body(fd, req, n, 6u << 20, &len);
    static const uint8_t PNG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!body || len < 8 || memcmp(body, PNG, 8)) { free(body); result_json(fd, -1, "Send a PNG picture (up to 6 MB)."); return; }
    mkdir(CUI_THEME_DIR, 0777);
    FILE *f = fopen(CUI_CUSTOM_BG ".tmp", "wb");
    int ok = f && fwrite(body, 1, len, f) == len;
    if (f && fclose(f)) ok = 0;
    free(body);
    if (!ok || rename(CUI_CUSTOM_BG ".tmp", CUI_CUSTOM_BG)) { unlink(CUI_CUSTOM_BG ".tmp"); result_json(fd, -1, "Could not save the picture."); return; }
    cui_background_changed();
    cui_config c;
    cui_get(&c);
    if (c.background != CUI_BG_CUSTOM) { c.background = CUI_BG_CUSTOM; cui_update(&c); }
    web_log("web: new background photo (%zu bytes)", len);
    result_json(fd, 0, "Background photo set.");
}

static void *client(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[4096];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n > 0) {
        req[n] = 0;
        char method[8] = "", uri[512] = "";
        sscanf(req, "%7s %511s", method, uri);
        char *q = strchr(uri, '?');
        if (q) *q++ = 0;
        char msg[200] = "";
        int post = !strcmp(method, "POST");
        if (!strcmp(uri, "/") || !strcmp(uri, "/index.html")) serve_file(fd, "/index.html", 0);
        else if (!strcmp(uri, "/api/state")) state_json(fd);
        else if (!strcmp(uri, "/api/roster")) serve_file(fd, "/roster.json", 0);
        else if (post && !strcmp(uri, "/api/place")) {
            uint32_t id = (uint32_t)qint(q, "id", 0);
            int slot = (int)qint(q, "slot", -1);
            if (!id) result_json(fd, -1, "Missing figure id.");
            else {
                if (slot < 0 || slot >= DI_SLOTS) slot = game_route(id, slot);
                int r = game_place(slot, id, msg, sizeof msg);
                result_json(fd, r, msg);
            }
        } else if (post && !strcmp(uri, "/api/remove")) {
            int r = game_remove((int)qint(q, "slot", -1), msg, sizeof msg);
            result_json(fd, r, msg);
        } else if (post && !strcmp(uri, "/api/move")) {
            int r = game_move((int)qint(q, "from", -1), (int)qint(q, "to", -1), msg, sizeof msg);
            result_json(fd, r, msg);
        } else if (post && !strcmp(uri, "/api/clear")) {
            int r = game_clear_all(msg, sizeof msg);
            result_json(fd, r, msg);
        } else if (!strcmp(uri, "/api/favorites")) {
            favorites_json(fd);
        } else if (post && !strcmp(uri, "/api/fav")) {
            uint32_t id = (uint32_t)qint(q, "id", 0);
            if (id) favs_set(id, (int)qint(q, "on", 1) != 0);
            favorites_json(fd);
        } else if (!strcmp(uri, "/api/settings") && !post) {
            settings_json(fd);
        } else if (post && !strcmp(uri, "/api/settings")) {
            cui_config c;
            cui_get(&c);
            c.opacity = (int)qint(q, "opacity", c.opacity);
            c.ui_size = (int)qint(q, "ui_size", c.ui_size);
            c.peek_size = (int)qint(q, "peek_size", c.peek_size);
            c.fast_load = (int)qint(q, "fast_load", c.fast_load);
            c.theme = (int)qint(q, "theme", c.theme);
            c.background = (int)qint(q, "background", c.background);
            long pk = qint(q, "peek", -1);
            uint32_t m;
            if (pk >= 0 && cui_peek_choice((int)pk, &m)) c.peek_mask = m;
            long op = qint(q, "open", -1);
            if (op >= 0 && cui_open_choice((int)op, &m)) c.combo = m;
            cui_update(&c);
            settings_json(fd);
        } else if (!strcmp(uri, "/api/background")) {
            if (post) {
                struct timeval up = {15, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &up, sizeof up);
                background_post(fd, q, req, (size_t)n);
            } else {
                FILE *f = fopen(CUI_CUSTOM_BG, "rb");
                if (!f) reply(fd, "404 Not Found", "text/plain", "no background", 13, 0);
                else {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    char *d = sz > 0 && sz < (8 << 20) ? malloc((size_t)sz) : 0;
                    if (d && fread(d, 1, (size_t)sz, f) == (size_t)sz) reply(fd, "200 OK", "image/png", d, (size_t)sz, 0);
                    else reply(fd, "404 Not Found", "text/plain", "no background", 13, 0);
                    free(d);
                    fclose(f);
                }
            }
        } else if ((!strncmp(uri, "/ui/", 4) || !strncmp(uri, "/assets/", 8)) && !strstr(uri, "..")) {
            serve_file(fd, uri, 1);
        } else reply(fd, "404 Not Found", "text/plain", "404 Not Found", 13, 0);
    }
    close(fd);
    __atomic_fetch_sub(&g_clients, 1, __ATOMIC_RELAXED);
    return 0;
}

static void *server(void *arg) {
    int port = (int)(intptr_t)arg;
    int s = -1;
    for (int attempt = 0; s < 0; attempt++) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) {
            if (attempt == 0) web_log("web: port %d busy (errno %d), retrying", port, errno);
            close(s);
            s = -1;
            sleep(2);   /* a replaced injector may still hold the port for a moment */
        }
    }
    web_log("web: phone remote on port %d", port);
    for (;;) {
        int c = accept(s, 0, 0);
        if (c < 0) { usleep(10000); continue; }
        if (__atomic_load_n(&g_clients, __ATOMIC_RELAXED) >= 12) { close(c); continue; }
        __atomic_fetch_add(&g_clients, 1, __ATOMIC_RELAXED);
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&t, &at, client, (void *)(intptr_t)c)) { close(c); __atomic_fetch_sub(&g_clients, 1, __ATOMIC_RELAXED); }
        pthread_attr_destroy(&at);
    }
    return 0;
}

int web_start(int port) {
    pthread_t t;
    return pthread_create(&t, 0, server, (void *)(intptr_t)port);
}
