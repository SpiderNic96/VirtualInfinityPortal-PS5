#!/usr/bin/env python3
"""Build data/dimod (copied to /data/dimod on the PS5):

  web/index.html        phone remote (from src/web/index.html)
  web/roster.json       editions -> categories -> items, for the phone remote
  web/ui/catalog.tsv    id, series, kind, name, section - read by the payload (TV picker, slot rules, names)
  web/ui/p<id>.png      a badge per figure / disc / piece: your art from tools/art if there is
                        one (trimmed, fitted and locked to 160 x 160 RGBA), else a generated one
  web/ui/base.png       the Infinity base picture
  web/ui/glow.png ring.png hexring.png frame.png   white masks the TV picker tints

The figure list (tools/figures.tsv) is RPCS3's Infinity figure table: ids and names for every
1.0, 2.0 and 3.0 figure, power disc and play set piece. Kinds come from the id ranges the game
uses: 1xxxxxx characters, 2xxxxxx play set / toy box pieces, 3xxxxxx round power discs,
4xxxxxx hexagonal power discs. Disc sections (Vehicles, Mounts, Skies...) come from
tools/disc_groups.tsv.

Real art: drop PNGs into tools/art (see tools/art/README.txt), then run this again.
  python3 tools/build_data.py --check-art     only check tools/art, build nothing (exit 1 on errors)"""
import os
import json, math, pathlib, re, shutil, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "dimod"
WEB, UI = OUT / "web", OUT / "web" / "ui"
KINDS = {1: "F", 2: "P", 3: "R", 4: "H"}
KIND_LABEL = {"F": "CHARACTER", "P": "PLAY SET", "R": "POWER DISC", "H": "HEX DISC"}
SERIES = {1: ("Disney Infinity 1.0", (56, 189, 248)), 2: ("Disney Infinity 2.0", (239, 68, 68)),
          3: ("Disney Infinity 3.0", (168, 85, 247))}
ART_SIZE = 160          # every badge is exactly this, square RGBA: the TV and phone boxes assume it
ART_MIN = 96            # smaller source art is upscaled, with a warning
KIND_ORDER = "FPRH"
CATEGORIES = [("F", "Characters"), ("P", "Play Sets & Toy Box games"), ("R", "Power Discs (round)"),
              ("H", "Power Discs (hexagonal)")]


def font(size, bold=True):
    for p in ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "C:/Windows/Fonts/arialbd.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            pass
    return ImageFont.load_default()


