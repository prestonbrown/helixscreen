// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The codepoints helix::gcode::contains_cjk() treats as CJK, one inclusive
// range per row:
//   HELIX_CJK_RANGE(first, last, policy)
// policy ALL: every codepoint is baked into the CJK filename glyphs.
// policy LISTED: only the characters of the checked-in lists are baked
// (scripts/translations/hanzi_level1.txt, joyo_kanji.txt); the rest of the
// block deliberately renders as boxes.
// scripts/translations/cjk_filename_fonts.py bakes from these rows and
// check_cjk_font_coverage.py holds the bins to them, so a filename that loads
// the glyphs can only contain what was baked or what is declared unbaked.
// Every range lies in U+3000..U+FFFF (a 3-byte UTF-8 sequence). No #if here:
// the scripts read the rows as text.
HELIX_CJK_RANGE(0x3000, 0x303F, ALL)    // CJK Symbols and Punctuation
HELIX_CJK_RANGE(0x3041, 0x3096, ALL)    // Hiragana
HELIX_CJK_RANGE(0x3099, 0x309F, ALL)    // Hiragana voicing and iteration marks
HELIX_CJK_RANGE(0x30A0, 0x30FF, ALL)    // Katakana
HELIX_CJK_RANGE(0x3400, 0x4DBF, LISTED) // CJK Unified Ideographs Extension A
HELIX_CJK_RANGE(0x4E00, 0x9FFF, LISTED) // CJK Unified Ideographs
HELIX_CJK_RANGE(0xFF01, 0xFF5E, ALL)    // Fullwidth ASCII variants
HELIX_CJK_RANGE(0xFF61, 0xFF9F, ALL)    // Halfwidth katakana and punctuation
