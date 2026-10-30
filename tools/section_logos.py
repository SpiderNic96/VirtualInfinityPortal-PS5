#!/usr/bin/env python3
"""Logos for the power disc sections (they have no official ones): an emblem shaped like the discs in
that section - round with a red rim and the star tab for Power-Ups, hexagonal with an orange rim for
Power Discs - with a picture of the category, and the section's name beside it.

  python3 tools/section_logos.py [out_dir]     (default tools/plaques)"""
import math, pathlib, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

S = 4                 # supersampling
E = 400               # emblem size (px, final)
FONT = ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/arialbd.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]


def font(px):
    for p in FONT:
        try:
            return ImageFont.truetype(p, px)
        except OSError:
            pass
    return ImageFont.load_default()


def hexpts(cx, cy, r, rot=90):
    return [(cx + r * math.cos(math.radians(rot + 60 * k)), cy + r * math.sin(math.radians(rot + 60 * k))) for k in range(6)]


def radial(size, inner, outer, cx=0.42, cy=0.36):
    """a radial gradient: inner colour at (cx, cy), outer at the edge"""
    w = h = size
    g = Image.radial_gradient("L").resize((w * 2, h * 2))
    g = g.crop((int(w * (1 - cx)), int(h * (1 - cy)), int(w * (1 - cx)) + w, int(h * (1 - cy)) + h))
    g = g.point(lambda v: min(255, int(v * 1.25)))
    return Image.composite(Image.new("RGB", (w, h), outer), Image.new("RGB", (w, h), inner), g)


def shape_mask(size, hexagonal, r):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    c = size / 2
    if hexagonal:
        d.polygon(hexpts(c, c, r), fill=255)
    else:
        d.ellipse([c - r, c - r, c + r, c + r], fill=255)
    return m


class Pen:
    """draw in icon units: (0, 0) the centre, 1 = the icon radius"""
    def __init__(self, im, cx, cy, r):
        self.im, self.d, self.cx, self.cy, self.r = im, ImageDraw.Draw(im), cx, cy, r

    def P(self, pts, rot=0, dx=0, dy=0, k=1.0):
        a = math.radians(rot)
        out = []
        for x, y in pts:
            x, y = x * k, y * k
            x, y = x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)
            out.append((self.cx + (x + dx) * self.r, self.cy + (y + dy) * self.r))
        return out

    def poly(self, pts, fill, **kw):
        self.d.polygon(self.P(pts, **kw), fill=fill)

    def circle(self, x, y, rr, fill=None, outline=None, width=0):
        X, Y = self.cx + x * self.r, self.cy + y * self.r
        R = rr * self.r
        self.d.ellipse([X - R, Y - R, X + R, Y + R], fill=fill, outline=outline, width=int(width * self.r))

    def line(self, pts, fill, width, **kw):
        self.d.line(self.P(pts, **kw), fill=fill, width=int(width * self.r), joint="curve")


W = (255, 255, 255, 255)


# ---------------------------------------------------------------- the pictures

def boosts(p):   # three rising chevrons and a plus: stats going up
    ch = [(-0.55, 0.18), (0, -0.3), (0.55, 0.18), (0.55, 0.42), (0, -0.06), (-0.55, 0.42)]
    for k, (dy, a) in enumerate(((0.42, 150), (0.02, 205), (-0.38, 255))):
        p.poly(ch, (255, 255, 255, a), dx=-0.08, dy=dy, k=0.92)
    p.poly([(-0.07, -0.3), (0.07, -0.3), (0.07, -0.07), (0.3, -0.07), (0.3, 0.07), (0.07, 0.07), (0.07, 0.3),
            (-0.07, 0.3), (-0.07, 0.07), (-0.3, 0.07), (-0.3, -0.07), (-0.07, -0.07)], (255, 236, 120, 255), dx=0.55, dy=-0.55, k=0.75)


