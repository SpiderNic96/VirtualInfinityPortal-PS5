#!/usr/bin/env python3
"""Pictures for the picker's display furniture (made by build_data.py into web/ui):

  shelf_back.png    the back panel of a white display shelf (behind the character / play set grid)
  shelf_plank.png   one shelf board seen from the front: its top surface, front edge and the shadow
                    it throws on the back panel; stretched across a row, figures stand on its top
  shelf_sign.png    a small plaque for a shelf's name
  album_page.png    a page of a power disc collector's album (behind the disc grid)
  album_slot_r.png  a round cut-out in the page with its finger notch, where a disc sits
  album_slot_h.png  the same for hexagonal discs
Geometry the picker relies on (fractions): the plank's top surface is its top 22 %, the front edge
22..80 %, the shadow below; a slot's opening fills the middle 86 % of its picture."""
import math, pathlib
from PIL import Image, ImageChops, ImageDraw, ImageFilter

SS = 2


def vgrad(w, h, top, bot):
    g = Image.linear_gradient("L").resize((w, h))
    a, b = Image.new("RGB", (w, h), top), Image.new("RGB", (w, h), bot)
    return Image.composite(b, a, g)


def shelf_back(w=1040, h=620):
    im = vgrad(w, h, (238, 236, 232), (214, 211, 206))
    # soft light from the top-left and a vignette, like a lit display case
    lt = Image.new("L", (w, h), 0)
    ImageDraw.Draw(lt).ellipse([-w * 0.3, -h * 0.6, w * 0.7, h * 0.7], fill=60)
    lt = lt.filter(ImageFilter.GaussianBlur(w * 0.12))
    im = Image.composite(Image.new("RGB", (w, h), (255, 255, 252)), im, lt)
    vg = Image.new("L", (w, h), 0)
    ImageDraw.Draw(vg).rectangle([0, 0, w, h], outline=255, width=int(w * 0.03))
    vg = vg.filter(ImageFilter.GaussianBlur(w * 0.03)).point(lambda v: int(v * 0.35))
    im = Image.composite(Image.new("RGB", (w, h), (150, 146, 140)), im, vg)
    return im.convert("RGBA")


def shelf_plank(w=1040, h=48):
    W, H = w * SS, h * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    top_h, front_h = int(H * 0.22), int(H * 0.58)
    im.paste(vgrad(W, top_h, (250, 249, 246), (232, 230, 226)).convert("RGBA"), (0, 0))
    im.paste(vgrad(W, front_h, (255, 255, 254), (226, 223, 218)).convert("RGBA"), (0, top_h))
    d = ImageDraw.Draw(im)
    d.line([0, top_h, W, top_h], fill=(255, 255, 255, 255), width=SS * 2)          # the front edge catches the light
    d.line([0, top_h + front_h - SS, W, top_h + front_h - SS], fill=(190, 186, 180, 255), width=SS)
    sh = Image.new("L", (W, H - top_h - front_h), 0)
    sh_g = Image.linear_gradient("L").rotate(180).resize(sh.size).point(lambda v: int(v * 0.55))
    shadow = Image.new("RGBA", sh.size, (60, 55, 50, 0)); shadow.putalpha(sh_g)
    im.alpha_composite(shadow, (0, top_h + front_h))
    return im.resize((w, h), Image.LANCZOS)


def shelf_sign(w=360, h=96):
    W, H = w * SS, h * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, SS * 4, W - 1, H - 1], radius=H * 0.18, fill=(40, 34, 30, 120))
    body = vgrad(W - SS * 6, H - SS * 8, (64, 52, 44), (38, 30, 26)).convert("RGBA")
    m = Image.new("L", body.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, body.width - 1, body.height - 1], radius=H * 0.16, fill=255)
    body.putalpha(m)
    im.alpha_composite(body, (SS * 3, 0))
    d.rounded_rectangle([SS * 3 + SS * 4, SS * 4, W - SS * 3 - SS * 4, H - SS * 12], radius=H * 0.12,
                        outline=(196, 160, 96, 200), width=SS * 2)
    return im.resize((w, h), Image.LANCZOS)


def album_page(w=1040, h=620, band=True):
    im = vgrad(w, h, (236, 238, 242), (205, 209, 216))
    sw = Image.new("L", (w * 2, h * 2), 0)
    d = ImageDraw.Draw(sw)
    for k, (off, wid) in enumerate(((0.38, 34), (0.47, 10), (0.55, 22), (0.6, 6))):
        pts = [(x, h * 2 * (off + 0.1 * math.sin(x / (w * 2) * math.pi * 1.6 + k))) for x in range(0, w * 2 + 20, 20)]
        d.line(pts, fill=255, width=wid)
    sw = sw.filter(ImageFilter.GaussianBlur(6)).resize((w, h), Image.LANCZOS).point(lambda v: int(v * 0.55))
    im = Image.composite(Image.new("RGB", (w, h), (255, 255, 255)), im, sw)
    if not band:
        return im.convert("RGBA")
    # the album's red and black band along the bottom
    band = int(h * 0.034)   # thin, so it stays under the bottom row's names
    d = ImageDraw.Draw(im)
    pts = [(x, h - band + 3 * math.sin(x / w * math.pi * 2.2)) for x in range(0, w + 10, 10)]
    d.polygon([(0, h)] + pts + [(w, h)], fill=(196, 28, 36))
    pts2 = [(x, y + band * 0.42) for x, y in pts]
    d.polygon([(0, h)] + pts2 + [(w, h)], fill=(22, 22, 24))
    return im.convert("RGBA")


