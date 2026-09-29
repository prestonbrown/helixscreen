#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if a runtime CJK .bin font lacks a codepoint its manifest records.

The manifest (assets/fonts/cjk/.cjk_codepoints.manifest) is what
regen_text_fonts.sh INTENDED to bake. This gate parses the .bin files
themselves — the cmap tables lv_binfont_loader.c walks at runtime — so a
stale or half-written bake cannot hide behind a freshly written manifest.
A translated string whose glyph is missing renders as tofu with no build
error and no runtime warning; the manifest alone cannot prove otherwise.

Format reference: lib/lvgl/src/font/binfont_loader/lv_binfont_loader.c
(cmap_table_bin_t, the section label layout) and
lib/lvgl/src/font/fmt_txt/lv_font_fmt_txt.c#get_glyph_dsc_id (what "covered"
means per cmap format type).
"""

from __future__ import annotations

import argparse
import glob
import struct
import sys
from pathlib import Path

# lv_font_fmt_txt.h cmap types. These numeric codes are a wire format shared
# with lv_font_conv's bin writer (lib/font/table_cmap.js), NOT an enum whose
# order can be rearranged: 0=format0 with u8 glyph deltas, 1=sparse with u16
# code list + u16 id list, 2=format0_tiny contiguous with no data, 3=sparse
# with u16 code list only. Subtable data is 4-byte aligned.
FMT0_FULL, SPARSE_FULL, FMT0_TINY, SPARSE_TINY = 0, 1, 2, 3


class BinFormatError(Exception):
    pass


def _read_label(data: bytes, pos: int, expect: bytes) -> int:
    if pos + 8 > len(data):
        raise BinFormatError(f'label at {pos} past end of file')
    length, magic = struct.unpack_from('<I4s', data, pos)
    if magic != expect:
        raise BinFormatError(f'expected {expect!r} label at {pos}, found {magic!r}')
    return length


def covered_codepoints(data: bytes) -> set[int]:
    """Codepoints the runtime lookup would resolve to a glyph in this .bin."""
    header_len = _read_label(data, 0, b'head')
    _read_label(data, header_len, b'cmap')

    pos = header_len + 8  # past the cmap label
    (num_tables,) = struct.unpack_from('<I', data, pos)
    pos += 4

    covered: set[int] = set()
    for i in range(num_tables):
        entry_pos = pos + 16 * i
        if entry_pos + 16 > len(data):
            raise BinFormatError(f'cmap table {i} header past end of file')
        data_off, range_start, range_length, _gid_start, entries, fmt, _pad = \
            struct.unpack_from('<IIHHHBB', data, entry_pos)
        table_pos = header_len + data_off

        if fmt == FMT0_TINY:
            # Contiguous range, no data section at all.
            covered.update(range(range_start, range_start + range_length))
        elif fmt == FMT0_FULL:
            # u8 glyph delta per slot in the range; a 0 delta marks a missing
            # character everywhere except slot 0, where 0 is legitimate.
            for rcp in range(range_length):
                (gid_ofs,) = struct.unpack_from('<B', data, table_pos + rcp)
                if gid_ofs != 0 or rcp == 0:
                    covered.add(range_start + rcp)
        elif fmt in (SPARSE_TINY, SPARSE_FULL):
            # Sorted u16 codepoint deltas from range_start; presence in the
            # list is coverage. SPARSE_FULL trails a u16 glyph-id list that
            # does not change which codepoints the range covers.
            for j in range(entries):
                (ofs,) = struct.unpack_from('<H', data, table_pos + 2 * j)
                covered.add(range_start + ofs)
        else:
            raise BinFormatError(f'cmap table {i}: unknown format type {fmt}')
    return covered


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='.', help='repository root to check')
    parser.add_argument('--summary', action='store_true',
                        help='print pass counts even when green')
    args = parser.parse_args()

    cjk_dir = Path(args.root) / 'assets' / 'fonts' / 'cjk'
    manifest_path = cjk_dir / '.cjk_codepoints.manifest'
    if not manifest_path.is_file():
        print(f'✗ {manifest_path} not found — run make regen-text-fonts')
        return 1

    manifest = set()
    for line in manifest_path.read_text().splitlines():
        line = line.strip().lower()
        if line.startswith('0x'):
            manifest.add(int(line, 16))
    if not manifest:
        print(f'✗ {manifest_path} is empty — refusing to pass vacuously')
        return 1

    bins = sorted(glob.glob(str(cjk_dir / '*.bin')))
    if not bins:
        print(f'✗ no .bin fonts under {cjk_dir} — the runtime CJK faces are gone')
        return 1

    failures = 0
    for bin_path in bins:
        data = Path(bin_path).read_bytes()
        try:
            covered = covered_codepoints(data)
        except (BinFormatError, struct.error) as exc:
            print(f'✗ {Path(bin_path).name}: cannot parse ({exc})')
            failures += 1
            continue
        missing = manifest - covered
        if missing:
            cps = ' '.join(f'0x{cp:04x}' for cp in sorted(missing))
            chars = ''.join(chr(cp) for cp in sorted(missing))
            print(f'✗ {Path(bin_path).name}: {len(missing)} manifest codepoint(s) '
                  f'have no glyph: {cps} ({chars})')
            failures += 1

    if failures:
        print('The manifest records codepoints the baked .bin does not carry.')
        print('Run make regen-text-fonts and rebuild — do not hand-edit the manifest.')
        return 1

    if args.summary:
        print(f'✓ {len(bins)} CJK .bin fonts cover all {len(manifest)} manifest codepoints')
    return 0


if __name__ == '__main__':
    sys.exit(main())
