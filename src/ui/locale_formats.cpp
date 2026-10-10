// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "locale_formats.h"

#include "ui_format_utils.h"

#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <array>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

namespace helix::ui {

// ---------------------------------------------------------------------------
// Cached locale state
// ---------------------------------------------------------------------------
static bool s_use_system_locale = false;
static std::string s_current_lang = "en";

// ---------------------------------------------------------------------------
// Language code → POSIX locale name
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, const char*> k_locale_map = {
    {"en", "en_US.UTF-8"}, {"de", "de_DE.UTF-8"}, {"fr", "fr_FR.UTF-8"},
    {"es", "es_ES.UTF-8"}, {"ru", "ru_RU.UTF-8"}, {"pt", "pt_BR.UTF-8"},
    {"it", "it_IT.UTF-8"}, {"zh", "zh_CN.UTF-8"}, {"ja", "ja_JP.UTF-8"},
};

// ---------------------------------------------------------------------------
// Fallback translation tables
// ---------------------------------------------------------------------------

// Day abbreviations indexed by tm_wday (0 = Sunday)
static const std::unordered_map<std::string, std::array<const char*, 7>> k_day_abbr = {
    {"en", {{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}}},
    {"de", {{"So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa."}}},
    {"fr", {{"dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam."}}},
    {"es", {{"dom.", "lun.", "mar.", "mié.", "jue.", "vie.", "sáb."}}},
    {"ru", {{u8"вс", u8"пн", u8"вт", u8"ср", u8"чт", u8"пт", u8"сб"}}},
    {"pt", {{"dom.", "seg.", "ter.", "qua.", "qui.", "sex.", "sáb."}}},
    {"it", {{"dom.", "lun.", "mar.", "mer.", "gio.", "ven.", "sab."}}},
    {"zh", {{u8"日", u8"一", u8"二", u8"三", u8"四", u8"五", u8"六"}}},
    {"ja", {{u8"日", u8"月", u8"火", u8"水", u8"木", u8"金", u8"土"}}},
};

// Month abbreviations indexed by tm_mon (0 = January)
static const std::unordered_map<std::string, std::array<const char*, 12>> k_month_abbr = {
    {"en", {{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}}},
    {"de",
     {{"Jan.", "Feb.", u8"Mär.", "Apr.", "Mai", "Jun.", "Jul.", "Aug.", "Sep.", "Okt.", "Nov.",
       "Dez."}}},
    {"fr",
     {{"janv.", u8"févr.", "mars", "avr.", "mai", "juin", "juil.", u8"août", "sept.", "oct.",
       "nov.", u8"déc."}}},
    {"es",
     {{"ene.", "feb.", "mar.", "abr.", "may.", "jun.", "jul.", "ago.", "sept.", "oct.", "nov.",
       "dic."}}},
    {"ru",
     {{u8"янв.", u8"февр.", u8"мар.", u8"апр.", u8"мая", u8"июн.", u8"июл.", u8"авг.", u8"сент.",
       u8"окт.", u8"нояб.", u8"дек."}}},
    {"pt",
     {{"jan.", "fev.", "mar.", "abr.", "mai.", "jun.", "jul.", "ago.", "set.", "out.", "nov.",
       "dez."}}},
    {"it",
     {{"gen.", "feb.", "mar.", "apr.", "mag.", "giu.", "lug.", "ago.", "set.", "ott.", "nov.",
       "dic."}}},
    {"zh",
     {{u8"1月", u8"2月", u8"3月", u8"4月", u8"5月", u8"6月", u8"7月", u8"8月", u8"9月", u8"10月",
       u8"11月", u8"12月"}}},
    {"ja",
     {{u8"1月", u8"2月", u8"3月", u8"4月", u8"5月", u8"6月", u8"7月", u8"8月", u8"9月", u8"10月",
       u8"11月", u8"12月"}}},
};

// ---------------------------------------------------------------------------
// Format pattern groups
// ---------------------------------------------------------------------------

enum class DatePatternGroup {
    EN,         // "{day}, {mon} {dd}"
    DE,         // "{day}, {dd}. {mon}"
    ROMANCE_RU, // "{day}, {dd} {mon}"  (fr, es, pt, it, ru)
    CJK,        // "{mm}/{dd} ({day})"  (zh, ja)
};

enum class ModifiedDatePatternGroup {
    EN,         // "{mon} {dd} {time}"
    DE,         // "{dd}. {mon} {time}"
    ROMANCE_RU, // "{dd} {mon} {time}"
    CJK,        // "{mm}/{dd} {time}"
};

static DatePatternGroup get_date_pattern(const std::string& lang) {
    if (lang == "de")
        return DatePatternGroup::DE;
    if (lang == "zh" || lang == "ja")
        return DatePatternGroup::CJK;
    if (lang == "fr" || lang == "es" || lang == "pt" || lang == "it" || lang == "ru")
        return DatePatternGroup::ROMANCE_RU;
    // en and unknown languages
    return DatePatternGroup::EN;
}

static ModifiedDatePatternGroup get_modified_date_pattern(const std::string& lang) {
    if (lang == "de")
        return ModifiedDatePatternGroup::DE;
    if (lang == "zh" || lang == "ja")
        return ModifiedDatePatternGroup::CJK;
    if (lang == "fr" || lang == "es" || lang == "pt" || lang == "it" || lang == "ru")
        return ModifiedDatePatternGroup::ROMANCE_RU;
    // en and unknown languages
    return ModifiedDatePatternGroup::EN;
}

// ---------------------------------------------------------------------------
// Helpers to get day/month name (system locale or fallback)
// ---------------------------------------------------------------------------

static std::string get_day_name(const struct tm* tm_info) {
    if (s_use_system_locale) {
        char buf[32];
        strftime(buf, sizeof(buf), "%a", tm_info);
        return buf;
    }

    auto it = k_day_abbr.find(s_current_lang);
    if (it == k_day_abbr.end()) {
        it = k_day_abbr.find("en");
    }
    int wday = tm_info->tm_wday;
    if (wday < 0 || wday > 6)
        wday = 0;
    return it->second[static_cast<size_t>(wday)];
}

static std::string get_month_name(const struct tm* tm_info) {
    if (s_use_system_locale) {
        char buf[32];
        strftime(buf, sizeof(buf), "%b", tm_info);
        return buf;
    }

    auto it = k_month_abbr.find(s_current_lang);
    if (it == k_month_abbr.end()) {
        it = k_month_abbr.find("en");
    }
    int mon = tm_info->tm_mon;
    if (mon < 0 || mon > 11)
        mon = 0;
    return it->second[static_cast<size_t>(mon)];
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string locale_current_language() {
    return s_current_lang;
}

namespace {
std::map<uint64_t, std::function<void(const std::string&)>>& language_listeners() {
    static std::map<uint64_t, std::function<void(const std::string&)>> s;
    return s;
}
} // namespace

uint64_t locale_add_language_listener(std::function<void(const std::string&)> fn) {
    static uint64_t next_id = 1;
    language_listeners()[next_id] = std::move(fn);
    return next_id++;
}

void locale_remove_language_listener(uint64_t id) {
    language_listeners().erase(id);
}

namespace {
void apply_locale(const std::string& lang_code) {
    helix::format::set_translator(lv_translation_get);
    s_current_lang = lang_code;
    s_use_system_locale = false;

    // English works fine with C locale strftime
    if (lang_code == "en") {
        spdlog::info("locale: language='en', using C locale (English default)");
        return;
    }

    auto it = k_locale_map.find(lang_code);
    if (it == k_locale_map.end()) {
        spdlog::warn("locale: unknown language '{}', falling back to tables", lang_code);
        return;
    }

    const char* result = std::setlocale(LC_TIME, it->second);
    if (!result) {
        spdlog::info("locale: setlocale(LC_TIME, '{}') failed, using fallback tables", it->second);
        return;
    }

    // Probe: format a known Monday to see if locale is working
    struct tm probe = {};
    probe.tm_wday = 1;   // Monday
    probe.tm_mon = 0;    // January
    probe.tm_mday = 6;   // Jan 6 2025 was a Monday
    probe.tm_year = 125; // 2025

    char buf[32];
    strftime(buf, sizeof(buf), "%a", &probe);

    // If result is still "Mon", the locale didn't take effect
    if (std::strcmp(buf, "Mon") == 0) {
        spdlog::info("locale: setlocale succeeded but strftime still returns English "
                     "for lang='{}', using fallback tables",
                     lang_code);
        return;
    }

    s_use_system_locale = true;
    spdlog::info("locale: system locale active for lang='{}' (locale='{}', probe='{}' for Monday)",
                 lang_code, it->second, buf);
}
} // namespace

void locale_set_language(const std::string& lang_code) {
    apply_locale(lang_code);
    for (auto& [id, fn] : language_listeners())
        fn(lang_code);
}

std::string format_localized_date(const struct tm* tm_info) {
    if (!tm_info) {
        return helix::format::UNAVAILABLE;
    }

    std::string day = get_day_name(tm_info);
    std::string mon = get_month_name(tm_info);
    int dd = tm_info->tm_mday;
    int mm = tm_info->tm_mon + 1;

    char buf[64];

    switch (get_date_pattern(s_current_lang)) {
    case DatePatternGroup::EN:
        // "Fri, Feb 28"
        snprintf(buf, sizeof(buf), "%s, %s %d", day.c_str(), mon.c_str(), dd);
        break;
    case DatePatternGroup::DE:
        // "Fr., 28. Feb."
        snprintf(buf, sizeof(buf), "%s, %d. %s", day.c_str(), dd, mon.c_str());
        break;
    case DatePatternGroup::ROMANCE_RU:
        // "ven., 28 févr."
        snprintf(buf, sizeof(buf), "%s, %d %s", day.c_str(), dd, mon.c_str());
        break;
    case DatePatternGroup::CJK:
        // "2/28 (金)"
        snprintf(buf, sizeof(buf), "%d/%d (%s)", mm, dd, day.c_str());
        break;
    }

    return buf;
}

std::string format_localized_modified_date(const struct tm* tm_info) {
    if (!tm_info) {
        return helix::format::UNAVAILABLE;
    }

    std::string mon = get_month_name(tm_info);
    std::string time_str = format_time(tm_info);
    int dd = tm_info->tm_mday;
    int mm = tm_info->tm_mon + 1;

    char buf[64];

    switch (get_modified_date_pattern(s_current_lang)) {
    case ModifiedDatePatternGroup::EN:
        // "Feb 28 2:30 PM"
        snprintf(buf, sizeof(buf), "%s %d %s", mon.c_str(), dd, time_str.c_str());
        break;
    case ModifiedDatePatternGroup::DE:
        // "28. Feb. 14:30"
        snprintf(buf, sizeof(buf), "%d. %s %s", dd, mon.c_str(), time_str.c_str());
        break;
    case ModifiedDatePatternGroup::ROMANCE_RU:
        // "28 févr. 14:30"
        snprintf(buf, sizeof(buf), "%d %s %s", dd, mon.c_str(), time_str.c_str());
        break;
    case ModifiedDatePatternGroup::CJK:
        // "2/28 14:30"
        snprintf(buf, sizeof(buf), "%d/%d %s", mm, dd, time_str.c_str());
        break;
    }

    return buf;
}

std::string format_localized_short_date(const struct tm* tm_info) {
    if (!tm_info) {
        return helix::format::UNAVAILABLE;
    }

    std::string mon = get_month_name(tm_info);
    int dd = tm_info->tm_mday;
    int mm = tm_info->tm_mon + 1;
    int yy = tm_info->tm_year + 1900;

    // Show year when date is not from the current year
    time_t now = time(nullptr);
    struct tm now_tm {};
    localtime_r(&now, &now_tm);
    bool show_year = (tm_info->tm_year != now_tm.tm_year);

    char buf[32];

    switch (get_modified_date_pattern(s_current_lang)) {
    case ModifiedDatePatternGroup::EN:
        // "Mar 09" or "Mar 09 '25"
        if (show_year)
            snprintf(buf, sizeof(buf), "%s %02d '%02d", mon.c_str(), dd, yy % 100);
        else
            snprintf(buf, sizeof(buf), "%s %02d", mon.c_str(), dd);
        break;
    case ModifiedDatePatternGroup::DE:
        // "09. Mär." or "09. Mär. '25"
        if (show_year)
            snprintf(buf, sizeof(buf), "%02d. %s '%02d", dd, mon.c_str(), yy % 100);
        else
            snprintf(buf, sizeof(buf), "%02d. %s", dd, mon.c_str());
        break;
    case ModifiedDatePatternGroup::ROMANCE_RU:
        // "09 mars" or "09 mars '25"
        if (show_year)
            snprintf(buf, sizeof(buf), "%02d %s '%02d", dd, mon.c_str(), yy % 100);
        else
            snprintf(buf, sizeof(buf), "%02d %s", dd, mon.c_str());
        break;
    case ModifiedDatePatternGroup::CJK:
        // "3/9" or "3/9/25"
        if (show_year)
            snprintf(buf, sizeof(buf), "%d/%d/%02d", mm, dd, yy % 100);
        else
            snprintf(buf, sizeof(buf), "%d/%d", mm, dd);
        break;
    }

    return buf;
}

bool locale_default_24h(const std::string& lang_code) {
    // English and unknown languages default to 12-hour format
    if (lang_code == "en" || k_locale_map.find(lang_code) == k_locale_map.end()) {
        return false;
    }
    return true;
}

} // namespace helix::ui
