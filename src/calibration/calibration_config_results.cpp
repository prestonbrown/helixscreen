// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_config_results.h"

#include "core/text_io.h"
#include "hardware_object_table.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
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

// The Z probe's offset, from its model for a probe with one. Klipper requires
// z_offset on most probes, so 0, the value a stock config starts from, counts
// as untuned. A probe-type section with no offset of its own does not hide
// the next one.
void z_offset(const json& settings, ConfigResults& out) {
    static const json kNameOnly = json::object();
    for (const auto& [name, body] : settings.items()) {
        if (!body.is_object() ||
            hw::classify_section(name, kNameOnly).kind != hw::ObjectKind::Probe) {
            continue;
        }
        const json* value = nullptr;
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
        if (value && value->get<double>() != 0.0) {
            out["z_offset"] = format_z_offset(value->get<double>());
            return;
        }
    }
}

// The hotend's control, when it is a tuned one: watermark has nothing to tune.
// Klipper requires control on every heater, so a stock config shows its
// vendor's values here.
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
        return format_shaper_axis(type, freq->get<double>());
    };
    const std::string joined = join_shaper_axes(axis('x'), axis('y'));
    if (!joined.empty()) {
        out["input_shaper"] = joined;
    }
}

/// Three significant figures, without an exponent for large values.
std::string sig3(double v) {
    const double a = v < 0 ? -v : v;
    if (a >= 100.0) {
        return fmt::format("{:.0f}", v);
    }
    if (a >= 10.0) {
        return fmt::format("{:.1f}", v);
    }
    return fmt::format("{:.2f}", v);
}

/// A pending value: Klipper stages each as the text it will write.
std::optional<double> pending_number(const json& section, const char* option) {
    if (!section.is_object()) {
        return std::nullopt;
    }
    const auto it = section.find(option);
    if (it == section.end()) {
        return std::nullopt;
    }
    if (it->is_number()) {
        return it->get<double>();
    }
    return it->is_string() ? text_io::parse_double(it->get<std::string>()) : std::nullopt;
}

} // namespace

std::string format_z_offset(double mm) {
    return fmt::format("{:.3f} mm", mm);
}

std::string format_shaper_axis(std::string_view type, double freq_hz) {
    return fmt::format("{} {:.1f}", text_io::to_upper(type), freq_hz);
}

std::string join_shaper_axes(const std::string& x, const std::string& y) {
    if (x.empty() && y.empty()) {
        return {};
    }
    return (x.empty() ? y : y.empty() ? x : fmt::format("{} · {}", x, y)) + " Hz";
}

std::string format_pid(double kp, double ki, double kd) {
    return fmt::format("{} / {} / {}", sig3(kp), sig3(ki), sig3(kd));
}

std::string format_mpc(double block_heat_capacity) {
    return fmt::format("MPC {:.1f} J/K", block_heat_capacity);
}

std::string format_mesh_range(double range_mm) {
    return fmt::format(fmt::runtime(lv_tr("{:.2f} mm range")), range_mm);
}

std::optional<double> mesh_points_range(const json& points) {
    std::optional<double> lo;
    std::optional<double> hi;
    if (!points.is_array()) {
        return std::nullopt;
    }
    for (const auto& row : points) {
        if (!row.is_array()) {
            continue;
        }
        for (const auto& z : row) {
            if (!z.is_number()) {
                continue;
            }
            const double v = z.get<double>();
            lo = lo ? std::min(*lo, v) : v;
            hi = hi ? std::max(*hi, v) : v;
        }
    }
    if (!lo) {
        return std::nullopt;
    }
    return *hi - *lo;
}

std::optional<double> staged_z_offset(const json& pending_items) {
    if (!pending_items.is_object()) {
        return std::nullopt;
    }
    static const json kNameOnly = json::object();
    for (const auto& [name, body] : pending_items.items()) {
        const bool probe = hw::classify_section(name, kNameOnly).kind == hw::ObjectKind::Probe;
        bool model = false;
        for (const auto& row : kModelOffsetRows) {
            model = model || name.rfind(std::string(row.model_prefix), 0) == 0;
        }
        if (probe || model) {
            for (const char* option : {"z_offset", "model_offset"}) {
                if (const auto v = pending_number(body, option)) {
                    return v;
                }
            }
        }
    }
    const auto stepper = pending_items.find("stepper_z");
    return stepper != pending_items.end() ? pending_number(*stepper, "position_endstop")
                                          : std::nullopt;
}

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
