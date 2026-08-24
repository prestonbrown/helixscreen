// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <vector>

namespace helix {

/// Preset-driven wizard skip policy, derived purely from whether a complete
/// preset is applied and how many printers are configured. Single source of
/// truth so the LVGL wizard and tests agree, and so the two concerns can't drift:
///   - skip_hardware: the preset already configures heater/fan/AMS/LED/filament/
///     input-shaper, so those pickers are redundant. True for ANY printer with a
///     preset — the first one or a later one added via the printer manager.
///   - first_run: additionally skip the summary and show the one-time telemetry
///     opt-in. First configured printer only — telemetry is a global, one-time
///     prompt, so it must not re-fire when adding subsequent printers.
struct WizardPresetPlan {
    bool skip_hardware = false;
    bool first_run = false;
};
WizardPresetPlan wizard_preset_plan(bool has_preset, int printer_count);

/// Config key (appended to Config::df()) recording that the top-level "preset"
/// marker was written by a wizard run that had not finished yet.
inline constexpr const char* WIZARD_PRESET_PROVISIONAL = "preset_provisional";

/// Whether a persisted preset marker is allowed to collapse the wizard's
/// printer-identify + hardware steps.
///
/// A preset seeded before the wizard ever ran (install-time detection, factory
/// image) is authoritative immediately — that fast path is the whole point of
/// shipping presets. A preset written *by* the printer-identify step is only
/// provisional until the run finishes: the marker alone made an interrupted
/// wizard (crash, power cut, user quits) come back with identify, every
/// hardware picker and the summary gone, locking the user to a pick they never
/// confirmed and offering no in-app way back.
///
/// Within the same process the preset stays authoritative, so the existing
/// in-run "preset applied during identify cleanup → collapse the rest"
/// redirect keeps working unchanged.
///
/// @param preset_marker    Config::has_preset() — a preset name is persisted
/// @param provisional      WIZARD_PRESET_PROVISIONAL is set for this printer
/// @param wizard_completed The active printer finished its wizard
/// @param applied_this_session The wizard applied the preset in this process
bool wizard_preset_is_authoritative(bool preset_marker, bool provisional, bool wizard_completed,
                                    bool applied_this_session);

/// Record that the printer-identify step applied a preset in this process.
void wizard_mark_preset_applied_this_session();

/// @see wizard_mark_preset_applied_this_session
bool wizard_preset_applied_this_session();

/// Clear the process-local flag. Tests only — no production caller.
void wizard_reset_preset_session_state();

/// Config key (appended to Config::df()) recording that this printer's wizard
/// finished without ever reaching Klipper, so the hardware pickers had nothing
/// to offer and hardware/expected was never populated.
///
/// Per-printer from the start, alongside `preset` and WIZARD_PRESET_PROVISIONAL.
/// A root-level flag would make a second printer inherit — or clear — the first
/// one's debt (#1162 for the preset marker; the same trap).
inline constexpr const char* WIZARD_HARDWARE_SETUP_DEFERRED = "hardware_setup_deferred";

/// Whether a finishing wizard run owes its expected-hardware snapshot to a
/// later boot.
///
/// b73781ca8 made it possible to finish setup while Klipper is in `error` —
/// necessary, because that state was previously an inescapable dead end. But
/// discovery never ran, so the heater/fan/LED/sensor pickers had empty lists
/// and the user selected nothing. Committing that as the expected-hardware
/// snapshot makes the first boot where Klipper does come up report every fan,
/// filament sensor and LED as newly appeared (#1160).
///
/// A preset-seeded run is NOT deferred even with Klipper down: the preset
/// supplies real hardware names, so the snapshot it produces is meaningful.
///
/// @param discovery_succeeded Klipper answered and hardware discovery ran
/// @param snapshot_has_entries The run produced at least one hardware name
bool wizard_hardware_snapshot_is_deferred(bool discovery_succeeded, bool snapshot_has_entries);

class Config;

/// Record (or settle) the deferred-snapshot debt for @p config's ACTIVE printer.
///
/// Writes WIZARD_HARDWARE_SETUP_DEFERRED under Config::df() when the finishing run
/// owes a snapshot, and clears a debt an earlier Klipper-down run recorded when
/// it does not — a `--wizard` re-run that reaches Klipper settles it. Does not
/// save; the caller batches this with its other completion writes.
///
/// @return true if the snapshot was deferred
bool wizard_apply_hardware_snapshot_decision(Config* config, bool discovery_succeeded,
                                             bool snapshot_has_entries);

/// Whether @p config's ACTIVE printer still owes a deferred hardware snapshot.
bool wizard_hardware_setup_deferred(Config* config);

/// Clear @p config's ACTIVE printer's deferred-snapshot debt. Does not save.
/// @return true if a debt was present and cleared
bool wizard_clear_hardware_setup_deferred(Config* config);

// ============================================================================
// Id-based pure navigation over the step registry. Operates on a vector of
// {StepId, skipped} entries — the registry-driven representation. No LVGL;
// fully testable. This is the sole navigation API; the wizard and tests both
// drive it.
// ============================================================================

namespace wizard {
enum class StepId;
}

/// One registry entry's navigation state: its id and whether it is skipped.
struct StepSkip {
    wizard::StepId id;
    bool skipped;
};

/// The hardware steps a deferred setup re-run should show, in wizard order,
/// filtered against @p skips so the re-run never presents a step the printer
/// has nothing for (no AMS, no LEDs, sparse filament sensors) and never
/// re-asks what a preset already answered.
///
/// A targeted session runs its list verbatim — ui_wizard_create_targeted() does
/// no skip filtering of its own — so the filtering has to happen here. An empty
/// result means there is nothing to offer and the prompt must not be shown.
std::vector<wizard::StepId> wizard_deferred_hardware_steps(const std::vector<StepSkip>& skips);

/// Count of non-skipped entries.
int wizard_visible_count(const std::vector<StepSkip>&);

/// 1-based display number for `current`: 1 + number of visible entries strictly
/// before it.
int wizard_display_number(wizard::StepId current, const std::vector<StepSkip>&);

/// First non-skipped entry after `current`, or nullopt if none.
std::optional<wizard::StepId> wizard_next(wizard::StepId, const std::vector<StepSkip>&);

/// First non-skipped entry before `current`, or nullopt if none.
std::optional<wizard::StepId> wizard_prev(wizard::StepId, const std::vector<StepSkip>&);

/// True if there is no visible entry after `current`.
bool wizard_is_last(wizard::StepId, const std::vector<StepSkip>&);

} // namespace helix
