"""Target list icon for the "Invasion" entry: a group of four player outlines, in blue.

    python3 build_icon.py <sblytbnd raw> <tpf raw> <out sblytbnd.dcx> <out tpf.dcx>
    python3 build_icon.py --png <out.png>          just draw the icon

Menu icons are sub-rectangles of big atlas textures; a ".layout" XML file per atlas names them
("MENU_MenuIcon_<id>.png"). We draw into an unused corner of the DLC atlas SB_MenuIcon_DLC01_0 and add
one line to its layout, so no file changes size or structure. The atlas is BC7; our blocks are encoded
here (mode 6 only). Needs Pillow.
"""
import io
import re
import struct
import sys
from functools import lru_cache
from itertools import combinations
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).parent))
from fsformats import bnd4_read, bnd4_replace, dcx_write_deflate

ICON_ID = 41813          # next free id after the game's Target icons (41800-41812)
SIZE = 248               # same as the Nightlord icons
ATLAS = "SB_MenuIcon_DLC01_0"
DDS_HEADER = 148         # "DDS " + header + DX10 extension


# --- drawing -------------------------------------------------------------------------------------

def draw_icon():
    """RGBA SIZE x SIZE: dark blue medallion, four figure outlines (one in front, three behind)."""
    k = 4                                     # supersampling
    n = SIZE * k
    c = n // 2

    def disc(radius, blur=0):
        m = Image.new("L", (n, n), 0)
        ImageDraw.Draw(m).ellipse((c - radius, c - radius, c + radius, c + radius), fill=255)
        return m.filter(ImageFilter.GaussianBlur(blur)) if blur else m

    def flat(colour):
        return Image.new("RGBA", (n, n), colour)

    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    r = 118 * k
    img.paste(flat((7, 12, 30, 255)), mask=disc(r))                           # medallion
    img.paste(flat((24, 48, 110, 255)), mask=ImageChops.multiply(disc(r), disc(70 * k, 34 * k)))

    def figure(x, y, s):
        """Mask of a hooded bust: head at (x, y), scale s (head radius in icon pixels)."""
        m = Image.new("L", (n, n), 0)
        d = ImageDraw.Draw(m)
        x, y, s = x * k, y * k, s * k
        d.ellipse((x - s, y - s, x + s, y + s), fill=255)
        top, w = y + s * 1.15, s * 2.1
        d.rounded_rectangle((x - w, top, x + w, top + s * 8), radius=s * 1.5, fill=255)
        return m

    # back to front: (x, y, head radius, outline colour)
    for x, y, s, colour in ((124, 62, 15, (70, 120, 210)), (66, 88, 18, (95, 155, 235)),
                            (182, 88, 18, (95, 155, 235)), (124, 112, 24, (170, 215, 255))):
        body = figure(x, y, s)
        edge = ImageChops.subtract(body, body.filter(ImageFilter.MinFilter(4 * k + 1)))
        img.paste(flat((8, 16, 40, 255)), mask=body)                          # hides the ones behind
        glow = edge.filter(ImageFilter.GaussianBlur(3 * k)).point(lambda v: min(255, v * 2))
        img.paste(flat((40, 110, 255, 255)), mask=ImageChops.multiply(glow, body.filter(ImageFilter.MaxFilter(8 * k + 1))))
        img.paste(flat(colour + (255,)), mask=edge)

    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(img, mask=disc(r))                                              # clip to the medallion
    rim = ImageChops.subtract(disc(r), disc(r - 6 * k))
    out.paste(flat((58, 74, 112, 255)), mask=rim)
    inner = ImageChops.subtract(disc(r - 9 * k), disc(r - 11 * k))
    out.paste(flat((40, 80, 170, 255)), mask=inner)
    return out.resize((SIZE, SIZE), Image.LANCZOS)


# --- BC7 -----------------------------------------------------------------------------------------

_WEIGHTS = (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)


