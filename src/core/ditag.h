/* Disney Infinity figure tags (320-byte MIFARE Mini dumps): create, decode, verify.
 * Same file format as Dolphin / RPCS3 / shadPS4 figure files, so their .bin files work as-is.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define DITAG_SIZE 320

/* SHA1("(c) Disney 2013" salt || uid[7]) with every 32-bit word byte-swapped */
void ditag_key(const uint8_t uid[7], uint8_t key[16]);

/* new blank tag for figure number `id` (1000001 ...) of series 1, 2 or 3; uid7 = 7 random bytes.
 * Returns 0, or -1 if the id does not fit the 24-bit field. */
int ditag_create(uint8_t out[DITAG_SIZE], uint32_t id, int series, const uint8_t uid7[7]);

/* figure number stored in block 1, or 0 if the block does not decrypt to a valid figure
 * (bad CRC). series_out (optional) receives byte 9 of the block. */
uint32_t ditag_figure_id(const uint8_t tag[DITAG_SIZE], int *series_out);

/* exposed for tests */
void     ditag_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void     ditag_aes_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void     ditag_aes_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
uint32_t ditag_crc32(const uint8_t *buf, size_t n);
