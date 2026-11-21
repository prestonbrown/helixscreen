// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_config_results.h"

#include "core/text_io.h"
#include "hardware_object_table.h"

#include <fmt/format.h>
#include <string_view>

#include "hv/json.hpp"

namespace helix::calibration {

namespace {

using json = nlohmann::json;

const json* section_of(const json& settings, std::string_view name) {
    const auto it = settings.find(std::string(name));
    return it != settings.end() && it->is_object() ? &*it : nullptr;
}

const json* number_in(const json* section, std::string_view option) {
    if (!section) {
        return nullptr;
    }
    const auto it = section->find(std::string(option));
    return it != section->end() && it->is_number() ? &*it : nullptr;
}

// Probes that keep the offset in a model section of their own, one row each:
// the model section's prefix, the option, and where the active model's name is
// configured (empty: always "default").
struct ModelOffsetRow {
    std::string_view probe;
    std::string_view model_prefix;
    std::string_view offset_option;
    std::string_view active_model_option;
};
constexpr ModelOffsetRow kModelOffsetRows[] = {
    // beacon3d/beacon_klipper beacon.py#BeaconModel.save; contact mode's offset
    // is fixed at zero, so only a proximity model holds one.
    {"beacon", "beacon model ", "model_offset", "default_model_name"},
    // Cartographer3D/cartographer3d-plugin interfaces/configuration.py:
    // touch first, as CARTOGRAPHER_TOUCH_CALIBRATE is the paper-free test.
    {"cartographer", "cartographer touch_model ", "z_offset", ""},
    {"cartographer", "cartographer scan_model ", "z_offset", ""},
};

const json* model_offset(const json& settings, const ModelOffsetRow& row) {
    std::string model = "default";
    if (!row.active_model_option.empty()) {
        if (const json* probe = section_of(settings, row.probe)) {
            const auto it = probe->find(std::string(row.active_model_option));
            if (it != probe->end() && it->is_string()) {
                model = it->get<std::string>();
            }
        }
    }
    return number_in(section_of(settings, std::string(row.model_prefix) + model),
                     row.offset_option);
}

// The probe's z_offset (from its model, for a probe with one), else stepper_z's
// position_endstop. Klipper allows one Z probe, so the endstop is read only
// without a probe.
void z_offset(const json& settings, ConfigResults& out) {
    static const json kNameOnly = json::object();
    const json* value = nullptr;
    bool has_probe = false;
    for (const auto& [name, body] : settings.items()) {
        if (!body.is_object() ||
            hw::classify_section(name, kNameOnly).kind != hw::ObjectKind::Probe) {
            continue;
        }
        has_probe = true;
        bool modeled = false;
        for (const auto& row : kModelOffsetRows) {
            if (row.probe == name) {
                modeled = true;
                value = value ? value : model_offset(settings, row);
            }
        }
        if (!modeled) {
            value = number_in(&body, "z_offset");
        }
        break;
    }
    if (!has_probe) {
        value = number_in(section_of(settings, "stepper_z"), "position_endstop");
    }
    if (value) {
        out["z_offset"] = fmt::format("{:.3f} mm", value->get<double>());
    }
}

// The hotend's control, when it is a tuned one: watermark has nothing to tune.
void heaters(const json& settings, ConfigResults& out) {
    const json* extruder = section_of(settings, "extruder");
    if (!extruder) {
        return;
    }
    const auto control = extruder->find("control");
    if (control == extruder->end() || !control->is_string()) {
        return;
    }
    const std::string kind = control->get<std::string>();
    if (kind == "pid" || kind == "pid_v" || kind == "mpc") {
        out["heaters"] = text_io::to_upper(kind == "pid_v" ? std::string("pid") : kind);
    }
}

// A saved profile is a [bed_mesh <name>] section; "default" is named first.
void bed_mesh(const json& settings, ConfigResults& out) {
    static constexpr std::string_view kPrefix = "bed_mesh ";
    std::string found;
    for (const auto& [name, body] : settings.items()) {
        if (name.size() > kPrefix.size() && name.compare(0, kPrefix.size(), kPrefix) == 0) {
            found = name.substr(kPrefix.size());
            if (found == "default") {
                break;
            }
        }
    }
    if (!found.empty()) {
        out["bed_mesh"] = fmt::format("profile {}", found);
    }
}

// shaper_freq_<axis> defaults to 0, which disables shaping on that axis.
void input_shaper(const json& settings, ConfigResults& out) {
    const json* shaper = section_of(settings, "input_shaper");
    if (!shaper) {
        return;
    }
    const auto axis = [shaper](char a) -> std::string {
        const json* freq = number_in(shaper, fmt::format("shaper_freq_{}", a));
        if (!freq || freq->get<double>() <= 0.0) {
            return {};
        }
        std::string type = "mzv";
        for (const auto& key : {fmt::format("shaper_type_{}", a), std::string("shaper_type")}) {
            const auto it = shaper->find(key);
            if (it != shaper->end() && it->is_string()) {
                type = it->get<std::string>();
                break;
            }
        }
        return fmt::format("{} {:.1f}", text_io::to_upper(type), freq->get<double>());
    };
    const std::string x = axis('x');
    const std::string y = axis('y');
    if (x.empty() && y.empty()) {
        return;
    }
    out["input_shaper"] = (x.empty() ? y : y.empty() ? x : fmt::format("{} · {}", x, y)) + " Hz";
}

} // namespace

ConfigResults results_in_config(const json& settings) {
    ConfigResults out;
    if (!settings.is_object()) {
        return out;
    }
    z_offset(settings, out);
    heaters(settings, out);
    bed_mesh(settings, out);
    input_shaper(settings, out);
    return out;
}

} // namespace helix::calibration
