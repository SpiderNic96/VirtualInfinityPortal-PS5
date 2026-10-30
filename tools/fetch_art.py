#!/usr/bin/env python3
"""Fetch figure / disc / play set renders from the Disney Infinity wikis into tools/art.

Runs anywhere Python 3.8+ runs, Termux included. Put it in the project's tools/ folder and it works
on tools/art; run it on its own (e.g. ~/fetch_art.py in Termux) and it uses its built-in figure
list and saves everything to a dimod_art/ folder next to itself (or --out DIR). Copy
dimod_art/art/* into the project's tools/art afterwards. Standard library only (Pillow is used if it is
installed, for a better transparency check). For your own use: the renders belong to Disney, so
keep tools/art out of anything you publish (it is in .gitignore).

How a match is made
  1. The wiki is searched for the figure's model number, "INF-<id>" (the infobox shows it), and a
     page only counts if its text really contains that number. Name matching alone is unsafe:
     "Mr. Incredible" also finds the Costume, Statue and Sidekick pages.
  2. No page with the number: the page named like the figure is used, marked "check".
  3. On the page: the infobox picture first, then the page image, then the other files, scored
     (PNG, "render" / "Character-" style names, the figure's name) with logos, icons, screenshots
     and packaging pushed down. The original full-size file is downloaded.

Every decision is written to tools/art_sources.tsv, one line per figure:
    id  name  status  wiki  page  file  note
  status  auto     found by model number, has a transparent background
          check    worth a look: matched by name only, or no transparent background
          missing  nothing found
          manual   YOU set it: put the right page and/or file in, the script uses exactly that
          skip     YOU set it: never fetch this one
Then open tools/art_review.html (every match on one page, on a checkerboard so transparency
shows) and fix what's wrong in art_sources.tsv. Running again only fetches what is missing.

  python3 tools/fetch_art.py --contact you@example.com              everything
  python3 tools/fetch_art.py --kinds RH                             only power discs
  python3 tools/fetch_art.py --ids 1000001,3000003 --force          redo these two
  python3 tools/fetch_art.py --dry-run --limit 20                   only find matches
  python3 tools/fetch_art.py --sheet                                only rebuild the review page
Then build as usual: python3 tools/build_data.py (it fits every file to its 160x160 box)."""
import argparse, base64, difflib, zlib, html, json, os, pathlib, re, struct, sys, time, urllib.error, urllib.parse, urllib.request

HERE = pathlib.Path(__file__).resolve().parent
FIGS = next((p for p in (HERE / "figures.tsv", HERE / "tools" / "figures.tsv") if p.exists()), None)


def set_out(base):
    """where the art, the sources list and the review page go"""
    global ART, SOURCES, SHEET
    ART, SOURCES, SHEET = base / "art", base / "art_sources.tsv", base / "art_review.html"


# inside the project (tools/fetch_art.py or ./fetch_art.py): tools/art; on its own: ./dimod_art
set_out(FIGS.parent if FIGS else HERE / "dimod_art")
WIKIS = "disneyinfinity-archive,disneyinfinity"
KINDS = {1: "F", 2: "P", 3: "R", 4: "H"}
COLS = ["id", "name", "status", "wiki", "page", "file", "note"]
IMG_EXT = {"image/png": "png", "image/webp": "webp", "image/jpeg": "jpg", "image/gif": "gif"}
BAD = re.compile(r"disambig|stub|placeholder|logo|icon|screen(shot)?|\bss\b|box.?art|package|packag|blister|card|concept|gameplay|banner|menu|"
                 r"wordmark|title|trailer|poster|promo|avatar|thumb|button|map|wallpaper|sprite|spark|loading", re.I)
# the wiki names its renders "<Type>-<Franchise>-<Name>": Character-Incredibles-Mr. Incredible.png,
# Ability-Bolt-Bolt's Super Strength.png ...
PATTERN = re.compile(r"^[A-Za-z]+-[A-Za-z0-9&'.]+-")
GOOD = re.compile(r"^[A-Za-z]+-[A-Za-z0-9&'.]+-|render|^character|^ability|^costume|^team.?up|^customi[sz]|^skydome|^sky|^terrain|^texture|^weapon|^tool|figure|^disc|power.?disc|^playset|play.?set|^toy|^vehicle|^mount|^gadget", re.I)


