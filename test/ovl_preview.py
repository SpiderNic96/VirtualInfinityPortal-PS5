#!/usr/bin/env python3
"""PC stand-in for the SceShellUI renderer: speaks ovl_proto.h on 127.0.0.1:39261 and composites
the retained widgets with Pillow. Touch <snapdir>/<name>.req to get <snapdir>/<name>.png."""
import os, select, socket, struct, sys, io, time
from PIL import Image, ImageDraw, ImageFont

PORT, MAGIC = 39261, 0x4F4D4944
CAPS = 1 | 4 | 8 | 16 | 32   # pixels, filter, panel, trim, scene
snapdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ovl"
os.makedirs(snapdir, exist_ok=True)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
fonts = {}
def font(px):
    if px not in fonts: fonts[px] = ImageFont.truetype(FONT, max(6, px))
    return fonts[px]

tex, types, W = {}, [], []
def new_w(): return dict(x=0, y=0, w=0, h=0, vis=0, alpha=1.0, c=(255, 255, 255, 255), tex=0, text="", font=20, align=0, lines=1)

def paste(img, src, x, y):
    """composite src at (x, y), clipped to the frame (widgets may hang off any edge, as on the console)"""
    sx, sy = max(0, -x), max(0, -y)
    if sx >= src.width or sy >= src.height or x >= img.width or y >= img.height: return
    img.alpha_composite(src.crop((sx, sy, src.width, src.height)), (x + sx, y + sy))


def render(path):
    img = Image.new("RGBA", (1920, 1080), (40, 70, 40, 255))   # "game" backdrop
    d = ImageDraw.Draw(img)
    d.text((40, 40), "(game frame)", font=font(40), fill=(90, 130, 90, 255))
    for i, w in enumerate(W):
        if not w["vis"] or w["alpha"] <= 0: continue
        t = types[i]
        x, y, ww, hh = int(w["x"]), int(w["y"]), max(1, int(w["w"])), max(1, int(w["h"]))
        if t == 2:   # panel
            r, g, b, a = w["c"]
            layer = Image.new("RGBA", (ww, hh), (r, g, b, int(a * w["alpha"])))
            paste(img, layer, x, y)
        elif t == 1 and w["tex"] in tex:
            src = tex[w["tex"]].resize((ww, hh), Image.LANCZOS)
            r, g, b, a = w["c"]
            if (r, g, b) != (255, 255, 255):
                px = src.split()
                src = Image.merge("RGBA", [px[0].point(lambda v: v * r // 255), px[1].point(lambda v: v * g // 255),
                                           px[2].point(lambda v: v * b // 255), px[3]])
            if w["alpha"] < 1:
                al = src.split()[3].point(lambda v: int(v * w["alpha"]))
                src.putalpha(al)
            paste(img, src, x, y)
        elif t == 3 and w["text"]:
            f = font(w["font"])
            txt = w["text"]
            while w["w"] > 0 and d.textlength(txt, font=f) > w["w"] and len(txt) > 4: txt = txt[:-4] + "..."
            tw = d.textlength(txt, font=f)
            tx = x if w["align"] == 0 else x + (w["w"] - tw) / 2 if w["align"] == 1 else x + w["w"] - tw
            r, g, b, a = w["c"]
            d.text((tx, y), txt, font=f, fill=(r, g, b, int(a * w["alpha"])))
    img.convert("RGB").save(path)

def recvn(s, n):
    b = b""
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k: raise ConnectionError
        b += k
    return b

def handle(s):
    global types, W
    h = recvn(s, 12)
    magic, typ, ver, ln = struct.unpack("<IHHI", h)
    body = recvn(s, ln) if ln else b""
    if typ == 2:
        i, w, hh = struct.unpack_from("<IHH", body)
        tex[i] = Image.frombytes("RGBA", (w, hh), body[8:8 + w * hh * 4])
    elif typ == 3:
        i, w, hh = struct.unpack_from("<IHH", body)
        tex[i] = Image.open(io.BytesIO(body[8:])).convert("RGBA")
    elif typ == 4:
        tex.pop(struct.unpack_from("<I", body)[0], None)
    elif typ == 5:
        n = struct.unpack_from("<H", body)[0]
        types = list(body[2:2 + n]); W = [new_w() for _ in range(n)]
    elif typ == 6:
        n = struct.unpack_from("<H", body)[0]; o = 2
        for _ in range(n):
            wid, mask = struct.unpack_from("<HH", body, o); o += 4
            w = W[wid]
            if mask & 1: w["x"], w["y"], w["w"], w["h"] = struct.unpack_from("<4f", body, o); o += 16
            if mask & 2: w["vis"] = struct.unpack_from("<I", body, o)[0]; o += 4
            if mask & 4: w["alpha"] = struct.unpack_from("<f", body, o)[0]; o += 4
            if mask & 8: w["c"] = tuple(body[o:o + 4]); o += 4
            if mask & 16: w["tex"] = struct.unpack_from("<I", body, o)[0]; o += 4
            if mask & 32:
                w["font"], w["align"], w["lines"], L = struct.unpack_from("<HBBH", body, o); o += 6
                w["text"] = body[o:o + L].decode("utf-8", "replace"); o += L
                while L & 3: o += 1; L += 1

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT)); srv.listen(1)
print("preview renderer listening", flush=True)
while True:
    c, _ = srv.accept()
    c.sendall(struct.pack("<IHHI", MAGIC, 1, 1, 24) + struct.pack("<IffiII", CAPS, 1920.0, 1080.0, 4242, 0, 0))
    try:
        while True:
            r, _, _ = select.select([c], [], [], 0.05)
            if r: handle(c)
            for f in os.listdir(snapdir):
                if f.endswith(".req"):
                    render(os.path.join(snapdir, f[:-4] + ".png")); os.unlink(os.path.join(snapdir, f))
    except ConnectionError:
        print("client gone", flush=True)
