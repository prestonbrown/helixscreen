// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix {

/// Load the CJK fonts the persisted UI language needs, and register the
/// displayed-filename hook (core/gcode_filename.h) that loads the CJK filename
/// glyphs, for the fonts the filename labels use, the first time a filename
/// containing CJK is formatted for display. A printer that never shows one
/// loads nothing.
void init_cjk_fonts(const std::string& lang);

/// After the font tokens move (a resize across a breakpoint), load the filename
/// glyphs for the new sizes too. Does nothing until a CJK filename has been shown.
void refresh_cjk_filename_glyphs();

} // namespace helix
