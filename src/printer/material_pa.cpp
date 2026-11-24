// SPDX-License-Identifier: GPL-3.0-or-later

#include "material_pa.h"

#include "material_z_offset.h"

#include <spdlog/fmt/fmt.h>

namespace helix::material_pa {
namespace {

std::string num(float v) {
    return fmt::format("{:.6f}", v);
}

} // namespace

Entry linear_entry(float k) {
    Entry e;
    e.model = pa::ModelKind::Linear;
    e.advance = k;
    return e;
}

Entry nonlinear_entry(const pa::NonlinearValues& v) {
    Entry e;
    e.model = v.model == pa::ModelKind::Recipr ? pa::ModelKind::Recipr : pa::ModelKind::Tanh;
    e.advance = v.linear_advance;
    e.offset = v.nonlinear_offset;
    e.velocity = v.linearization_velocity;
    e.time_offset = v.time_offset;
    e.smooth_time = v.smooth_time;
    return e;
}

std::optional<std::string> storage_key(std::string_view material) {
    auto key = material_zoffset::helix_key(material);
    // An empty material is the Z offset's DEFAULT; pressure advance has no
    // such entry, the config is its default.
    if (!key || *key == material_zoffset::kDefaultKey) {
        return std::nullopt;
    }
    return key;
}

std::string var_name(std::string_view key, std::string_view field) {
    return fmt::format("{}{}_{}", kVarPrefix, key, field);
}

std::string save_gcode(std::string_view material, const Entry& entry) {
    const auto key = storage_key(material);
    if (!key) {
        return {};
    }
    std::string g;
    const auto save = [&g](const std::string& var, const std::string& value) {
        g += fmt::format("SAVE_VARIABLE VARIABLE={} VALUE={}\n", var, value);
    };
    save(var_name(*key, "advance"), num(entry.advance));
    const bool nonlinear =
        entry.model == pa::ModelKind::Tanh || entry.model == pa::ModelKind::Recipr;
    if (nonlinear) {
        save(var_name(*key, "offset"), num(entry.offset));
        save(var_name(*key, "velocity"), num(entry.velocity));
        save(var_name(*key, "time_offset"), num(entry.time_offset));
        save(var_name(*key, "smooth_time"), num(entry.smooth_time));
    }
    // Quoted twice: the shell-style split takes the outer quotes and the
    // stored value is read as a Python literal.
    save(var_name(*key, "model"), fmt::format("'\"{}\"'", pa::model_name(entry.model)));
    g += enable_gcode(true);
    return g;
}

std::string enable_gcode(bool on) {
    return fmt::format("SAVE_VARIABLE VARIABLE={} VALUE={}", kEnabledVar, on ? 1 : 0);
}

} // namespace helix::material_pa