def load_figures():
    rows = []
    for ln in (ROOT / "tools" / "figures.tsv").read_text(encoding="utf-8").splitlines():
        if not ln or ln.startswith("#"):
            continue
        i, s, n = ln.split("\t", 2)
        i, s = int(i), int(s)
        rows.append({"id": i, "series": s, "kind": KINDS[i // 1000000], "name": n})
    return rows


def load_lightfx():
    """{id: ((r, g, b), kind)} light effects (tools/lightfx.tsv): kind "saber" (Light FX blade lit by the
    pad) or "glow" (glow-in-the-dark: charges in the pad light, glows when it goes dark)"""
    out = {}
    p = ROOT / "tools" / "lightfx.tsv"
    if p.exists():
        for ln in p.read_text(encoding="utf-8").splitlines():
            ln = ln.split("#", 1)[0] if ln.startswith("#") else ln
            v = [x.strip() for x in ln.split("\t")]
            if len(v) >= 2 and v[0].isdigit():
                c = v[1].lstrip("#")
                kind = v[2] if len(v) > 2 and v[2] in ("saber", "glow") else "saber"
                out[int(v[0])] = (tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)), kind)
    return out


def saber_glow(badge_im, colour, size=ART_SIZE):
    """g<id>.png: the blade of a Light FX figure as a coloured glow, in the badge's own frame, so the
    picker can lay it over the badge and fade it with the pad light. None if no blade is found."""
    import colorsys
    import numpy as np
    from scipy import ndimage
    a = np.asarray(badge_im.convert("RGBA")).astype(float) / 255
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    mx, mn = a[..., :3].max(-1), a[..., :3].min(-1)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    hue = np.vectorize(lambda R, G, B: colorsys.rgb_to_hsv(R, G, B)[0])(r, g, b)
    th = colorsys.rgb_to_hsv(*[c / 255 for c in colour])[0]
    dh = np.minimum(abs(hue - th), 1 - abs(hue - th))
    tinted = (dh < 0.08) & (sat > 0.38) & (mx > 0.5) & (al > 0.5)         # the coloured edge of the blade
    near = ndimage.binary_dilation(tinted, iterations=2)
    hot = near & (mx > 0.8) & (sat < 0.5) & (al > 0.5)                       # its white-hot middle
    blade = ndimage.binary_closing(tinted | hot, iterations=1) & (al > 0.3)
    lab, n = ndimage.label(blade, structure=np.ones((3, 3)))

    def stretch(mask):   # how line-like a piece is: long axis / short axis
        ys, xs = np.nonzero(mask)
        if len(xs) < 4:
            return 1.0
        ev = np.linalg.eigvalsh(np.cov(np.vstack([xs, ys])))
        return float(np.sqrt(max(ev[1], 1e-6) / max(ev[0], 1e-6)))
    pieces = [(int(ndimage.sum(blade, lab, i)), stretch(lab == i), i) for i in range(1, n + 1)]
    lines = [p for p in pieces if p[0] >= 6 and p[1] >= 2.6]   # blades are long and thin; skin, sleeves are not
    keep = [p[2] for p in lines] or ([max(pieces)[2]] if pieces else [])   # a blade pointing at the camera: the biggest piece
    blade = np.isin(lab, keep)
    if blade.sum() < 10:
        return None
    m = Image.fromarray((blade * 255).astype(np.uint8))
    halo = np.asarray(m.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(size / 32))).astype(float) / 255
    wide = np.asarray(m.filter(ImageFilter.GaussianBlur(size / 14))).astype(float) / 255
    alpha = np.clip(blade * 0.9 + halo * 0.85 + wide * 0.9, 0, 1)
    whiten = np.clip(blade * 0.55 + halo * 0.15, 0, 1)[..., None]
    rgb = np.array(colour, float)[None, None, :] / 255 * (1 - whiten) + whiten
    out = np.dstack([rgb, alpha[..., None]])
    return Image.fromarray((out * 255).astype(np.uint8), "RGBA")


def phosphor_glow(badge_im, colour, size=ART_SIZE):
    """g<id>.png for a glow-in-the-dark figure: the light parts of the figure (white and pale-green
    plastic, the parts that are luminous on the real one) as a soft green glow. Skin, dark parts,
    the base and the clear disc stay out."""
    import colorsys
    import numpy as np
    from scipy import ndimage
    a = np.asarray(badge_im.convert("RGBA")).astype(float) / 255
    al = a[..., 3]
    mx, mn = a[..., :3].max(-1), a[..., :3].min(-1)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    hue = np.vectorize(lambda R, G, B: colorsys.rgb_to_hsv(R, G, B)[0])(a[..., 0], a[..., 1], a[..., 2])
    green = (abs(hue - 0.24) < 0.08) & (sat > 0.25) & (mx > 0.35)
    white = (sat < 0.2) & (mx > 0.6)
    lum = (green | white) & (al > 0.6)
    ys = np.nonzero(al > 0.5)[0]
    if len(ys):   # the base and its clear disc: the bottom of the figure
        top, bot = ys.min(), ys.max()
        lum[int(bot - (bot - top) * 0.17):, :] = False
    lum = ndimage.binary_opening(lum, iterations=1)
    if lum.sum() < 30:
        return None
    m = Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8))
    halo = np.asarray(m.filter(ImageFilter.GaussianBlur(size / 18))).astype(float) / 255
    body = np.asarray(m).astype(float) / 255
    alpha = np.clip(body * 0.78 + halo * 0.7, 0, 1)
    rgb = np.array(colour, float)[None, None, :] / 255 * np.ones_like(alpha)[..., None]
    return Image.fromarray((np.dstack([rgb, alpha[..., None]]) * 255).astype(np.uint8), "RGBA")