def log(*a):
    print(*a, flush=True)


# ---------------------------------------------------------------- wiki API

class Wiki:
    def __init__(self, name, ua, delay):
        self.name = name
        self.api = name if "://" in name else f"https://{name}.fandom.com/api.php"
        self.ua, self.delay, self.last = ua, delay, 0.0

    def _open(self, url, tries=5, headers=None):
        for k in range(tries):
            wait = self.last + self.delay - time.time()
            if wait > 0:
                time.sleep(wait)
            self.last = time.time()
            try:
                h = {"User-Agent": self.ua, "Accept-Encoding": "identity"}
                h.update(headers or {})
                req = urllib.request.Request(url, headers=h)
                with urllib.request.urlopen(req, timeout=40) as r:
                    return r.read(), r.headers.get("Content-Type", "")
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    return None, ""
                if e.code not in (429, 500, 502, 503, 504) or k == tries - 1:
                    raise
                pause = int(e.headers.get("Retry-After") or 0) or 5 * (k + 1)
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                if k == tries - 1:
                    raise
                pause = 5 * (k + 1)
            log(f"    (wiki busy, waiting {pause}s)")
            time.sleep(pause)
        return None, ""

    def site(self):
        return self.api[: -len("api.php")] if self.api.endswith("api.php") else self.api.rsplit("/", 1)[0] + "/"

    def download(self, url, file, page):
        """the image file's bytes, or (None, why). The image server (static.wikia.nocookie.net) is
        asked like a browser showing the wiki page would ask it: with the page as Referer and an
        image Accept header. If it still refuses, the other addresses Fandom serves the same file at
        are tried: without the cache-buster, a large rendition, and the wiki's Special:FilePath."""
        ref = self.site() + "wiki/" + urllib.parse.quote((page or "File:" + file).replace(" ", "_"))
        hdr = {"Referer": ref, "Accept": "image/png,image/*;q=0.8,*/*;q=0.5"}   # PNG keeps the transparency; asking for WebP gets WebP
        base = url.split("?")[0]
        variants = [url, base]
        if "/revision/latest" in base:
            variants.append(base.split("/revision/latest")[0] + "/revision/latest/scale-to-width-down/1200")
        variants.append(self.site() + "wiki/Special:FilePath/" + urllib.parse.quote(file.replace(" ", "_")))
        whys = []
        for v in dict.fromkeys(variants):
            try:
                data, ctype = self._open(v, tries=3, headers=hdr)
            except urllib.error.HTTPError as e:
                whys.append(f"HTTP {e.code} from {urllib.parse.urlsplit(v).netloc}")
                continue
            except Exception as e:
                whys.append(f"{type(e).__name__}: {e}")
                continue
            if data and (data[:8] == b"\x89PNG\r\n\x1a\n" or data[:3] == b"\xff\xd8\xff" or data[8:12] == b"WEBP" or data[:4] == b"GIF8"):
                return data, ""
            whys.append(f"not an image ({ctype or 'empty'}) from {urllib.parse.urlsplit(v).netloc}")
        return None, "; ".join(dict.fromkeys(whys))

    def get(self, **p):
        p.update(format="json", formatversion="2")
        body, _ = self._open(self.api + "?" + urllib.parse.urlencode(p))
        return json.loads(body) if body else {}

    def wikitext(self, title):
        j = self.get(action="parse", page=title, prop="wikitext", redirects=1)
        if "parse" not in j:
            return None, None
        return j["parse"]["title"], j["parse"].get("wikitext", "")

    def model_index(self):
        """{id: page} from the wiki's own "Model Numbers" list (each row: INF-<id> and the item's link)"""
        if hasattr(self, "_models"):
            return self._models
        self._models = {}
        for page in ("Model Numbers", "Model numbers", "List of model numbers"):
            title, text = self.wikitext(page)
            if not title:
                continue
            for row in re.split(r"\n\|-|\n(?=[*#])|\n\n", text):
                nums = set(re.findall(r"INF\s*[-_ ]?\s*(\d{7})", row, re.I))
                links = [l.strip() for l in re.findall(r"\[\[([^\]|#]+)", row) if ":" not in l]
                if len(nums) == 1 and links:
                    self._models.setdefault(int(nums.pop()), links[0])
            if self._models:
                log(f"    ({len(self._models)} items in {self.name}'s {title} list)")
                break
        return self._models

    def search(self, q, n=10, ns=0):
        j = self.get(action="query", list="search", srsearch=q, srlimit=n, srnamespace=ns)
        return [h["title"] for h in j.get("query", {}).get("search", [])]

    def page_files(self, title):
        j = self.get(action="parse", page=title, prop="images", redirects=1)
        return j.get("parse", {}).get("images", [])

    def page_image(self, title):
        j = self.get(action="query", titles=title, prop="pageimages", piprop="name", redirects=1)
        for pg in j.get("query", {}).get("pages", []):
            if pg.get("pageimage"):
                return pg["pageimage"]
        return None

    def file_info(self, files):
        """{file name: (url, width, height, mime)} for up to 20 files"""
        if not files:
            return {}
        j = self.get(action="query", titles="|".join("File:" + f for f in files[:20]), prop="imageinfo", iiprop="url|size|mime")
        out = {}
        for pg in j.get("query", {}).get("pages", []):
            ii = (pg.get("imageinfo") or [None])[0]
            if ii and ii.get("url"):
                out[norm_file(pg["title"])] = (ii["url"], ii.get("width", 0), ii.get("height", 0), ii.get("mime", ""))
        return out


