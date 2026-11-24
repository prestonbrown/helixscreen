// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_config_results.h"

#include "core/text_io.h"
#include "hardware_object_table.h"
#include "json_utils.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <optional>
#include <string_view>

#include "hv/json.hpp"

namespace helix::calibration {

namespace {

using json = nlohmann::json;

const json* section_of(const json& settings, std::string_view name) {
    return helix::json_util::find_object(settings, std::string(name));
}

const json* number_in(const json* section, std::string_view option) {
    if (!section) {
        return nullptr;
    }
    const json* value = helix::json_util::find_member(*section, std::string(option));
    return value != nullptr && value->is_number() ? value : nullptr;
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
            if (const auto it = helix::json_util::read_field<std::string>(
                    *probe, std::string(row.active_model_option))) {
                model = *it;
            }
        }
    }
    return number_in(section_of(settings, std::string(row.model_prefix) + model),
                     row.offset_option);
}

// The Z probe's offset, from its model for a probe with one. Klipper requires
// z_offset on most probes, so 0, the value a stock config starts from, counts
// as untuned. A probe-type section with no offset of its own does not hide
// the next one; a fallback probe section counts only when no other probe is
// configured (hw::is_fallback_probe_section).
void z_offset(const json& settings, ConfigResults& out) {
    for (const auto& row : kModelOffsetRows) {
        if (section_of(settings, row.probe)) {
            if (const json* value = model_offset(settings, row);
                value && value->get<double>() != 0.0) {
                out["z_offset"] = format_z_offset(value->get<double>());
                return;
            }
        }
    }
    static const json kNameOnly = json::object();
    const auto is_probe = [](const std::string& name) {
        return hw::classify_section(name, kNameOnly).kind == hw::ObjectKind::Probe;
    };
    bool other_probe = false;
    for (const auto& [name, body] : settings.items()) {
        other_probe = other_probe || (is_probe(name) && !hw::is_fallback_probe_section(name));
    }
    for (const auto& [name, body] : settings.items()) {
        const std::string& section = name; // a lambda cannot capture a structured binding
        const bool modeled =
            std::any_of(std::begin(kModelOffsetRows), std::end(kModelOffsetRows),
                        [&section](const auto& row) { return row.probe == section; });
        if (!body.is_object() || !is_probe(name) || modeled ||
            (other_probe && hw::is_fallback_probe_section(name))) {
            continue;
        }
        if (const json* value = number_in(&body, "z_offset");
            value && value->get<double>() != 0.0) {
            out["z_offset"] = format_z_offset(value->get<double>());
            return;
        }
    }
}

// A heater section's control when it is a tuned one, "pid_v" read as "pid";
// watermark has nothing to tune. Klipper refuses a PID heater without its
// gains, but Kalico accepts `control: mpc` before MPC_CALIBRATE has saved the
// model, and runs the heater only once these three are set (control_mpc.py
// is_valid); a model value left unset is absent from the settings.
std::optional<std::string> tuned_control(const json& section) {
    const auto control = helix::json_util::read_field<std::string>(section, "control");
    if (!control) {
        return std::nullopt;
    }
    const std::string kind = *control;
    if (kind == "pid" || kind == "pid_v") {
        return std::string("pid");
    }
    if (kind == "mpc" && section.contains("block_heat_capacity") &&
        section.contains("sensor_responsiveness") && section.contains("ambient_transfer")) {
        return kind;
    }
    return std::nullopt;
}