def album_slot(hexagonal, size=200):
    """the cut-out where a disc sits: the page is cut away (you see the dark backing card), with an
    inner shadow along the top, a light edge along the bottom and a round finger notch on top"""
    S = size * SS
    c, r = S / 2, S * 0.43
    hole = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(hole)
    if hexagonal:
        d.polygon([(c + r * math.cos(math.radians(90 + 60 * k)), c + r * math.sin(math.radians(90 + 60 * k))) for k in range(6)], fill=255)
    else:
        d.ellipse([c - r, c - r, c + r, c + r], fill=255)
    nr = S * 0.11   # the finger notch
    d.ellipse([c - nr, c - r - nr * 0.9, c + nr, c - r + nr * 1.1], fill=255)
    backing = vgrad(S, S, (150, 152, 158), (182, 184, 190)).convert("RGBA")
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(backing, (0, 0), hole)
    # inner shadow from the page's top edge
    sh = ImageChops.subtract(hole, ImageChops.offset(hole, 0, int(S * 0.04))).filter(ImageFilter.GaussianBlur(S * 0.02))
    shadow = Image.new("RGBA", (S, S), (40, 42, 48, 0)); shadow.putalpha(ImageChops.multiply(sh, hole).point(lambda v: min(255, v * 2)))
    out.alpha_composite(shadow)
    # a light edge where the cut catches the light at the bottom
    lip = ImageChops.subtract(hole, ImageChops.offset(hole, 0, -int(S * 0.012)))
    lipc = Image.new("RGBA", (S, S), (255, 255, 255, 0)); lipc.putalpha(lip.filter(ImageFilter.GaussianBlur(SS)))
    out.alpha_composite(lipc)
    return out.resize((size, size), Image.LANCZOS)


def shelf_row(w=1040, h=168):
    """one shelf row for the phone: transparent, with the board along the bottom (repeats down)"""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    im.alpha_composite(shelf_plank(w, 30), (0, h - 30))
    return im


def glass_door(w=520, h=640):
    """one glass door of the display case (the left one; the picker mirrors it for the right):
    a white frame, faintly tinted glass with reflections, a small handle by the inner edge"""
    W, H = w * SS, h * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    f = int(W * 0.035)
    glass = Image.new("RGBA", (W - 2 * f, H - 2 * f), (214, 232, 244, 70))
    ref = Image.new("L", glass.size, 0)
    d = ImageDraw.Draw(ref)
    gw, gh = glass.size
    for x0, wd, a in ((0.05, 0.16, 110), (0.3, 0.05, 80), (0.62, 0.1, 60)):   # diagonal reflections
        x = gw * x0
        d.polygon([(x, gh), (x + gw * wd, gh), (x + gw * wd + gh * 0.55, 0), (x + gh * 0.55, 0)], fill=a)
    ref = ref.filter(ImageFilter.GaussianBlur(SS * 6))
    white = Image.new("RGBA", glass.size, (255, 255, 255, 0)); white.putalpha(ref)
    glass.alpha_composite(white)
    im.alpha_composite(glass, (f, f))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W - 1, H - 1], outline=(246, 246, 243, 255), width=f)
    d.rectangle([f, f, W - f - 1, H - f - 1], outline=(200, 200, 196, 255), width=SS)
    d.line([SS, SS, W - SS, SS], fill=(255, 255, 255, 255), width=SS)
    hx, hy, hh = W - f - SS * 18, H * 0.42, H * 0.16   # the handle, by the inner edge
    d.rounded_rectangle([hx, hy, hx + SS * 8, hy + hh], radius=SS * 4, fill=(176, 178, 182, 255), outline=(120, 122, 128, 255), width=SS)
    return im.resize((w, h), Image.LANCZOS)


def page_shade(w=64, h=256):
    """the shadow along the fold of a turning album page: dark at the fold, fading out"""
    g = Image.linear_gradient("L").rotate(90, expand=True).resize((w, h)).point(lambda v: int((v / 255) ** 1.8 * 170))
    im = Image.new("RGBA", (w, h), (20, 22, 28, 0)); im.putalpha(g)
    return im


PLQ_W, PLQ_H = 360, 232   # every section plaque: the same size, the same plate


def plaque_name(section):
    """web/ui file of a section's plaque: plq_<FNV-1a of the name>.png (the payload computes the same)"""
    h = 2166136261
    for c in section.encode("utf-8"):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return "plq_%08x" % (h or 1)