def norm_file(f):
    f = re.sub(r"^(file|image)\s*:", "", f.strip(), flags=re.I).strip()
    f = f.replace("_", " ")
    return f[:1].upper() + f[1:]


def has_model(text, fid):
    """2 = the infobox says this is the item (| model = INF-<id>), 1 = the page only mentions it, 0 = no"""
    num = rf"INF\s*[-_ ]?\s*0*{fid}(?!\d)"
    if re.search(rf"\|\s*[\w ]*\s*=\s*(?:\[\[[^\]|]*\|)?\s*{num}", text or "", re.I):
        return 2
    return 1 if re.search(num, text or "", re.I) else 0


# ---------------------------------------------------------------- matching

def is_list(text):
    """a list of many items (the Model Numbers page...), not one item's page"""
    return len(set(re.findall(r"INF\s*[-_ ]?\s*(\d{7})", text or "", re.I))) > 3


def find_page(wiki, fid, name):
    """(title, wikitext, verified)"""
    listed = wiki.model_index().get(fid)
    if listed:   # the wiki's own model number list names the page
        title, text = wiki.wikitext(listed)
        if title and not is_disambig(text) and not is_list(text):
            return title, text, True
    tried, cands = set(), []
    for t in wiki.search(f'"INF-{fid}"') + wiki.search(f"INF-{fid}"):
        if t not in cands:
            cands.append(t)
    # the closest title to the figure's name first; costume / statue / sidekick pages last
    extra = re.compile(r"costume|statue|sidekick|outfit|skill|achievement|gallery|list of|mobile", re.I)
    def rank(t):
        r = difflib.SequenceMatcher(None, t.lower(), name.lower()).ratio()
        r -= 0.5 if extra.search(t) and not extra.search(name) else 0
        r += 0.3 if re.search(r"\((figure|character|power disc|disc|play set|toy box)\)", t, re.I) else 0
        return -r
    cands.sort(key=rank)
    weak = None
    for t in [name] + cands[:6]:
        if t in tried:
            continue
        tried.add(t)
        title, text = wiki.wikitext(t)
        if not title or is_list(text):
            continue
        m = has_model(text, fid)
        if m == 2:
            return title, text, True
        if m == 1 and not weak:
            weak = (title, text)
    if weak:   # only mentions the number: probably right, but have a look
        return weak[0], weak[1], False
    title, text = wiki.wikitext(name)
    if title and is_disambig(text):   # "Lone Ranger may refer to:" - follow the right link
        links = []
        for l in re.findall(r"\[\[([^\]|#]+)", text):
            l = l.strip()
            if l and ":" not in l and l not in links and l != title:
                links.append(l)
        def lrank(t):
            r = difflib.SequenceMatcher(None, t.lower(), name.lower()).ratio()
            r -= 0.6 if extra.search(t) and not extra.search(name) else 0
            r += 0.4 if re.search(r"\((figure|character|disney infinity|infinity|power disc|disc|play set|toy box)", t, re.I) else 0
            r -= 0.4 if re.search(r"\((film|movie|series|show|episode|level|enemy|npc|mission|quest)", t, re.I) else 0
            return -r
        links.sort(key=lrank)
        first = None
        for l in links[:4]:
            t2, x2 = wiki.wikitext(l)
            if not t2 or is_disambig(x2):
                continue
            if has_model(x2, fid) == 2:
                return t2, x2, True
            first = first or (t2, x2)
        if first:
            return first[0], first[1], False
    if not title:   # the page has a slightly different name ("Armour", no "!") - search for it
        for t in wiki.search(name, n=8):
            if difflib.SequenceMatcher(None, squash(t), squash(name)).ratio() >= 0.8:
                title, text = wiki.wikitext(t)
                if title and not is_disambig(text):
                    return title, text, has_model(text, fid) == 2
    return (title, text, False) if title else (None, None, False)


