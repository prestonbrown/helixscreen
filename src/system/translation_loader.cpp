// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "translation_loader.h"

#include "data_root_resolver.h"
#include "helix_regex.h"
#include "lvgl/src/misc/lv_text_private.h"

#include <spdlog/spdlog.h>

#include <array>
#include <lvgl.h>
#include <set>
#include <string>
#include <vector>

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

namespace {

bool is_pua(uint32_t cp) {
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
           (cp >= 0x100000 && cp <= 0x10FFFD);
}

// Approximates Python's str.isalpha()/isdigit() outside ASCII by
// range (Latin letters, then everything from Greek up except punctuation,
// symbol and PUA blocks). Exact Unicode categories would need a table; the
// verdict only matters for strings of three code points or fewer.
bool is_letter_or_digit(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z');
    if (cp >= 0xC0 && cp <= 0x24F)
        return cp != 0xD7 && cp != 0xF7;
    if (cp < 0x370 || is_pua(cp))
        return false;
    return !(cp >= 0x2000 && cp <= 0x2BFF) && !(cp >= 0x3000 && cp <= 0x303F) &&
           !(cp >= 0xFE30 && cp <= 0xFE4F) && !(cp >= 0xFF00 && cp <= 0xFF0F);
}

std::string_view strip_ascii(std::string_view s) {
    constexpr std::string_view ws = " \t\n\r\f\v";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
        return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::vector<uint32_t> code_points(std::string_view s) {
    std::string owned(s);
    std::vector<uint32_t> out;
    uint32_t i = 0;
    while (i < owned.size())
        out.push_back(lv_text_encoded_next(owned.c_str(), &i));
    return out;
}

const std::array<const char*, 24> kLanguageNames = {
    "Deutsch", "English", "Español", "Français", "Italiano", "Português",  "Русский",    "中文",
    "日本語",  "한국어",  "العربية", "हिन्दी",    "Türkçe",   "Nederlands", "Polski",     "Svenska",
    "Norsk",   "Dansk",   "Suomi",   "Čeština",  "Magyar",   "Română",     "Українська", "Ελληνικά",
};

bool listed_non_key(std::string_view text) {
    for (const char* t : {"true", "false", "xl", "lg", "md", "sm", "xs", "#RRGGBB"})
        if (text == t)
            return true;
    for (const char* name : kLanguageNames)
        if (text == name)
            return true;
    return false;
}

bool lv_is_translation_key(const char* text) {
    return is_translation_key(text);
}

// Static registration reaches the app, the test binary and the ESP32 image
// alike: each links this file, and the setter only stores a pointer.
const bool kKeyCallbackRegistered = (lv_xml_set_translation_key_cb(lv_is_translation_key), true);

} // namespace

bool is_translation_key(std::string_view text) {
    // Every rejection mirrors a rule in should_skip_text().
    std::string_view stripped = strip_ascii(text);
    if (stripped.empty())
        return false;
    if (text.find('$') != std::string_view::npos || text.front() == '@')
        return false;

    // The common case, a word or phrase, decided without a regex: it starts
    // with an ASCII letter and has nothing a pattern rule below needs (`=`, `_`,
    // a backslash, a newline, `://`, or the trailing digit of "PLA 205").
    unsigned char first = static_cast<unsigned char>(text.front());
    bool starts_with_letter = (first | 0x20) >= 'a' && (first | 0x20) <= 'z';
    unsigned char last = static_cast<unsigned char>(stripped.back());
    if (starts_with_letter && !(last >= '0' && last <= '9') &&
        text.find_first_of("=_\\\n") == std::string_view::npos &&
        text.find("://") == std::string_view::npos)
        return !listed_non_key(text);

    static const Regex const_ref("^#[a-z_][a-z0-9_]*$");
    static const Regex numeric("^[\\d.]+%?$");
    static const Regex font_name("^(mdi_icons_|noto_sans_)\\w+$");
    static const Regex hex_color("^#[0-9A-Fa-f]{6}$");
    static const Regex size_attr("^size=");
    static const Regex xml_attr_value("(?:value|height)\\s*=");
    static const Regex signed_numeric("^[+-]\\.?\\d*\\.?\\d*$");
    static const Regex paren_tech("^\\(.{0,8}\\)$");
    static const Regex snake_case("^[a-z][a-z0-9]*(_[a-z0-9]+)+$");
    static const Regex url("https?://");
    static const Regex material_temp("^[A-Z]+ \\d+$");
    static const Regex temp_value("^\\d(?:\\d|-|\xe2\x80\x93)*\xc2\xb0"
                                  "C?(\\s*/\\s*\\d+\xc2\xb0"
                                  "C?)?(\\s+\\d+%)?$");
    static const Regex measurement("^\\d+(\\.\\d+)?\\s*(mm|cm|g|kg|ml|l|s|ms)$");
    static const Regex numeric_placeholder("^\\s*\\d+\\s*/\\s*\\d+\\s*$");

    if (regex_search(text, const_ref) || regex_search(stripped, numeric))
        return false;

    auto cps = code_points(text);
    bool all_pua = true;
    for (uint32_t cp : cps)
        all_pua = all_pua && is_pua(cp);
    if (all_pua)
        return false;

    if (regex_search(text, font_name) || regex_search(text, hex_color) ||
        regex_search(text, size_attr))
        return false;
    if (listed_non_key(text) || regex_search(text, xml_attr_value))
        return false;

    auto stripped_cps = code_points(stripped);
    if (stripped_cps.size() <= 3) {
        bool any = false;
        for (uint32_t cp : stripped_cps)
            any = any || is_letter_or_digit(cp);
        if (!any)
            return false;
    }
    if (regex_search(stripped, signed_numeric) && stripped != "+" && stripped != "-")
        return false;
    if (regex_search(stripped, paren_tech) || stripped.front() == '^')
        return false;
    if (text.find("\\n") != std::string_view::npos || text.find('\n') != std::string_view::npos)
        return false;
    if (regex_search(stripped, snake_case) || regex_search(text, url) ||
        regex_search(stripped, material_temp) || regex_search(stripped, temp_value) ||
        regex_search(stripped, measurement) || regex_search(stripped, numeric_placeholder))
        return false;
    return true;
}

} // namespace helix::ui