def slug(section):
    import re
    return re.sub(r"[^a-z0-9]+", "-", section.lower()).strip("-")


def plaque(section, logo_dir, font):
    """the section's plaque: its logo (logo_dir/<slug>.png) on a standard plate - a white acrylic one,
    or a dark one for a light logo - else its name on the white plate"""
    import numpy as np
    W, H = PLQ_W * SS, PLQ_H * SS
    logo_p = pathlib.Path(logo_dir) / f"{slug(section)}.png"
    logo = Image.open(logo_p).convert("RGBA") if logo_p.exists() else None
    dark = False
    if logo is not None:
        a = np.asarray(logo).astype(float)
        w = a[..., 3] / 255
        lum = (a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11)
        dark = (lum * w).sum() / max(w.sum(), 1) > 172   # a light logo would vanish on the white plate
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pad, r = SS * 8, SS * 22
    shadow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shadow).rounded_rectangle([pad, pad + SS * 5, W - pad, H - pad + SS * 3], radius=r, fill=120)
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0)); sh.putalpha(shadow.filter(ImageFilter.GaussianBlur(SS * 5)))
    out.alpha_composite(sh)
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).rounded_rectangle([pad, pad, W - pad, H - pad - SS * 4], radius=r, fill=255)
    face = vgrad(W, H, (52, 50, 56) if dark else (255, 255, 253), (26, 25, 30) if dark else (232, 230, 225)).convert("RGBA")
    face.putalpha(m)
    out.alpha_composite(face)
    d = ImageDraw.Draw(out)
    d.rounded_rectangle([pad, pad, W - pad, H - pad - SS * 4], radius=r, outline=(90, 86, 96, 255) if dark else (196, 192, 186, 255), width=SS * 2)
    d.rounded_rectangle([pad + SS * 3, pad + SS * 3, W - pad - SS * 3, H - pad - SS * 7], radius=r - SS * 3,
                        outline=(255, 255, 255, 40 if dark else 200), width=SS)
    bx, by, bw, bh = pad + SS * 16, pad + SS * 14, W - 2 * pad - SS * 32, H - 2 * pad - SS * 32
    if logo is not None:
        k = min(bw / logo.width, bh / logo.height)
        lg = logo.resize((max(1, round(logo.width * k)), max(1, round(logo.height * k))), Image.LANCZOS)
        out.alpha_composite(lg, (int(bx + (bw - lg.width) / 2), int(by + (bh - lg.height) / 2)))
    else:   # the name, as big as fits in up to three lines
        words, best = section.split(), None
        for size in range(SS * 46, SS * 18, -SS * 2):
            f = font(size)
            lines, cur = [], ""
            for wd in words:
                t = (cur + " " + wd).strip()
                if d.textlength(t, font=f) <= bw or not cur: cur = t
                else: lines.append(cur); cur = wd
            lines.append(cur)
            if len(lines) <= 3 and all(d.textlength(l, font=f) <= bw for l in lines) and len(lines) * size * 1.15 <= bh:
                best = (f, size, lines); break
        f, size, lines = best or (font(SS * 18), SS * 18, [section])
        y = by + (bh - len(lines) * size * 1.15) / 2
        for l in lines:
            d.text((W / 2, y + size * 0.575), l, font=f, fill=(40, 40, 46, 255), anchor="mm")
            y += size * 1.15
    return out.resize((PLQ_W, PLQ_H), Image.LANCZOS)


def plaques(ui, sections, logo_dir, font):
    """a plaque for every section; returns {section: file name}"""
    out = {}
    for sct in sections:
        n = plaque_name(sct)
        plaque(sct, logo_dir, font).save(pathlib.Path(ui) / f"{n}.png", optimize=True)
        out[sct] = n + ".png"
    return out


def build(ui):
    ui = pathlib.Path(ui)
    door = glass_door()
    door.save(ui / "glass_door_l.png", optimize=True)
    door.transpose(Image.FLIP_LEFT_RIGHT).save(ui / "glass_door_r.png", optimize=True)
    sh = page_shade()   # dark on its right edge as made: flip it so page_shade is dark on the left
    sh.transpose(Image.FLIP_LEFT_RIGHT).save(ui / "page_shade.png", optimize=True)
    sh.save(ui / "page_shade_r.png", optimize=True)
    shelf_row().save(ui / "shelf_row.png", optimize=True)
    shelf_back().save(ui / "shelf_back.png", optimize=True)
    shelf_plank().save(ui / "shelf_plank.png", optimize=True)
    shelf_sign().save(ui / "shelf_sign.png", optimize=True)
    album_page().save(ui / "album_page.png", optimize=True)
    album_slot(False).save(ui / "album_slot_r.png", optimize=True)
    album_slot(True).save(ui / "album_slot_h.png", optimize=True)


if __name__ == "__main__":
    import sys
    build(sys.argv[1] if len(sys.argv) > 1 else ".")
