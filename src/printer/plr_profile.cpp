// SPDX-License-Identifier: GPL-3.0-or-later
#include "plr_profile.h"

#include "json_utils.h"

#include <algorithm>
#include <cmath>

#include "hv/json.hpp"

namespace helix {

namespace {

constexpr int kMaxTier = 3;

const char* backend_name(PlrBackendKind b) {
    switch (b) {
    case PlrBackendKind::QidiPrintState:
        return "qidi_print_state";
    case PlrBackendKind::Vendor:
        return "vendor";
    case PlrBackendKind::Generic:
        break;
    }
    return "generic";
}

const char* storage_name(StorageHint s) {
    switch (s) {
    case StorageHint::Emmc:
        return "emmc";
    case StorageHint::Sd:
        return "sd";
    case StorageHint::Nand:
        return "nand";
    case StorageHint::Ssd:
        return "ssd";
    case StorageHint::Unknown:
        break;
    }
    return "unknown";
}

std::optional<PlrBackendKind> parse_backend(const std::string& s) {
    if (s == "generic") {
        return PlrBackendKind::Generic;
    }
    if (s == "qidi_print_state") {
        return PlrBackendKind::QidiPrintState;
    }
    if (s == "vendor") {
        return PlrBackendKind::Vendor;
    }
    return std::nullopt;
}

std::optional<StorageHint> parse_storage(const std::string& s) {
    for (StorageHint h : {StorageHint::Emmc, StorageHint::Sd, StorageHint::Nand, StorageHint::Ssd,
                          StorageHint::Unknown}) {
        if (s == storage_name(h)) {
            return h;
        }
    }
    return std::nullopt;
}

/// A finite number in [lo, hi] under @p key, else nullopt.
std::optional<float> bounded_number(const nlohmann::json& obj, const char* key, float lo,
                                    float hi) {
    const nlohmann::json* v = json_util::find_member(obj, key);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    const double d = v->get<double>();
    if (!std::isfinite(d) || d < lo || d > hi) {
        return std::nullopt;
    }
    return static_cast<float>(d);
}

std::optional<std::string> nonempty_string(const nlohmann::json& obj, const char* key) {
    const nlohmann::json* v = json_util::find_member(obj, key);
    if (v == nullptr || !v->is_string() || v->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    return v->get<std::string>();
}

} // namespace

std::optional<PlrDbBlock> plr_parse_db_block(const nlohmann::json& printer_entry) {
    const nlohmann::json* block = json_util::find_member(printer_entry, "plr");
    if (block == nullptr || !block->is_object()) {
        return std::nullopt;
    }
    PlrDbBlock db;
    if (const nlohmann::json* t = json_util::find_member(*block, "tier");
        t != nullptr && t->is_number_integer()) {
        const int tier = t->get<int>();
        if (tier >= 0 && tier <= kMaxTier) {
            db.tier = tier;
        }
    }
    if (auto b = parse_backend(json_util::safe_string(*block, "backend", ""))) {
        db.backend = *b;
    }
    if (auto s = parse_storage(json_util::safe_string(*block, "storage", ""))) {
        db.storage = *s;
    }
    if (const nlohmann::json* z = json_util::find_member(*block, "z_holds_unpowered");
        z != nullptr && z->is_boolean()) {
        db.z_holds_unpowered = z->get<bool>();
    }
    if (auto g = nonempty_string(*block, "xy_home_gcode")) {
        db.xy_home_gcode = *g;
    }
    if (auto lift = bounded_number(*block, "z_lift_mm", 0.0f, 50.0f)) {
        db.z_lift_mm = *lift;
    }
    if (auto purge = nonempty_string(*block, "purge_gcode")) {
        db.purge_gcode = *purge;
    }
    if (auto skip = bounded_number(*block, "resume_skip_fraction", 0.0f, 1.0f)) {
        db.resume_skip_fraction = *skip;
    }
    if (auto reason = nonempty_string(*block, "reason")) {
        db.reason = *reason;
    }
    if (const nlohmann::json* mv = json_util::find_member(*block, "macro_vars");
        mv != nullptr && mv->is_object()) {
        for (auto it = mv->begin(); it != mv->end(); ++it) {
            if (!it.value().is_array()) {
                continue;
            }
            std::vector<std::string> vars;
            for (const auto& var : it.value()) {
                if (var.is_string()) {
                    vars.push_back(var.get<std::string>());
                }
            }
            db.macro_vars[it.key()] = std::move(vars);
        }
    }
    return db;
}

PlrProfile plr_resolve_profile(const std::optional<PlrDbBlock>& db, const PlrUserOverride& user,
                               const PlrHomingCheck& homing, bool plugin_guards_unlisted) {
    PlrProfile p;
    if (!db && plugin_guards_unlisted) {
        p.tier = kPlrUnlistedTier;
    }
    if (db) {
        p.listed = true;
        p.reason = db->reason;
        p.tier = db->tier;
        // Trusting the held Z is only right where the steppers hold unpowered.
        if (p.tier == 2 && !db->z_holds_unpowered) {
            p.tier = 1;
        }
        p.backend = db->backend;
        p.storage_hint = db->storage;
        p.z_holds_unpowered = db->z_holds_unpowered;
        p.xy_home_gcode = db->xy_home_gcode;
        p.z_lift_mm = db->z_lift_mm;
        p.purge_gcode = db->purge_gcode;
        p.resume_skip_fraction = db->resume_skip_fraction;
        p.macro_vars = db->macro_vars;
    }
    if (user.xy_home_gcode && !user.xy_home_gcode->empty()) {
        p.xy_home_gcode = *user.xy_home_gcode;
    }
    if (user.purge_gcode && !user.purge_gcode->empty()) {
        p.purge_gcode = *user.purge_gcode;
    }
    if (user.resume_skip_fraction && std::isfinite(*user.resume_skip_fraction)) {
        p.resume_skip_fraction = std::clamp(*user.resume_skip_fraction, 0.0f, 1.0f);
    }
    if (user.tier) {
        const int wanted = *user.tier;
        const bool homing_ok = (homing.known && homing.xy_home_safe) ||
                               homing.has_xy_home_override ||
                               (user.xy_home_gcode && !user.xy_home_gcode->empty());
        // Trusting the held Z is a fact about the printer, not a preference.
        const int ceiling = (db && db->z_holds_unpowered) ? 2 : 1;
        if (wanted <= p.tier && wanted >= 0) {
            p.tier = wanted;
        } else if (wanted == 1 || wanted == 2) {
            p.tier = homing_ok ? std::min(wanted, ceiling) : 0;
        }
    }
    p.user_enabled = user.enabled;
    return p;
}

nlohmann::json plr_profile_json(const PlrProfile& profile) {
    nlohmann::json macro_vars = nlohmann::json::object();
    for (const auto& [macro, vars] : profile.macro_vars) {
        macro_vars[macro] = vars;
    }
    return nlohmann::json{
        {"tier", profile.tier},
        {"backend", backend_name(profile.backend)},
        {"storage_hint", storage_name(profile.storage_hint)},
        {"z_holds_unpowered", profile.z_holds_unpowered},
        {"xy_home_gcode", profile.xy_home_gcode},
        {"z_lift_mm", profile.z_lift_mm},
        {"purge_gcode", profile.purge_gcode ? nlohmann::json(*profile.purge_gcode) : nullptr},
        {"resume_skip_fraction", profile.resume_skip_fraction},
        {"macro_vars", macro_vars},
        {"listed", profile.listed},
        {"reason", profile.reason ? nlohmann::json(*profile.reason) : nullptr},
    };
}

nlohmann::json plr_user_json(bool enabled) {
    return nlohmann::json{{"enabled", enabled}};
}

} // namespace helix
