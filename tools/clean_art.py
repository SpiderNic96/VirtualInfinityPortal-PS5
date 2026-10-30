#!/usr/bin/env python3
"""Tidy fetched art before the build (needs Pillow: pkg install python-pillow in Termux).

  python3 tools/clean_art.py [ART_DIR] [--reject 1000030,3000187 ...]

What it does to each file in ART_DIR (default tools/art, or dimod_art/art next to this script):
  * in-game "card" pictures (a power disc on a blue panel with its name and description under
    it, 540 x 632) are cropped to the disc and the blue panel is made transparent
  * a render on a flat white / plain background gets that background made transparent
    (only the background touching the edges, so white parts of the figure stay)
  * files that are really WebP / JPEG with a .png name are rewritten as real PNGs, at most 320 px (twice the badge size)
  * --reject moves the named ids to ART_DIR/_rejected (wrong picture), so they can be fetched again
The original of every changed file is kept in ART_DIR/_originals. The build ignores both folders."""
import pathlib, re, shutil, sys
from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
MAX_PX = 320
CARD_BG = (24, 69, 115)       # the panel behind the disc on the in-game cards
CARD_EDGE = (176, 221, 249)   # their light-blue frame


def near(a, b, tol):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def has_alpha(im):
    if im.mode not in ("RGBA", "LA", "PA") and "transparency" not in im.info:
        return False
    return im.convert("RGBA").getchannel("A").getextrema()[0] < 250


def is_card(im):
    w, h = im.size
    if not (0.8 < w / h < 0.9 and w >= 300):
        return False
    rgb = im.convert("RGB")
    return near(rgb.getpixel((3, 3)), CARD_EDGE, 40) and near(rgb.getpixel((int(w * 0.1), int(h * 0.3))), CARD_BG, 30)


def flood_clear(rgb, tol, seeds=None):
    """make the background connected to the edges transparent; returns RGBA, or None if that
    would clear too little (not a flat background) or nearly everything"""
    w, h = rgb.size
    work = rgb.copy()
    KEY = (255, 0, 254)
    if seeds is None:
        seeds = [(x, y) for x in (0, w // 4, w // 2, 3 * w // 4, w - 1) for y in (0, h - 1)] + \
                [(x, y) for y in (h // 4, h // 2, 3 * h // 4) for x in (0, w - 1)]
    for s in seeds:
        if work.getpixel(s) != KEY:
            ImageDraw.floodfill(work, s, KEY, thresh=tol)
    px = work.load()
    mask = Image.new("L", (w, h), 255)
    mp = mask.load()
    cleared = 0
    for y in range(h):
        for x in range(w):
            if px[x, y] == KEY:
                mp[x, y] = 0
                cleared += 1
    frac = cleared / (w * h)
    if frac < 0.08 or frac > 0.92:
        return None
    out = rgb.convert("RGBA")
    out.putalpha(mask)
    return out


def flat_edges(rgb, tol=18):
    """the edge pixels are (nearly) one colour: a plain background"""
    w, h = rgb.size
    pts = [rgb.getpixel((x, y)) for x in range(0, w, max(1, w // 20)) for y in (0, h - 1)] + \
          [rgb.getpixel((x, y)) for y in range(0, h, max(1, h // 20)) for x in (0, w - 1)]
    c = pts[0]
    return sum(1 for p in pts if near(p, c, tol)) >= len(pts) * 0.85, c


def clean(path, originals):
    im = Image.open(path)
    fmt = im.format
    im.load()
    out, what = None, []
    if is_card(im):
        w, h = im.size
        top = im.convert("RGB").crop((int(w * 0.09), int(h * 0.035), int(w * 0.91), int(h * 0.565)))   # past the frame's corner marks
        cw, ch = top.size
        cleared = flood_clear(top, 34, [(x, y) for x in (2, cw // 2, cw - 3) for y in (2, ch - 3)] + [(2, ch // 2), (cw - 3, ch // 2)])
        if cleared:
            out = cleared.crop(cleared.getchannel("A").getbbox())
            what.append("card cropped to the disc")
    elif not has_alpha(im):
        rgb = im.convert("RGB")
        flat, c = flat_edges(rgb)
        w, h = rgb.size
        edge = [rgb.getpixel((x, y)) for x in range(0, w, max(1, w // 24)) for y in (0, h - 1)] + \
               [rgb.getpixel((x, y)) for y in range(0, h, max(1, h // 24)) for x in (0, w - 1)]
        studio = all(min(p) > 165 and max(p) - min(p) < 22 for p in edge)   # light grey studio backdrop
        if studio:
            seeds = [(x, y) for x in range(0, w, max(1, w // 24)) for y in (0, h - 1)] + \
                    [(x, y) for y in range(0, h, max(1, h // 24)) for x in (0, w - 1)]
            cleared = flood_clear(rgb, 22, seeds)
            if cleared:
                out, what = cleared, ["studio backdrop made transparent"]
        elif flat and (sum(c) > 600 or sum(c) < 60 or max(c) - min(c) < 25):   # white, black or grey backdrop
            cleared = flood_clear(rgb, 26)
            if cleared:
                out, what = cleared, ["plain background made transparent"]
    if out is None and fmt != "PNG":
        out, what = im.convert("RGBA"), [f"{fmt} rewritten as PNG"]
    if out is None:
        return None
    originals.mkdir(exist_ok=True)
    keep = originals / path.name
    if not keep.exists():
        shutil.copy2(path, keep)
    dest = path.with_suffix(".png")
    if max(out.size) > MAX_PX:   # the badges are 160 px: 320 is twice that and keeps files small
        out.thumbnail((MAX_PX, MAX_PX), Image.LANCZOS)
        what.append(f"scaled to {out.width}x{out.height}")
    out.save(dest, optimize=True)
    if dest != path:
        path.unlink()
    return ", ".join(what)


def main():
    args = sys.argv[1:]
    reject = set()
    if "--reject" in args:
        i = args.index("--reject")
        reject = {int(x) for x in re.split(r"[,\s]+", args[i + 1]) if x}
        del args[i:i + 2]
    art = pathlib.Path(args[0]).expanduser() if args else next(
        (p for p in (HERE / "art", HERE / "dimod_art" / "art") if p.is_dir()), HERE / "art")
    if not art.is_dir():
        sys.exit(f"no art folder at {art}")
    rej, orig = art / "_rejected", art / "_originals"
    n = 0
    for p in sorted(art.iterdir()):
        if not p.is_file() or p.suffix.lower() not in (".png", ".jpg", ".jpeg", ".webp"):
            continue
        m = re.match(r"^p?(\d{7})", p.stem)
        if m and int(m.group(1)) in reject:
            rej.mkdir(exist_ok=True)
            shutil.move(str(p), rej / p.name)
            print(f"rejected  {p.name}")
            continue
        try:
            what = clean(p, orig)
        except Exception as e:
            print(f"ERROR     {p.name}: {e}")
            continue
        if what:
            n += 1
            print(f"cleaned   {p.name}: {what}")
    print(f"{n} cleaned, {len(reject)} rejected; originals in {orig}")


if __name__ == "__main__":
    main()
