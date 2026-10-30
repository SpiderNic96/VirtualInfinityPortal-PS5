/* Link between the injected game and the injector's UIs (TV picker, phone remote).
 * Only the monitor thread touches game memory (the SDK's kernel r/w is not thread safe):
 * UIs queue a request and wait; game_service() runs it and refreshes the snapshot.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../core/diproto.h"
#include "../core/ledanim.h"

typedef struct {
    int linked;                 /* game injected and alive */
    int opened;                 /* the game opened the virtual base */
    int activated;              /* the game sent the base's activate command */
    uint32_t cmds, delivered;
    int present[DI_SLOTS];
    uint32_t fig[DI_SLOTS];
    di_led led[3];
    led_anim la[3];             /* the lights as the real base would show them (ledanim.h) */
    int led_unit;               /* ms per protocol time unit */
} game_snap;

void game_set_led_unit(int ms);
/* the buttons that open the picker: written into the running game at once (and on every attach) */
void game_set_combo(uint32_t mask);

void game_attach(pid_t pid, intptr_t mem);
void game_detach(void);
/* monitor thread only */
void game_service(pid_t pid, intptr_t mem);

void game_snapshot(game_snap *s);
/* figure commands; block until the game handled them. 0 = ok, message in msg either way */
int game_place(int slot, uint32_t id, char *msg, size_t n);
int game_remove(int slot, char *msg, size_t n);
int game_move(int from, int to, char *msg, size_t n);
int game_clear_all(char *msg, size_t n);
/* Where an item goes, by the game's rules: one character per player's pad (a new one swaps it
 * out), two power discs under each character, and on the hexagon three pieces at most of which
 * one play set / Toy Box piece. `want` (a chosen spot, -1 = none) wins when the item fits there.
 * side: the player placing (0 left, 1 right, -1 unknown); players: controllers in use. With one
 * controller the player's pad is the one their character stands on (left unless only right). */
int game_route_rules(const game_snap *s, uint32_t id, int want, int side, int players);
/* the same from the live snapshot, for the phone (nobody named) */
int game_route(uint32_t id, int want);

/* console overlay controller state (di_header.ext) */
typedef struct {
    int valid;
    uint32_t mode, open, open_seq, close_req, close_ack, raw, wr, heartbeat;
    uint32_t ring[32];
    uint32_t owner;             /* controller index that opened the picker (0 = first seen) */
    int connected;              /* controllers read as connected in the last ~2 s */
} game_ext;
void game_ext_snapshot(game_ext *e);
void game_ext_request(int mode, int close);   /* mode < 0 leaves it; close != 0 asks the hook to close */

/* status line shared by both UIs */
void game_status_set(const char *fmt, ...);
void game_status_get(char *buf, size_t n, uint64_t *when_ms);
uint64_t game_ms(void);
const char *game_slot_name(int slot);
