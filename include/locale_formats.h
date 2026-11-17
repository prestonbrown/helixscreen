// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct tm;

namespace helix::locale {

/// Call when language setting changes. Attempts setlocale(LC_TIME) for the
/// new language and probes strftime. Caches whether system locale is active
/// or fallback tables are needed.
void locale_set_language(const std::string& lang_code);

/** @brief The active language code ("en", "de", ...), as last set */
std::string locale_current_language();

/** @brief True when names come from the system's strftime() rather than the built-in tables */
bool locale_uses_system_locale();

/**
 * @brief Subscribe to language changes; the id removes the listener
 *
 * Listeners run at the end of locale_set_language, after the active language
 * and the formatting tables are updated, with the new code.
 */
uint64_t locale_add_language_listener(std::function<void(const std::string&)> fn);
void locale_remove_language_listener(uint64_t id);

/// Format a date string using locale-appropriate order and translated names.
/// Uses cached locale state from locale_set_language().
/// Examples: "Fri, Feb 28" (en), "ven., 28 févr." (fr), "2/28 (金)" (ja)
std::string format_localized_date(const struct tm* tm_info);

/// Format a full date+time string for file modified timestamps.
/// Respects both language (date order/names) and time format (12h/24h).
/// Examples: "Feb 28 2:30 PM" (en), "28 févr. 14:30" (fr)
std::string format_localized_modified_date(const struct tm* tm_info);

/// The date half of format_localized_modified_date(), without the time.
/// Examples: "Feb 28" (en), "28. Feb." (de), "28 févr." (fr), "2/28" (ja)
std::string format_localized_month_day(const struct tm* tm_info);

/// Format a short date (no time) for compact displays like card metadata.
/// Examples: "Mar 09" (en), "09. Mär." (de), "3/9" (ja)
std::string format_localized_short_date(const struct tm* tm_info);

/// Localized weekday abbreviation ("Mon", "lun.", "月") from tm_wday.
std::string format_localized_weekday_abbr(const struct tm* tm_info);

/// Long date with the month's full name and the year, from tm_mday, tm_mon and tm_year.
/// Examples: "October 21, 2019" (en), "21. Oktober 2019" (de), "21 de octubre de 2019" (es),
/// "2019年10月21日" (ja)
std::string format_localized_long_date(const struct tm* tm_info);

/// Localized full weekday name ("Monday", "lundi", "月曜日") from tm_wday.
std::string format_localized_weekday(const struct tm* tm_info);

/// Get the default time format for a language (true = 24h, false = 12h)
bool locale_default_24h(const std::string& lang_code);

} // namespace helix::locale
