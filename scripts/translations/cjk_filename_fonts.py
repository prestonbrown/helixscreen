#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The CJK glyphs a print filename can show, and the pixel sizes it shows them at.

A filename is whatever the user typed, so the translation-derived runtime set
(cjk_charset.py) cannot cover it. The filename set comes from
include/core/cjk_ranges.inc, the ranges helix::gcode::contains_cjk() detects:
every codepoint of an ALL range, and from a LISTED range only the characters of
the checked-in lists (level 1 of the Table of General Standard Chinese
Characters, hanzi_level1.txt, and the Joyo kanji, joyo_kanji.txt).

The filename bins under assets/fonts/cjk/filename/ hold only the part of that
set the translation bins do not: at runtime a filename face falls back to a
translation bin first and to the filename bin behind it.

Sizes come from the three font tokens the filename labels use (text_body,
text_small and text_xs read font_body, font_small and font_xs), resolved per
breakpoint tier from ui_xml/globals.xml, then stepped up by the UI scale the
way DisplayMetrics::scaled_font_name() does: font_scale_reach.txt, which
tests/unit/test_display_metrics.cpp keeps equal to that function, lists every
face each face can become. A step-up only lands on a face the build links, so
the faces come from include/cjk_font_faces.inc at the build's font tier.

Consumers: regen_text_fonts.sh (what to bake, at which sizes),
check_cjk_font_coverage.py (what the bake must cover) and
platform_manifest.py prune-assets (which sizes a package keeps).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
LISTS = (HERE / 'hanzi_level1.txt', HERE / 'joyo_kanji.txt')
SCALE_REACH = HERE / 'font_scale_reach.txt'
RANGES_INC = Path('include') / 'core' / 'cjk_ranges.inc'
FACES_INC = Path('include') / 'cjk_font_faces.inc'
FEATURE_DEFAULTS = Path('include') / 'helix_features.h'

FILENAME_FONT_TOKENS = ('font_body', 'font_small', 'font_xs')
TIERS = ('micro', 'tiny', 'small', 'medium', 'large', 'xlarge', 'xxlarge')
MAX_FONT_TIER = 6

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


def cjk_ranges(root: Path = REPO_ROOT) -> list[tuple[int, int, str]]:
    """(first, last, policy) rows of the range table contains_cjk() reads."""
    text = (root / RANGES_INC).read_text(encoding='utf-8')
    if re.search(r'^\s*#\s*if', text, re.M):
        raise FilenameFontError(f'{RANGES_INC}: no #if allowed, the rows are read as text')
    rows = [(int(a, 16), int(b, 16), p) for a, b, p in re.findall(
        r'^HELIX_CJK_RANGE\((0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*([A-Z]+)\)', text, re.M)]
    if not rows:
        raise FilenameFontError(f'{RANGES_INC}: no HELIX_CJK_RANGE rows')
    for first, last, policy in rows:
        if policy not in ('ALL', 'LISTED') or first > last:
            raise FilenameFontError(f'{RANGES_INC}: bad row {first:#x}..{last:#x} {policy}')
    return rows


def listed_characters() -> set[int]:
    """The union of the checked-in character lists."""
    cps: set[int] = set()
    for path in LISTS:
        chars = [line.strip() for line in path.read_text(encoding='utf-8').splitlines()
                 if line.strip() and not line.startswith('#')]
        bad = [c for c in chars if len(c) != 1]
        if bad:
            raise FilenameFontError(f'{path.name}: not one character per line: {bad[:5]}')
        if len(set(chars)) != len(chars):
            raise FilenameFontError(f'{path.name}: a character is listed twice')
        cps.update(ord(c) for c in chars)
    return cps


