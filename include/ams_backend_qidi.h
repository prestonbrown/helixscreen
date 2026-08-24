// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

/// AMS backend stub for the QIDI Box filament changer.
///
/// The QIDI Box is a 4-slot hub-style filament changer (chainable up to 16
/// slots across 4 boxes) sold by QIDI for the PLUS4, Q2, and MAX4 printers.
/// It is NOT supported on Q1 Pro or X-Max 3 — those use a different hub board.
///
/// Architecturally it is closest to the FlashForge AD5X IFS or Bambu AMS
/// (converging hub), NOT a lane-selector MMU. RFID MIFARE Classic tags carry
/// per-spool metadata and the unit has active filament drying. USB-serial
/// connection with udev rule `QIDI_BOX_V1`.
///
/// === Current status: STUB ===
///
/// No protocol work has been done yet — no live hardware is available to
/// validate against. This class registers AmsType::QIDI_BOX and provides
/// empty/no-op implementations for every AmsBackend virtual method so the
/// factory can return a non-null backend and the rest of HelixScreen can
/// treat QIDI Box as "detected but uninitialised" without crashing.
///
/// === Protocol reference (when hardware arrives) ===
///
/// The stock QIDI firmware drives the box through Klipper gcode macros
/// (BOX_CHANGE_FILAMENT, _BOX_START, _BOX_*) plus printer-object polling
/// and save_variables. There are no dedicated Moonraker endpoints.
///
/// The qidi-community Plus4-Wiki ships a "customisable_qidibox_firmware"
/// project with Python replacements for QIDI's obfuscated .so modules —
/// that is the best starting point for reverse-engineering the actual
/// command set when this stub graduates to a real backend:
///
///   https://github.com/qidi-community/Plus4-Wiki
///
/// Until then: every operation logs a "not yet implemented" warning and
/// returns a sensible no-op / NOT_SUPPORTED error.
class AmsBackendQidi : public AmsSubscriptionBackend {
  public:
    AmsBackendQidi(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendQidi() override;

    /// Single-box default. Chainable up to 16 slots (4 boxes × 4 slots) on
    /// supported hardware, but the stub only advertises a single box.
    static constexpr int NUM_SLOTS = 4;

    // --- AmsBackend interface ---
    [[nodiscard]] AmsType get_type() const override {
        return AmsType::QIDI_BOX;
    }
    // QIDI Box exposes user-tunable per-lane eject distance + velocity, which the
    // device-operations overlay surfaces as slider rows.
    [[nodiscard]] bool supports_configurable_eject_params() const override {
        return true;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;

    /// Forward map (index = tool, value = global slot), derived from the
    /// per-slot mapped_tool the save_variables read-path writes. The base
    /// default returns {}, which — on a backend that advertises tool mapping —
    /// silently disables the print-start remap snapshot/restore and the AMS
    /// context menu's mapping read, and makes AmsState fall back to a 1:1
    /// topology that contradicts a user remap.
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;
    // QIDI Box reassigns a logical tool to a physical slot by rewriting the
    // save_variables entry (value_t<N>="slot<M>") that the stock T<N> macros
    // read at load time — applied via set_tool_mapping(). Same shape as CFS, so
    // it joins the unified Native remap path (the single FilamentMappingModal +
    // print-start set_tool_mapping apply); without this it inherits the base
    // None default and the unified apply_remap would no-op for QIDI.
    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] bool is_bypass_active() const override;

    /// The Box publishes a per-slot state word (save_variables slot<N>, where 2
    /// means loaded), which is its own statement about that slot and needs no
    /// active-slot pointer to interpret. The aggregate pair by contrast is
    /// written only from last_load_slot, so on a Box that never writes that
    /// variable nothing would ever read as loaded despite slot<N>: 2 on the
    /// wire. parse_save_variables() reconciles the two at the end of every pass
    /// so the stamp cannot fall behind the aggregate either
    /// (prestonbrown/helixscreen#1199).
    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

    // The box exposes a PTC dryer heater (heater_generic heater_box<N>) plus an
    // aht20_f humidity/temperature chip, so the per-unit environment indicator
    // (temp/humidity + drying controls) must be reachable. Without this override
    // the indicator widget is hard-hidden in ams_detail_pre_show_env_indicator().
    [[nodiscard]] bool has_environment_sensors() const override {
        return true;
    }

    // load_filament() drives the stock EXTRUDER_LOAD primitive directly and
    // manages hotend temperature itself (heat → load → clear → cool), so the UI
    // must not run its own preheat for QIDI loads.
    [[nodiscard]] bool supports_auto_heat_on_load() const override {
        return true;
    }

  protected:
    // Operations. Gated by AmsSubscriptionBackend's NVI wrapper.
    // select_slot_moves_toolhead() stays false: do_select_slot() is
    // not_supported here — load_filament is the only path.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

    /// QIDI's filament ops have never consulted running_ or
    /// system_info_.action — they were ungated entirely until 329e731e9 added
    /// the print-active check, which chose the narrower gate to preserve that.
    /// The box DOES report AmsAction::LOADING (parse_variables sets it from
    /// `is_tool_change`), so this is not a no-op: widening it to Standard would
    /// newly refuse a load while the box calls itself busy. That is a behaviour
    /// change on hardware nobody here owns, so it stays a deliberate narrowing
    /// rather than an accident preserved by silence.
    [[nodiscard]] FilamentOpGate filament_op_gate() const override {
        return FilamentOpGate::PrintActiveOnly;
    }

  public:
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    // Per-lane eject for non-loaded lanes. Q2/Plus 4: FORCE_MOVE on the box_stepper
    // (#1041), gated on [force_move] enable_force_move. Max 4: MULTI_COLOR_BOX_UNLOAD
    // via the multi_color_controller dialect, no [force_move] needed (#1083).
    AmsError eject_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_eject() const override {
        // Max 4 (multi_color_controller) ejects via MULTI_COLOR_BOX_UNLOAD, which
        // needs no [force_move]; the Q2/Plus 4 box_stepper FORCE_MOVE path does. #1083
        return box_uses_multi_color_ || fw_force_move_enabled_;
    }

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;
    void clear_slot_override(int slot_index) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;

    // --- Dryer / box-heater control (issue #1019) ---
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS QIDI Box]";
    }

  private:
    friend class QidiBoxTestAccess;

    /// Apply a save_variables snapshot onto system_info_ / per-slot state.
    /// Input is the inner `variables` object (already unwrapped from the
    /// `save_variables.variables` envelope).
    void parse_save_variables(const nlohmann::json& variables);

    /// Re-derive system_info_.tool_to_slot_map from the per-slot mapped_tool
    /// values, and write the normalised result back onto the slots.
    ///
    /// The Box states its mapping one way only — save_variables value_t<N>
    /// names the slot tool N prints from — so the read-path naturally writes
    /// only SlotInfo::mapped_tool. AmsSystemInfo carries the forward map as
    /// well, and consumers split across the two: the AMS panel badges a lane
    /// from mapped_tool, while the filament panel resolves which lane the
    /// Load/Unload buttons act on through tool_to_slot_map. Publishing one
    /// without the other gated the buttons on the wrong lane after a remap.
    ///
    /// Both directions are produced by a single SlotRegistry pass rather than
    /// by two hand-written writers, so they cannot drift.
    ///
    /// Caller must hold mutex_.
    void rebuild_tool_map_locked();

    /// Scan notification for `heater_generic heater_box<N>` and
    /// `aht20_f heater_box<N>` entries; update unit environment with the
    /// max temperature and max humidity observed across all boxes.
    void apply_heater_status(const nlohmann::json& notification);

    /// Refine dryer_info_.max_temp_c from the Klipper configfile settings
    /// object (`printer.configfile.settings`). Handles two section spellings:
    ///   "heater_generic heater_box<N>" → key `max_temp`
    ///   "box_config box<N>"            → key `target_max_temp_heater_generic`
    void apply_config_settings(const nlohmann::json& settings);

    /// Parse `box_extras` object from a status notification, extracting
    /// `box_drying_state.box<N>.{dry_state, end_time}` to update the
    /// drying countdown in dryer_info_.
    void apply_box_extras(const nlohmann::json& box_extras);

    /// Unwrap a printer.objects.query response (`result.status.{...}`)
    /// and feed the inner object through handle_status_update so the
    /// bootstrap fetch reuses every parser already exercised by the
    /// notification path.
    void apply_query_response(const nlohmann::json& response);

    /// Build the stock unload g-code for the box, chosen by firmware capability.
    /// M603 is the verified stock unload (Q2 1.1.1); if a firmware revision drops
    /// it, fall back to the box_stepper EXTRUDER_UNLOAD primitive that UNLOAD_T<n>
    /// wraps. slot_index >= 0 targets a specific lane; -1 unloads whatever is in
    /// the extruder.
    [[nodiscard]] std::string build_unload_gcode(int slot_index, int temp) const;

    /// Detect firmware-version-dependent capabilities from the macro set and log
    /// a fingerprint. The 1.1.x -> 01.01.02 QIDI refactor changes the macro
    /// surface (e.g. T0-T3 absent on Q2 1.1.1), so we branch on capability rather
    /// than a version string. Called from on_started() once the macro cache is up.
    void detect_firmware_capabilities();

    // Firmware-capability flags (see detect_firmware_capabilities()). Optimistic
    // defaults match verified Q2 1.1.1 stock firmware so tests and a discovery
    // race never downgrade off the known-good path.
    bool fw_has_m603_ = true;            ///< M603 stock unload macro present
    bool fw_has_clear_nozzle_ = true;    ///< CLEAR_NOZZLE post-load wipe macro present
    bool fw_force_move_enabled_ = false; ///< [force_move] enable_force_move -> lane eject
    /// [multi_color_controller] config section present -> Max 4 box dialect: eject/
    /// unload go through MULTI_COLOR_* commands, not the box_stepper FORCE_MOVE that
    /// the Max 4 rejects with "Invalid pin value". Set in apply_config_settings. #1083
    bool box_uses_multi_color_ = false;

    /// Raw RFID indices read from save_variables. Per-slot side-table so we
    /// don't pollute SlotInfo with backend-specific fields. Resolution to
    /// material/color/brand happens via the officiall_filas_list.cfg lookup
    /// (separate cycle). 0 = unknown.
    struct QidiSlotRfid {
        int filament_id = 0; ///< 1-99, index into officiall_filas_list.cfg
        int color_id = 0;    ///< 1-24, palette index
        int vendor_id = 0;   ///< Always 1 in the wild so far
    };
    std::vector<QidiSlotRfid> slot_rfid_;

    // Dryer state for the box PTC heater (issue #1019).
    std::vector<DryerInfo> dryer_info_;      ///< Per-unit (per-box) dryer state, index = unit
    std::vector<std::time_t> dry_end_epoch_; ///< Per-unit drying end time (epoch s), 0 = none
    bool drying_timer_supported_ = false;    ///< box_extras drying timer seen -> use ENABLE_BOX_DRY
    std::function<std::time_t()> now_fn_ = [] { return std::time(nullptr); };

  public:
    /// Temperature profile for a single fila entry from
    /// officiall_filas_list.cfg. Public so test friend can return one.
    struct FilaProfile {
        std::string name; ///< Human label from `filament` (e.g. "PLA Rapido")
        std::string type; ///< Material class from `type` (e.g. "PLA", "ABS-GF")
        int nozzle_min = 0;
        int nozzle_max = 0;
        int box_min = 0;
        int box_max = 0;
    };

  private:
    /// Parse a ConfigParser-formatted officiall_filas_list.cfg payload and
    /// populate fila_profiles_, color_palette_, and vendor_names_. Tolerant of
    /// whitespace, blank lines, and `#` / `;` comments. Non-matching sections
    /// (anything but `[fila<N>]`, `[colordict]`, `[vendor_list]`) are ignored.
    void apply_filas_list(const std::string& content);

    /// fila_id (1-99) → temperature profile, populated by apply_filas_list().
    std::map<int, FilaProfile> fila_profiles_;
    /// colordict id (1-24) → packed 0xRRGGBB, populated by apply_filas_list().
    std::map<int, std::uint32_t> color_palette_;
    /// vendor_list id → vendor name, populated by apply_filas_list().
    std::map<int, std::string> vendor_names_;

    // --- Reverse lookups for set_slot_info() (pure, no member access) ---
    //
    // Map a SlotInfo back onto the three stock RFID indices written to
    // save_variables. Static + parameterised by the parsed maps so they
    // unit-test without a live backend (exposed via QidiBoxTestAccess).

    /// Best-effort fila id from a SlotInfo's material/name. Exact
    /// case-insensitive `name` match wins; else first profile whose `type`
    /// case-insensitively equals `material`. Returns 0 when nothing matches.
    static int resolve_fila_id(const std::map<int, FilaProfile>& profiles,
                               const std::string& material, const std::string& name);

    /// Nearest palette entry to `rgb` by squared RGB distance. Returns 0 when
    /// the palette is empty.
    static int resolve_color_id(const std::map<int, std::uint32_t>& palette, std::uint32_t rgb);

    /// Vendor id from a case-insensitive name match against `vendors`. Falls
    /// back to the id whose name is "Generic" (case-insensitive) if present,
    /// else 0.
    static int resolve_vendor_id(const std::map<int, std::string>& vendors,
                                 const std::string& brand);
};