// The hotend's control, when it is a tuned one. Klipper requires control on
// every heater, so a stock config shows its vendor's values here.
void heaters(const json& settings, ConfigResults& out) {
    const json* extruder = section_of(settings, "extruder");
    if (!extruder) {
        return;
    }
    if (const auto kind = tuned_control(*extruder)) {
        out["heaters"] = text_io::to_upper(*kind);
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
            if (const auto it = helix::json_util::read_field<std::string>(*shaper, key)) {
                type = *it;
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

namespace {
/// What join_shaper_axes() puts between the two axes.
constexpr std::string_view kShaperAxisJoin = " · ";
} // namespace

std::string join_shaper_axes(const std::string& x, const std::string& y) {
    // Each axis names itself: the two rarely share a shaper or a frequency, and
    // a printer shaping one axis must still say which. No unit: with one, both
    // axes do not fit a half-width tile at 800x480.
    const auto labeled = [](char axis, const std::string& shaper) {
        return shaper.empty() ? std::string() : fmt::format("{}: {}", axis, shaper);
    };
    const std::string lx = labeled('X', x);
    const std::string ly = labeled('Y', y);
    return lx.empty() ? ly : ly.empty() ? lx : lx + std::string(kShaperAxisJoin) + ly;
}

std::string shaper_axes_on_lines(std::string_view joined) {
    std::string out(joined);
    const auto at = out.find(kShaperAxisJoin);
    if (at != std::string::npos && out.compare(0, 3, "X: ") == 0 &&
        out.compare(at + kShaperAxisJoin.size(), 3, "Y: ") == 0) {
        out.replace(at, kShaperAxisJoin.size(), "\n");
    }
    return out;
}

std::string format_pid(double kp, double ki, double kd) {
    return fmt::format("{} / {} / {}", sig3(kp), sig3(ki), sig3(kd));
}

std::string format_mpc(double block_heat_capacity) {
    return fmt::format("MPC {:.1f} J/K", block_heat_capacity);
}

std::string format_heater_controls(const std::vector<HeaterControl>& heaters) {
    std::string out;
    for (const auto& h : heaters) {
        const char* who = h.bed ? lv_tr("bed") : lv_tr("nozzle");
        const std::string one = h.control == "mpc"
                                    ? fmt::format(fmt::runtime(lv_tr("{} MPC")), who)
                                    : fmt::format(fmt::runtime(lv_tr("{} PID")), who);
        out += out.empty() ? one : " · " + one;
    }
    return out;
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

std::optional<PidGains> staged_pid(const json& pending_items, const std::string& heater) {
    if (!pending_items.is_object()) {
        return std::nullopt;
    }
    const auto section = pending_items.find(heater);
    if (section == pending_items.end()) {
        return std::nullopt;
    }
    // The config writer lowercases option names; the command names them pid_Kp.
    const auto gain = [&](const char* lower, const char* named) {
        const auto v = pending_number(*section, lower);
        return v ? v : pending_number(*section, named);
    };
    const auto kp = gain("pid_kp", "pid_Kp");
    const auto ki = gain("pid_ki", "pid_Ki");
    const auto kd = gain("pid_kd", "pid_Kd");
    if (!kp || !ki || !kd) {
        return std::nullopt;
    }
    return PidGains{*kp, *ki, *kd};
}

std::optional<PidGains> newly_staged_pid(const json& before, const json& now,
                                         const std::string& heater) {
    const auto gains = staged_pid(now, heater);
    if (!gains) {
        return std::nullopt;
    }
    const auto old = staged_pid(before, heater);
    if (old && old->kp == gains->kp && old->ki == gains->ki && old->kd == gains->kd) {
        return std::nullopt;
    }
    return gains;
}

namespace {

/// A probe section, or a scanning probe's model section: where a Z offset is staged.
bool is_probe_offset_section(const std::string& name) {
    static const json kNameOnly = json::object();
    if (hw::classify_section(name, kNameOnly).kind == hw::ObjectKind::Probe) {
        return true;
    }
    for (const auto& row : kModelOffsetRows) {
        if (name.rfind(std::string(row.model_prefix), 0) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_z_offset_section(const std::string& name) {
    return name == "stepper_z" || is_probe_offset_section(name);
}

std::optional<double> staged_z_offset(const json& pending_items) {
    if (!pending_items.is_object()) {
        return std::nullopt;
    }
    for (const auto& [name, body] : pending_items.items()) {
        if (is_probe_offset_section(name)) {
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

HeaterControls heater_controls_in_config(const json& settings) {
    HeaterControls out;
    if (!settings.is_object()) {
        return out;
    }
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        if (const auto kind = tuned_control(*it)) {
            out[it.key()] = *kind;
        }
    }
    return out;
}

} // namespace helix::calibration
