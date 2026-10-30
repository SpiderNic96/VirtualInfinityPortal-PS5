#!/usr/bin/env python3
"""What the game does with the base lights, from /data/dimod/usb_sniff.log.

    python3 tools/led_summary.py usb_sniff.log            # every LED command, with timing
    python3 tools/led_summary.py usb_sniff.log --stats    # which commands / values the game uses

Shows each colour / fade / flash command with the pad, colour and parameters, and how long the
game waited before the next command for that pad. Those gaps are what tell us how long one
"time unit" of a fade or flash really is (led_unit_ms in config.ini).
Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later"""
import collections, sys

PADS = {0: "all", 1: "hexagon", 2: "left", 3: "right"}


def parse(path):
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) != 3 or p[1] != "OUT":
            continue
        try:
            t, b = int(p[0]), bytes.fromhex(p[2])
        except ValueError:
            continue
        if len(b) < 5 or b[0] != 0xFF or b[2] not in (0x90, 0x92, 0x93):
            continue
        cmd, pad, d = b[2], b[4], b[5:]
        if cmd == 0x90 and len(d) >= 3:
            yield t, pad, "colour", d[0:3], ()
        elif cmd == 0x92 and len(d) >= 5:
            yield t, pad, "fade", d[2:5], (("time", d[0]), ("count", d[1]))
        elif cmd == 0x93 and len(d) >= 6:
            yield t, pad, "flash", d[3:6], (("on", d[0]), ("off", d[1]), ("count", d[2]))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    ev = list(parse(sys.argv[1]))
    if not ev:
        print("no LED commands in that log (sniff_log=1 in config.ini, then play a little)")
        return 1
    if "--stats" in sys.argv:
        kinds = collections.Counter((k, tuple(v for _, v in prm)) for _, _, k, _, prm in ev)
        cols = collections.Counter(("%02x%02x%02x" % tuple(c)) for _, _, _, c, _ in ev)
        print(f"{len(ev)} LED commands")
        print("\ncommand / parameters        times")
        for (k, prm), n in kinds.most_common():
            print(f"  {k:7s} {str(prm):20s} {n}")
        print("\ncolours")
        for c, n in cols.most_common(20):
            print(f"  #{c}  {n}")
        return 0
    last = {}
    print("   time ms  pad      command  colour   parameters              next cmd on this pad")
    nxt = {}
    for i in range(len(ev) - 1, -1, -1):
        t, pad = ev[i][0], ev[i][1]
        nxt[i] = last.get(pad)
        last[pad] = t
    t0 = ev[0][0]
    for i, (t, pad, kind, c, prm) in enumerate(ev):
        gap = f"+{nxt[i] - t} ms" if nxt[i] is not None else ""
        prm_s = " ".join(f"{k}={v}" for k, v in prm)
        print(f"{t - t0:10d}  {str(PADS.get(pad, pad)):8s} {kind:7s}  #{c[0]:02x}{c[1]:02x}{c[2]:02x}  {prm_s:22s}  {gap}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