def filename_codepoints(root: Path = REPO_ROOT) -> set[int]:
    """Every codepoint a filename label must be able to render."""
    rows = cjk_ranges(root)
    listed = listed_characters()
    in_listed = lambda cp: any(a <= cp <= b for a, b, p in rows if p == 'LISTED')  # noqa: E731
    outside = sorted(cp for cp in listed if not in_listed(cp))
    if outside:
        raise FilenameFontError('listed characters outside every LISTED range, so contains_cjk() '
                                f'would never load them: {"".join(map(chr, outside[:10]))}')
    cps = set(listed)
    for first, last, policy in rows:
        if policy == 'ALL':
            cps.update(range(first, last + 1))
    return cps


def baked_codepoints(root: Path) -> set[int]:
    """The filename set minus what the translation bins already carry."""
    return filename_codepoints(root) - read_manifest(root / UI_MANIFEST)


def face_rows(max_font_tier: int, root: Path = REPO_ROOT) -> list[tuple[str, str, int]]:
    """(face, translation bin, px) for every face cjk_font_faces.inc maps at a
    build's HELIX_MAX_FONT_TIER: the host C preprocessor evaluates the table's
    own guards with the feature defaults, so this is the build's own list."""
    cmd = ['cc', '-E', '-P', '-x', 'c', f'-DHELIX_MAX_FONT_TIER={int(max_font_tier)}',
           '-include', str(root / FEATURE_DEFAULTS),
           '-DHELIX_CJK_FACE(face,bin,px)=HELIX_CJK_ROW face bin px', str(root / FACES_INC)]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise FilenameFontError(f'cannot run the C preprocessor for {FACES_INC}: {exc}')
    if out.returncode != 0:
        raise FilenameFontError(f'preprocessing {FACES_INC} failed: {out.stderr.strip()}')
    rows = [(f, b, int(p)) for f, b, p in
            re.findall(r'HELIX_CJK_ROW\s+(\w+)\s+"([^"]+)"\s+(\d+)', out.stdout)]
    if not rows:
        raise FilenameFontError(f'{FACES_INC} maps no faces at tier {max_font_tier}')
    return rows


def scale_reach() -> dict[str, set[str]]:
    """Face -> every face DisplayMetrics::scaled_font_name() can step it up to."""
    reach = {}
    for line in SCALE_REACH.read_text().splitlines():
        face, _, rest = line.partition(':')
        if face and rest:
            reach[face.strip()] = set(rest.split())
    if not reach:
        raise FilenameFontError(f'{SCALE_REACH.name} is empty')
    return reach


def _tier_faces(root: Path) -> dict[str, set[str]]:
    text = (root / 'ui_xml' / 'globals.xml').read_text(encoding='utf-8')
    values = dict(re.findall(r'<string\s+name="([a-z_]+)"\s+value="([a-z0-9_]+)"', text))
    out = {}
    for tier in TIERS:
        faces = set()
        for token in FILENAME_FONT_TOKENS:
            face = values.get(f'{token}_{tier}')
            if not re.fullmatch(r'noto_sans(?:_light|_bold)?_\d+', face or ''):
                raise FilenameFontError(
                    f'globals.xml: {token}_{tier} = {face!r} is not a noto_sans face')
            faces.add(face)
        out[tier] = faces
    return out


def sizes_by_tier(root: Path = REPO_ROOT,
                  max_font_tier: int = MAX_FONT_TIER) -> dict[str, list[int]]:
    """Pixel sizes a filename label can take at each breakpoint tier, at any UI
    scale, in a build of that font tier."""
    linked = {face: px for face, _bin, px in face_rows(max_font_tier, root)}
    reach = scale_reach()
    out = {}
    for tier, faces in _tier_faces(root).items():
        sizes = set()
        for face in faces:
            if face not in reach:
                raise FilenameFontError(f'{SCALE_REACH.name} has no row for {face}')
            sizes.update(linked[f] for f in reach[face] | {face} if f in linked)
        out[tier] = sorted(sizes)
    return out


def all_sizes(root: Path = REPO_ROOT) -> list[int]:
    return sorted({px for sizes in sizes_by_tier(root).values() for px in sizes})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(REPO_ROOT), help='repository root')
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
