// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "display_settings_manager.h"
#include "locale_formats.h"
#include "system_settings_manager.h"

#include <string>

namespace helix::test {

/// Sets the app language for one scope and puts English back after it.
struct LanguageScope {
    explicit LanguageScope(const std::string& lang) {
        set(lang);
    }
    ~LanguageScope() {
        set("en");
    }
    LanguageScope(const LanguageScope&) = delete;
    LanguageScope& operator=(const LanguageScope&) = delete;
    static void set(const std::string& lang) {
        helix::SystemSettingsManager::instance().set_language(lang);
        helix::ui::locale_set_language(lang);
    }
};

/// Pins the Time Format setting for one scope and restores what it was.
struct TimeFormatScope {
    explicit TimeFormatScope(helix::TimeFormat format)
        : saved(helix::DisplaySettingsManager::instance().get_time_format()) {
        helix::DisplaySettingsManager::instance().set_time_format(format);
    }
    ~TimeFormatScope() {
        helix::DisplaySettingsManager::instance().set_time_format(saved);
    }
    TimeFormatScope(const TimeFormatScope&) = delete;
    TimeFormatScope& operator=(const TimeFormatScope&) = delete;
    helix::TimeFormat saved;
};

} // namespace helix::test
