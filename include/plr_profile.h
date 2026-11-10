// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The power-loss recovery profile the HelixPrint plugin acts on. The app
// resolves it from the printer database's `plr` block and the user's overrides
// and writes it to Moonraker (`helix-screen` / `plr_profile`), so the plugin has
// it with the screen off. The user's on/off switch is a separate record
// (`helix-screen` / `plr_user` = {"enabled": bool}), the only one the plugin
// reads for saving. Pure: no LVGL, no network.

#include "json_fwd.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// Moonraker database records the plugin reads.
inline constexpr const char* kPlrDbNamespace = "helix-screen";
inline constexpr const char* kPlrProfileKey = "plr_profile";
inline constexpr const char* kPlrUserKey = "plr_user";

enum class PlrBackendKind { Generic, QidiPrintState, Vendor };
enum class StorageHint { Emmc, Sd, Nand, Ssd, Unknown };

/// Whether `G28 X Y` is safe on this printer. The plugin re-checks it
/// against configfile; the app's answer only gates a user tier raise, and an
/// answer the app does not have (`known` false) counts as unsafe for that.
struct PlrHomingCheck {
    bool xy_home_safe = false;
    bool has_xy_home_override = false;
    bool known = false;
};

/// The tier a printer without a `plr` block resolves to: assisted Z, where
/// the user jogs the nozzle onto the part top. Known-bad designs and printers
/// whose firmware owns recovery carry a block at tier 0 instead.
inline constexpr int kPlrUnlistedTier = 1;

/// The printer database's `plr` block.
struct PlrDbBlock {
    int tier = 0;
    PlrBackendKind backend = PlrBackendKind::Generic;
    StorageHint storage = StorageHint::Unknown;
    bool z_holds_unpowered = false;
    std::string xy_home_gcode = "G28 X Y";
    float z_lift_mm = 5.0f;
    std::optional<std::string> purge_gcode;
    float resume_skip_fraction = 0.0f;
    /// Macro variables to restore on resume: macro name -> variable names.
    std::map<std::string, std::vector<std::string>> macro_vars;
    /// Why the model sits at its tier, as a plugin reason code
    /// (`gantry_drops`, `delta_kinematics`, `firmware_recovery`); the plugin
    /// reports it when the tier is 0.
    std::optional<std::string> reason;
};

/// The user's settings (`plr_user` plus the overrides the profile folds in).
struct PlrUserOverride {
    std::optional<bool> enabled;
    std::optional<int> tier;
    std::optional<std::string> xy_home_gcode, purge_gcode;
    std::optional<float> resume_skip_fraction;
};

/// What the app writes as `plr_profile`.
struct PlrProfile {
    int tier = 0;
    PlrBackendKind backend = PlrBackendKind::Generic;
    StorageHint storage_hint = StorageHint::Unknown;
    bool z_holds_unpowered = false;
    std::string xy_home_gcode = "G28 X Y";
    float z_lift_mm = 5.0f;
    std::optional<std::string> purge_gcode;
    std::optional<bool> user_enabled;
    float resume_skip_fraction = 0.0f;
    std::map<std::string, std::vector<std::string>> macro_vars;
    /// The printer database has a block for this model. The plugin guards an
    /// unlisted printer's default tier against what Klipper reports.
    bool listed = false;
    std::optional<std::string> reason;
};

/// The entry's `plr` block; nullopt when the entry has none (or it is not an
/// object). A bad-typed or out-of-range key keeps its default.
std::optional<PlrDbBlock> plr_parse_db_block(const nlohmann::json& printer_entry);

/// DB block plus user overrides. No block is kPlrUnlistedTier when
/// @p plugin_guards_unlisted (the plugin's `plr` feature is at
/// kPlrUnlistedTierMinVersion or newer), else tier 0. Tier 2 always needs the
/// database's `z_holds_unpowered`, whoever asks for it; without it the tier is
/// at most 1. The user may lower the tier, or raise it on a printer whose X/Y
/// homing is known safe or overridden; a raise on unsafe or unknown homing with
/// no override resolves to 0.
PlrProfile plr_resolve_profile(const std::optional<PlrDbBlock>& db, const PlrUserOverride& user,
                               const PlrHomingCheck& homing, bool plugin_guards_unlisted);

/// The `plr_profile` record, in the contract's snake_case shape.
nlohmann::json plr_profile_json(const PlrProfile& profile);

/// The `plr_user` record.
nlohmann::json plr_user_json(bool enabled);

} // namespace helix
