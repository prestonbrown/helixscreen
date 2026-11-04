// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Standing print options: the user's per-print choices that still apply to a
// print some other client started (Mainsail, a slicer, Moonraker's job queue).
// The screen renders them with the same backend functions its own start uses
// and keeps the result in Moonraker's database, where the HelixPrint plugin's
// `print_hooks` feature sends it at each such start. When that feature cannot
// run, the screen sends it itself. Pure rules here; the wiring lives in
// StandingPrintOptionsController.

#include "helix_plugin_features.h"
#include "json_fwd.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

class AmsBackend;

/// The plugin feature that sends the record, and the contract this app reads.
inline constexpr const char* kPrintHooksFeature = "print_hooks";
inline constexpr int kPrintHooksMinVersion = 1;

/// Where the record lives: kPluginFeaturesNamespace, this key.
inline constexpr const char* kStandingOptionsKey = "standing_print_options";
inline constexpr int kStandingOptionsVersion = 1;

/// The plugin endpoint a direct start from this screen is announced to.
inline constexpr const char* kScreenStartRpc = "server.helix.print_hooks.screen_start";

/// The standing gcode of every backend, in order, newline-joined; empty when
/// none has anything to send.
std::string render_standing_gcode(const std::vector<const AmsBackend*>& backends);

/// The database record for @p gcode: {"version": 1, "gcode": ...}.
nlohmann::json standing_options_record(const std::string& gcode);

/// Who sends the standing options at a job start.
enum class StandingOptionsSender {
    None,   ///< nobody: the screen started it, the user opted out, or nothing to send
    Plugin, ///< the plugin's print_hooks feature
    App,    ///< this screen, since the plugin cannot
};

/// The one choice between the plugin and this screen for a job start.
/// @p hooks is plugin_feature(kPrintHooksFeature, kPrintHooksMinVersion);
/// @p opted_in the user's print_hooks opt-in as stored, which is all the
/// screen has to go on when the plugin reports no such feature;
/// @p screen_started that this screen made the start (it sent its options
/// then); @p has_gcode that the rendered options are not empty; @p role this
/// instance's fallback_sender_role(), so only one HelixScreen on a printer
/// sends.
StandingOptionsSender standing_options_sender(PluginFeatureState hooks, bool opted_in,
                                              bool screen_started, bool has_gcode,
                                              FallbackSenderRole role);

/// Whether this screen writes the shared record. It is one per printer but
/// rendered from this screen's settings, so only the fallback's Sender writes
/// it, and never an empty one: before the filament backend reports, nothing
/// is known, and no record beats a wrong one.
bool writes_standing_record(FallbackSenderRole role, const std::string& gcode);

/// Whether a direct start from this screen is announced to the plugin, so
/// the plugin does not send the options its pre-print step already carried.
bool announce_screen_start(PluginFeatureState hooks);

/// Starts this screen made, so the job history announces next can be told
/// apart from one some other client started. Thread-safe: starts are noted on
/// whatever thread sends them.
class ScreenStartLedger {
  public:
    using Clock = std::chrono::steady_clock;

    /// How long a noted start waits for its job: a print-start command is
    /// acknowledged after its job is added, so this spans only the command
    /// reaching Klipper.
    static constexpr std::chrono::seconds kWindow{120};

    /// This screen is starting @p filename.
    void note(const std::string& filename, Clock::time_point now = Clock::now());

    /// This screen is starting a job whose history name it does not know (a
    /// print through the plugin's link, a resume the plugin starts). A start
    /// that heats and purges before its job begins waits @p window for it.
    void note_any(Clock::time_point now = Clock::now(), Clock::duration window = kWindow);

    /// Take back the newest note for @p filename, for a start that failed.
    /// False when there was none.
    bool withdraw(const std::string& filename);

    /// Take back the newest note_any(), for a start that failed. False when
    /// there was none.
    bool withdraw_any();

    /// Whether the job history added for @p filename is one this screen
    /// started; a match is used up.
    bool claim(const std::string& filename, Clock::time_point now = Clock::now());

    /// What announce_screen_start() answers now, set on the main thread for
    /// starts sent from any thread.
    void set_announce(bool announce);
    bool announce() const;

  private:
    struct Entry {
        std::optional<std::string> filename; ///< nullopt matches any job
        Clock::time_point expires;
    };
    void expire(Clock::time_point now);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    bool announce_ = false;
};

/// The process-wide ledger the job API notes starts in.
ScreenStartLedger& screen_start_ledger();

} // namespace helix
