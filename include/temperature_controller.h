// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_heater_config.h" // helix::HeaterType, HEATER_TYPE_COUNT
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "heater_limits.h"
#include "moonraker_error.h"
#include "preset_materials.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace helix {
class PrinterState;
struct DryerInfo;
namespace chamber {
class ChamberHeaterBackend;
}
} // namespace helix
class IMoonrakerAPI;

namespace helix {

// HeaterPresets lives in ui_heater_config.h (included above) so heater_config_t
// and this controller share one definition instead of two parallel copies.

/**
 * @brief Compute a heater's preset temperatures from the user's preset materials.
 *
 * Slot i corresponds to helix::presets::name(i).
 *  - Nozzle / Bed: derived from the filament database entry for that slot's
 *    material (user MaterialSettingsManager overrides already folded in).
 *  - Chamber: a documented, slot-indexed enclosure ladder that is deliberately
 *    NOT material-derived — see CHAMBER_PRESET_LADDER_C in the .cpp for why.
 *
 * Free function so callers without a controller instance (TemperatureService
 * construction) share one derivation instead of duplicating the lookups.
 */
HeaterPresets compute_heater_presets(HeaterType type);

/// Options for a heater set-target call.
/// - toast: show the standard error toast on failure (default true).
/// - on_success / on_error: optional caller hooks fired on the main thread after the RPC completes.
struct SendOptions {
    bool toast = true;
    /// Swap-preheat guard (nozzle only). When true, the requested target is floored
    /// at max(latched last-nonzero nozzle target, current actual nozzle temp) so a
    /// filament switch never drops the nozzle below what's needed to purge the
    /// previous material. Set by "switching material" call sites (preset tap, load).
    /// Leave false for deliberate manual sets and cooldown-to-0.
    /// Note: when this floors the target, it emits its own "holding" toast and
    /// clears on_success so the caller doesn't fire a contradictory "set to X" toast.
    bool keep_previous_hot = false;
    std::function<void()> on_success = nullptr;
    std::function<void(const MoonrakerError&)> on_error = nullptr;
};

struct KeypadRange {
    float min = 0.0f;
    float max = 0.0f;
};

/// Single authority for heater target control: name resolution, configured-max
/// limits, presets, and the one send. No LVGL widgets/subjects — uses the
/// NOTIFY_* notification system for toasts, so logic is unit-testable.
class TemperatureController {
  public:
    TemperatureController(PrinterState& state, IMoonrakerAPI* api);

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /// Klipper object name to target. Nozzle -> active extruder; Bed ->
    /// "heater_bed"; Chamber -> resolved discovery name (never the bare default).
    std::string resolved_name(HeaterType type) const;

    /// Klipper-configured max_temp in °C, or 0 if not yet fetched.
    int configured_max(HeaterType type) const;

    /// Keypad input range: min..effective ceiling (configured max if known,
    /// otherwise the heater default).
    KeypadRange keypad_range(HeaterType type) const;

    /// The effective ceiling for ANY temperature-input surface, in one place:
    /// ensure_limits() then keypad_range(). While the configfile cap is still
    /// unknown the heater default applies (not the caller's fallback); the
    /// fallback only covers a controller reporting no ceiling at all. Every
    /// keypad/edit view must ask this rather than composing the primitives
    /// itself, so no two input surfaces can disagree about the ceiling.
    float effective_keypad_max(HeaterType type, float fallback_deg);

    /// Keypad ceiling for a heater named by its Klipper object (e.g. "heater_generic
    /// filament_dryer"): its configfile max_temp when the printer reported one,
    /// otherwise @p fallback_deg.
    float keypad_max_for(const std::string& klipper_name, float fallback_deg) const;

    /// Fetch the Klipper configfile max_temp for this heater if not yet known.
    /// No-op if api_ is null or the value is already populated.
    void ensure_limits(HeaterType type);

    /// The heater's preset target values (°C).
    const HeaterPresets& presets(HeaterType type) const;

    /// Recompute every heater's preset temperatures from the user's currently
    /// configured preset materials. Call after a preset slot is reassigned.
    void refresh_presets();

    /// Whether a preset value should be shown given the configured max (hidden if above it).
    bool preset_visible(HeaterType type, int value_c) const;

    /// Send a temperature target by heater type.  Resolves to the klipper object name first.
    void set_target(HeaterType type, double celsius, SendOptions opts = {});

    /// Send a temperature target by explicit klipper object name (e.g. "heater_generic
    /// chamber_heater" or "extruder").  Returns immediately if api_ is null or name is empty.
    void set_target(const std::string& klipper_name, double celsius, SendOptions opts = {});

