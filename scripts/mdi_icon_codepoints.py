#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The icon font's codepoint list, derived from the code that draws the icons.

Two places name icon glyphs: the name -> glyph table in include/ui_icon_codepoints.h
(XML `<icon src=...>` and ui_icon lookups) and the `ICON_*` macros in
include/ui_fonts.h (C++ that writes a glyph directly). The fonts must contain every
glyph either names, and nothing more, so the list regen_mdi_fonts.sh and
esp32_regen_compressed_fonts.sh feed lv_font_conv is computed here from both.

Each glyph is read from its UTF-8 escape bytes, the value the compiler sees. A
trailing comment naming a codepoint (`// F0026 alert`, `// U+F02DC home`) must
agree with the bytes; a disagreement is an error, since it means one of the two was
edited without the other.

  --mdi     comma-separated codepoints in the MDI block (0xF0000-0xFDFFF)
  --custom  comma-separated codepoints in the custom block (0xFE000 and up),
            served from assets/fonts/helix-custom-icons.ttf
  --table   one "0xFXXXX description" line per codepoint, for
            verify_mdi_codepoints.py

Exit 1 on any malformed entry or an empty list: a short list would silently drop
glyphs from every font face.
"""

import argparse
import re
import sys
from pathlib import Path

CUSTOM_BLOCK_START = 0xFE000
MDI_BLOCK_START = 0xF0000
PLANE_15_END = 0xFFFFD

ESCAPES = r'"((?:\\x[0-9A-Fa-f]{2})+)"'
COMMENT_CP = r'(?://\s*(?:U\+)?([0-9A-Fa-f]{5})\b\s*(.*))?'
TABLE_ENTRY = re.compile(r'\{"([a-z0-9_]+)",\s*' + ESCAPES + r'\s*\},?\s*' + COMMENT_CP)
MACRO_ENTRY = re.compile(r'^#define\s+(ICON_\w+)\s+' + ESCAPES + r'\s*' + COMMENT_CP)


def decode_glyph(escapes):
    raw = bytes(int(h, 16) for h in re.findall(r"\\x([0-9A-Fa-f]{2})", escapes))
    text = raw.decode("utf-8")
    if len(text) != 1:
        raise ValueError(f"{escapes} is {len(text)} codepoints, not one")
    return ord(text)


def collect(root):
    """codepoint -> description, from both headers. Raises ValueError on a bad entry."""
    sources = [
        (root / "include" / "ui_icon_codepoints.h", TABLE_ENTRY, False),
        (root / "include" / "ui_fonts.h", MACRO_ENTRY, True),
    ]
    glyphs = {}
    for path, pattern, anchored in sources:
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            m = pattern.match(line) if anchored else pattern.search(line)
            if not m:
                continue
            name, escapes, comment_cp, comment = m.groups()
            where = f"{path.relative_to(root)}:{lineno} {name}"
            try:
                cp = decode_glyph(escapes)
            except (ValueError, UnicodeDecodeError) as e:
                raise ValueError(f"{where}: {e}") from None
            if comment_cp and int(comment_cp, 16) != cp:
                raise ValueError(f"{where}: bytes are U+{cp:X} but the comment says {comment_cp}")
            if not MDI_BLOCK_START <= cp <= PLANE_15_END:
                raise ValueError(f"{where}: U+{cp:X} is outside the icon blocks")
            glyphs.setdefault(cp, (comment or name).strip())
    return glyphs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--mdi", action="store_true")
    group.add_argument("--custom", action="store_true")
    group.add_argument("--table", action="store_true")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)

    try:
        glyphs = collect(args.root)
    except (OSError, ValueError) as e:
        print(f"mdi_icon_codepoints: {e}", file=sys.stderr)
        return 1

    if args.table:
        selected = sorted(glyphs)
    elif args.custom:
        selected = sorted(cp for cp in glyphs if cp >= CUSTOM_BLOCK_START)
    else:
        selected = sorted(cp for cp in glyphs if cp < CUSTOM_BLOCK_START)
    if not selected:
        print("mdi_icon_codepoints: no codepoints found", file=sys.stderr)
        return 1

    if args.table:
        for cp in selected:
            print(f"0x{cp:X} {glyphs[cp]}")
    else:
        print(",".join(f"0x{cp:X}" for cp in selected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