def is_disambig(text):
    return re.search(r"\{\{\s*(disambig|disambiguation|dab)\b|may (also )?refer to", text or "", re.I) is not None


def squash(x):
    x = x.lower().replace("armour", "armor").replace("colour", "color").replace("doctor", "dr")
    return re.sub(r"[^a-z0-9]", "", x)


def name_part(file):
    """"Ability-WreckIt-Ralph's Power of Destruction.png" -> "Ralph's Power of Destruction" """
    return PATTERN.sub("", re.sub(r"\.\w+$", "", file))


VARIANT = re.compile(r"\s*\((crystal|light fx|glow(?: in the dark)?|gold|clear|metallic)\)\s*$", re.I)


def name_forms(name):
    """"Mr. Incredible (Crystal)" -> ["Mr. Incredible (Crystal)", "Crystal Mr. Incredible", "Mr. Incredible Crystal"]"""
    m = VARIANT.search(name)
    if not m:
        return [name]
    base, tag = name[:m.start()].strip(), m.group(1)
    return [name, f"{tag.title()} {base}", f"{base} {tag.title()}", f"{base} ({tag.title()})"]


def words(x):
    return set(re.findall(r"[a-z0-9]+", x.lower().replace("'s", "s")))


def same_words(file, name):
    """every word of the name is a whole word in the file name: "Mr" never matches "Mrs" """
    w = words(name_part(file))
    return all(t in w for t in words(name) if len(t) > 1 or t.isdigit())


def name_score(file, name):
    a, b = squash(name_part(file)), squash(name)
    if not a or not b:
        return 0.0
    return 1.0 if a == b else min(0.99, difflib.SequenceMatcher(None, a, b).ratio())


def render_match(file, name):
    """the file is the item's own render: "<Type>-<Franchise>-<exactly this name>.png" (exact, so
    Mrs. Incredible never passes for Mr. Incredible, nor Mike's New Car for Mike)"""
    stem = re.sub(r"\.\w+$", "", file)
    return bool(PATTERN.search(stem)) and not BAD.search(stem.lower()) and any(name_score(file, n) == 1.0 for n in name_forms(name))


def find_file(wiki, name):
    """no page with a picture: look for the render among the wiki's files by name"""
    hits = []
    for q in list(dict.fromkeys(name_forms(name) + [re.sub(r"[^\w\s'-]", " ", name)])):
        for t in wiki.search(q, n=15, ns=6):
            f = norm_file(t)
            if f not in hits and re.search(r"\.(png|webp)$", f, re.I) and not BAD.search(f.lower()):
                hits.append(f)
    hits.sort(key=lambda f: -(name_score(f, name) + (0.2 if PATTERN.search(f) else 0)))
    m = VARIANT.search(name)
    tag = words(m.group(1)) if m else set()
    def ok(f):   # close to the name, every word present, and a variant's word (crystal, light...) too
        return max(name_score(f, n) for n in name_forms(name)) >= 0.75 and \
            any(same_words(f, n) for n in name_forms(name)) and tag <= words(name_part(f))
    best = [f for f in hits if ok(f)][:5]
    info = wiki.file_info(best)
    for f in best:
        if f in info and info[f][3] in IMG_EXT and min(info[f][1], info[f][2]) >= 64:
            return (f,) + info[f]
    return None


