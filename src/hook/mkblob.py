"""Turn the linked hook ELF into a flat image and patch the hook offsets into the header.
The hook order is read from DI_HOOK_LIST in dihook.h so the two can never drift apart."""
import re, sys, struct, pathlib
from elftools.elf.elffile import ELFFile

src, dst = sys.argv[1], sys.argv[2]
hdr_text = (pathlib.Path(__file__).parent / "dihook.h").read_text()
block = re.search(r"#define DI_HOOK_LIST\(X\)(.*?)\n\n", hdr_text, re.S).group(1)
HOOKS = re.findall(r"X\((\w+)\)", block)

with open(src, "rb") as f:
    e = ELFFile(f)
    for s in e.iter_sections():
        if s.name.startswith(".rela") and s.data_size:
            sys.exit(f"ERROR: dynamic relocations present in {s.name}; image is not position independent")
    sym = {s.name: s["st_value"] for s in e.get_section_by_name(".symtab").iter_symbols()}
    size = sym["_end"]
    img = bytearray(size)
    for s in e.iter_sections():
        if s["sh_flags"] & 2 and s["sh_type"] != "SHT_NOBITS":
            d = s.data()
            img[s["sh_addr"]:s["sh_addr"] + len(d)] = d

assert sym["HDR"] == 0, "header must be at offset 0"
struct.pack_into("<I", img, 8, size)
for i, h in enumerate(HOOKS):
    struct.pack_into("<I", img, 12 + 4 * i, sym["h_" + h])
struct.pack_into("<I", img, 12 + 4 * len(HOOKS), sym["di_ping"])
open(dst, "wb").write(img)
print(f"{dst}: {size} bytes ({len(HOOKS)} hooks)")
