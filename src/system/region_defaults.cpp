// SPDX-License-Identifier: GPL-3.0-or-later

#include "region_defaults.h"

#include <array>

namespace helix {

namespace {
// The US zones the timezone dropdown offers, plus the aliases a host may report.
constexpr std::array<const char*, 9> kUsZones = {
    "America/New_York", "America/Chicago",     "America/Denver",
    "America/Phoenix",  "America/Los_Angeles", "America/Anchorage",
    "Pacific/Honolulu", "America/Detroit",     "America/Boise",
};
} // namespace

DateFormat default_date_format_for_language(const std::string& language) {
    if (language == "zh" || language == "ja") {
        return DateFormat::YMD;
    }
    if (language == "de" || language == "fr" || language == "es" || language == "it" ||
        language == "pt" || language == "ru") {
        return DateFormat::DMY;
    }
    return DateFormat::MDY;
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