    /// Send nozzle/bed/chamber targets in a single call.  Chamber is skipped when its resolved
    /// name is empty or chamber == 0.
    void apply_material(double nozzle, double bed, double chamber, SendOptions opts = {});

    /// Backend-provided chamber action surface (issue #1290). Wired by
    /// PrinterState when the resolved chamber heater is the discovery pick;
    /// empty strings / 0 disable each capability.
    void set_chamber_actions(std::string reset_gcode, std::string filter_fan_pin,
                             double conservative_max);

    /// Clear a latched chamber-heater fault (backend gcode). No-op when the
    /// backend has no reset gcode or the api is gone.
    void reset_chamber_fault();

    /// Switch the chamber filtration fan (binary output_pin). No-op when the
    /// backend has no filter pin or the api is gone.
    void set_chamber_filter_fan(bool on);

    /// Chamber filament dryer (#1299): the matched backend whose drying cycle
    /// the actions below drive. nullptr, or a backend without one, disables it.
    /// @p has_heated_bed enables the bed assist.
    void set_chamber_dryer(const chamber::ChamberHeaterBackend* backend,
                           bool has_heated_bed = false);

    /// What the chamber's drying cycle accepts; supported=false when none.
    [[nodiscard]] DryerInfo chamber_dryer() const;

    /// Bed target the assist sets, in C; 0 when there is no heated bed.
    [[nodiscard]] int chamber_dryer_bed_assist_c() const;

    /// Start a drying cycle, clamped to chamber_dryer(). No-op without a dryer.
    /// @p heat_bed also heats the bed to chamber_dryer_bed_assist_c(), which the
    /// cycle ending turns back off; refused while a job holds the machine.
    /// @p hold_idle false leaves Klipper's idle timeout to a caller that holds
    /// it for a longer run of its own.
    void start_chamber_drying(float temp_c, int duration_min, bool heat_bed = false,
                              bool hold_idle = true);

    /// End the running drying cycle, and the bed assist with it. No-op without
    /// a dryer.
    void stop_chamber_drying();

    /// First tokens of the chamber dryer's start and stop commands, for the
    /// spools-on-the-bed allowlist; empty without a dryer.
    [[nodiscard]] std::vector<std::string> chamber_dryer_tokens() const;

    /// Klipper's configured idle_timeout.timeout in seconds (600 when the
    /// section is absent), handed to @p on_main on the main thread. Nothing is
    /// called when configfile cannot be read, since then there is no value to
    /// restore.
    void read_configured_idle_timeout(std::function<void(int)> on_main);

  private:
    friend struct TemperatureControllerTestAccess;
    void set_configured_max(HeaterType type, int deg);

    struct HeaterModel {
        float keypad_min = 0.0f;
        float keypad_max_default = 0.0f; // 350 nozzle / 150 bed / 80 chamber
        int configured_max = 0;          // °C from configfile, 0 = unknown
        HeaterPresets presets{};
    };
    std::array<HeaterModel, HEATER_TYPE_COUNT> model_{};
    AsyncLifetimeGuard lifetime_;

    static int idx(HeaterType t) {
        return static_cast<int>(t);
    }

    PrinterState& state_;
    IMoonrakerAPI* api_;

    std::string chamber_reset_gcode_;
    std::string chamber_filter_fan_pin_;
    double conservative_chamber_max_ = 0;
    const chamber::ChamberHeaterBackend* chamber_dryer_backend_ = nullptr;
    bool chamber_dryer_has_bed_ = false;

    /// A drying cycle we started, owed its cleanup when it ends. seen_running
    /// holds the cleanup back until the cycle has actually been reported
    /// running, so the idle frames before the appliance picks up the start do
    /// not end it. bed_c is the bed it heated (0: none); idle_restore_s is the
    /// configured idle timeout it held off (0: none held yet).
    struct DryRun {
        bool armed = false;
        bool seen_running = false;
        int bed_c = 0;
        int idle_restore_s = 0;
        uint32_t id = 0;
    } dry_run_;
    uint32_t next_dry_run_id_ = 0;
    ObserverGuard dryer_active_observer_;
    void on_chamber_dryer_active(bool running);
    void hold_idle_timeout(uint32_t run_id, int run_s);
    void end_dry_run(const char* why);
};

/// Null-safe face of TemperatureController::effective_keypad_max(): the shared
/// ceiling when a controller is reachable, the caller's own fallback otherwise.
/// Every keypad/edit surface asks this — never composes ensure_limits +
/// keypad_range at its call site — so no two input surfaces can disagree about
/// the ceiling (#1615, #1619).
inline float keypad_ceiling(TemperatureController* c, HeaterType type, float fallback_deg) {
    return c ? c->effective_keypad_max(type, fallback_deg) : fallback_deg;
}

} // namespace helix