def special_attacks(p):   # an energy blast: starburst with a lightning bolt
    pts = []
    for k in range(24):
        rr = 0.86 if k % 2 == 0 else 0.48
        a = math.radians(-90 + 15 * k)
        pts.append((rr * math.cos(a), rr * math.sin(a)))
    p.poly(pts, (255, 214, 64, 255))
    p.poly([(x * 0.66, y * 0.66) for x, y in pts], (255, 246, 196, 255))
    p.poly([(0.16, -0.66), (-0.34, 0.08), (-0.02, 0.08), (-0.16, 0.66), (0.36, -0.12), (0.04, -0.12)], (214, 52, 24, 255))


def team_ups(p):   # two heroes side by side, and a target for the strike
    for (x, y, k, col) in ((-0.3, -0.02, 0.85, (255, 255, 255, 170)), (0.2, 0.12, 1.0, W)):
        p.circle(x, y - 0.34 * k, 0.22 * k, fill=col)
        p.d.chord([p.cx + (x - 0.42 * k) * p.r, p.cy + (y - 0.05 * k) * p.r, p.cx + (x + 0.42 * k) * p.r, p.cy + (y + 0.75 * k) * p.r],
                  180, 360, fill=col)
    tx, ty = 0.5, -0.5
    p.circle(tx, ty, 0.24, outline=(255, 236, 120, 255), width=0.07)
    p.circle(tx, ty, 0.06, fill=(255, 236, 120, 255))
    for a in range(4):
        dx, dy = math.cos(a * math.pi / 2), math.sin(a * math.pi / 2)
        p.line([(tx + dx * 0.16, ty + dy * 0.16), (tx + dx * 0.36, ty + dy * 0.36)], (255, 236, 120, 255), 0.06)


def costumes(p):   # a hero suit on a hanger
    p.d.arc([p.cx - 0.13 * p.r, p.cy - 0.92 * p.r, p.cx + 0.13 * p.r, p.cy - 0.66 * p.r], 180, 90, fill=W, width=int(0.06 * p.r))
    p.line([(0, -0.66), (0, -0.56)], W, 0.06)
    p.line([(-0.62, -0.28), (0, -0.56), (0.62, -0.28)], W, 0.07)
    shirt = [(-0.66, -0.2), (-0.24, -0.42), (-0.1, -0.32), (0.1, -0.32), (0.24, -0.42), (0.66, -0.2),
             (0.5, 0.1), (0.34, 0.02), (0.34, 0.66), (-0.34, 0.66), (-0.34, 0.02), (-0.5, 0.1)]
    p.poly(shirt, W)
    p.poly([(-0.1, -0.32), (0, -0.2), (0.1, -0.32)], (210, 200, 230, 255))
    star = []
    for k in range(10):
        rr = 0.2 if k % 2 == 0 else 0.085
        a = math.radians(-90 + 36 * k)
        star.append((rr * math.cos(a), 0.14 + rr * math.sin(a)))
    p.poly(star, (150, 70, 200, 255))
    p.line([(-0.34, 0.44), (0.34, 0.44)], (150, 70, 200, 255), 0.06)


def vehicles(p):   # a sports car with speed lines
    for y, x0 in ((-0.2, -0.98), (0.0, -1.0), (0.2, -0.94)):
        p.line([(x0, y), (x0 + 0.26, y)], (255, 255, 255, 150), 0.05)
    body = [(-0.7, 0.24), (-0.72, 0.0), (-0.5, -0.08), (-0.28, -0.38), (0.24, -0.38), (0.5, -0.08), (0.82, 0.0), (0.86, 0.24)]
    p.poly(body, W)
    p.poly([(-0.22, -0.3), (-0.06, -0.3), (-0.06, -0.08), (-0.4, -0.08)], (40, 120, 130, 255))
    p.poly([(0.02, -0.3), (0.2, -0.3), (0.38, -0.08), (0.02, -0.08)], (40, 120, 130, 255))
    p.poly([(0.7, -0.01), (0.84, 0.02), (0.84, 0.1), (0.72, 0.08)], (255, 220, 110, 255))
    for x in (-0.4, 0.5):
        p.circle(x, 0.26, 0.22, fill=(30, 34, 40, 255))
        p.circle(x, 0.26, 0.12, fill=(200, 206, 214, 255))
        p.circle(x, 0.26, 0.04, fill=(30, 34, 40, 255))


