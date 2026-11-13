// SPDX-License-Identifier: GPL-3.0-or-later
#include "start_macro_injection.h"

#include "core/text_io.h"
#include "json_utils.h"

#include <string>

#include "hv/json.hpp"

namespace helix {

namespace {

/// OpenCreator's Kalico (FlashForge Creator 5): `[virtual_sdcard]
/// auto_creator5_start: True` runs C5_PRINT_START from the file's head before
/// streaming it.
bool opencreator_auto_start(const json& settings) {
    const json* section = json_util::find_member(settings, "virtual_sdcard");
    const json* value = section ? json_util::find_member(*section, "auto_creator5_start") : nullptr;
    return value && value->is_boolean() && value->get<bool>();
}

/// A gcode_macro variable Klipper hands back as the text it was configured
/// with, read as Jinja would: 0 and False are off.
bool macro_variable_is_off(const json& value) {
    if (value.is_boolean()) {
        return !value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() == 0.0;
    }
    if (!value.is_string()) {
        return false;
    }
    const std::string text = text_io::to_lower(text_io::trim(value.get_ref<const std::string&>()));
    return text == "0" || text == "false";
}

/// Reforge's `[ff_print]` (FlashForge Creator 5): it wraps SDCARD_PRINT_FILE
/// and M23 and runs its before_macro (FF_BEFORE_PRINT_START unless set) ahead
/// of the file. The shipped macro runs START_PRINT only while its
/// variable_prepare is on; off, the slicer profile calls START_PRINT from the
/// file and the position leaves 0 before the start sequence does.
bool reforge_ff_print(const json& settings) {
    const json* section = json_util::find_member(settings, "ff_print");
    if (section == nullptr) {
        return false;
    }
    const std::string configured = json_util::safe_string(*section, "before_macro", "");
    const std::string_view macro = text_io::trim(configured);
    const std::string body_name =
        "gcode_macro " + text_io::to_lower(macro.empty() ? "FF_BEFORE_PRINT_START" : macro);
    const json* body = json_util::find_member(settings, body_name.c_str());
    const json* prepare = body ? json_util::find_member(*body, "variable_prepare") : nullptr;
    return !(prepare && macro_variable_is_off(*prepare));
}

using InjectionRow = bool (*)(const json& configfile_settings);

constexpr InjectionRow kInjectionRows[] = {
    opencreator_auto_start,
    reforge_ff_print,
};

} // namespace

bool firmware_runs_start_macro_before_file(const json& configfile_settings) {
    for (const auto row : kInjectionRows) {
        if (row(configfile_settings)) {
            return true;
        }
    }
    return false;
}

} // namespace helix
