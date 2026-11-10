#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The CJK glyphs a print filename can show, and the pixel sizes it shows them at.

A filename is whatever the user typed, so the translation-derived runtime set
(cjk_charset.py) cannot cover it. The filename set is the 3,500 characters of
level 1 of the Table of General Standard Chinese Characters (hanzi_level1.txt)
plus hiragana, katakana, CJK punctuation and the fullwidth forms.

The filename bins under assets/fonts/cjk/filename/ hold only the part of that
set the translation bins do not: at runtime a filename face falls back to a
translation bin first and to the filename bin behind it.

Sizes come from the three font tokens the filename labels use (text_body,
text_small and text_xs read font_body, font_small and font_xs), resolved per
breakpoint tier from ui_xml/globals.xml.

Consumers: regen_text_fonts.sh (what to bake, at which sizes),
check_cjk_font_coverage.py (what the bake must cover) and
platform_manifest.py prune-assets (which sizes a panel can ask for).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LEVEL1_LIST = Path(__file__).with_name('hanzi_level1.txt')

# Inclusive ranges added to the hanzi list.
EXTRA_RANGES = (
    (0x3000, 0x303F),  # CJK Symbols and Punctuation: 、。「」【】
    (0x3041, 0x3096),  # Hiragana
    (0x3099, 0x309F),  # Hiragana voicing marks and iteration marks
    (0x30A0, 0x30FF),  # Katakana
    (0xFF01, 0xFF5E),  # Fullwidth ASCII variants: （）！？
)

FILENAME_FONT_TOKENS = ('font_body', 'font_small', 'font_xs')
TIERS = ('micro', 'tiny', 'small', 'medium', 'large', 'xlarge', 'xxlarge')

FILENAME_BIN_DIR = Path('assets') / 'fonts' / 'cjk' / 'filename'
FILENAME_MANIFEST = FILENAME_BIN_DIR / '.filename_codepoints.manifest'
UI_MANIFEST = Path('assets') / 'fonts' / 'cjk' / '.cjk_codepoints.manifest'


class FilenameFontError(Exception):
    pass


def bin_name(px: int) -> str:
    return f'noto_sans_cjk_filename_{px}.bin'


def read_manifest(path: Path) -> set[int]:
    return {int(line, 16) for line in path.read_text().split()
            if line.lower().startswith('0x')}


def filename_codepoints(list_path: Path = LEVEL1_LIST) -> set[int]:
    """Every codepoint a filename label must be able to render."""
    hanzi = [line.strip() for line in list_path.read_text(encoding='utf-8').splitlines()
             if line.strip() and not line.startswith('#')]
    bad = [h for h in hanzi if len(h) != 1 or not 0x3400 <= ord(h) <= 0x9FFF]
    if bad:
        raise FilenameFontError(f'{list_path}: not one hanzi per line: {bad[:5]}')
    if len(set(hanzi)) != 3500:
        raise FilenameFontError(f'{list_path}: {len(set(hanzi))} distinct hanzi, expected 3500')
    cps = {ord(h) for h in hanzi}
    for lo, hi in EXTRA_RANGES:
        cps.update(range(lo, hi + 1))
    return cps


def baked_codepoints(root: Path) -> set[int]:
    """The filename set minus what the translation bins already carry."""
    return filename_codepoints() - read_manifest(root / UI_MANIFEST)


def sizes_by_tier(root: Path) -> dict[str, list[int]]:
    """Pixel sizes the filename tokens resolve to at each breakpoint tier."""
    text = (root / 'ui_xml' / 'globals.xml').read_text(encoding='utf-8')
    values = dict(re.findall(r'<string\s+name="([a-z_]+)"\s+value="([a-z0-9_]+)"', text))
    out = {}
    for tier in TIERS:
        sizes = set()
        for token in FILENAME_FONT_TOKENS:
            face = values.get(f'{token}_{tier}')
            m = re.fullmatch(r'noto_sans(?:_light|_bold)?_(\d+)', face or '')
            if not m:
                raise FilenameFontError(
                    f'globals.xml: {token}_{tier} = {face!r} is not a noto_sans face')
            sizes.add(int(m.group(1)))
        out[tier] = sorted(sizes)
    return out


def all_sizes(root: Path) -> list[int]:
    return sorted({px for sizes in sizes_by_tier(root).values() for px in sizes})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='.', help='repository root')
    what = parser.add_mutually_exclusive_group(required=True)
    what.add_argument('--codepoints', action='store_true',
                      help='the codepoints to bake, one 0xXXXX per line')
    what.add_argument('--sizes', action='store_true',
                      help='every pixel size to bake, space-separated')
    args = parser.parse_args()
    root = Path(args.root)
    try:
        if args.codepoints:
            for cp in sorted(baked_codepoints(root)):
                print(f'0x{cp:04x}')
        else:
            print(' '.join(str(px) for px in all_sizes(root)))
    except (OSError, FilenameFontError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
