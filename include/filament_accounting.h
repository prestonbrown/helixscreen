// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Filament accounting per lane and tool. Moonraker's spoolman component is the
// only deductor: it charges ONE active spool for every millimetre the E
// position rises. Usage is therefore right exactly when the active spool is
// switched at each tool or lane change. The HelixPrint plugin's
// `filament_accounting` feature switches it for printers whose filament system
// does not, from a map this app keeps in Moonraker's database
// (`helix-screen` / `filament_spools`), and records per job what each spool
// was charged. When the plugin cannot, this app switches it on tool changes
// itself. Pure rules here, shared with the plugin through
// tests/fixtures/filament_accounting/cases.json; the wiring lives in
// FilamentAccountingController.

#include "display_numbering.h"
#include "helix_plugin_features.h"
#include "json_fwd.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

class PrinterDiscovery;

/// The plugin feature and the contract this app reads.
inline constexpr const char* kFilamentAccountingFeature = "filament_accounting";
inline constexpr int kFilamentAccountingMinVersion = 1;

/// Where the map lives: kPluginFeaturesNamespace, this key.
inline constexpr const char* kFilamentSpoolsKey = "filament_spools";
inline constexpr int kFilamentSpoolsVersion = 1;

/// The plugin endpoint serving a job's per-spool record.
inline constexpr const char* kFilamentUsageRpc = "server.helix.filament_usage";

/// Who switches Moonraker's active spool on this printer.
enum class ActiveSpoolOwner {
    NoSpoolman, ///< no Spoolman: there is no active spool to switch
    Backend,    ///< the filament system does it (AFC, Happy Hare with spoolman on)
    Printer,    ///< the printer's firmware does it at every extruder change
    Macros,     ///< the printer's T<n> macros call SET_ACTIVE_SPOOL
    Helix,      ///< nobody: the plugin, or this app's fallback, does it
};

/// The one ownership rule. @p manages_active_spool is what the filament
/// backends report (AmsBackend::manages_active_spool()), @p
/// printer_switches_active_spool whether the firmware switches it itself
/// (printer_switches_active_spool()), @p macros_set_active_spool whether a
/// T<n> macro switches it.
ActiveSpoolOwner active_spool_owner(bool spoolman, bool manages_active_spool,
                                    bool printer_switches_active_spool,
                                    bool macros_set_active_spool);

/// Whether the printer's firmware switches Moonraker's active spool at every
/// extruder change by itself: the Snapmaker U1 extended firmware's SpoolLink
/// (a `spoollink` Klipper object or `[spoollink]` section, installed only
/// with Spoolman configured).
bool printer_switches_active_spool(const PrinterDiscovery& hw);

/// Whether this app sets the active spool for the user's explicit link of the
/// loaded lane (or the external spool). The automatic switches stand down for
/// any owner but Helix; an edit is different, because a switcher that acts on
/// a load or a tool change never sees it. So: Helix sets; Backend sets unless
/// the backend sets it itself on a link (@p backend_sets_on_link); Macros
/// sets, since the macros run only at the next tool change; Printer leaves it
/// to firmware that keeps its own assignment (SpoolLink); NoSpoolman has none.
bool app_sets_spool_on_edit(ActiveSpoolOwner owner, bool backend_sets_on_link);

/// The record spelling of an owner, as the case table names it.
const char* active_spool_owner_name(ActiveSpoolOwner owner);

/// The lookup key of a status value: an integer (or integral float) as its
/// digits, a non-empty string as itself; empty for anything else.
std::string feed_signal_key(const nlohmann::json& value);

/// What one value of a feed signal means.
struct FeedEntry {
    int tool = -1;          ///< the tool feeding, -1 when none is named
    int lane = -1;          ///< the lane (backend slot) feeding, -1 when none
    int spool_id = 0;       ///< the linked Spoolman spool, 0 when unlinked
    float grams_per_mm = 0; ///< 0 when the filament is not known
};

/// A status field that says what is feeding now: the Klipper object, the
/// path inside it, and each value's feed (keyed by feed_signal_key()).
struct FeedSignal {
    std::string object;
    std::vector<std::string> path;
    std::vector<std::pair<std::string, FeedEntry>> values;
};