@lru_cache(maxsize=None)
def bc7_block(pixels):
    """Encode a tuple of 16 RGBA tuples (row-major 4x4) as a BC7 mode 6 block: two RGBA endpoints, 4-bit indices.
    Endpoints are the pair of the block's own colours that gives the smallest error."""
    # Fully transparent pixels take the colour of the visible ones, so that an edge block is a
    # plain alpha ramp (one line in RGBA space) instead of a detour through black.
    seen = [p for p in pixels if p[3]]
    if seen and len(seen) < 16:
        tint = tuple(sum(p[i] for p in seen) // len(seen) for i in range(3))
        pixels = [p if p[3] else tint + (0,) for p in pixels]
    best = None
    for e0, e1 in combinations(sorted(set(pixels)), 2) if len(set(pixels)) > 1 else [(pixels[0],) * 2]:
        ends = []
        for e in (e0, e1):                     # 7 bits per channel plus one shared low bit
            pbit = e[3] & 1                    # keeps alpha 0 and 255 exact
            hi = [min(127, max(0, (ch - pbit + 1) >> 1)) for ch in e]
            ends.append((hi, pbit, [(h << 1) | pbit for h in hi]))
        a, b = ends[0][2], ends[1][2]
        palette = [[((64 - w) * p + w * q + 32) >> 6 for p, q in zip(a, b)] for w in _WEIGHTS]
        idx, error = [], 0
        for p in pixels:
            errs = [sum((x - y) ** 2 for x, y in zip(p, c)) for c in palette]
            i = min(range(16), key=errs.__getitem__)
            idx.append(i)
            error += errs[i]
        if best is None or error < best[0]:
            best = (error, ends, idx)
    _, ends, idx = best
    if idx[0] >= 8:                            # the first index is stored without its top bit
        ends.reverse()
        idx = [15 - i for i in idx]
    bits, pos = 1 << 6, 7                      # mode 6
    for ch in range(4):
        for hi, _, _ in ends:
            bits |= hi[ch] << pos
            pos += 7
    for _, pbit, _ in ends:
        bits |= pbit << pos
        pos += 1
    bits |= idx[0] << pos
    pos += 3
    for i in idx[1:]:
        bits |= i << pos
        pos += 4
    assert pos == 128
    return bits.to_bytes(16, "little")


# --- atlas ---------------------------------------------------------------------------------------

def tpf_textures(tpf):
    """{name: (offset, size)} of the DDS files in a TPF."""
    out = {}
    for i in range(struct.unpack_from("<I", tpf, 8)[0]):
        off, size, _, name_off = struct.unpack_from("<IIII", tpf, 0x10 + i * 0x14)
        end = name_off
        while tpf[end:end + 2] != b"\0\0":
            end += 2
        out[tpf[name_off:end].decode("utf-16le")] = (off, size)
    return out


def free_spot(layout, width, height):
    """Top-left corner of an unused SIZE square in the atlas, on the 4-pixel block grid."""
    used = [tuple(map(int, m)) for m in
            re.findall(r'<SubTexture [^>]*x="(\d+)" width="(\d+)" y="(\d+)" height="(\d+)"', layout)]
    pad = 4
    for y in range(height - SIZE - pad, pad, -4):
        for x in range(width - SIZE - pad, pad, -4):
            if all(x + SIZE + pad <= ux or ux + uw + pad <= x or y + SIZE + pad <= uy or uy + uh + pad <= y
                   for ux, uw, uy, uh in used):
                return x, y
    raise RuntimeError("no room in the atlas")


def main(bundle_path, tpf_path, out_bundle, out_tpf):
    bundle = Path(bundle_path).read_bytes()
    name, layout = next((n, b.decode()) for n, b in bnd4_read(bundle) if n.endswith(ATLAS + ".layout"))
    assert f"_{ICON_ID}." not in layout
    width, height = map(int, re.search(r'<TextureAtlas [^>]*width="(\d+)" height="(\d+)"', layout).groups())
    x, y = free_spot(layout, width, height)

    tpf = bytearray(Path(tpf_path).read_bytes())
    off, size = tpf_textures(tpf)[ATLAS]
    dds = bytes(tpf[off:off + size])
    assert dds[84:88] == b"DX10" and struct.unpack_from("<I", dds, 128)[0] == 98, "expected a BC7 atlas"
    assert struct.unpack_from("<II", dds, 12) == (height, width) and size == DDS_HEADER + width * height

    icon = draw_icon()
    px = icon.load()
    for by in range(SIZE // 4):
        for bx in range(SIZE // 4):
            block = tuple(px[bx * 4 + i % 4, by * 4 + i // 4] for i in range(16))
            at = off + DDS_HEADER + ((y // 4 + by) * (width // 4) + x // 4 + bx) * 16
            tpf[at:at + 16] = bc7_block(block)

    # Check: decode the patched atlas the way a GPU would and compare with what we drew.
    atlas = Image.open(io.BytesIO(bytes(tpf[off:off + size]))).convert("RGBA")
    got = atlas.crop((x, y, x + SIZE, y + SIZE))
    black = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255))   # compare as seen on a background
    diff = ImageChops.difference(Image.alpha_composite(black, got), Image.alpha_composite(black, icon))
    worst = max(hi for _, hi in diff.getextrema())
    mean = sum(n * v for v, n in enumerate(diff.convert("L").histogram())) / SIZE ** 2
    assert mean < 3 and worst < 80, f"BC7 encoding is off: mean {mean:.1f}, worst {worst}"

    line = (f'\t<SubTexture name="MENU_MenuIcon_{ICON_ID}.png" x="{x}" width="{SIZE}" y="{y}" height="{SIZE}"'
            f' originalWidth="{SIZE}" originalHeight="{SIZE}" half="0"/>\r\n')
    layout = layout.replace("</TextureAtlas>", line + "</TextureAtlas>")
    for path, data in ((out_bundle, bnd4_replace(bundle, {name: layout.encode()})), (out_tpf, bytes(tpf))):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(dcx_write_deflate(data))
    print(f"icon {ICON_ID}: {SIZE}x{SIZE} at ({x}, {y}) in {ATLAS}, mean error {mean:.1f}")


if __name__ == "__main__":
    if sys.argv[1] == "--png":
        draw_icon().save(sys.argv[2])
    else:
        main(*sys.argv[1:5])