def lead_files(text):
    """file names in the infobox / lead, in order"""
    lead = re.split(r"\n==", text or "", maxsplit=1)[0]
    out = []
    for m in re.finditer(r"\|\s*(?:image|img|picture|photo|render|figure)\w*\s*=\s*([^\n|}]+)", lead, re.I):
        v = m.group(1).strip()
        inner = re.search(r"(?:File|Image)\s*:\s*([^|\]\n]+)", v, re.I)
        v = inner.group(1) if inner else re.sub(r"<[^>]+>|\[|\]", "", v).strip()
        if re.search(r"\.(png|jpe?g|webp|gif)$", v, re.I):
            out.append(norm_file(v))
    for m in re.finditer(r"\[\[\s*(?:File|Image)\s*:\s*([^|\]]+)", lead, re.I):
        out.append(norm_file(m.group(1)))
    for m in re.finditer(r"^\s*((?:File:|Image:)?[^|\n=<>]+\.(?:png|jpe?g|webp))\s*(?:\||$)", lead, re.I | re.M):
        out.append(norm_file(m.group(1)))   # <gallery> / <tabber> lines
    seen = []
    for f in out:
        if f not in seen:
            seen.append(f)
    return seen


def pick_file(wiki, title, text, name):
    """(file, url, width, height, mime) of the best picture on the page, or None"""
    score = {}
    for i, f in enumerate(lead_files(text)):
        score[f] = score.get(f, 0) + 8 - min(i, 4)
    pi = wiki.page_image(title)
    if pi:
        score[norm_file(pi)] = score.get(norm_file(pi), 0) + 4
    for f in wiki.page_files(title):
        score.setdefault(norm_file(f), 0)
    toks = [t for t in re.findall(r"[a-z0-9]+", name.lower()) if len(t) > 2 and t not in ("the", "and", "disc", "play", "set")]
    for f in score:
        low = f.lower()
        score[f] += 3 if low.endswith(".png") else -1
        score[f] += 3 if GOOD.search(low) else 0
        score[f] -= 7 if BAD.search(low) else 0
        score[f] += 2 * sum(1 for t in toks if t in low)
    best = sorted(score, key=lambda f: -score[f])[:8]
    info = wiki.file_info(best)
    pick = None
    for f in best:
        if f not in info:
            continue
        url, w, h, mime = info[f]
        if mime not in IMG_EXT or min(w, h) < 64:
            continue
        s = score[f] + (2 if mime == "image/png" else 0) + min(3, max(w, h) / 300)
        if w > h * 1.45:          # landscape: a gameplay screenshot or a banner, never a figure / disc render
            s -= 7
        if max(w, h) < 200:       # a small menu icon
            s -= 4
        if not pick or s > pick[0]:
            pick = (s, f, url, w, h, mime)
    return pick[1:] if pick else None


# ---------------------------------------------------------------- files

def transparent(data):
    """True / False if the image clearly has / hasn't a transparent background, None if unsure"""
    try:
        from PIL import Image
        import io
        im = Image.open(io.BytesIO(data))
        if im.mode not in ("RGBA", "LA", "PA") and "transparency" not in im.info:
            return False
        a = im.convert("RGBA").getchannel("A")
        w, h = a.size
        return min(a.getpixel(p) for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))) < 250
    except ImportError:
        pass
    except Exception:
        return None
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return False if data[:3] == b"\xff\xd8\xff" else None
    if data[25] in (4, 6):        # grey + alpha, RGBA
        return True
    i = 8
    while i + 8 <= len(data):     # palette PNG with a tRNS chunk
        n, typ = struct.unpack(">I4s", data[i:i + 8])
        if typ == b"tRNS":
            return True
        if typ == b"IDAT":
            break
        i += 12 + n
    return False


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")[:40] or "x"


def art_files(fid):
    return [p for p in ART.glob(f"*{fid}*") if re.match(rf"^p?{fid}(?:[ _\-.]|$)", p.stem + ".") and p.suffix.lower() != ".txt"]


