#!/usr/bin/env python3
"""The base as it looks on a table: assets/infinity_base.png (seen from above) tilted back into a
three-quarter view, with its side wall and a soft shadow, plus the light masks that line up with
it exactly and the geometry the TV picker and the phone use to stand figures on it.

Writes into web/ui:
  base3d.png                    the tilted base
  l3d_ring<p>.png  l3d_spill<p>.png   white masks per pad (p: 0 hexagon, 1 left, 2 right): the
                                rim that lights up, and the light thrown on the white plastic
  l3d_up.png                    light coming up into a figure's clear base (any pad)
  base3d.txt                    geometry, all in fractions of the picture's width:
      aspect <h/w>
      pad <p> <cx> <cy> <rx> <ry>          the pad / hexagon surface (an ellipse for the pads)
      ring <p> <x> <y> <w> <h>             where l3d_ring<p>.png goes
      spill <p> <x> <y> <w> <h>            where l3d_spill<p>.png goes
      layer <px>                           how much one disc lifts what is on top of it
      plate <p> <x> <y> <w> <h>            where l3d_plate<p>.png (that pad cut out of the base with
                                           soft edges, for the sneak peek) comes from
Same numbers in base3d.json for the phone."""
import json, math
from PIL import Image, ImageDraw, ImageFilter, ImageChops

SRC_W, SRC_H = 500, 552
OUT_W = 1000
# measured on assets/infinity_base.png: pads (centre, radii) and the hexagon's corners
PADS_SRC = {1: (138, 368, 83, 78), 2: (370, 361, 83, 79)}
HEX_SRC = [(269, 104), (328, 135), (329, 232), (234, 268), (175, 238), (173, 141)]
# the tilt: where the picture's corners go (top edge pulled in and the whole thing flattened)
DST_H = 560
DST = [(150, 0), (850, 0), (1000, DST_H), (0, DST_H)]
WALL = 30          # side wall height (px at OUT_W)
PAD_TOP = 40       # room above for the hanging cable, shadow below
SS = 3             # supersampling for the masks


def solve_homography(src, dst):
    """3x3 h with dst ~ h * src (8 unknowns, 4 points)"""
    A, b = [], []
    for (x, y), (u, v) in zip(src, dst):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); b.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); b.append(v)
    n = 8
    M = [row[:] + [bb] for row, bb in zip(A, b)]
    for c in range(n):
        piv = max(range(c, n), key=lambda r: abs(M[r][c]))
        M[c], M[piv] = M[piv], M[c]
        for r in range(n):
            if r != c:
                f = M[r][c] / M[c][c]
                M[r] = [a - f * bb for a, bb in zip(M[r], M[c])]
    h = [M[i][n] / M[i][i] for i in range(n)] + [1.0]
    return h


SRC_CORNERS = [(0, 0), (SRC_W, 0), (SRC_W, SRC_H), (0, SRC_H)]
H_FWD = solve_homography(SRC_CORNERS, DST)          # source -> tilted
H_INV = solve_homography(DST, SRC_CORNERS)          # tilted -> source (what PIL.transform wants)


