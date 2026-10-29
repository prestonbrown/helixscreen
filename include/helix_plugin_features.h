// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HelixPrint plugin features: the `features` map its server.helix.status
// reports, and the one question callers ask of it. A caller gates on a
// feature and the minimum contract version it needs, never on the plugin's
// version string, so an older plugin loses only the features it lacks.

#include "moonraker_error.h"
#include "translation_loader.h" // TR_NOOP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/// What a caller may do with one plugin feature.
enum class PluginFeatureState {
    Absent,   ///< no plugin, no such feature, too old a contract, or it cannot run
    Disabled, ///< the feature can run but its switch is off
    Ready,    ///< the feature can run and is switched on
};

/// One entry of the status `features` map.
struct PluginFeatureEntry {
    int version = 0;        ///< the feature's own contract version
    bool available = false; ///< its module loaded and its requirements hold
    bool enabled = false;   ///< switched on (never true while unavailable)
    /// The feature's own saving switch as applied, where it reports one apart
    /// from `enabled` (plr: `enabled` also holds while a snapshot waits).
    std::optional<bool> saving;
    std::string reason; ///< why it is unavailable; empty when it is available

    bool operator==(const PluginFeatureEntry& o) const {
        return version == o.version && available == o.available && enabled == o.enabled &&
               saving == o.saving && reason == o.reason;
    }
    bool operator!=(const PluginFeatureEntry& o) const {
        return !(*this == o);
    }
};

using PluginFeatureMap = std::map<std::string, PluginFeatureEntry>;

/// Whether a features map read again announces itself to the probe observers
/// when it is the same map: a discovery pass always does (its observers
/// re-subscribe to the new connection); a periodic re-read only on a change.
enum class FeatureAnnounce {
    Always,
    IfChanged,
};

/// The `features` map of a server.helix.status result. Empty for a plugin
/// that predates the map, or when it is not an object; an entry that is not
/// an object is skipped.
PluginFeatureMap parse_plugin_features(const nlohmann::json& status_result);

/// Whether the plugin reports feature @p name at contract @p min_version or
/// newer, available or not. An unavailable entry still means the plugin reads
/// whatever the app writes for that feature.
bool plugin_feature_reported(bool plugin_available, const PluginFeatureMap& features,
                             const std::string& name, int min_version);

/// The state of feature @p name for a caller that needs contract
/// @p min_version or newer. @p plugin_available is the plugin itself being
/// installed and enabled: without it every feature is Absent.
PluginFeatureState plugin_feature_state(bool plugin_available, const PluginFeatureMap& features,
                                        const std::string& name, int min_version);

/// Where the user's opt-ins live: the app's settings namespace, this key,
/// {"<feature>": bool, ...}. The plugin reads it when a feature acts.
inline constexpr const char* kPluginFeaturesNamespace = "helix-screen";
inline constexpr const char* kPluginFeaturesKey = "plugin_features";

/// The user's opt-in for feature @p name from the plugin_features record, by
/// the plugin's own rule: a bool under the name is the choice, anything else
/// the default, on for a feature that only observes and off for one that
/// @p acts on a print or a spool.
bool plugin_feature_opted_in(const nlohmann::json& record, const std::string& name, bool acts);

/// How a settings row for a plugin feature's switch shows, by the plugin's
/// state. Values are what the row's mode subject carries.
enum class PluginSwitchMode {
    Hidden = 0,      ///< the plugin runs, but the feature cannot on this printer
    Switch = 1,      ///< a working switch, no hint
    NeedsPlugin = 2, ///< greyed, "Requires the HelixPrint plugin", Install
    NeedsUpdate = 3, ///< greyed, the installed plugin is too old for it
    /// A working switch the app's fallback serves on a remote screen while no
    /// plugin is installed: a hint that it only covers prints while this app
    /// is connected, and Install.
    FallbackInstallHint = 4,
    /// The same with a plugin too old for the feature: the hint asks for an update.
    FallbackUpdateHint = 5,
};

/// The row mode for feature @p name at contract @p min_version.
/// @p has_fallback: the app does the feature itself when the plugin cannot,
/// so its switch keeps working without one. @p is_local: this screen runs on
/// the printer's own host, where the app is up whenever the printer is, so a
/// fallback needs no hint there. A plugin-only feature the plugin reports
/// `available: false` cannot run on this printer at all and shows no row; one
/// with a fallback keeps its switch, which is what the fallback acts on.
PluginSwitchMode plugin_switch_mode(bool plugin_available, const PluginFeatureMap& features,
                                    const std::string& name, int min_version, bool has_fallback,
                                    bool is_local);