def mounts(p):   # a horse's head with its bridle
    head = [(-0.08, -0.86), (0.06, -0.6), (0.3, -0.48), (0.56, -0.18), (0.8, 0.12), (0.8, 0.26), (0.66, 0.34),
            (0.46, 0.26), (0.26, 0.2), (0.1, 0.34), (0.02, 0.6), (-0.04, 0.86), (-0.6, 0.86), (-0.5, 0.36),
            (-0.4, -0.1), (-0.26, -0.48), (-0.16, -0.62)]
    p.poly(head, W)
    p.poly([(-0.16, -0.62), (-0.26, -0.48), (-0.4, -0.1), (-0.5, 0.36), (-0.6, 0.86), (-0.74, 0.86), (-0.7, 0.62),
            (-0.78, 0.48), (-0.64, 0.3), (-0.7, 0.12), (-0.56, -0.06), (-0.6, -0.26), (-0.42, -0.42), (-0.4, -0.62),
            (-0.22, -0.72)], (120, 70, 36, 255))
    p.poly([(-0.02, -0.82), (0.04, -0.62), (-0.1, -0.64)], (220, 210, 200, 255))
    p.circle(0.26, -0.26, 0.06, fill=(40, 30, 24, 255))
    p.circle(0.7, 0.12, 0.035, fill=(120, 110, 100, 255))
    br = (120, 70, 36, 255)
    p.line([(0.12, -0.42), (0.5, 0.04), (0.62, 0.24)], br, 0.05)
    p.line([(0.5, 0.04), (0.2, 0.12)], br, 0.05)
    p.circle(0.5, 0.06, 0.06, fill=(220, 190, 90, 255))


def gadgets(p):   # a sword crossed with a wrench
    def sword(rot):
        p.poly([(-0.13, 0.36), (-0.13, -0.78), (0, -1.02), (0.13, -0.78), (0.13, 0.36)], (236, 240, 246, 255), rot=rot)
        p.poly([(-0.025, 0.34), (-0.025, -0.82), (0.025, -0.82), (0.025, 0.34)], (176, 184, 198, 255), rot=rot)
        p.poly([(-0.4, 0.34), (0.4, 0.34), (0.4, 0.5), (-0.4, 0.5)], (236, 192, 70, 255), rot=rot)
        p.poly([(-0.09, 0.5), (0.09, 0.5), (0.09, 0.86), (-0.09, 0.86)], (130, 80, 40, 255), rot=rot)
        p.circle(*rot_pt(0, 0.92, rot), 0.11, fill=(236, 192, 70, 255))
    def wrench(rot):
        p.poly([(-0.13, -0.5), (0.13, -0.5), (0.13, 0.84), (-0.13, 0.84)], W, rot=rot)
        jaw = [(0.36 * math.cos(math.radians(a)), -0.66 + 0.36 * math.sin(math.radians(a))) for a in range(0, 360, 10)]
        p.poly(jaw, W, rot=rot)
        p.poly([(-0.13, -1.06), (0.13, -1.06), (0.13, -0.62), (-0.13, -0.62)], (90, 100, 120, 255), rot=rot)
        p.circle(*rot_pt(0, 0.72, rot), 0.06, fill=(90, 100, 120, 255))
    def rot_pt(x, y, rot):
        a = math.radians(rot)
        return x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)
    wrench(-42)
    sword(42)