def foot(badge_im):
    """where a standing figure / play set piece touches the ground in its badge: centre x, bottom y
    and width of its base, as fractions of the badge (so the picker can stand it on a pad)"""
    import numpy as np
    al = np.asarray(badge_im.convert("RGBA").getchannel("A"))
    a = al > min(128, int(al.max()) // 2)   # clear crystal figures are only partly opaque
    rows = np.nonzero(a.sum(1) >= 3)[0]
    if not len(rows):
        return 0.5, 0.96, 0.6
    bot, top = rows.max(), rows.min()
    band = a[max(top, bot - max(3, int((bot - top) * 0.08))):bot + 1]
    cols = np.nonzero(band.any(0))[0]
    n = a.shape[1]
    return (cols.min() + cols.max() + 1) / 2 / n, (bot + 1) / a.shape[0], (cols.max() - cols.min() + 1) / n


def load_aliases():
    """{id: (display name or "", search words)} from tools/aliases.tsv"""
    out = {}
    p = ROOT / "tools" / "aliases.tsv"
    if p.exists():
        for ln in p.read_text(encoding="utf-8").splitlines():
            if not ln.strip() or ln.startswith("#"):
                continue
            v = ln.split("\t") + ["", ""]
            if v[0].strip().isdigit():
                out[int(v[0])] = (v[1].strip(), v[2].strip())
    return out


GROUP_RANK = {}   # id -> position in its groups file (character shelves keep that order)


def load_groups():
    """id -> section, and the section order (first appearance): character shelves from
    char_groups.tsv, power disc sections from disc_groups.tsv"""
    groups, order = {}, []
    for name in ("char_groups.tsv", "disc_groups.tsv"):
        p = ROOT / "tools" / name
        if not p.exists():
            continue
        for ln in p.read_text(encoding="utf-8").splitlines():
            ln = ln.split("#", 1)[0].rstrip() if not ln.startswith("#") else ""
            if not ln:
                continue
            i, g = [x.strip() for x in ln.split("\t")[:2]]
            groups[int(i)] = g
            GROUP_RANK[int(i)] = len(GROUP_RANK)
            if g not in order:
                order.append(g)
    return groups, order


def hexagon(cx, cy, r, rot=90):
    return [(cx + r * math.cos(math.radians(rot + 60 * k)), cy + r * math.sin(math.radians(rot + 60 * k))) for k in range(6)]


def wrap(draw, text, fnt, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=fnt) <= width or not cur:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def badge(item, size=160):
    """original generic badge: series colour, shape by kind, the name in the middle"""
    S = size * 2  # supersample
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    col = SERIES[item["series"]][1]
    dark = tuple(int(c * 0.22) for c in col) + (255,)
    mid = tuple(int(c * 0.55) for c in col) + (255,)
    c, r = S / 2, S / 2 - 6
    if item["kind"] in "PH":
        d.polygon(hexagon(c, c, r, 90), fill=col + (255,))
        d.polygon(hexagon(c, c, r - 14, 90), fill=dark)
        d.polygon(hexagon(c, c, r - 26, 90), outline=mid, width=4)
    else:
        d.ellipse([c - r, c - r, c + r, c + r], fill=col + (255,))
        d.ellipse([c - r + 14, c - r + 14, c + r - 14, c + r - 14], fill=dark)
        d.ellipse([c - r + 26, c - r + 26, c + r - 26, c + r - 26], outline=mid, width=4)
    name = item["name"]
    for fs, maxl in ((44, 3), (38, 3), (32, 3), (28, 3), (26, 4), (23, 4), (20, 5)):
        f = font(fs)
        lines = wrap(d, name, f, S * 0.66)
        if len(lines) <= maxl and all(d.textlength(l, font=f) <= S * 0.70 for l in lines):
            break
    lh = fs * 1.12
    y = c - lh * len(lines) / 2 - 10
    for l in lines[:maxl]:
        d.text((c, y + lh / 2), l, font=f, fill=(248, 250, 252, 255), anchor="mm")
        y += lh
    tag = KIND_LABEL[item["kind"]] + f"  {item['series']}.0"
    d.text((c, c + r * 0.62), tag, font=font(20), fill=col + (255,), anchor="mm")
    return im.resize((size, size), Image.LANCZOS)


# ---------------------------------------------------------------- real art

ART_NAME = re.compile(r"^p?(\d{7})(?:[ _\-.].*)?$", re.I)
ART_EXT = {".png", ".webp", ".jpg", ".jpeg"}


def shape_mask(kind, size):
    """the box a badge must fit: circle for characters / round discs, hexagon for play sets / hex discs"""
    S = size * 4
    m = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(m)
    if kind in "PH":
        d.polygon(hexagon(S / 2, S / 2, S / 2, 90), fill=255)
    else:
        d.ellipse([0, 0, S - 1, S - 1], fill=255)
    return m.resize((size, size), Image.LANCZOS)


def has_transparency(im):
    a = im.getchannel("A")
    lo, _ = a.getextrema()
    if lo == 255:
        return False
    corners = [a.getpixel(p) for p in ((0, 0), (im.width - 1, 0), (0, im.height - 1), (im.width - 1, im.height - 1))]
    return min(corners) < 250 or sum(1 for v in a.getdata() if v < 250) > im.width * im.height * 0.02


def normalize_art(src, kind, size=ART_SIZE):
    """any render -> size x size RGBA that fits its box. Returns (image, notes, errors).
      * transparent margins are trimmed, then the art is scaled to fit and centred, never stretched
      * characters keep 4% padding; discs fill their circle / hexagon
      * art without a transparent background: discs and hexagons are cut to their shape, a
        character is kept but flagged (it will show as a square on the base)"""
    notes, errs = [], []
    try:
        im = Image.open(src)
        fmt = im.format
        im.load()
    except Exception as e:
        return None, notes, [f"can't read it ({e})"]
    if fmt != "PNG":
        notes.append(f"{fmt} converted to PNG")
    if im.mode == "P" and "transparency" in im.info:
        im = im.convert("RGBA")
    im = im.convert("RGBA")
    if min(im.size) < 8:
        return None, notes, [f"too small ({im.width}x{im.height})"]
    transparent = has_transparency(im)
    if transparent:
        bbox = im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
        if not bbox:
            return None, notes, ["fully transparent"]
        if bbox != (0, 0, im.width, im.height):
            im = im.crop(bbox)
            notes.append(f"trimmed to {im.width}x{im.height}")
    else:
        if kind == "F":
            notes.append("NO TRANSPARENT BACKGROUND - shows as a square on the base")
        else:
            notes.append(f"no transparent background - cut to its {'hexagon' if kind in 'PH' else 'circle'}")
    if max(im.size) < ART_MIN:
        notes.append(f"small source ({im.width}x{im.height}), upscaled")
    if abs(im.width - im.height) > max(im.size) * 0.04:
        notes.append(f"not square ({im.width}x{im.height}), fitted without stretching")
    pad = round(size * 0.04) if kind == "F" else 0
    box = size - 2 * pad
    k = min(box / im.width, box / im.height)
    w, h = max(1, round(im.width * k)), max(1, round(im.height * k))
    art = im.resize((w, h), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.alpha_composite(art, ((size - w) // 2, (size - h) // 2))
    if not transparent and kind != "F":
        out.putalpha(ImageChops.multiply(out.getchannel("A"), shape_mask(kind, size)))
    return out, notes, errs


def import_art(figs, art_dir, out_dir=None):
    """match tools/art files to figures by id; returns ({id: image}, report lines, error count)"""
    by_id = {it["id"]: it for it in figs}
    found, report, nerr = {}, [], 0
    if not art_dir.is_dir():
        return found, report, 0
    files = [p for p in art_dir.iterdir() if p.is_file() and not p.name.startswith(".") and p.suffix.lower() != ".txt"]
    files.sort(key=lambda p: (p.stem.lower().lstrip("p")[:7], len(p.stem), p.name.lower()))   # 1000001.png before 1000001_x.png
    for p in files:
        m = ART_NAME.match(p.stem)
        if p.suffix.lower() not in ART_EXT:
            report.append(f"SKIP  {p.name}: not an image (PNG with a transparent background please)"); continue
        if not m:
            report.append(f"ERROR {p.name}: name must start with the 7-digit figure id, e.g. 1000001.png or 1000001_mr-incredible.png")
            nerr += 1; continue
        fid = int(m.group(1))
        if fid not in by_id:
            report.append(f"ERROR {p.name}: {fid} is not in tools/figures.tsv"); nerr += 1; continue
        if fid in found:
            report.append(f"ERROR {p.name}: a second file for {fid} ({by_id[fid]['name']}), only the first is used"); nerr += 1; continue
        it = by_id[fid]
        im, notes, errs = normalize_art(p, it["kind"])
        if errs:
            report.append(f"ERROR {p.name}: " + "; ".join(errs)); nerr += 1; continue
        found[fid] = im
        report.append(f"OK    {p.name} -> p{fid}.png  {it['name']} [{KIND_LABEL[it['kind']].lower()}]" + (f"  ({'; '.join(notes)})" if notes else ""))
    return found, report, nerr


def masks():
    S = 256
    g = Image.new("L", (S, S), 0)
    gd = ImageDraw.Draw(g)
    for k in range(64, 0, -1):   # soft radial glow
        a = int(255 * (1 - k / 64) ** 1.6)
        gd.ellipse([S / 2 - k * 2, S / 2 - k * 2, S / 2 + k * 2, S / 2 + k * 2], fill=a)
    g = g.filter(ImageFilter.GaussianBlur(6))
    Image.merge("RGBA", [Image.new("L", (S, S), 255)] * 3 + [g]).save(UI / "glow.png")

    def ring_png(name, poly):
        im = Image.new("L", (S * 2, S * 2), 0)
        dd = ImageDraw.Draw(im)
        poly(dd, S, S, S - 8)
        im = im.resize((S, S), Image.LANCZOS)
        Image.merge("RGBA", [Image.new("L", (S, S), 255)] * 3 + [im]).save(UI / f"{name}.png")

    ring_png("ring", lambda d, cx, cy, r: d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=22))
    ring_png("hexring", lambda d, cx, cy, r: d.polygon(hexagon(cx, cy, r, 90), outline=255, width=22))
    ring_png("hexfill", lambda d, cx, cy, r: d.polygon(hexagon(cx, cy, r, 90), fill=255))
    ring_png("circfill", lambda d, cx, cy, r: d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255))
    st = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    pts = [(64 + (58 if k % 2 == 0 else 24) * math.cos(math.radians(-90 + 36 * k)),
            64 + (58 if k % 2 == 0 else 24) * math.sin(math.radians(-90 + 36 * k))) for k in range(10)]
    sd = ImageDraw.Draw(st)
    sd.polygon(pts, fill=(250, 204, 21, 255), outline=(120, 80, 0, 255))
    st.resize((64, 64), Image.LANCZOS).save(UI / "star.png")
    fr = Image.new("L", (440, 440), 0)
    ImageDraw.Draw(fr).rounded_rectangle([6, 6, 433, 433], radius=28, outline=255, width=14)
    fr = fr.resize((220, 220), Image.LANCZOS)
    Image.merge("RGBA", [Image.new("L", (220, 220), 255)] * 3 + [fr]).save(UI / "frame.png")


def backgrounds():
    """bg1 Starfield, bg2 Aurora, bg3 Toy Box: pictures behind the picker (960x540, 16:9)"""
    import numpy as np
    W, H = 960, 540
    rng = np.random.default_rng(7)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    # Starfield: navy fade, two faint nebulae, stars
    t = yy / H
    img = np.stack([8 + 10 * t, 12 + 14 * t, 32 + 26 * t], -1)
    for cx, cy, r, col in ((250, 150, 260, (70, 40, 140)), (760, 400, 300, (20, 90, 150))):
        k = np.exp(-(((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * r * r)))[..., None]
        img = img + k * np.array(col, np.float32) * 0.55
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")
    glow = Image.new("L", (W, H), 0)
    sd, gd = ImageDraw.Draw(im), ImageDraw.Draw(glow)
    for _ in range(520):
        x, y = rng.uniform(0, W), rng.uniform(0, H)
        b = rng.uniform(0.35, 1.0) ** 2
        r = 0.6 + 1.6 * b
        v = int(150 + 105 * b)
        sd.ellipse([x - r, y - r, x + r, y + r], fill=(v, v, min(255, v + 20)))
        if b > 0.6:
            gd.ellipse([x - 6, y - 6, x + 6, y + 6], fill=int(120 * b))
    glow = glow.filter(ImageFilter.GaussianBlur(4))
    im = Image.composite(Image.new("RGB", (W, H), (190, 215, 255)), im, glow)
    im.save(UI / "bg1.png", optimize=True)

    # Aurora: dark sky with soft green / violet curtains
    img = np.stack([6 + 8 * t, 14 + 10 * t, 24 + 18 * t], -1)
    for amp, freq, ph, y0, col, w in ((40, 0.010, 0.0, 190, (40, 230, 160), 46), (55, 0.007, 2.1, 250, (120, 80, 240), 60),
                                      (30, 0.014, 4.0, 150, (40, 200, 230), 30)):
        centre = y0 + amp * np.sin(xx * freq + ph) + 18 * np.sin(xx * freq * 3.1 + ph * 2)
        d = (yy - centre) / w
        band = np.exp(-d * d) * (0.55 + 0.45 * np.sin(xx * 0.05 + ph) ** 2)
        rays = 0.75 + 0.25 * np.sin(xx * 0.21 + ph * 5)
        img = img + (band * rays)[..., None] * np.array(col, np.float32) * 0.8
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").filter(ImageFilter.GaussianBlur(1.2))
    sd = ImageDraw.Draw(im)
    for _ in range(160):
        x, y = rng.uniform(0, W), rng.uniform(0, H * 0.7)
        v = int(rng.uniform(120, 230))
        sd.point((x, y), fill=(v, v, v))
    im.save(UI / "bg2.png", optimize=True)

    # Toy Box: bright blue with a hexagon pattern, like the Infinity menus
    img = np.stack([20 + 40 * (1 - t), 70 + 60 * (1 - t), 150 + 70 * (1 - t)], -1)
    k = np.exp(-(((xx - W * 0.5) ** 2) / (2 * 380 ** 2) + ((yy - H * 0.35) ** 2) / (2 * 260 ** 2)))[..., None]
    img = img + k * np.array((60, 80, 70), np.float32)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")
    lines = Image.new("L", (W * 2, H * 2), 0)
    ld = ImageDraw.Draw(lines)
    R = 64
    dx, dy = R * math.sqrt(3), R * 1.5
    row = 0
    y = -R
    while y < H * 2 + R:
        x = -R + (dx / 2 if row % 2 else 0)
        while x < W * 2 + R:
            ld.polygon(hexagon(x, y, R - 4, 90), outline=255, width=5)
            x += dx
        y += dy
        row += 1
    lines = lines.resize((W, H), Image.LANCZOS)
    fade = Image.fromarray((np.clip(0.25 + 0.75 * t, 0, 1) * 95).astype(np.uint8), "L")
    lines = Image.fromarray((np.asarray(lines, np.float32) * np.asarray(fade, np.float32) / 255).astype(np.uint8), "L")
    im = Image.composite(Image.new("RGB", (W, H), (200, 230, 255)), im, lines)
    im.save(UI / "bg3.png", optimize=True)


def main():
    figs = load_figures()
    groups, gorder = load_groups()
    lfx = load_lightfx()
    aliases = load_aliases()
    for it in figs:   # shown names and extra search words
        name, aka = aliases.get(it["id"], ("", ""))
        if name:
            it["name"] = name
        it["aka"] = aka
    for it in figs:
        it["group"] = groups.get(it["id"], "Other") if it["kind"] in "FRH" else ""
    gorder = gorder + (["Other"] if any(it["group"] == "Other" for it in figs) else [])
    gidx = {g: i for i, g in enumerate(gorder)}
    # catalog order = page order: by kind, then disc section, then id (the TV picker lists in this order)
    figs.sort(key=lambda it: (KIND_ORDER.index(it["kind"]), gidx.get(it["group"], -1),
                              GROUP_RANK.get(it["id"], 0) if it["kind"] == "F" else it["id"]))
    art_dir = ROOT / "tools" / "art"
    art, report, nerr = import_art(figs, art_dir)
    glows = {}
    for fid, (col, kind) in lfx.items():   # Light FX / glow figures with real art: their glow layer
        if fid in art:
            g = (phosphor_glow if kind == "glow" else saber_glow)(art[fid], col)
            if g:
                glows[fid] = g
            else:
                print(f"  light FX {fid}: no lightsaber found in its art, so no glow (check its colour in tools/lightfx.tsv)")
    if "--check-art" in sys.argv:
        print("\n".join(report) or f"no art in {art_dir}")
        print(f"{len(art)} usable, {nerr} errors")
        return 1 if nerr else 0
    if OUT.exists():
        shutil.rmtree(OUT)
    UI.mkdir(parents=True)
    (OUT / "tags").mkdir()
    (OUT / "tags" / "README.txt").write_text(
        "Figure files (320-byte .bin, any name) live here. Dumps of your own figures and figure files\n"
        "from Dolphin / RPCS3 / shadPS4 work as-is. New figures are created as <id>.bin the first time\n"
        "you place them, and the game's progress (levels, skill trees) is saved back into them.\n")
    with open(UI / "catalog.tsv", "w", encoding="utf-8") as f:
        f.write("# id\tseries\tkind\tname\tsection\tfx\tfoot (centre x, bottom y, base width of a standing figure in its badge)\taka\n")
        for it in figs:
            fx = lfx[it["id"]][1] if it["id"] in glows else ""
            it["fx"] = fx
            ft = ""
            if it["kind"] in "FP":
                it["foot"] = [round(v, 3) for v in foot(art.get(it["id"]) or badge(it))]
                ft = ",".join(f"{v:.3f}" for v in it["foot"])
            f.write(f"{it['id']}\t{it['series']}\t{it['kind']}\t{it['name']}\t{it['group']}\t{fx}\t{ft}\t{it['aka']}\n")
    roster = {"editions": [], "sections": {k: [g for g in gorder if any(it["kind"] == k and it["group"] == g for it in figs)] for k in "FRH"}}
    for s, (title, col) in SERIES.items():
        ed = {"id": f"di{s}", "title": title, "series": s, "color": "#%02x%02x%02x" % col, "categories": []}
        for k, ct in CATEGORIES:
            items = [dict({"id": it["id"], "name": it["name"]}, **({"group": it["group"]} if it["group"] else {}),
                          **({"fx": it["fx"]} if it.get("fx") else {}), **({"foot": it["foot"]} if it.get("foot") else {}),
                          **({"aka": it["aka"]} if it.get("aka") else {}))
                     for it in figs if it["series"] == s and it["kind"] == k]
            if items:
                ed["categories"].append({"kind": k, "title": ct, "items": items})
        roster["editions"].append(ed)
    (WEB / "roster.json").write_text(json.dumps(roster, indent=1, ensure_ascii=False), encoding="utf-8")
    for it in figs:
        (art.get(it["id"]) or badge(it)).save(UI / f"p{it['id']}.png", optimize=True)
        if it["id"] in glows:
            glows[it["id"]].save(UI / f"g{it['id']}.png", optimize=True)
    if report:
        (OUT / "art_report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
        print(f"art: {len(art)} of {len(figs)} badges from tools/art, {nerr} errors (data/dimod/art_report.txt)")
        for ln in report:
            if not ln.startswith("OK") or "NO TRANSPARENT" in ln:
                print("  " + ln)
    import base3d   # the base on a table, its light masks and where things stand on it
    base3d.build(ROOT / "assets" / "infinity_base.png", UI)
    import display  # the display shelf and the disc album the picker shows items in
    display.build(UI)
    # a plaque per character shelf / disc section: the franchise logo from tools/plaques, else its name
    if not (ROOT / "tools" / "plaques" / "boosts.png").exists():   # the disc sections' own logos
        import section_logos
        for n in section_logos.SECTIONS:
            section_logos.logo(n).save(ROOT / "tools" / "plaques" / f"{section_logos.slug(n)}.png", optimize=True)
    roster["plaques"] = display.plaques(UI, gorder, ROOT / "tools" / "plaques", font)
    (WEB / "roster.json").write_text(json.dumps(roster, indent=1, ensure_ascii=False), encoding="utf-8")
    base = Image.open(ROOT / "assets" / "infinity_base.png").convert("RGBA")
    if base.size != (500, 552):
        base = base.resize((500, 552), Image.LANCZOS)
    base.save(UI / "base.png", optimize=True)
    masks()
    backgrounds()
    (OUT / "themes").mkdir()
    (OUT / "themes" / "README.txt").write_text(
        "Your own picker background: background.png here (PNG, 16:9, up to 1280x720 is best).\n"
        "Easiest is the phone remote: Settings > Background > Choose photo. It resizes and uploads it.\n")
    build = os.environ.get("DIMOD_BUILD", "dev")
    page = (ROOT / "src" / "web" / "index.html").read_text(encoding="utf-8")
    (WEB / "index.html").write_text(page.replace("__DIMOD_BUILD__", build).replace("__DIMOD_VERSION__", os.environ.get("DIMOD_VERSION", "0.6")), encoding="utf-8")
    (WEB / "build.txt").write_text(build + "\n")   # the payload compares this with its own build
    n = sum(1 for _ in UI.iterdir())
    print(f"data/dimod: {len(figs)} figures, {n} files in web/ui")


if __name__ == "__main__":
    sys.exit(main())
