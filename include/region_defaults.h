// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/temperature_units.h"

#include <cstdint>
#include <optional>
#include <string>

namespace helix {

/// The order a date's day, month and year are written in. Stored as this int in settings.
enum class DateFormat : uint8_t { DMY = 0, MDY = 1, YMD = 2 };

/// The date order a language writes: Chinese and Japanese year first, the other
/// offered languages but US English day first. US English ("en") and any unknown
/// language read month first; British English ("en-GB") writes the day first. A
/// regional variant without its own default takes its base language's.
DateFormat default_date_format_for_language(const std::string& language);

/// Whether a language's clock reads 24-hour by default: every offered language but
/// US English ("en"), which reads 12-hour, as does an unknown or empty language.
bool default_24h_clock_for_language(const std::string& language);

/// The date order the user saved, or nullopt while none is saved (dates then follow
/// the language). Kept apart from Config and the settings subject because dates are
/// formatted on the WebSocket thread too; DisplaySettingsManager keeps it current.
std::optional<DateFormat> saved_date_format();
void set_saved_date_format(std::optional<DateFormat> format);

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
