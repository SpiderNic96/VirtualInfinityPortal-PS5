#!/usr/bin/env python3
"""Cross-check test_core's output against an independent implementation: hashlib SHA-1,
the `cryptography` package's AES, zlib's CRC table semantics and a literal port of RPCS3's
Infinity.cpp scramble / descramble / get_next / generate_seed."""
import hashlib, subprocess, sys, struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

M32 = 0xFFFFFFFF
MASK = 0x8E55AA1B3999E8AA

def rotl(x, k): return ((x << k) | (x >> (32 - k))) & M32

class Base:  # RPCS3 infinity_base, line by line
    def seed(self, s):
        self.a, self.b, self.c, self.d = 0xF1EA5EED, s, s, s
        for _ in range(23): self.next()
    def next(self):
        a, b, c = self.a, self.b, self.c
        ret = rotl(self.b, 27)
        temp = (a + (((ret ^ M32) + 1) & M32)) & M32
        b ^= rotl(c, 17)
        a = self.d
        c = (c + a) & M32
        ret = (b + temp) & M32
        a = (a + temp) & M32
        self.c, self.a, self.b, self.d = a, b, c, ret
        return ret

def descramble(v):
    m, r = MASK, 0
    for _ in range(64):
        if m & (1 << 63): r = ((r << 1) | (v & 1)) & M32
        v >>= 1; m = (m << 1) & ((1 << 64) - 1)
    return r

def scramble(v, g):
    m, r = MASK, 0
    for _ in range(64):
        r = (r << 1) & ((1 << 64) - 1)
        if m & 1: r |= v & 1; v >>= 1
        else: r |= g & 1; g >>= 1
        m >>= 1
    return r

SALT = bytes([0xAF,0x62,0xD2,0xEC,0x04,0x91,0x96,0x8C,0xC5,0x2A,0x1A,0x71,0x65,0xF8,0x65,0xFE]) + b"(c) Disney 2013"

def key_of(uid):
    d = hashlib.sha1(SALT + uid).digest()
    return b"".join(d[i*4:i*4+4][::-1] for i in range(4))

def crc_infinity(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8): c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
    return c

def aes(key, blk, dec=False):
    c = Cipher(algorithms.AES(key), modes.ECB())
    o = c.decryptor() if dec else c.encryptor()
    return o.update(blk) + o.finalize()

out = subprocess.run([sys.argv[1]], capture_output=True, text=True).stdout
print(out.strip().splitlines()[-1])
lines = {}
for ln in out.splitlines():
    k, _, v = ln.partition(" ")
    lines.setdefault(k, []).append(v)

bad = 0
def check(name, ok):
    global bad
    print(("ok   " if ok else "FAIL ") + name)
    bad += not ok

s = lines["scr"][0].split()
check("scramble vs RPCS3", int(s[0], 16) == scramble(0x12345678, 0) and int(s[1], 16) == scramble(0xDEADBEEF, 0xFFFFFFFF))
check("descramble vs RPCS3", int(lines["des"][0], 16) == descramble(0x0123456789ABCDEF))

b = Base()
b.seed(descramble(int.from_bytes(bytes([0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE]), "big")))
want = [scramble(b.next(), 0).to_bytes(8, "big").hex() for _ in range(3)]
check("challenge replies vs RPCS3 PRNG", lines["chal"] == want)

uid = bytes([0x04,0xA1,0xB2,0xC3,0xD4,0xE5,0xF6])
check("AES key (SHA1 salt||uid, words swapped)", lines["key"][0] == key_of(uid).hex())
tag = bytes.fromhex(lines["tag"][0])
k = key_of(tag[:7])
blk = aes(k, tag[16:32], dec=True)
check("block 1 figure number 1000001", int.from_bytes(blk[1:4], "big") == 1000001)
check("block 1 CRC32 (init 0, big endian)", int.from_bytes(blk[12:16], "big") == crc_infinity(blk[:12]))
check("block 1 constants 00 01 D1 1F, date 13/08/18", blk[7:12] == bytes([0, 0, 1, 0xD1, 0x1F]) and blk[4:7] == bytes([0x0D, 8, 0x12]))
check("blank blocks 4/8/12/13 = AES(0)", all(tag[i*16:i*16+16] == aes(k, bytes(16)) for i in (4, 8, 12, 13)))
check("UID suffix 89 44 00 C2", tag[7:11] == bytes([0x89, 0x44, 0x00, 0xC2]))
check("sector trailer access bits", tag[0x36:0x39] == bytes([0x17,0x87,0x8E]) and all(tag[s*0x40+0x36:s*0x40+0x39] == bytes([0x77,0x87,0x88]) for s in range(1,5)))
sys.exit(1 if bad or "FAILED" in out else 0)
