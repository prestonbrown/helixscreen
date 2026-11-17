// SPDX-License-Identifier: GPL-3.0-or-later

#include "region_defaults.h"

#include "core/locale_code.h"

#include <array>
#include <atomic>

namespace helix {

namespace {
// The US zones the timezone dropdown offers, plus the aliases a host may report.
constexpr std::array<const char*, 9> kUsZones = {
    "America/New_York", "America/Chicago",     "America/Denver",
    "America/Phoenix",  "America/Los_Angeles", "America/Anchorage",
    "Pacific/Honolulu", "America/Detroit",     "America/Boise",
};
} // namespace

namespace {
constexpr int8_t kNoSavedDate = -1;
std::atomic<int8_t> g_saved_date_format{kNoSavedDate};
} // namespace

std::optional<DateFormat> saved_date_format() {
    const int8_t v = g_saved_date_format.load(std::memory_order_relaxed);
    if (v == kNoSavedDate) {
        return std::nullopt;
    }
    return static_cast<DateFormat>(v);
}

void set_saved_date_format(std::optional<DateFormat> format) {
    g_saved_date_format.store(format ? static_cast<int8_t>(*format) : kNoSavedDate,
                              std::memory_order_relaxed);
}

namespace {
/// What a language writes by default. A regional variant ("en-GB") has its own row only
/// where it differs from its base language; any other variant reads the base's row.
struct LanguageDefaults {
    const char* code;
    DateFormat date_format;
    bool clock_24h;
};

constexpr LanguageDefaults kLanguageDefaults[] = {
    {"en", DateFormat::MDY, false}, {"en-GB", DateFormat::DMY, true}, {"de", DateFormat::DMY, true},
    {"fr", DateFormat::DMY, true},  {"es", DateFormat::DMY, true},    {"it", DateFormat::DMY, true},
    {"pt", DateFormat::DMY, true},  {"ru", DateFormat::DMY, true},    {"zh", DateFormat::YMD, true},
    {"ja", DateFormat::YMD, true},
};

const LanguageDefaults* find_language_defaults(std::string_view language) {
    for (const std::string_view code : {language, locale_base_language(language)}) {
        for (const auto& row : kLanguageDefaults) {
            if (code == row.code) {
                return &row;
            }
        }
    }
    return nullptr;
}
} // namespace

DateFormat default_date_format_for_language(const std::string& language) {
    const LanguageDefaults* row = find_language_defaults(language);
    return row ? row->date_format : DateFormat::MDY;
}

bool default_24h_clock_for_language(const std::string& language) {
    const LanguageDefaults* row = find_language_defaults(language);
    return row && row->clock_24h;
}

bool is_us_timezone(const std::string& timezone_iana) {
    if (timezone_iana.rfind("US/", 0) == 0) {
        return true;
    }
    for (const char* zone : kUsZones) {
        if (timezone_iana == zone) {
            return true;
        }
    }
    return false;
}

RegionDefaults region_defaults(const std::string& language, const std::string& timezone_iana) {
    if (is_us_timezone(timezone_iana)) {
        return {units::TempUnit::Fahrenheit, DateFormat::MDY};
    }
    DateFormat date = default_date_format_for_language(language);
    if (date == DateFormat::MDY && !timezone_iana.empty()) {
        date = DateFormat::DMY;
    }
    return {units::TempUnit::Celsius, date};
}

} // namespace helix
