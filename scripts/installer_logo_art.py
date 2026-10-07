#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the installer banner's logo art for one terminal capability.

Usage: installer_logo_art.py MODE ICON.png
  half256, half16  half-block text art (▀ ▄), 26x11 cells, 256 or 16 colors
  sixel            DEC sixel image whose unset pixels show the terminal background
  kitty            kitty graphics protocol, PNG payload, 10 rows tall
  iterm            iTerm2 inline image, PNG payload, 10 rows tall
  size             the image's pixel width and height, which the sixel sits at

Graphics payloads are wrapped at 76 columns for the heredoc; the banner strips
the newlines before writing them. Called by scripts/render-installer-logo.sh.
"""
import base64
import io
import sys

from PIL import Image, ImageFilter

TEXT_COLS, TEXT_ROWS = 26, 11
IMAGE_ROWS = 10
ALPHA_CUT = 0.45

# xterm's default 16-color palette, in SGR order 30-37 then 90-97.
ANSI16 = [
    (0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0),
    (0, 0, 238), (205, 0, 205), (0, 205, 205), (229, 229, 229),
    (127, 127, 127), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (92, 92, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]


def load(path):
    im = Image.open(path).convert("RGBA")
    return im.crop(im.getbbox())


def solid_ribbon(im):
    """The ribbon as one band: its strands are thinner than a cell, so sampled
    as-is they break into fragments. Returns (colour, alpha) images."""
    alpha = im.getchannel("A")
    band = alpha.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.MinFilter(7))
    # Gap pixels take the colour of the strands around them: blur the
    # premultiplied colour and divide the blurred alpha back out.
    pre = Image.composite(im.convert("RGB"), Image.new("RGB", im.size), alpha)
    blur, blur_a = pre.filter(ImageFilter.GaussianBlur(6)), alpha.filter(ImageFilter.GaussianBlur(6))
    px, pa = blur.load(), blur_a.load()
    fill = Image.new("RGB", im.size)
    fp = fill.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            d = pa[x, y] / 255 or 1
            fp[x, y] = tuple(min(255, int(c / d)) for c in px[x, y])
    return fill, band


def xterm256(c):
    step = lambda v: 0 if v < 48 else 1 if v < 115 else (v - 35) // 40
    r, g, b = (step(v) for v in c)
    return 16 + 36 * r + 6 * g + b


def nearest16(c):
    return min(range(16), key=lambda i: sum((a - b) ** 2 for a, b in zip(c, ANSI16[i])))


def half_blocks(im, colors):
    fill, band = solid_ribbon(im)
    w, h = TEXT_COLS, TEXT_ROWS * 2
    rgb, alpha = fill.resize((w, h), Image.BOX), band.resize((w, h), Image.BOX)
    if colors == 256:
        fg = lambda c: f"38;5;{xterm256(c)}"
        bg = lambda c: f"48;5;{xterm256(c)}"
    else:
        fg = lambda c: str((30 if nearest16(c) < 8 else 82) + nearest16(c))
        bg = lambda c: str((40 if nearest16(c) < 8 else 92) + nearest16(c))
    lines = []
    for y in range(0, h, 2):
        row = ""
        for x in range(w):
            top, bot = alpha.getpixel((x, y)) > ALPHA_CUT * 255, alpha.getpixel((x, y + 1)) > ALPHA_CUT * 255
            ct, cb = rgb.getpixel((x, y)), rgb.getpixel((x, y + 1))
            if top and bot:
                row += f"\033[{fg(ct)};{bg(cb)}m▀\033[0m"
            elif top:
                row += f"\033[{fg(ct)}m▀\033[0m"
            elif bot:
                row += f"\033[{fg(cb)}m▄\033[0m"
            else:
                row += " "
        lines.append(row)
    return "\n".join(lines)


def sixel(im):
    alpha = im.getchannel("A").load()
    pal = im.convert("RGB").quantize(64)
    idx, rgb = pal.load(), pal.getpalette()
    w, h = im.size
    used = sorted({idx[x, y] for y in range(h) for x in range(w) if alpha[x, y] >= 128})
    # P2=1: pixels no colour sets keep the terminal's own background.
    out = [f'\033P0;1;0q"1;1;{w};{h}']
    for n in used:
        r, g, b = (v * 100 // 255 for v in rgb[n * 3:n * 3 + 3])
        out.append(f"#{n};2;{r};{g};{b}")

    def rle(s):
        res, i = [], 0
        while i < len(s):
            j = i
            while j < len(s) and s[j] == s[i]:
                j += 1
            res.append(f"!{j - i}{s[i]}" if j - i > 3 else s[i] * (j - i))
            i = j
        return "".join(res)

    for y0 in range(0, h, 6):
        rows = range(y0, min(y0 + 6, h))
        bands = []
        for n in used:
            cols = [sum(1 << (y - y0) for y in rows if alpha[x, y] >= 128 and idx[x, y] == n)
                    for x in range(w)]
            if any(cols):
                bands.append(f"#{n}" + rle("".join(chr(63 + c) for c in cols).rstrip("?")))
        out.append("$".join(bands) + "-")
    out.append("\033\\")
    return "".join(out)


def png_b64(im):
    buf = io.BytesIO()
    im.quantize(64, method=Image.Quantize.FASTOCTREE).save(buf, "PNG", optimize=True)
    return base64.b64encode(buf.getvalue()).decode()


def kitty(im):
    data = png_b64(im)
    chunks = [data[i:i + 4096] for i in range(0, len(data), 4096)]
    out = []
    for i, chunk in enumerate(chunks):
        more = 1 if i < len(chunks) - 1 else 0
        # C=1 leaves the cursor where the image starts; q=2 silences replies.
        keys = f"a=T,f=100,r={IMAGE_ROWS},C=1,q=2,m={more}" if i == 0 else f"m={more}"
        out.append(f"\033_G{keys};{chunk}\033\\")
    return "".join(out)


def iterm(im):
    return f"\033]1337;File=inline=1;height={IMAGE_ROWS};preserveAspectRatio=1:{png_b64(im)}\a"


def wrap(s):
    return "\n".join(s[i:i + 76] for i in range(0, len(s), 76))


def main():
    mode, src = sys.argv[1], sys.argv[2]
    im = load(src)
    art = {
        "half256": lambda: half_blocks(im, 256),
        "half16": lambda: half_blocks(im, 16),
        "sixel": lambda: wrap(sixel(im)),
        "kitty": lambda: wrap(kitty(im)),
        "iterm": lambda: wrap(iterm(im)),
        "size": lambda: f"{im.size[0]} {im.size[1]}",
    }[mode]()
    sys.stdout.write(art + "\n")


if __name__ == "__main__":
    main()
