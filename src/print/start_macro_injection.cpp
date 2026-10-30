// SPDX-License-Identifier: GPL-3.0-or-later
#include "start_macro_injection.h"

#include "json_utils.h"

#include "hv/json.hpp"

namespace helix {

namespace {

/// A config option that, set true, makes the firmware inject its start macro.
struct InjectionOption {
    const char* section;
    const char* option;
};

constexpr InjectionOption kInjectionOptions[] = {
    // OpenCreator's Kalico (FlashForge Creator 5): virtual_sdcard runs
    // C5_PRINT_START from the file's head before streaming it.
    {"virtual_sdcard", "auto_creator5_start"},
};

} // namespace

bool firmware_runs_start_macro_before_file(const json& configfile_settings) {
    for (const auto& opt : kInjectionOptions) {
        const json* section = json_util::find_member(configfile_settings, opt.section);
        const json* value = section ? json_util::find_member(*section, opt.option) : nullptr;
        if (value && value->is_boolean() && value->get<bool>()) {
            return true;
        }
    }
    return false;
}

} // namespace helix