def terrains(p):   # mountains, hills and a tree
    p.poly([(-1, 0.5), (-0.52, -0.42), (-0.32, -0.12), (-0.02, -0.66), (0.5, 0.2), (1, 0.5)], (150, 160, 186, 255))
    p.poly([(-0.02, -0.66), (-0.14, -0.44), (-0.06, -0.48), (0.02, -0.4), (0.1, -0.48), (0.14, -0.44)], W)
    p.poly([(-0.52, -0.42), (-0.62, -0.24), (-0.52, -0.28), (-0.44, -0.26)], W)
    p.d.chord([p.cx - 1.3 * p.r, p.cy + 0.04 * p.r, p.cx + 0.5 * p.r, p.cy + 1.1 * p.r], 180, 360, fill=(110, 186, 80, 255))
    p.d.chord([p.cx - 0.2 * p.r, p.cy + 0.12 * p.r, p.cx + 1.4 * p.r, p.cy + 1.2 * p.r], 180, 360, fill=(80, 156, 64, 255))
    p.poly([(-1, 0.6), (1, 0.6), (1, 1), (-1, 1)], (126, 92, 56, 255))
    p.poly([(0.42, 0.0), (0.46, 0.0), (0.46, 0.22), (0.42, 0.22)], (100, 66, 36, 255))
    p.poly([(0.44, -0.36), (0.6, 0.06), (0.28, 0.06)], (46, 120, 56, 255))


def skies(p):   # the sun behind a cloud, a star and a bird
    sx, sy = 0.28, -0.24
    for k in range(12):
        a = math.radians(30 * k + 15)
        p.line([(sx + 0.4 * math.cos(a), sy + 0.4 * math.sin(a)), (sx + 0.58 * math.cos(a), sy + 0.58 * math.sin(a))], (255, 214, 64, 255), 0.07)
    p.circle(sx, sy, 0.32, fill=(255, 200, 40, 255))
    p.circle(sx - 0.06, sy - 0.07, 0.2, fill=(255, 232, 120, 255))
    for (x, y, rr) in ((-0.42, 0.24, 0.26), (-0.1, 0.1, 0.34), (0.26, 0.26, 0.26), (0.5, 0.36, 0.18), (-0.66, 0.38, 0.17)):
        p.circle(x, y, rr, fill=W)
    p.poly([(-0.82, 0.38), (0.66, 0.38), (0.66, 0.54), (-0.82, 0.54)], W)
    star = []
    for k in range(8):
        rr = 0.16 if k % 2 == 0 else 0.05
        a = math.radians(-90 + 45 * k)
        star.append((-0.56 + rr * math.cos(a), -0.52 + rr * math.sin(a)))
    p.poly(star, (255, 246, 196, 255))
    p.line([(-0.26, -0.32), (-0.18, -0.38), (-0.1, -0.32), (-0.02, -0.38), (0.06, -0.32)], (255, 255, 255, 220), 0.04)


SECTIONS = {
    # name: (drawing, hexagonal, centre colour, edge colour)
    "Boosts": (boosts, False, (96, 200, 96), (28, 110, 44)),
    "Special Attacks": (special_attacks, False, (240, 110, 60), (150, 30, 20)),
    "Team-Ups & Strikes": (team_ups, False, (80, 150, 240), (24, 62, 150)),
    "Costumes": (costumes, False, (190, 120, 236), (96, 40, 150)),
    "Vehicles": (vehicles, True, (70, 196, 206), (20, 96, 116)),
    "Mounts": (mounts, True, (232, 182, 110), (150, 92, 40)),
    "Gadgets & Weapons": (gadgets, True, (130, 146, 176), (52, 62, 84)),
    "Terrains": (terrains, True, (150, 210, 240), (70, 140, 200)),
    "Skies": (skies, True, (110, 180, 250), (36, 96, 190)),
}


