// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_devices.h"

#include "device_display_name.h"
#include "i18n_plural.h"
#include "lvgl.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace helix::led {

namespace {

constexpr const char* kChamberLightNames[] = {"chamber_light", "chamber_LED", "case_light",
                                              "caselight"};

/// "neopixel chamber_light" -> "chamber_light"; an id without a prefix is its own name.
std::string object_name(const std::string& id) {
    const auto space = id.find(' ');
    return space == std::string::npos ? id : id.substr(space + 1);
}

/// Lower-cased, with one trailing 's' dropped: "case_lights" and "case_light" name one light.
std::string light_name_key(const std::string& name) {
    std::string key = helix::text_io::to_lower(name);
    if (key.size() > 1 && key.back() == 's') {
        key.pop_back();
    }
    return key;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

std::string resolve_chamber_light(const std::vector<LedStripInfo>& devices,
                                  const std::string& fallback) {
    for (const char* wanted : kChamberLightNames) {
        const std::string want = light_name_key(wanted);
        for (const auto& d : devices) {
            if (d.backend != LedBackendType::NATIVE && d.backend != LedBackendType::OUTPUT_PIN) {
                continue;
            }
            if (light_name_key(object_name(d.id)) == want) {
                return d.id;
            }
        }
    }
    return fallback;
}

std::vector<std::string> resolve_light_targets(const std::string& key,
                                               const std::vector<std::string>& switchable,
                                               const std::string& chamber) {
    if (key == kLightButtonAll) {
        return switchable;
    }
    if (!key.empty() && contains(switchable, key)) {
        return {key};
    }
    if (chamber.empty()) {
        return {};
    }
    return {chamber};
}

std::vector<std::string> toggle_target(const std::vector<std::string>& current,
                                       const std::string& id) {
    std::vector<std::string> out = current;
    const auto it = std::find(out.begin(), out.end(), id);
    if (it == out.end()) {
        out.push_back(id);
    } else if (out.size() > 1) {
        out.erase(it);
    }
    return out;
}

std::vector<std::string> union_light_targets(const std::vector<std::string>& keys,
                                             const std::vector<std::string>& switchable,
                                             const std::string& chamber) {
    if (keys.empty()) {
        return resolve_light_targets("", switchable, chamber);
    }
    std::vector<std::string> out;
    for (const auto& key : keys) {
        for (auto& id : resolve_light_targets(key, switchable, chamber)) {
            if (!contains(out, id)) {
                out.push_back(std::move(id));
            }
        }
    }
    return out;
}

SelectionMigration plan_selection_migration(const std::vector<std::string>& selected,
                                            const std::vector<std::string>& switchable) {
    SelectionMigration m;
    m.auto_state_strips = selected;
    if (selected.size() == 1) {
        m.light_button = selected.front();
        return m;
    }
    const bool every =
        !selected.empty() && !switchable.empty() &&
        std::all_of(switchable.begin(), switchable.end(),
                    [&selected](const std::string& id) { return contains(selected, id); });
    if (every) {
        m.light_button = kLightButtonAll;
    }
    return m;
}

std::vector<uint32_t> migrate_color_presets(const std::vector<uint32_t>& saved) {
    const std::vector<uint32_t> pre(std::begin(kPre11DefaultColorPresets),
                                    std::end(kPre11DefaultColorPresets));
    if (saved.empty() || saved == pre) {
        return {std::begin(kDefaultColorPresets), std::end(kDefaultColorPresets)};
    }
    return saved;
}

std::string pick_overlay_focus(const std::string& requested, const std::string& last_focused,
                               const std::string& chamber,
                               const std::vector<std::string>& devices) {
    for (const std::string* want : {&requested, &last_focused, &chamber}) {
        if (!want->empty() && contains(devices, *want)) {
            return *want;
        }
    }
    return devices.empty() ? std::string() : devices.front();
}

std::string device_display_name(const LedStripInfo& device) {
    if (device.backend == LedBackendType::MACRO) {
        return strip_macro_name(device.id);
    }
    return helix::prettify_name(object_name(device.id));
}

std::string macro_device_note(const LedMacroInfo& macro) {
    auto or_dash = [](const std::string& s) { return s.empty() ? std::string("—") : s; };
    switch (macro.type) {
    case MacroLedType::ON_OFF:
        return fmt::format(fmt::runtime(lv_tr("ON: {} | OFF: {}")), or_dash(macro.on_macro),
                           or_dash(macro.off_macro));
    case MacroLedType::TOGGLE:
        return fmt::format(fmt::runtime(lv_tr("TOGGLE: {}")), or_dash(macro.toggle_macro));
    case MacroLedType::PRESET:
        if (macro.presets.empty()) {
            return lv_tr("No presets configured");
        }
        return fmt::format(
            fmt::runtime(ui::tr_plural("{} preset", "{} presets", macro.presets.size())),
            macro.presets.size());
    }
    return {};
}

bool next_power_on(const std::vector<PowerState>& states, bool last_sent_on) {
    bool any_off = false;
    for (auto s : states) {
        if (s == PowerState::On) {
            return false;
        }
        any_off = any_off || s == PowerState::Off;
    }
    return any_off ? true : !last_sent_on;
}

} // namespace helix::led
