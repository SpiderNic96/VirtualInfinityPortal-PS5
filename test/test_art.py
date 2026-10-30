#!/usr/bin/env python3
"""Art import checks (tools/build_data.py): whatever shape, size or format a render comes in,
the badge must come out 160 x 160 RGBA, fitted inside its box, centred, never stretched, with
a transparent outside. Run by test/run_tests.sh."""
import importlib.util, pathlib, sys, tempfile
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("build_data", ROOT / "tools" / "build_data.py")
bd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bd)
N = bd.ART_SIZE
fails = 0


def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    fails += 0 if cond else 1


def bbox(im):
    return im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()


with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td)
    # 1. tall character render, transparent background, lots of empty margin, off centre
    im = Image.new("RGBA", (700, 1000), (0, 0, 0, 0))
    ImageDraw.Draw(im).rectangle([400, 150, 640, 950], fill=(200, 40, 40, 255))
    im.save(d / "1000001_mr-incredible.png")
    # 2. round disc as an opaque square JPEG (no transparency at all)
    Image.new("RGB", (300, 300), (30, 120, 200)).save(d / "3000003.jpg")
    # 3. hex disc, transparent, small and wide
    im = Image.new("RGBA", (90, 60), (0, 0, 0, 0))
    ImageDraw.Draw(im).polygon(bd.hexagon(45, 30, 28, 90), fill=(250, 200, 20, 255))
    im.save(d / "p4000018.png")
    # 4. character without a transparent background
    Image.new("RGB", (512, 512), (240, 240, 240)).save(d / "1000002.png")
    # 5. problems
    Image.new("RGBA", (64, 64)).save(d / "mickey.png")             # no id in the name
    Image.new("RGBA", (64, 64), (1, 1, 1, 255)).save(d / "9999999.png")  # unknown id
    Image.new("RGBA", (64, 64), (1, 1, 1, 255)).save(d / "1000001_mr-incredible-old.png")  # duplicate
    (d / "2000001.png").write_bytes(b"not a png")                  # unreadable

    figs = bd.load_figures()
    art, report, nerr = bd.import_art(figs, d)
    print("\n".join("     " + r for r in report))
    check(nerr == 4, f"4 problems reported (got {nerr})")
    check(set(art) == {1000001, 3000003, 4000018, 1000002}, "4 badges imported")
    for fid, im in art.items():
        check(im.size == (N, N) and im.mode == "RGBA", f"{fid}: {N}x{N} RGBA")
        c = [im.getpixel(p)[3] for p in ((0, 0), (N - 1, 0), (0, N - 1), (N - 1, N - 1))]
        if fid != 1000002:
            check(max(c) == 0, f"{fid}: corners transparent")

    b = bbox(art[1000001])
    pad = round(N * 0.04)
    check(b[1] >= pad - 1 and b[3] <= N - pad + 1 and b[3] - b[1] >= N - 2 * pad - 2, f"character fills its height inside the margin {b}")
    w = b[2] - b[0]
    check(abs(w / (b[3] - b[1]) - 241 / 801) < 0.03, f"character keeps its aspect (w {w})")
    check(abs((b[0] + b[2]) / 2 - N / 2) <= 1, "character centred")
    b = bbox(art[3000003])
    check(b == (0, 0, N, N) and art[3000003].getpixel((N // 2, N // 2))[3] == 255, "opaque disc cut to a full circle")
    b = bbox(art[4000018])
    check(b[2] - b[0] >= N - 2 or b[3] - b[1] >= N - 2, f"small hex disc scaled up to fill its box {b}")
    check(any("NO TRANSPARENT" in r for r in report), "opaque character flagged")

sys.exit(1 if fails else 0)
