// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cjk_font_manager.h"

#include "ui_fonts.h"

#include "spdlog/spdlog.h"

#include <algorithm>

#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6 // default: all tiers (micro=0 .. xxlarge=6)
#endif

namespace helix::system {

namespace {

struct FaceRow {
    lv_font_t* compiled;
    const char* bin_filename;
    int px;
};

#define HELIX_CJK_FACE(face, bin, px) {&face, bin, px},
const FaceRow kFaces[] = {
#include "cjk_font_faces.inc"
};
#undef HELIX_CJK_FACE

constexpr const char* kCjkFontDir = "A:assets/fonts/cjk/";
constexpr const char* kFilenameFontDir = "A:assets/fonts/cjk/filename/";

lv_font_t* load_bin(const std::string& path) {
    lv_font_t* font = lv_binfont_create(path.c_str());
    if (!font) {
        spdlog::warn("[CjkFontManager] Failed to load: {}", path);
    }
    return font;
}

} // namespace

CjkFontManager& CjkFontManager::instance() {
    static CjkFontManager s_instance;
    return s_instance;
}

bool CjkFontManager::needs_cjk(const std::string& lang) {
    return lang == "zh" || lang == "ja";
}

void CjkFontManager::on_language_changed(const std::string& lang) {
    if (needs_cjk(lang)) {
        if (!loaded_) {
            if (!load()) {
                spdlog::warn("[CjkFontManager] Failed to load CJK fonts for '{}'", lang);
            }
        }
        current_lang_ = lang;
    } else {
        if (loaded_) {
            unload();
        }
        current_lang_.clear();
    }
}

// Every fallback pointer is derived here from what is loaded, so the language
// and filename paths never have to know about each other's links.
void CjkFontManager::relink() {
    auto filename_at = [this](int px) -> const FilenameFonts* {
        for (const auto& f : filename_fonts_) {
            if (f.px == px && f.names)
                return &f;
        }
        return nullptr;
    };
    for (const auto& row : kFaces) {
        const FilenameFonts* names = filename_at(row.px);
        lv_font_t* lang = nullptr;
        for (auto& entry : loaded_fonts_) {
            if (entry.compiled_font == row.compiled)
                lang = entry.cjk_font;
        }
        if (lang) {
            lang->fallback = names ? names->names : nullptr;
            row.compiled->fallback = lang;
        } else if (names) {
            row.compiled->fallback = names->ui ? names->ui : names->names;
        } else {
            row.compiled->fallback = nullptr;
        }
    }
    for (auto& f : filename_fonts_) {
        if (f.ui)
            f.ui->fallback = f.names;
    }
}

bool CjkFontManager::load() {
    spdlog::info("[CjkFontManager] Loading CJK runtime fonts...");

    for (const auto& row : kFaces) {
        if (lv_font_t* cjk = load_bin(std::string(kCjkFontDir) + row.bin_filename)) {
            loaded_fonts_.push_back({row.compiled, cjk});
        }
    }

    if (loaded_fonts_.empty()) {
        spdlog::error("[CjkFontManager] No CJK fonts loaded — check assets/fonts/cjk/");
        return false;
    }

    loaded_ = true;
    relink();
    // The language bins now sit in front of the filename glyphs.
    destroy_filename_ui_fonts();
    spdlog::info("[CjkFontManager] Loaded {} CJK fallback fonts", loaded_fonts_.size());
    return true;
}

void CjkFontManager::unload() {
    if (!loaded_)
        return;

    spdlog::info("[CjkFontManager] Unloading {} CJK fonts", loaded_fonts_.size());

    // Relink before destroying, so no render call can follow a dangling pointer.
    loaded_ = false;
    std::vector<FontEntry> doomed;
    doomed.swap(loaded_fonts_);
    load_filename_ui_fonts();
    relink();
    for (auto& entry : doomed) {
        lv_binfont_destroy(entry.cjk_font);
    }
    current_lang_.clear();
}

void CjkFontManager::load_filename_ui_fonts() {
    if (loaded_)
        return;
    for (auto& f : filename_fonts_) {
        if (!f.names || f.ui)
            continue;
        for (const auto& row : kFaces) {
            if (row.px == f.px) {
                f.ui = load_bin(std::string(kCjkFontDir) + row.bin_filename);
                break;
            }
        }
    }
}

void CjkFontManager::destroy_filename_ui_fonts() {
    std::vector<lv_font_t*> doomed;
    for (auto& f : filename_fonts_) {
        if (f.ui) {
            doomed.push_back(f.ui);
            f.ui = nullptr;
        }
    }
    relink();
    for (lv_font_t* font : doomed) {
        lv_binfont_destroy(font);
    }
}

void CjkFontManager::ensure_filename_glyphs(std::initializer_list<const lv_font_t*> faces) {
#if HELIX_HAS_CJK
    bool added = false;
    for (const lv_font_t* face : faces) {
        const auto row = std::find_if(std::begin(kFaces), std::end(kFaces),
                                      [face](const FaceRow& r) { return r.compiled == face; });
        if (row == std::end(kFaces))
            continue;
        const int px = row->px;
        const bool known = std::any_of(filename_fonts_.begin(), filename_fonts_.end(),
                                       [px](const FilenameFonts& f) { return f.px == px; });
        if (known)
            continue;
        // A size with no bin (pruned, or a high-DPI rung) is remembered as
        // tried, so later filenames do not retry the open.
        FilenameFonts fonts{px};
        fonts.names = lv_binfont_create((std::string(kFilenameFontDir) + "noto_sans_cjk_filename_" +
                                         std::to_string(px) + ".bin")
                                            .c_str());
        spdlog::info("[CjkFontManager] Filename glyphs at {}px: {}", px,
                     fonts.names ? "loaded" : "no bin, skipped");
        filename_fonts_.push_back(fonts);
        added = added || fonts.names != nullptr;
    }
    if (added) {
        load_filename_ui_fonts();
        relink();
        // A label laid out before this load measured its CJK text with missing
        // glyphs; a style refresh makes every label measure and wrap it again.
        lv_obj_report_style_change(nullptr);
    }
#else
    (void)faces;
#endif
}

std::vector<int> CjkFontManager::filename_glyph_sizes() const {
    std::vector<int> sizes;
    for (const auto& f : filename_fonts_) {
        if (f.names)
            sizes.push_back(f.px);
    }
    std::sort(sizes.begin(), sizes.end());
    return sizes;
}

void CjkFontManager::shutdown() {
    std::vector<FilenameFonts> doomed;
    doomed.swap(filename_fonts_);
    if (loaded_)
        unload();
    else
        relink();
    for (auto& f : doomed) {
        if (f.ui)
            lv_binfont_destroy(f.ui);
        if (f.names)
            lv_binfont_destroy(f.names);
    }
}

} // namespace helix::system