def load_figures():
    rows = []
    text = FIGS.read_text(encoding="utf-8") if FIGS else zlib.decompress(base64.b64decode(BUILTIN_FIGS)).decode("utf-8")
    for ln in text.splitlines():
        if ln and not ln.startswith("#"):
            i, s, n = ln.split("\t", 2)
            rows.append({"id": int(i), "series": int(s), "kind": KINDS[int(i) // 1000000], "name": n})
    return rows


def load_sources():
    out = {}
    if SOURCES.exists():
        for ln in SOURCES.read_text(encoding="utf-8").splitlines():
            if not ln or ln.startswith("#"):
                continue
            v = ln.split("\t") + [""] * len(COLS)
            if v[0].isdigit():
                out[int(v[0])] = dict(zip(COLS, (x.strip() for x in v[:len(COLS)])))
    return out


def save_sources(src, figs):
    tmp = SOURCES.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("# Where every badge came from (tools/fetch_art.py). Fix a wrong one: put the right page and/or file\n"
                "# in and set status to manual. Never fetch one: status skip. Columns are tab-separated.\n")
        f.write("\t".join(COLS) + "\n")
        for it in figs:
            r = src.get(it["id"])
            if r:
                f.write("\t".join(str(r.get(c, "")).replace("\t", " ") for c in COLS) + "\n")
    os.replace(tmp, SOURCES)


# ---------------------------------------------------------------- review page

def write_sheet(src, figs):
    colour = {"auto": "#16a34a", "check": "#d97706", "missing": "#dc2626", "manual": "#2563eb", "skip": "#6b7280", "": "#6b7280"}
    KN = {"F": "Characters", "P": "Play Sets", "R": "Power-Ups", "H": "Power Discs"}
    cells = []
    for it in figs:
        r = src.get(it["id"], {})
        files = art_files(it["id"])
        pic = f'<img loading="lazy" src="art/{html.escape(urllib.parse.quote(files[0].name))}">' if files else "<div class=none>no file</div>"
        st = r.get("status", "") or ("own" if files else "")
        link = ""
        if r.get("wiki") and r.get("page"):
            base = r["wiki"] if "://" in r["wiki"] else f'https://{r["wiki"]}.fandom.com/api.php'
            link = f'<a href="{html.escape(base.replace("api.php", "wiki/") + urllib.parse.quote(r["page"].replace(" ", "_")))}">{html.escape(r["page"])}</a>'
        cells.append(f'<div class="c" data-s="{st}" data-k="{it["kind"]}"><div class="p">{pic}</div><b>{it["id"]}</b> {html.escape(it["name"])}'
                     f'<span class="s" style="background:{colour.get(st, "#6b7280")}">{st or "none"}</span><small>{link}<br>'
                     f'{html.escape(r.get("file", ""))}<br><i>{html.escape(r.get("note", ""))}</i></small></div>')
    counts = {}
    for it in figs:
        s = src.get(it["id"], {}).get("status", "") or "none"
        counts[s] = counts.get(s, 0) + 1
    btn = "".join(f'<button data-f="{s}">{s} ({n})</button>' for s, n in sorted(counts.items()))
    kbtn = "".join(f'<button data-g="{k}">{v}</button>' for k, v in KN.items())
    SHEET.write_text(f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Art review</title><style>
body{{margin:0;font:13px system-ui,sans-serif;background:#0f172a;color:#e2e8f0}}
header{{position:sticky;top:0;background:#0f172ae8;padding:10px;display:flex;flex-wrap:wrap;gap:6px;z-index:2}}
button{{background:#1e293b;color:#e2e8f0;border:1px solid #334155;border-radius:8px;padding:6px 10px;font:inherit}}
button.on{{background:#38bdf8;color:#082f49}}
main{{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px;padding:10px}}
.c{{background:#1e293b;border-radius:10px;padding:8px;overflow:hidden;word-break:break-word}}
.p{{aspect-ratio:1;border-radius:8px;margin-bottom:6px;display:flex;align-items:center;justify-content:center;
   background:repeating-conic-gradient(#cbd5e1 0 25%,#f8fafc 0 50%) 0 0/16px 16px}}
.p img{{max-width:100%;max-height:100%;object-fit:contain}}
.none{{color:#64748b}}
.s{{display:inline-block;margin-left:4px;padding:1px 6px;border-radius:6px;color:#fff;font-size:11px}}
small{{display:block;color:#94a3b8;margin-top:4px}} a{{color:#7dd3fc}}
</style></head><body><header><button data-f="" class="on">all ({len(figs)})</button>{btn}<span style="width:12px"></span>{kbtn}</header><main>{"".join(cells)}</main>
<script>
let f='',g='';const apply=()=>document.querySelectorAll('.c').forEach(c=>c.style.display=(!f||c.dataset.s===f||(f==='none'&&!c.dataset.s))&&(!g||c.dataset.k===g)?'':'none');
document.querySelectorAll('[data-f]').forEach(b=>b.onclick=()=>{{f=b.dataset.f;document.querySelectorAll('[data-f]').forEach(x=>x.classList.toggle('on',x===b));apply();}});
document.querySelectorAll('[data-g]').forEach(b=>b.onclick=()=>{{g=g===b.dataset.g?'':b.dataset.g;document.querySelectorAll('[data-g]').forEach(x=>x.classList.toggle('on',x.dataset.g===g));apply();}});
</script></body></html>""", encoding="utf-8")


# ---------------------------------------------------------------- main

def fetch_one(wikis, it, row, args):
    fid, name = it["id"], it["name"]
    manual = row.get("status") == "manual"
    tries = [w for w in wikis if not manual or not row.get("wiki") or w.name == row["wiki"]] or wikis
    if manual and row.get("wiki") and row["wiki"] not in [w.name for w in wikis]:
        tries = [Wiki(row["wiki"], wikis[0].ua, wikis[0].delay)]
    for wiki in tries:
        if manual and row.get("file") and not row.get("page"):
            title, text, verified = None, "", True
        elif manual and row.get("page"):
            title, text = wiki.wikitext(row["page"])
            verified = True
            if not title:
                continue
        else:
            title, text, verified = find_page(wiki, fid, name)
        if manual and row.get("file"):
            f = norm_file(row["file"])
            inf = wiki.file_info([f]).get(f)
            if not inf:
                return dict(row, note=f"file not found on {wiki.name}")
            pick = (f,) + inf
        else:
            pick = pick_file(wiki, title, text, name) if title else None
            if pick and not verified and not render_match(pick[0], name) and title:
                alt = find_file(wiki, name)   # the page's own picture isn't clearly this item: is its render on the wiki?
                if alt and render_match(alt[0], name):
                    pick = alt
            if not pick and not manual:
                pick = find_file(wiki, name)
                if pick:
                    verified = False
        if not pick:
            if verified:
                return dict(row, id=fid, name=name, status="manual" if manual else "missing", wiki=wiki.name, page=title or "", file="",
                            note="page found, no usable picture")
            continue
        f, url, w, h, mime = pick
        out = dict(id=fid, name=name, status="manual" if manual else ("auto" if verified else "check"), wiki=wiki.name,
                   page=title or "", file=f, note=f"{w}x{h}" + ("" if verified else ", matched by name only"))
        if not verified and not manual and render_match(f, name):   # the page by name, and the file is clearly its render
            out.update(status="auto", note=f"{w}x{h}, matched by name and render file name")
        if args.dry_run:
            return out
        data, why = wiki.download(url, f, title)
        if not data:
            return dict(out, status="missing", note=f"download refused: {why}", url=url)
        tr = transparent(data)
        if tr is False:
            out["note"] += ", no transparent background"
            if out["status"] == "auto":
                out["status"] = "check"
        for old in art_files(fid):
            old.unlink()
        real = "png" if data[:8] == b"\x89PNG\r\n\x1a\n" else "webp" if data[8:12] == b"WEBP" else "jpg" if data[:3] == b"\xff\xd8\xff" \
            else IMG_EXT.get(mime, "png")   # the server may send another format than the file's name says
        dest = ART / f"{fid}_{slug(name)}.{real}"
        dest.write_bytes(data)
        return out
    return dict(row, id=fid, name=name, status="manual" if manual else "missing", note="not found on " + ", ".join(w.name for w in tries))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--contact", default="", help="your e-mail or GitHub name, sent in the User-Agent so the wiki can reach you")
    ap.add_argument("--wikis", default=WIKIS, help=f"Fandom wikis to try, in order (default {WIKIS})")
    ap.add_argument("--kinds", default="FPRH", help="F characters, P play sets, R round discs, H hex discs (default all)")
    ap.add_argument("--ids", default="", help="only these ids, comma separated")
    ap.add_argument("--limit", type=int, default=0, help="stop after this many")
    ap.add_argument("--delay", type=float, default=1.0, help="seconds between requests (default 1, please keep it >= 1)")
    ap.add_argument("--force", action="store_true", help="fetch again even if a file is already there")
    ap.add_argument("--dry-run", action="store_true", help="find matches, download nothing")
    ap.add_argument("--sheet", action="store_true", help="only rebuild the review page")
    ap.add_argument("--out", default="", help="folder for art/, art_sources.tsv and art_review.html")
    args = ap.parse_args()
    if args.out:
        set_out(pathlib.Path(args.out).expanduser().resolve())
    SOURCES.parent.mkdir(parents=True, exist_ok=True)
    log(f"fetch_art v0.8\nfigures: {FIGS or 'built-in list'}\nsaving:  {ART}")

    ART.mkdir(parents=True, exist_ok=True)
    figs = load_figures()
    src = load_sources()
    if args.sheet:
        write_sheet(src, figs)
        log(f"review page: {SHEET}")
        return 0
    ua = "dimod-fetch-art/0.8 (personal art lookup for a homebrew figure picker; one request a second" + \
         (f"; contact {args.contact}" if args.contact else "") + ")"
    wikis = [Wiki(w.strip(), ua, max(0.2, args.delay)) for w in args.wikis.split(",") if w.strip()]
    ids = {int(x) for x in args.ids.replace(" ", "").split(",") if x}
    todo = [it for it in figs if it["kind"] in args.kinds.upper() and (not ids or it["id"] in ids)]
    done = refused = 0
    try:
        for n, it in enumerate(todo, 1):
            row = src.get(it["id"], {})
            if row.get("status") == "skip":
                continue
            have = art_files(it["id"])
            if have and not args.force and row.get("status") != "manual":
                continue
            if row.get("status") == "manual" and have and not args.force and row.get("note", "").startswith("fetched"):
                continue
            log(f"[{n}/{len(todo)}] {it['id']} {it['name']}")
            try:
                r = fetch_one(wikis, it, row, args)
            except KeyboardInterrupt:
                raise
            except Exception as e:   # one bad item never stops the run
                r = dict(row, id=it["id"], name=it["name"], status=row.get("status") or "missing", note=f"error: {type(e).__name__}: {e}")
            url = r.pop("url", "")
            if r.get("status") == "manual" and not args.dry_run and art_files(it["id"]):
                r["note"] = "fetched " + r.get("note", "")
            src[it["id"]] = r
            log(f"    {r.get('status')}: {r.get('page') or '-'} / {r.get('file') or '-'}  {r.get('note', '')}")
            refused = refused + 1 if "download refused" in r.get("note", "") else 0
            if refused >= 3:
                log(f"\nThe image server refused 3 downloads in a row, so I stopped rather than keep asking.\n"
                    f"Last file: {url}\nOpen that link in your phone's browser: if it shows the picture there,\n"
                    f"send me this output. The pages found so far are saved, nothing is lost.")
                break
            save_sources(src, figs)
            done += 1
            if args.limit and done >= args.limit:
                break
    except KeyboardInterrupt:
        log("stopped - run it again to carry on where it left off")
    save_sources(src, figs)
    write_sheet(src, figs)
    counts = {}
    for it in figs:
        s = src.get(it["id"], {}).get("status", "none")
        counts[s] = counts.get(s, 0) + 1
    log("totals: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    log(f"sources: {SOURCES}\nreview:  {SHEET}\nnext:    python3 tools/build_data.py --check-art")
    return 0


# tools/figures.tsv (RPCS3's Infinity figure list), zlib + base64, for running this file on its own
BUILTIN_FIGS = """
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
vZXUQ9Jg65f/AaWigbo=
"""


if __name__ == "__main__":
    sys.exit(main())