/// Which HelixScreen on a printer sends a fallback's gcode, so a screen on the
/// printer and a tablet never both send it.
enum class FallbackSenderRole {
    Sender,  ///< this instance sends
    Standby, ///< another instance does, or it cannot be ruled out
};

/// The one rule: the instance on the printer's host sends; a remote one sends
/// only when no HelixScreen on the printer's host is connected.
/// @p local_screen_present is nullopt when that cannot be told, and a remote
/// instance then stands by rather than risk a second send.
FallbackSenderRole fallback_sender_role(bool is_local, std::optional<bool> local_screen_present);

/// The plugin_features record to use after reading it failed with @p err:
/// an empty one (every feature at its default) when Moonraker says there is
/// none, else nullopt, meaning keep the last record read. A transient error
/// must not switch a fallback off.
std::optional<nlohmann::json> opt_ins_after_failed_read(const MoonrakerError& err);

/// Where a HelixScreen on the printer's host announces itself, in the
/// kPluginFeaturesNamespace: {"seen_at": <unix seconds>}, rewritten every
/// kLocalScreenPresenceIntervalS while it is connected. Moonraker lists no
/// connected clients, so this record is how a remote instance knows.
inline constexpr const char* kLocalScreenPresenceKey = "local_screen";
inline constexpr int64_t kLocalScreenPresenceIntervalS = 600;

/// The presence record for @p now.
nlohmann::json local_screen_presence_record(int64_t now);

/// Whether a local screen is connected, from its presence record as read at
/// @p now: present when it announced itself within two intervals (one missed
/// beat, plus clock drift between the two hosts). A record that is not one
/// cannot be judged.
std::optional<bool> local_screen_present(const nlohmann::json& record, int64_t now);

/// Where a feature's setting lives, for the plugin overview to lead to it.
enum class PluginFeatureHome {
    None,     ///< no setting: the overview shows its status only
    Printing, ///< Settings > Printing
    Storage,  ///< Settings > System > Storage
    Spoolman, ///< Settings > Devices > Spoolman
    Devices,  ///< Settings > Devices
    Safety,   ///< Settings > Safety & Alerts
};

/// What this app knows about a plugin feature. Labels are English; the UI
/// translates them.
struct KnownPluginFeature {
    const char* name;
    const char* label;
    const char* icon;
    PluginFeatureHome home;
    /// Reasons, nullptr-terminated, that say the hardware this feature serves
    /// is not on the printer. The overview leaves the feature out while it is
    /// unavailable for one of them; any other reason still shows.
    const char* const* hidden_reasons = nullptr;
};

inline constexpr const char* kIfsAbsentReasons[] = {"no_adventurer_json", nullptr};
inline constexpr const char* kAceAbsentReasons[] = {"no_ace_bridge", "ace_objects_carry_slots",
                                                    nullptr};
inline constexpr const char* kForceSensorAbsentReasons[] = {"no_force_sensor", nullptr};

/// Every feature this app can name. A feature the plugin reports that is not
/// listed shows by its key, with no setting to lead to.
inline constexpr KnownPluginFeature kKnownPluginFeatures[] = {
    {"print_hooks", TR_NOOP("Start options for prints from other apps"), "play_circle",
     PluginFeatureHome::Printing},
    {"plr", TR_NOOP("Power-loss recovery"), "power_plug", PluginFeatureHome::Printing},
    {"filament_accounting", TR_NOOP("Spool switching per tool and lane"), "swap_horizontal",
     PluginFeatureHome::Spoolman},
    {"fit_check_blocks_autostart", TR_NOOP("Hold slicer prints that do not fit"), "ruler",
     PluginFeatureHome::Safety},
    {"convert_3mf", TR_NOOP("3MF conversion"), "cube_outline", PluginFeatureHome::None},
    {"file_analysis", TR_NOOP("File analysis"), "magnify_scan", PluginFeatureHome::None},
    {"event_journal", TR_NOOP("Printer events"), "bell", PluginFeatureHome::None},
    {"server_edits", TR_NOOP("Server-side edits"), "pencil", PluginFeatureHome::None},
    {"config_edit", TR_NOOP("Config editing"), "wrench", PluginFeatureHome::None},
    {"storage", TR_NOOP("Storage report"), "sd", PluginFeatureHome::Storage},
    {"storage_auto_cleanup", TR_NOOP("Automatic storage cleanup"), "delete",
     PluginFeatureHome::Storage},
    {"photos", TR_NOOP("Print photos"), "image_area", PluginFeatureHome::Devices},
    {"vendor_ad5x", TR_NOOP("IFS updates without polling"), "sync", PluginFeatureHome::None,
     kIfsAbsentReasons},
    {"vendor_ace", TR_NOOP("ACE updates without polling"), "sync", PluginFeatureHome::None,
     kAceAbsentReasons},
    {"klipper_companions", TR_NOOP("Klipper modules"), "puzzle_outline", PluginFeatureHome::None},
    {"pa_gauge", TR_NOOP("Pressure advance from the nozzle sensor"), "gauge",
     PluginFeatureHome::Printing, kForceSensorAbsentReasons},
};

