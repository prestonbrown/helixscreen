// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every compiled text face that takes a runtime CJK fallback, one row each:
//   HELIX_CJK_FACE(compiled face, "bin under assets/fonts/cjk/", pixel size)
// The includer defines HELIX_CJK_FACE. cjk_font_manager.cpp builds its tables
// from it, and platform_manifest.py prune-assets runs it through the C
// preprocessor with a build's HELIX_MAX_FONT_TIER to learn which bins that
// build can load, so a guard written here prunes and loads alike.
//
// xlarge (>=5) and xxlarge (>=6) faces are pruned from the link on constrained
// platforms (AD5M ships only the medium and large tiers). The 48/64 regular and
// bold and 32/40 light faces also need HELIX_HAS_HIDPI_FONTS: a platform can
// reach tier 6 (ESP32 does) without linking those high-DPI scale rungs. Taking
// an unlinked face's address is an undefined reference.
// clang-format off
HELIX_CJK_FACE(noto_sans_8, "noto_sans_cjk_8.bin", 8)
HELIX_CJK_FACE(noto_sans_10, "noto_sans_cjk_10.bin", 10)
HELIX_CJK_FACE(noto_sans_11, "noto_sans_cjk_11.bin", 11)
HELIX_CJK_FACE(noto_sans_12, "noto_sans_cjk_12.bin", 12)
HELIX_CJK_FACE(noto_sans_14, "noto_sans_cjk_14.bin", 14)
HELIX_CJK_FACE(noto_sans_16, "noto_sans_cjk_16.bin", 16)
HELIX_CJK_FACE(noto_sans_18, "noto_sans_cjk_18.bin", 18)
HELIX_CJK_FACE(noto_sans_20, "noto_sans_cjk_20.bin", 20)
HELIX_CJK_FACE(noto_sans_24, "noto_sans_cjk_24.bin", 24)
#if HELIX_MAX_FONT_TIER >= 3
HELIX_CJK_FACE(noto_sans_26, "noto_sans_cjk_26.bin", 26)
#endif
#if HELIX_MAX_FONT_TIER >= 4
HELIX_CJK_FACE(noto_sans_28, "noto_sans_cjk_28.bin", 28)
#endif
#if HELIX_MAX_FONT_TIER >= 5
HELIX_CJK_FACE(noto_sans_32, "noto_sans_cjk_32.bin", 32)
#endif
#if HELIX_MAX_FONT_TIER >= 6
HELIX_CJK_FACE(noto_sans_40, "noto_sans_cjk_40.bin", 40)
#if HELIX_HAS_HIDPI_FONTS
HELIX_CJK_FACE(noto_sans_48, "noto_sans_cjk_48.bin", 48)
HELIX_CJK_FACE(noto_sans_64, "noto_sans_cjk_64.bin", 64)
#endif
#endif

HELIX_CJK_FACE(noto_sans_bold_14, "noto_sans_cjk_bold_14.bin", 14)
HELIX_CJK_FACE(noto_sans_bold_16, "noto_sans_cjk_bold_16.bin", 16)
HELIX_CJK_FACE(noto_sans_bold_18, "noto_sans_cjk_bold_18.bin", 18)
HELIX_CJK_FACE(noto_sans_bold_20, "noto_sans_cjk_bold_20.bin", 20)
HELIX_CJK_FACE(noto_sans_bold_24, "noto_sans_cjk_bold_24.bin", 24)
HELIX_CJK_FACE(noto_sans_bold_28, "noto_sans_cjk_bold_28.bin", 28)
#if HELIX_MAX_FONT_TIER >= 5
HELIX_CJK_FACE(noto_sans_bold_32, "noto_sans_cjk_bold_32.bin", 32)
#endif
#if HELIX_MAX_FONT_TIER >= 6
HELIX_CJK_FACE(noto_sans_bold_40, "noto_sans_cjk_bold_40.bin", 40)
#if HELIX_HAS_HIDPI_FONTS
HELIX_CJK_FACE(noto_sans_bold_48, "noto_sans_cjk_bold_48.bin", 48)
HELIX_CJK_FACE(noto_sans_bold_64, "noto_sans_cjk_bold_64.bin", 64)
#endif
#endif

HELIX_CJK_FACE(noto_sans_light_10, "noto_sans_cjk_light_10.bin", 10)
HELIX_CJK_FACE(noto_sans_light_11, "noto_sans_cjk_light_11.bin", 11)
HELIX_CJK_FACE(noto_sans_light_12, "noto_sans_cjk_light_12.bin", 12)
#if HELIX_MAX_FONT_TIER >= 4
HELIX_CJK_FACE(noto_sans_light_14, "noto_sans_cjk_light_14.bin", 14)
#endif
#if HELIX_MAX_FONT_TIER >= 3
HELIX_CJK_FACE(noto_sans_light_16, "noto_sans_cjk_light_16.bin", 16)
#endif
#if HELIX_MAX_FONT_TIER >= 4
HELIX_CJK_FACE(noto_sans_light_18, "noto_sans_cjk_light_18.bin", 18)
#endif
#if HELIX_MAX_FONT_TIER >= 5
HELIX_CJK_FACE(noto_sans_light_20, "noto_sans_cjk_light_20.bin", 20)
#endif
#if HELIX_MAX_FONT_TIER >= 6
HELIX_CJK_FACE(noto_sans_light_26, "noto_sans_cjk_light_26.bin", 26)
#if HELIX_HAS_HIDPI_FONTS
HELIX_CJK_FACE(noto_sans_light_32, "noto_sans_cjk_light_32.bin", 32)
HELIX_CJK_FACE(noto_sans_light_40, "noto_sans_cjk_light_40.bin", 40)
#endif
#endif
// clang-format on
