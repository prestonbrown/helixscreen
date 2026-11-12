// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/temperature_units.h"

#include <cstdint>
#include <string>

namespace helix {

/// The order a date's day, month and year are written in. Stored as this int in settings.
enum class DateFormat : uint8_t { DMY = 0, MDY = 1, YMD = 2 };

/// The date order a language writes: Chinese and Japanese year first, the other
/// offered languages but English day first. English and any unknown language read
/// month first.
DateFormat default_date_format_for_language(const std::string& language);

/// What a region reads by default.
struct RegionDefaults {
    units::TempUnit temp_unit;
    DateFormat date_format;
};

/// Defaults for a language and an IANA timezone ("" while it follows the system).
/// A United States zone reads °F and month-first. Elsewhere temperatures read °C,
/// and an English speaker in a known non-US zone writes the day first. Without a
/// zone the language alone decides the date order.
RegionDefaults region_defaults(const std::string& language, const std::string& timezone_iana);

/// Whether @p timezone_iana is one of the United States zones.
bool is_us_timezone(const std::string& timezone_iana);

} // namespace helix