/// The database record: {"version", "manages_active_spool",
/// "printer_switches_active_spool", "macros_set_active_spool",
/// "signals": [{"object", "path", "values"}]}.
nlohmann::json filament_spools_record(bool manages_active_spool, bool printer_switches_active_spool,
                                      bool macros_set_active_spool,
                                      const std::vector<FeedSignal>& signals);

/// The toolhead.extruder signal of a printer whose tools each have their own
/// extruder: @p tool_extruders[i] is tool i's extruder name (empty for none),
/// @p feeds[i] its feed. An extruder two tools share says nothing about
/// which of them feeds, so it is left out; nullopt when no extruder is left.
std::optional<FeedSignal> extruder_feed_signal(const std::vector<std::string>& tool_extruders,
                                               const std::vector<FeedEntry>& feeds);

/// Whether a printer's macros switch the active spool: any T<n> macro in @p
/// macros for which @p sets answers true.
template <typename Macros, typename Sets>
bool tool_macros_set_active_spool(const Macros& macros, Sets sets);

/// Whether @p name is a tool-select macro: T followed by digits only.
bool is_tool_select_macro(const std::string& name);

/// What to do with the active spool for the feed just read.
struct SpoolSwitch {
    enum class Action { None, Set, Clear };
    Action action = Action::None;
    int spool_id = 0; ///< for Set
};

/// The one switch rule. @p feed is nullptr when nothing is known to feed;
/// @p active_spool is Moonraker's active spool, 0 for none; @p mapped every
/// spool the map links. An unlinked feed clears only a spool the map links to
/// another feed: one chosen outside the map is the user's.
SpoolSwitch decide_spool_switch(const FeedEntry* feed, int active_spool,
                                const std::vector<int>& mapped);

/// Who switches the active spool at a tool change.
enum class FilamentAccountingSender {
    None,   ///< nobody: another switcher owns it, the user opted out, or another screen sends
    Plugin, ///< the plugin's filament_accounting feature
    App,    ///< this screen, since the plugin cannot
};

/// The one choice between the plugin and this screen. @p accounting is
/// plugin_feature(kFilamentAccountingFeature, kFilamentAccountingMinVersion);
/// @p opted_in the stored opt-in, all the screen has when the plugin reports
/// no such feature; @p role this instance's fallback_sender_role().
FilamentAccountingSender filament_accounting_sender(PluginFeatureState accounting, bool opted_in,
                                                    ActiveSpoolOwner owner,
                                                    FallbackSenderRole role);

/// Whether this screen writes the shared map: only the fallback's Sender, so
/// two screens never take turns rewriting it, and only once it says something.
bool writes_filament_spools_record(FallbackSenderRole role, const std::vector<FeedSignal>& signals);

/// One line of a job's per-spool record.
struct SpoolUsage {
    int tool = -1;
    int lane = -1;
    int spool_id = 0;
    double mm = 0;
    std::optional<double> grams;
};

/// Whether a job's record is worth asking the plugin for: it keeps records
/// for jobs it counted, whether or not it is switched on now.
bool filament_usage_available(PluginFeatureState accounting);

/// The spools of a server.helix.filament_usage reply; empty for a job the
/// plugin did not count or a reply of another shape.
std::vector<SpoolUsage> parse_filament_usage(const nlohmann::json& reply);

/// One line per spool for the print history: "<who fed it> · <spool> ·
/// <amount>", newline-joined. Who fed it is the tool's G-code label, else the
/// lane in @p noun, the printer's word for one. @p spool_name names a linked
/// spool (empty when it cannot); the amount is grams where the record has
/// them, else length.
std::string describe_spool_usage(const std::vector<SpoolUsage>& usage,
                                 const std::function<std::string(int)>& spool_name,
                                 ui::LaneNoun noun);

template <typename Macros, typename Sets>
bool tool_macros_set_active_spool(const Macros& macros, Sets sets) {
    for (const auto& name : macros) {
        if (is_tool_select_macro(name) && sets(name)) {
            return true;
        }
    }
    return false;
}

} // namespace helix
