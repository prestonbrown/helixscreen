// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "translation_loader.h"

#include "data_root_resolver.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <set>
#include <string>

namespace helix::ui {

namespace {
// Tracks which locales have been registered with LVGL this session. LVGL
// exposes no lv_translation_remove_pack — packs accumulate until deinit.
// The set exists to skip redundant file reads on repeated switches to the
// same locale.
std::set<std::string>& loaded_locales() {
    static std::set<std::string> s;
    return s;
}
} // namespace

void ensure_translation_loaded(const std::string& lang) {
    if (lang.empty())
        return;

    // en.xml maps every tag to itself, so registering it buys nothing:
    // lv_translation_get() already falls back to the tag when no pack matches
    // the selected language. It was loaded anyway because the fallback path
    // logged `language is not found` on EVERY lookup; LVGL now reports that
    // once per language (patches/lvgl_translation_warn_once.patch), so the
    // ~140 KB of heap has no remaining justification. Skipping it also drops
    // English lookups from a linear scan of 2739 entries to an empty walk.
    if (lang == kIdentityLocale)
        return;

    if (loaded_locales().count(lang) > 0)
        return;

    std::string path = "A:" + helix::asset_path("ui_xml/translations/" + lang + ".xml");
    lv_result_t res = lv_xml_register_translation_from_file(path.c_str());
    if (res != LV_RESULT_OK) {
        spdlog::warn("[TranslationLoader] Failed to load '{}' — UI will fall back to English",
                     path);
        return;
    }

    loaded_locales().insert(lang);
    spdlog::debug("[TranslationLoader] Loaded translation pack for '{}'", lang);
}

} // namespace helix::ui
