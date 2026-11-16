// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace helix::system {

/// Attaches runtime-loaded CJK glyphs to the compiled text faces as LVGL font
/// fallbacks. Two independent reasons load them:
///  - a zh/ja UI language: every face gets its translation bin;
///  - a CJK print filename on screen, in any language: the faces at the
///    filename labels' pixel sizes get a filename bin (common hanzi and kana)
///    chained behind a translation bin of the same size.
/// Main thread only: the fallbacks are read by every label render.
class CjkFontManager {
  public:
    static CjkFontManager& instance();

    void on_language_changed(const std::string& lang);

    bool is_loaded() const {
        return loaded_;
    }

    /// Number of compiled faces currently carrying a CJK fallback. Compared
    /// against the count of baked .bin files on disk, this proves every baked
    /// fallback is reachable through the mapping tables and every mapped
    /// entry resolves to a real file.
    size_t loaded_font_count() const {
        return loaded_fonts_.size();
    }

    /// Load the filename glyphs for the pixel sizes of @p faces (the faces the
    /// filename labels currently use) and attach them to every face of those
    /// sizes. Idempotent per size; a size with no filename bin on disk, or a
    /// face outside the CJK face table, is skipped.
    void ensure_filename_glyphs(std::initializer_list<const lv_font_t*> faces);

    /// Pixel sizes whose filename bin is loaded, ascending.
    std::vector<int> filename_glyph_sizes() const;

    void shutdown();

  private:
    CjkFontManager() = default;

    static bool needs_cjk(const std::string& lang);
    bool load();
    void unload();
    void relink();
    void load_filename_ui_fonts();
    void destroy_filename_ui_fonts();

    struct FontEntry {
        lv_font_t* compiled_font;
        lv_font_t* cjk_font;
    };

    /// The filename glyphs at one pixel size. `ui` is a translation bin of the
    /// same size, owned here only while no UI language has loaded its own.
    struct FilenameFonts {
        int px;
        lv_font_t* names = nullptr;
        lv_font_t* ui = nullptr;
    };

    std::vector<FontEntry> loaded_fonts_;
    std::vector<FilenameFonts> filename_fonts_;
    std::string current_lang_;
    bool loaded_ = false;
};

} // namespace helix::system