def fwd(x, y, dy=PAD_TOP):
    h = H_FWD
    w = h[6] * x + h[7] * y + h[8]
    return (h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w + dy


def warp(src_img, scale=1):
    """tilt an image drawn in source coordinates (optionally supersampled by scale)"""
    out_h = DST_H + PAD_TOP + WALL + 60
    s = scale
    inv = H_INV
    # PIL maps output (x, y) -> input via (a x + b y + c) / (g x + h y + 1) ...; account for scale and top pad
    # output pixel (X, Y) in the scaled canvas = s * (x, y + PAD_TOP) -> input = s * H_INV(x, y)
    a, b, c, d, e, f, g, h_, i = inv
    # x = X/s, y = Y/s - PAD_TOP
    coeffs = [a, b, s * (c - b * PAD_TOP), d, e, s * (f - e * PAD_TOP), g / s, h_ / s, i - h_ * PAD_TOP]
    coeffs = [v / coeffs[8] for v in coeffs[:8]]
    return src_img.transform((OUT_W * s, out_h * s), Image.PERSPECTIVE, coeffs, Image.BICUBIC)


def build(base_png, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    src = Image.open(base_png).convert("RGBA")
    if src.size != (SRC_W, SRC_H):
        src = src.resize((SRC_W, SRC_H), Image.LANCZOS)
    top = warp(src.resize((SRC_W * SS, SRC_H * SS), Image.LANCZOS), SS).resize((OUT_W, DST_H + PAD_TOP + WALL + 60), Image.LANCZOS)
    W, Ht = top.size
    a = top.getchannel("A")
    # the side wall: the outline pushed down, shaded darker towards the bottom
    wall = Image.new("RGBA", (W, Ht), (0, 0, 0, 0))
    for k in range(WALL, 0, -1):
        shade = int(205 - 70 * k / WALL)
        layer = Image.new("RGBA", (W, Ht), (shade, shade, shade + 4, 255))
        m = ImageChops.offset(a, 0, k)
        wall.paste(layer, (0, 0), m)
    # a soft contact shadow under it all
    sh = ImageChops.offset(a, 0, WALL + 10).filter(ImageFilter.GaussianBlur(14)).point(lambda v: int(v * 0.45))
    shadow = Image.new("RGBA", (W, Ht), (0, 0, 0, 0)); shadow.putalpha(sh)
    out = Image.new("RGBA", (W, Ht), (0, 0, 0, 0))
    out.alpha_composite(shadow); out.alpha_composite(wall); out.alpha_composite(top)
    bb = out.getchannel("A").point(lambda v: 255 if v > 6 else 0).getbbox()
    out = out.crop((0, 0, W, bb[3]))
    W, Ht = out.size
    out.save(out_dir / "base3d.png", optimize=True)

    geo = {"aspect": Ht / W, "pads": {}, "ring": {}, "spill": {}, "layer": 0}

    def mask_png(name, draw_fn, blur_src=0):
        m = Image.new("L", (SRC_W * SS, SRC_H * SS), 0)
        draw_fn(ImageDraw.Draw(m), SS)
        if blur_src:
            m = m.filter(ImageFilter.GaussianBlur(blur_src * SS))
        rgba = Image.merge("RGBA", [Image.new("L", m.size, 255)] * 3 + [m])
        w = warp(rgba, SS).resize((OUT_W, DST_H + PAD_TOP + WALL + 60), Image.LANCZOS).crop((0, 0, W, Ht))
        box = w.getchannel("A").point(lambda v: 255 if v > 3 else 0).getbbox()
        piece = w.crop(box)
        piece.save(out_dir / f"{name}.png", optimize=True)
        return [box[0] / W, box[1] / W, (box[2] - box[0]) / W, (box[3] - box[1]) / W]

    def ellipse_pts(cx, cy, rx, ry, n=72):
        return [fwd(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]

    for p, (cx, cy, rx, ry) in PADS_SRC.items():
        pts = ellipse_pts(cx, cy, rx, ry)
        xs, ys = [q[0] for q in pts], [q[1] for q in pts]
        mcx, mcy = fwd(cx, cy)
        geo["pads"][p] = [mcx / W, mcy / W, (max(xs) - min(xs)) / 2 / W, (max(ys) - min(ys)) / 2 / W]
        geo["ring"][p] = mask_png(f"l3d_ring{p}", lambda d, s, cx=cx, cy=cy, rx=rx, ry=ry: d.ellipse(
            [(cx - rx) * s, (cy - ry) * s, (cx + rx) * s, (cy + ry) * s], outline=255, width=int(7 * s)), blur_src=2.2)
        geo["spill"][p] = mask_png(f"l3d_spill{p}", lambda d, s, cx=cx, cy=cy, rx=rx, ry=ry: d.ellipse(
            [(cx - rx * 1.25) * s, (cy - ry * 1.25) * s, (cx + rx * 1.25) * s, (cy + ry * 1.25) * s], fill=200), blur_src=22)
    hx = sum(x for x, _ in HEX_SRC) / 6; hy = sum(y for _, y in HEX_SRC) / 6
    pts = [fwd(x, y) for x, y in HEX_SRC]
    xs, ys = [q[0] for q in pts], [q[1] for q in pts]
    mcx, mcy = fwd(hx, hy)
    geo["pads"][0] = [mcx / W, mcy / W, (max(xs) - min(xs)) / 2 / W, (max(ys) - min(ys)) / 2 / W]
    geo["ring"][0] = mask_png("l3d_ring0", lambda d, s: d.polygon([(x * s, y * s) for x, y in HEX_SRC], outline=255, width=int(7 * s)), blur_src=2.2)
    big = [(hx + (x - hx) * 1.3, hy + (y - hy) * 1.3) for x, y in HEX_SRC]
    geo["spill"][0] = mask_png("l3d_spill0", lambda d, s: d.polygon([(x * s, y * s) for x, y in big], fill=200), blur_src=20)
    geo["layer"] = geo["pads"][1][3] * 0.16   # a disc is about a sixth of the pad's depth thick, seen like this

    # the sneak peek shows each pad on its own: cut it out of the tilted base with a soft edge
    geo["plate"] = {}
    for p in (0, 1, 2):
        sx, sy, sw, shh = geo["spill"][p]
        box = [int(sx * W), int(sy * W), int((sx + sw) * W), int((sy + shh) * W)]
        crop = out.crop(box)
        cw, ch = crop.size
        pcx, pcy, prx, pry = [q * W for q in geo["pads"][p]]
        ex, ey = pcx - box[0], pcy - box[1]
        kx, ky = (1.16, 1.18) if p else (1.2, 1.32)   # just past the rim (the logo stays out)
        v = Image.new("L", (cw * 2, ch * 2), 0)
        ImageDraw.Draw(v).ellipse([(ex - prx * kx) * 2, (ey - pry * ky) * 2, (ex + prx * kx) * 2, (ey + pry * ky) * 2], fill=255)
        v = v.filter(ImageFilter.GaussianBlur(prx * 0.09)).resize((cw, ch), Image.LANCZOS)
        crop.putalpha(ImageChops.multiply(crop.getchannel("A"), v))
        crop.save(out_dir / f"l3d_plate{p}.png", optimize=True)
        geo["plate"][p] = [box[0] / W, box[1] / W, cw / W, ch / W]

    # light coming up into a clear figure base: a soft, upward-fading glow (any pad, any size)
    S = 256
    up = Image.new("L", (S, S), 0)
    ImageDraw.Draw(up).ellipse([S * 0.08, S * 0.55, S * 0.92, S * 0.98], fill=255)
    up = up.filter(ImageFilter.GaussianBlur(S * 0.05))
    grad = Image.linear_gradient("L").resize((S, S))
    beam = ImageChops.multiply(Image.new("L", (S, S), 0).point(lambda v: 0), grad)
    col = Image.new("L", (S, S), 0)
    ImageDraw.Draw(col).rectangle([S * 0.22, 0, S * 0.78, S * 0.8], fill=255)
    col = ImageChops.multiply(col.filter(ImageFilter.GaussianBlur(S * 0.09)), grad.point(lambda v: int(v * 0.55)))
    up = ImageChops.lighter(up, col)
    Image.merge("RGBA", [Image.new("L", (S, S), 255)] * 3 + [up]).save(out_dir / "l3d_up.png", optimize=True)

    # a soft contact shadow for anything standing on a pad (tinted dark by the picker)
    sh = Image.new("L", (S, S // 2), 0)
    ImageDraw.Draw(sh).ellipse([S * 0.1, S * 0.08, S * 0.9, S * 0.42], fill=255)
    sh = sh.filter(ImageFilter.GaussianBlur(S * 0.06))
    Image.merge("RGBA", [Image.new("L", sh.size, 255)] * 3 + [sh]).save(out_dir / "l3d_shadow.png", optimize=True)

    with open(out_dir / "base3d.txt", "w") as f:
        f.write("# made by tools/base3d.py: the tilted base and where things go on it (fractions of its width)\n")
        f.write(f"aspect {geo['aspect']:.5f}\n")
        for p in (0, 1, 2):
            f.write("pad %d %.5f %.5f %.5f %.5f\n" % ((p,) + tuple(geo["pads"][p])))
            f.write("ring %d %.5f %.5f %.5f %.5f\n" % ((p,) + tuple(geo["ring"][p])))
            f.write("spill %d %.5f %.5f %.5f %.5f\n" % ((p,) + tuple(geo["spill"][p])))
        f.write(f"layer {geo['layer']:.5f}\n")
        for p in (0, 1, 2):
            f.write("plate %d %.5f %.5f %.5f %.5f\n" % ((p,) + tuple(geo["plate"][p])))
    (out_dir / "base3d.json").write_text(json.dumps(geo, indent=1))
    return geo


if __name__ == "__main__":
    import pathlib, sys
    root = pathlib.Path(__file__).resolve().parent.parent
    g = build(root / "assets" / "infinity_base.png", pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "data" / "dimod" / "web" / "ui")
    print(json.dumps(g, indent=1))
