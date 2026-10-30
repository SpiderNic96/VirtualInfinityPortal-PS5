/* Disney Infinity base (PDP 0x0E6F:0x0129) - protocol state machine.
 *
 * Freestanding (no libc): runs inside the game process as part of the hook blob, and on the
 * host in the unit tests. Behaviour follows the emulated bases in Dolphin, RPCS3 and shadPS4
 * (deReeperJosh's reverse engineering), which all boot the retail games.
 *
 * Wire format, both directions are 32-byte HID interrupt reports:
 *   host -> base (EP 0x01): FF <len> <cmd> <seq> <data...> <sum>   len counts cmd+seq+data
 *   base -> host (EP 0x81): AA <len> <seq> <data...> <sum>         command response
 *                           AB 04 <pad> 09 <order> <0 added|1 removed> <sum>   tag event
 * <sum> is the low byte of the sum of every byte before it.
 *
 * Positions (UI order, also our slot index):
 *   0 hexagon (play set / toy box piece)   1, 2 hexagon (stacked hex discs)
 *   3 player 1 figure  4, 5 player 1 round power discs
 *   6 player 2 figure  7, 8 player 2 round power discs
 * The base reports pads: 1 hexagon (0-2), 2 player one (3-5), 3 player two (6-8).
 *
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define DI_VID        0x0E6Fu
#define DI_PID        0x0129u
#define DI_TAG_SIZE   320          /* MIFARE Mini dump: 20 blocks x 16 bytes */
#define DI_SLOTS      9
#define DI_QMAX       32

enum { DI_LED_OFF = 0, DI_LED_SOLID = 1, DI_LED_FADE = 2, DI_LED_FLASH = 3 };

typedef struct {
    uint8_t  present;
    uint8_t  order;                /* order_added, 255 = never placed at this position */
    uint8_t  dirty;                /* the game wrote a block since the injector last saved */
    uint8_t  pad_;
    uint32_t fig_id;               /* figure number (informational, set by whoever places it) */
    uint8_t  data[DI_TAG_SIZE];
} di_figure;

/* last LED command per pad (0 hexagon, 1 player one, 2 player two), mirrored to the overlay */
typedef struct {
    uint8_t  mode;                 /* DI_LED_* */
    uint8_t  rgb[3];               /* target colour */
    uint8_t  from[3];              /* colour before this command (fade start) */
    uint8_t  a, b, c;              /* fade: speed, count / flash: on, off, count */
    uint8_t  pad_[2];
    uint32_t seq;                  /* bumped on every LED command for this pad */
} di_led;

typedef struct {
    uint32_t ra, rb, rc, rd;       /* the base's challenge PRNG */
    uint8_t  next_order;
    uint8_t  activated;            /* 0x80 seen */
    uint8_t  pad_[2];
    uint32_t q_head, q_tail;       /* command responses */
    uint32_t e_head, e_tail;       /* tag added / removed events (delivered first) */
    uint8_t  q[DI_QMAX][32];
    uint8_t  e[DI_QMAX][32];
    di_figure fig[DI_SLOTS];
    di_led   led[3];
    uint32_t cmds;                 /* OUT reports handled */
    uint32_t unknown;              /* commands not in the table (answered with a blank reply) */
    uint8_t  last_unknown;
    uint8_t  pad2_[3];
} di_base;

void    di_init(di_base *b);
uint8_t di_checksum(const uint8_t *d, int n);
uint8_t di_pad_of(int slot);       /* 1, 2, 3 (0 = invalid slot) */

/* one OUT report from the game; queues the response */
void di_write(di_base *b, const uint8_t *buf, uint32_t len);
/* 1 if an IN report is waiting */
int  di_pending(const di_base *b);
/* next IN report (events first), 0 if none */
int  di_read(di_base *b, uint8_t out[32]);

/* tag placement; both queue the matching event. Return 0 on success. */
int  di_place(di_base *b, int slot, const uint8_t data[DI_TAG_SIZE], uint32_t fig_id);
int  di_remove(di_base *b, int slot);

/* the challenge helpers, exposed for the unit tests */
uint32_t di_descramble(uint64_t v);
uint64_t di_scramble(uint32_t v, uint32_t garbage);
void     di_seed(di_base *b, uint32_t seed);
uint32_t di_next(di_base *b);