/// What a reason code the plugin reports means, for the overview. Text is
/// English; plugin_reason_text() translates it.
struct PluginReasonText {
    const char* code;
    const char* text;
};

/// Every reason code the plugin's feature modules report.
inline constexpr PluginReasonText kPluginReasonTexts[] = {
    {"plugin_disabled", TR_NOOP("The plugin is turned off in moonraker.conf.")},
    {"module_missing", TR_NOOP("Part of the plugin is missing. Reinstall the plugin.")},
    {"load_failed", TR_NOOP("Part of the plugin failed to load. Reinstall the plugin.")},
    {"start_failed", TR_NOOP("This feature failed to start. The Moonraker log says why.")},
    {"stopped", TR_NOOP("This feature stopped. Restart Moonraker to start it again.")},
    {"needs_file_analysis", TR_NOOP("This needs the plugin's file analysis, which isn't running.")},
    {"upload_hook_unsupported",
     TR_NOOP("This Moonraker version can't hold uploads. Update Moonraker.")},
    {"no_gcodes_dir", TR_NOOP("Moonraker has no G-code folder.")},
    {"python_too_old", TR_NOOP("Moonraker's Python is too old for this feature.")},
    {"no_virtual_sdcard", TR_NOOP("Klipper has no [virtual_sdcard] section.")},
    {"store_unwritable", TR_NOOP("The plugin can't save its recovery data to disk.")},
    {"not_listed", TR_NOOP("Power-loss recovery isn't supported on this printer yet.")},
    {"snapshot_unreachable",
     TR_NOOP("The plugin can't reach the camera, so this screen takes the photos.")},
    {"unsafe_staging_dir",
     TR_NOOP("A folder name in moonraker.conf is unsafe, so cleanup is off.")},
    {"unsafe_root",
     TR_NOOP("The timelapse or logs folder overlaps a protected folder, so cleanup is off.")},
    {"no_adventurer_json", TR_NOOP("This printer has no IFS.")},
    {"adventurer_json_unreadable", TR_NOOP("The IFS state file can't be read.")},
    {"no_ace_bridge", TR_NOOP("No ACE unit was found.")},
    {"ace_objects_carry_slots", TR_NOOP("Klipper already reports the ACE slots.")},
    {"bridge_unreadable", TR_NOOP("The ACE state can't be read.")},
    {"no_force_sensor", TR_NOOP("This printer has no nozzle force sensor.")},
    {"klipper_path_unknown", TR_NOOP("Moonraker doesn't say where Klipper is installed.")},
    {"install_failed", TR_NOOP("The plugin couldn't install its Klipper module.")},
    {"remove_failed", TR_NOOP("The plugin couldn't remove its Klipper module.")},
};

/// The overview's text for @p reason: a code in kPluginReasonTexts, read up to
/// any ": detail" suffix, as its translated text; any other reason unchanged.
std::string plugin_reason_text(std::string_view reason);

/// The icon of a feature this app does not know.
inline constexpr const char* kUnknownPluginFeatureIcon = "puzzle_outline";

/// A feature's state as the plugin overview reports it.
enum class PluginFeatureStatus {
    Active,      ///< available and switched on
    Off,         ///< available and switched off
    Unavailable, ///< cannot run here; the plugin says why
};

/// One line of the plugin overview.
struct PluginFeatureLine {
    std::string name;  ///< the feature's key in the status map
    std::string label; ///< English, or the key itself for an unknown feature
    std::string icon;
    PluginFeatureStatus status = PluginFeatureStatus::Unavailable;
    std::string reason; ///< why it is unavailable; empty otherwise
    PluginFeatureHome home = PluginFeatureHome::None;
};

/// The overview lines for @p features: known features in kKnownPluginFeatures
/// order, then unknown ones by name. A known feature unavailable for one of
/// its hidden_reasons gets no line.
std::vector<PluginFeatureLine> plugin_feature_lines(const PluginFeatureMap& features);

} // namespace helix