def emblem(draw_fn, hexagonal, inner, outer):
    Z = E * S
    c = Z / 2
    out = Image.new("RGBA", (Z, Z), (0, 0, 0, 0))
    R = Z * 0.46
    # drop shadow
    sh = shape_mask(Z, hexagonal, R).filter(ImageFilter.GaussianBlur(Z * 0.02))
    shadow = Image.new("RGBA", (Z, Z), (0, 0, 0, 0)); shadow.putalpha(ImageChops.offset(sh, 0, int(Z * 0.015)).point(lambda v: v // 2))
    out.alpha_composite(shadow)
    # metal rim, coloured rim, the picture's field
    metal = radial(Z, (246, 248, 252), (130, 138, 152), 0.35, 0.3).convert("RGBA")
    metal.putalpha(shape_mask(Z, hexagonal, R))
    out.alpha_composite(metal)
    rim = Image.new("RGBA", (Z, Z), (214, 40, 46, 255) if not hexagonal else (240, 130, 30, 255))
    rim.putalpha(shape_mask(Z, hexagonal, R * 0.86))
    out.alpha_composite(rim)
    field = radial(Z, inner, outer).convert("RGBA")
    fm = shape_mask(Z, hexagonal, R * 0.77)
    field.putalpha(fm)
    out.alpha_composite(field)
    # the picture, with a soft shadow, clipped to the field
    pic = Image.new("RGBA", (Z, Z), (0, 0, 0, 0))
    draw_fn(Pen(pic, c, c, R * 0.6))
    ps = Image.new("RGBA", (Z, Z), (0, 0, 0, 0)); ps.putalpha(ImageChops.offset(pic.getchannel("A"), int(Z * 0.006), int(Z * 0.01)).filter(ImageFilter.GaussianBlur(Z * 0.006)).point(lambda v: int(v * 0.35)))
    ps.putalpha(ImageChops.multiply(ps.getchannel("A"), fm))
    pic.putalpha(ImageChops.multiply(pic.getchannel("A"), fm))
    out.alpha_composite(ps)
    out.alpha_composite(pic)
    # gloss over the top half of the field
    gl = Image.new("L", (Z, Z), 0)
    ImageDraw.Draw(gl).ellipse([c - R * 0.9, c - R * 1.15, c + R * 0.9, c - R * 0.05], fill=70)
    gl = ImageChops.multiply(gl.filter(ImageFilter.GaussianBlur(Z * 0.03)), fm)
    g2 = Image.new("RGBA", (Z, Z), (255, 255, 255, 0)); g2.putalpha(gl)
    out.alpha_composite(g2)
    d = ImageDraw.Draw(out)
    if not hexagonal:   # the round discs' red tab with a star at the bottom
        tw, th = R * 0.3, R * 0.2
        d.rounded_rectangle([c - tw / 2, c + R * 0.68, c + tw / 2, c + R * 0.68 + th], radius=R * 0.03, fill=(196, 30, 38, 255))
        star = []
        for k in range(10):
            rr = th * 0.36 if k % 2 == 0 else th * 0.15
            a = math.radians(-90 + 36 * k)
            star.append((c + rr * math.cos(a), c + R * 0.68 + th / 2 + rr * math.sin(a)))
        d.polygon(star, fill=(255, 255, 255, 255))
    return out.resize((E, E), Image.LANCZOS)


def logo(name):
    fn, hexagonal, inner, outer = SECTIONS[name]
    em = emblem(fn, hexagonal, inner, outer)
    words = name.replace(" & ", " &\n", 1).split("\n") if "&" in name else name.split(" ") if len(name) > 10 else [name]
    size = 120
    f = font(size)
    tmp = ImageDraw.Draw(Image.new("L", (1, 1)))
    while max(tmp.textlength(w, font=f) for w in words) > 560 and size > 40:
        size -= 4; f = font(size)
    tw = int(max(tmp.textlength(w, font=f) for w in words))
    lh = int(size * 1.08)
    Wd, H = E + 40 + tw + 10, max(E, lh * len(words) + 20)
    out = Image.new("RGBA", (Wd, H), (0, 0, 0, 0))
    out.alpha_composite(em, (0, (H - E) // 2))
    d = ImageDraw.Draw(out)
    y = (H - lh * len(words)) // 2
    col = outer
    for w in words:
        d.text((E + 40 + 3, y + 4), w, font=f, fill=(0, 0, 0, 60))
        d.text((E + 40, y), w, font=f, fill=(34, 36, 44, 255))
        y += lh
    d.rounded_rectangle([E + 40, y + 4, E + 40 + min(tw, 260), y + 16], radius=6, fill=col + (255,))
    return out


def slug(s):
    import re
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")


if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent / "plaques"
    out.mkdir(parents=True, exist_ok=True)
    for n in SECTIONS:
        logo(n).save(out / f"{slug(n)}.png", optimize=True)
        print("made", slug(n))
