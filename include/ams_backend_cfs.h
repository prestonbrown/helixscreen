// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_CFS

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CfsTestAccess;

namespace helix::printer {

/// Static CFS utility functions (code stripping, color parsing, TNN
/// addressing). The material table this class used to own was retired in
/// favor of FilamentCatalog::load_codes("cfs") — see AmsBackendCfs::parse_box_status.
class CfsMaterialDb {
  public:
    /// Strip the firmware material_type brand-prefix digit (6-char code =
    /// prefix + 5-char catalog id): "101001" -> "01001" (K2, Creality prefix),
    /// "000003" -> "00003" (K1, Generic prefix), sentinels -> ""
    static std::string strip_code(const std::string& code);

    /// Parse CFS color: "0RRGGBB" -> 0xRRGGBB, sentinels -> 0x808080
    static uint32_t parse_color(const std::string& color_str);

    /// Global slot index -> TNN name: 0 -> "T1A", 4 -> "T2A"
    static std::string slot_to_tnn(int global_index);

    /// TNN name -> global slot index: "T1A" -> 0, "T2A" -> 4, invalid -> -1
    static int tnn_to_slot(const std::string& tnn);

    /// Default color for unknown/sentinel slots
    static constexpr uint32_t DEFAULT_COLOR = 0x808080;
};

/// Decode CFS key8xx error codes into human-readable AmsAlerts
class CfsErrorDecoder {
  public:
    /// Decode a CFS error code. Returns nullopt for unknown codes.
    static std::optional<AmsAlert> decode(const std::string& key_code, int unit_index,
                                          int slot_index);

    /// Look up just the message+hint for a code, without slot/unit context.
    /// Used by the global gcode-error toast handler to translate raw Klipper
    /// `!! {"code":"key***","msg":"..."}` lines into friendly text.
    /// Returns {message, hint} or nullopt for unknown codes.
    static std::optional<std::pair<const char*, const char*>>
    lookup_message(const std::string& key_code);

    /// Variant that splices the `values` array (e.g. `[1,"B"]` from
    /// `!! {"code":"key849","values":[1,"B"]}`) into the user-facing
    /// message when the code's value-format is known. Returns full
    /// `std::string` so the caller doesn't have to mix const-char + string
    /// concatenation. Falls back to the un-augmented message+hint when
    /// the values shape is unknown for that code.
    static std::optional<std::pair<std::string, std::string>>
    lookup_message_with_values(const std::string& key_code, const nlohmann::json& values);
};

/// Macro dialect emitted by the CFS backend.
///
/// K2 stock firmware exposes the CR_BOX_* primitives (CR_BOX_PRE_OPT,
/// CR_BOX_EXTRUDE, CR_BOX_WASTE, CR_BOX_FLUSH, CR_BOX_END_OPT, CR_BOX_CUT,
/// CR_BOX_RETRUDE) plus the BOX_* envelope (BOX_SAVE_FAN, BOX_MODE_WAIT,
/// BOX_GO_TO_EXTRUDE_POS, BOX_NOZZLE_CLEAN, BOX_MOVE_TO_SAFE_POS,
/// BOX_RESTORE_FAN). Selected when the printer is detected as a non-K1
/// Creality with a `box` Klipper object.
///
/// K1 official CFS upgrade firmware (≥ v2.3.5.33) exposes a different,
/// non-prefixed set: BOX_EXTRUDE_MATERIAL, BOX_MATERIAL_FLUSH,
/// BOX_NOZZLE_CLEAN, BOX_CUT_MATERIAL, BOX_RETRUDE_MATERIAL,
/// BOX_GO_TO_EXTRUDE_POS, BOX_MOVE_TO_SAFE_POS. The K2-only fan-save and
/// mode-wait helpers are absent. Selected when PrinterDetector reports a
/// K1-series printer. Issue #968.
///
/// Fork — community Kalico ports of the K2 whose reimplemented `box` module
/// replaces Creality's closed one. `T<n>` and `BOX_UNLOAD` are high-level and
/// self-contained: box.py owns the whole feed/purge/park sequence, so
/// HelixScreen sends no stock envelope. Detected by `api_version` in the box
/// payload. See docs/devel/printers/CREALITY_K2_SUPPORT.md §
/// "Community Kalico port".
enum class CfsMacroVariant {
    K2,
    K1,
    Fork,
};

/// Shape of the `box` Moonraker object, which varies INDEPENDENTLY of the macro
/// dialect above.
///
/// Stock — Creality's own module (K1 and K2 both): per-unit `T1`..`T4` objects,
/// each holding four parallel arrays (`color_value`, `material_type`, `vender`,
/// `remain_len`), plus top-level `filament` / `map` / `same_material` /
/// `auto_refill`. Material codes need the CfsMaterialDb/FilamentCatalog decode.
///
/// Flat — community Kalico ports carrying a reimplemented box.py: a single
/// `slots[]` array of self-describing objects, plus `loaded_slot`,
/// `slot_filament_mask`, `load_path`, `materials`, `temp_c`, `humidity_pct`.
/// Zero key overlap with Stock. Materials and colors are already resolved, so
/// no code table is involved.
///
/// Detected from the PAYLOAD, never from PrinterDetector: the affected printers
/// report as stock K2 Plus hardware by every model signal, so the firmware swap
/// is invisible to model detection. Bundle QJKZEMTS.
enum class CfsSchema {
    Stock,
    Flat,
};

/// CFS (Creality Filament System) backend — K1 + K2 series printers with RS-485 CFS units
class AmsBackendCfs : public AmsSubscriptionBackend {
  public:
    AmsBackendCfs(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /**
     * @brief Bare filament-sensor name CFS owns: "filament_sensor".
     *
     * K2 CFS exposes one filament_switch_sensor at the toolhead with the bare
     * name "filament_sensor". This name is conventional elsewhere, so it is
     * only claimed when CFS is the detected backend. Static and discovery-free;
     * @p discovery is accepted for signature uniformity. See
     * AmsBackend::sensor_belongs_to_backend (#1054).
     */
    static bool owns_filament_sensor(const std::string& bare_name,
                                     const helix::PrinterDiscovery& discovery);

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::CFS;
    }

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// handle_status_update() stamps SlotStatus::LOADED on the seated bay —
    /// the lane a unit names in T{n}.filament, once the toolhead switch says
    /// filament actually arrived — so the per-slot status carries the answer
    /// the aggregate pair used to hold alone. Before that stamp existed the
    /// parse wrote only AVAILABLE/EMPTY, which left the inherited
    /// can_unload_from_toolhead() false on every CFS slot (#1199).
    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

  protected:
    // Operations. Gated by AmsSubscriptionBackend's NVI wrapper.
    // select_slot_moves_toolhead() stays false: CFS has no select at all
    // (do_select_slot returns not_supported — it loads directly).
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

  public:
    AmsError reset() override;
    AmsError recover() override;
    AmsError cancel() override;

    // Load-vs-swap decision. K1 official CFS upgrade firmware reports a
    // *preloaded* (cassette-staged) slot via current_slot with the nozzle still
    // empty, so on K1 only filament_loaded implies a cut-before-load is needed.
    // K2 keeps the base behavior (filament_loaded OR current_slot >= 0). (#968)
    //
    // Every CFS bay merges into one extruder, so the base class's per-lane
    // independence arm can never fire here — deferring to it on K2 keeps that
    // path in one place without changing the answer.
    [[nodiscard]] bool needs_unload_before_load(const AmsSystemInfo& info,
                                                int target_slot) const override {
        if (macro_variant_ != CfsMacroVariant::K1) {
            return AmsBackend::needs_unload_before_load(info, target_slot);
        }
        return info.filament_loaded;
    }

    // Slot management (user overrides persisted via shared FilamentSlotOverrideStore)
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and fires
    // override_store_->clear_async. CFS firmware populates brand / color_name /
    // total_weight_g from its RFID material database, so those fields are
    // preserved. Only spool_name / spoolman_* / remaining_weight_g are zeroed.
    void clear_slot_override(int slot_index) override;

    /// Publish the external spool as the lane one past the last physical slot
    /// (lane{total_slots+1}) in our lane_data mirror, so OrcaSlicer can select
    /// it. Stock and fork dialects alike: the lane is OUR mirror record, no
    /// firmware involvement.
    void publish_external_spool_lane(const SlotInfo* spool) override;

    // Explicit Clear Spool action. Fork firmware owns the persisted profile;
    // stock CFS dialects have no equivalent command.
    void clear_box_slot_profile(int slot_index);

    // Bypass / external spool. Fork firmware owns the flow (`T<external>` to
    // feed, BOX_UNLOAD to eject — box.py registers T for the external slot
    // alongside the bays); stock K1/K2 firmware has no command for the holder,
    // so enabling is a declaration backed by toolhead-sensor confirmation and
    // the state is derived from the sensor + active-lane pair.
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // Capabilities

    /**
     * @brief CFS auto-refill: available, firmware-managed, no per-slot relation.
     *
     * The box picks the refill spool itself from its own `same_material` groups
     * and exposes no per-slot mapping, so this backend deliberately does NOT
     * override get_endless_spool_config() - the base's empty relation is the
     * truthful answer, and it is what keeps the UI from drawing a backup
     * dropdown that could only ever read "None".
     *
     * `enabled` comes from `box.auto_refill` (stock) / `box.runout_swap_enabled`
     * (flat fork) via AmsSystemInfo::endless_spool_enabled, so on and off are now
     * distinguishable; the old struct hardcoded `supported = true` and buried the
     * real state in an untranslated `description` string.
     *
     * @note Takes `mutex_`; callers must NOT hold it.
     */
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    /// True except on K1, where BOX_MODIFY_TN no-ops (#968) so no confirming
    /// box frame ever arrives. See the definition for the full rationale.
    [[nodiscard]] bool reports_firmware_tool_mapping() const override;

    [[nodiscard]] uint64_t firmware_tool_mapping_generation() const override;
    [[nodiscard]] bool supports_auto_heat_on_load() const override {
        return true;
    }
    [[nodiscard]] bool has_environment_sensors() const override {
        return true;
    }
    [[nodiscard]] bool tracks_weight_locally() const override {
        return false;
    }
    [[nodiscard]] bool manages_active_spool() const override {
        return false;
    }
    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }
    // CFS unloads filament from the toolhead at end-of-print and reloads it as
    // part of the next print-start sequence, so the toolhead is expected to be
    // empty at print-start. The runout sensor reading "no filament" then is by
    // design, not a fault — suppress the pre-print runout warning modal.
    [[nodiscard]] bool auto_unloads_after_print() const override {
        return true;
    }
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

    // Static parsers (public for testing)

    /// Decide which `box` shape this payload is. Stock is the default for
    /// anything ambiguous — every shipped CFS is stock, and misrouting one to
    /// the flat parser would report zero slots.
    [[nodiscard]] static CfsSchema detect_schema(const nlohmann::json& box_json);

    /// Parse a `box` object, dispatching on detect_schema().
    static AmsSystemInfo parse_box_status(const nlohmann::json& box_json);

    /// Stock (`T1`..`T4`) parse. Split out of parse_box_status when the flat
    /// schema arrived; behavior unchanged.
    static AmsSystemInfo parse_stock_box_status(const nlohmann::json& box_json);

    /// Flat (`slots[]`) parse — community Kalico box.py reimplementations.
    static AmsSystemInfo parse_flat_box_status(const nlohmann::json& box_json);

    /// True when this `box` payload comes from the community box.py, i.e. the
    /// firmware speaks CfsMacroVariant::Fork.
    ///
    /// Requires `api_version == 1`, the explicit version for this command
    /// dialect. Do not infer commands from the `slots[]` status layout alone;
    /// another firmware may expose the same flat layout. The Fork commands are
    /// registered in Python, so PrinterDiscovery::has_macro() cannot see them.
    [[nodiscard]] static bool detect_fork_dialect(const nlohmann::json& box_json);

    /// `_BOX_SLOT_SET` — the Fork counterpart to the stock BOX_MODIFY_TN_DATA
    /// color write. Returns "" when the module would reject the command.
    ///
    /// SLOT, MATERIAL and COLOR are all required by box.py's cmd_slot_set, so
    /// unlike the stock path this cannot be a color-only write; the caller must
    /// supply the slot's material. Material is uppercased here to match the
    /// module's own `str(material).strip().upper()` normalization, so a later
    /// status frame echoes back exactly what we sent.
    static std::string slot_set_gcode(int global_slot_index, const std::string& material,
                                      uint32_t color_rgb, const std::string& brand,
                                      const std::string& name, int spoolman_id);

    // GCode helpers (public for testing)
    static std::string load_gcode(int global_slot_index,
                                  CfsMacroVariant variant = CfsMacroVariant::K2);
    static std::string unload_gcode(CfsMacroVariant variant = CfsMacroVariant::K2);

    /// Unload the external / bypass spool on the stock (K1/K2) dialect.
    ///
    /// Not a variant of unload_gcode(). That script ends in the box's own
    /// retract primitive — `CR_BOX_RETRUDE` / `BOX_RETRUDE_MATERIAL` — which is
    /// keyed on a bay: `box_wrapper.cpython-39.so` carries the literal
    /// `[box] retrude, no tnn`. A bypass spool is hand-fed straight into the
    /// toolhead with the box stood down (`BOX_ENABLE_CFS_PRINT ENABLE=0`), so
    /// there is no bay to reel into and the retract silently no-ops. Observed on
    /// a K2 Plus 2026-08-18: the cut ran (`[box] cut to return OK`), nothing
    /// else did, and the filament stayed in the extruder.
    ///
    /// Creality's own answer is the `QUIT_MATERIAL` macro — the non-CFS spool
    /// holder unload that ships in the same config as the `BOX_*` family:
    /// go to the extrude position, heat if filament is present, cut, retract
    /// 10 mm with the extruder, park. Preferred whenever the printer defines it.
    ///
    /// @param variant           Macro dialect; Fork never reaches here (its
    ///                          `BOX_UNLOAD` picks the external branch itself).
    /// @param has_quit_material `QUIT_MATERIAL` is defined on this printer.
    static std::string bypass_unload_gcode(CfsMacroVariant variant, bool has_quit_material);

    /// Load the external / bypass spool on the stock (K1/K2) dialect.
    ///
    /// The mirror of bypass_unload_gcode(), and broken for the same reason
    /// before it existed: load_gcode() resolves a slot through
    /// CfsMaterialDb::slot_to_tnn(), which has no answer for the bypass
    /// sentinel, so do_load_filament() refused with invalid_slot and there was
    /// no way to load an external spool through the app at all.
    ///
    /// Creality's own answer is `LOAD_MATERIAL`, the non-CFS spool-holder load
    /// that ships beside QUIT_MATERIAL: go to the extrude position, save the
    /// fan, pre-flush, then heat and flush — both flush steps gated on the
    /// toolhead switch, so the vendor's workflow is exactly "the user feeds to
    /// the sensor and the macro pulls it in from there". That gating is also the
    /// trap: with nothing at the sensor the macro moves, cools and parks without
    /// touching the extruder, so it reports success having loaded nothing.
    ///
    /// @param variant           Macro dialect; Fork never reaches here (its own
    ///                          T<external> command owns the attended load).
    /// @param has_load_material `LOAD_MATERIAL` is defined on this printer.
    static std::string bypass_load_gcode(CfsMacroVariant variant, bool has_load_material);
    static std::string swap_gcode(int global_slot_index,
                                  CfsMacroVariant variant = CfsMacroVariant::K2);
    static std::string reset_gcode();

    /// Resume the box after an error. Dialect-dependent: K1's box extension
    /// registers no `cmd_error_resume_process`, so `BOX_ERROR_RESUME_PROCESS`
    /// is an unknown command there. See the implementation for the evidence.
    static std::string recover_gcode(CfsMacroVariant variant = CfsMacroVariant::K2);

    /// Result of checking a finished operation against what it was supposed to
    /// achieve. See `verify_phase_outcome()`.
    enum class PhaseVerdict {
        Ok,                    ///< End state matches the operation's intent
        Unverifiable,          ///< No toolhead filament-sensor reading has ever arrived
        LoadDidNotReachNozzle, ///< Load/swap finished with no filament at the nozzle
        UnloadLeftFilament,    ///< Unload finished with filament still at the nozzle
    };

    /// Decide whether a completed CFS operation actually did what it was asked.
    ///
    /// The `BOX_*` primitives are workflow internals, not an API with clean
    /// success semantics: on failure they *record and queue* an error rather
    /// than raising at the failing command, so the gcode script drains
    /// normally and every RPC reports success while nothing moved. Gcode
    /// acceptance therefore proves only that Klipper parsed our text. The
    /// toolhead filament switch is the one independent physical witness, and
    /// this is the rule that reads it. See
    /// docs/devel/CREALITY_CFS_INTERNALS.md § "Failures are deferred".
    ///
    /// Pure: no locking, no member access, so the policy is testable on its
    /// own. `op` is the latched intent (`PhaseTracker::intent`), never
    /// `system_info_.action`, which by completion holds a synthesized
    /// sub-phase instead.
    ///
    /// Deliberately conservative — it reports a failure only on unambiguous
    /// evidence. Without a sensor reading it answers `Unverifiable`, because a
    /// false "load failed" modal on a printer that loaded fine is worse than
    /// staying quiet.
    /// `bypass_unload` exempts the external-spool unload from the
    /// filament-must-be-gone rule. That unload ends with filament still at the
    /// toolhead switch by design: nothing reels a bypass spool back down a lane,
    /// so both QUIT_MATERIAL and our fallback retract clear of the melt zone and
    /// stop, leaving the user to pull the rest out (ui_manual_pull_prompt says
    /// so). Judging it by the bay rule turned every bypass unload into
    /// UnloadLeftFilament, which then disarmed the very prompt that was supposed
    /// to fire.
    [[nodiscard]] static PhaseVerdict verify_phase_outcome(AmsAction op, bool sensor_ever_read,
                                                           bool filament_at_end,
                                                           bool bypass_unload = false);

    /// Human-facing sentence for a non-Ok verdict, or empty for `Ok` /
    /// `Unverifiable`. Separate from the rule so the wording can move without
    /// touching the policy or its tests.
    [[nodiscard]] static std::string phase_verdict_message(PhaseVerdict verdict);

    /// Surface a verification failure to `AmsErrorBridge`, which polls this on
    /// the rising edge into `AmsAction::ERROR`.
    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    /// Recognize the CFS runout handler's give-up messages and turn them into a
    /// CRITICAL runout fault with recovery buttons.
    ///
    /// Unlike AFC's and Happy Hare's overrides this deliberately claims
    /// **non-`!!`** lines: the box announces that it will not swap spools with
    /// `respond_info()`, which reaches us as a `// `-prefixed response. `!!`
    /// lines are handed straight back so the generic classifier keeps owning
    /// every `key8xx` code (including key840's "Reset CFS" action) exactly as
    /// before — that separation is what stops a runout double-surfacing.
    ///
    /// See docs/devel/printers/CREALITY_K2_SUPPORT.md § "Runout and auto-refill"
    /// for the firmware sequence these strings come from.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line, const helix::ClassifyContext& ctx) const override;

    /// The #1199 pair read together for the pre-print unaccounted gate:
    /// toolhead switch detected while no bay letter (T{n}.filament) names the
    /// seated lane. nullopt until the switch has ever published a reading.
    /// current_slot < 0 deliberately includes the bypass sentinel (-2):
    /// bypass suppression is centralized in the gate layer's
    /// any_bypass_active early-out (gate_unaccounted_toolhead_filament,
    /// print_start_checks.cpp) — do not narrow to == -1.
    [[nodiscard]] std::optional<bool> toolhead_filament_unaccounted() const override;

  protected:
    /// Recovery buttons for a CFS runout. **Caller must hold mutex_** (base
    /// contract; this override takes no lock of its own and mutex_ is not
    /// recursive).
    [[nodiscard]] std::vector<helix::RecoveryAction> build_recovery_actions() const override;

    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS CFS]";
    }
    void on_started() override;

    /// Push the user's chosen slot identity back to firmware via the
    /// `BOX_MODIFY_TN_DATA` gcode (registered by the box_wrapper C extension,
    /// not in `gcode/help`). Format reverse-engineered from K2's master-server
    /// binary and confirmed against the K1 module: `BOX_MODIFY_TN_DATA
    /// ADDR=<1..4> NUM=<A|B|C|D> PART=<field> DATA=<value>`. Writes persist to
    /// the box userdata (`creality/userdata/box/tn_data.json`), whose shape is
    /// documented by the #968 reporter dumps.
    ///
    /// Two fields:
    ///  - `color_value`, always: `DATA=0RRGGBB` (RGB hex behind a constant 0
    ///    nibble, matching the firmware's own format).
    ///  - `material_type`, only when a code for the user's material is known:
    ///    `DATA=<6-char code>` (e.g. "000003"). Codes are NEVER synthesized —
    ///    only full codes this printer's firmware itself has reported in a box
    ///    status are eligible (see observed_material_*_), because a malformed
    ///    or unknown code can poison the wrapper's material-DB lookups (flush
    ///    temps, same-material matching) and the stock LCD's slot display.
    ///    Lookup order: catalog product id, then brand|material, then material
    ///    family. No code found → color-only write (previous behavior).
    ///
    /// The two PART writes go out as ONE gcode script. Firmware applies them
    /// sequentially and a status poll can land between the echoes, so the
    /// self-wipe expectation registered with rfid_tracker_ is the SET of
    /// fingerprints the slot may transiently or finally report (intermediate
    /// composite + final pair) via SlotFingerprintTracker::expect_any_of.
    ///
    /// **CRITICAL:** sending invalid args (ADDR=0, malformed payload) triggers
    /// a `TypeError` deep in box_wrapper which Klipper escalates to
    /// `invoke_shutdown` — the entire printer goes offline and needs a full
    /// `RESTART`. This method validates `global_index` in [0, 16) BEFORE
    /// formatting the gcode. Non-fatal on dispatch failure (the override is in
    /// lane_data either way).
    ///
    /// Marked virtual + protected so test subclasses can override and capture
    /// the gcode without a live Moonraker connection.
    virtual void push_slot_identity_to_firmware(int global_index, const std::string& material,
                                                const std::string& brand,
                                                const std::string& catalog_id, uint32_t color_rgb);

  private:
    friend class ::CfsTestAccess;

    std::string current_tnn_;
    bool motor_ready_ = true;

    // K1 vs K2 macro dialect, latched in ctor from PrinterDetector. Most
    // callers route through dispatch_action_script and pull the macro string
    // from the static helpers (load_gcode/unload_gcode/swap_gcode), so this
    // is read on the script-build side, not in hot paths.
    CfsMacroVariant macro_variant_ = CfsMacroVariant::K2;

    /// Monotonic count of box.map parses — firmware-sourced by construction,
    /// since the optimistic path writes system_info_ via assign_tool_slot()
    /// and never touches this (#1270).
    uint64_t firmware_map_generation_ = 0;

    /// Box schema last seen on the wire, latched by handle_status_update.
    ///
    /// Separate axis from macro_variant_ above: the dialect is latched once in
    /// the constructor from the printer model, but the schema cannot be — the
    /// affected printers report as stock K2 hardware, so it is only knowable
    /// from a payload. Stock until a payload says otherwise.
    ///
    /// Payload layout only. The command dialect is selected independently;
    /// Flat + Fork is supported, while an unidentified Flat implementation is
    /// kept off stock command paths.
    CfsSchema schema_ = CfsSchema::Stock;

    /// slots[] index of the `external: true` entry in the last Flat payload,
    /// -1 when none was seen. The Fork firmware registers `T<external_slot>`
    /// itself (external_slot = max_physical_slot + 1, so it moves with the
    /// configured box_count) — this records what the payload actually said
    /// rather than recomputing the arithmetic.
    int external_slot_index_ = -1;

    /// Stock dialect only: the user has declared bypass intent (sidebar toggle
    /// ON after the chained unload). Paired with BOX_ENABLE_CFS_PRINT ENABLE=0
    /// on enable (the box must stand down or it can drive bay filament into a
    /// tube the external spool occupies) and ENABLE=1 on disable. The engaged
    /// display state is confirmed by the toolhead sensor. The Fork dialect
    /// never sets it — its firmware owns the external slot natively.
    bool bypass_declared_ = false;

    /// SUCCESS for stock schemas and the identified Fork dialect; returns
    /// not_supported for an unidentified Flat implementation.
    [[nodiscard]] AmsError reject_if_flat_schema(const char* operation) const;

    /// Stock-dialect bypass derivation: filament at the toolhead with no
    /// active CFS lane means the user hand-fed the external holder — map
    /// current_slot to the -2 sentinel, and back to -1 when the filament
    /// leaves the sensor. Caller must hold mutex_. Flat/Fork is excluded
    /// (that firmware reports the external slot itself) as is any non-IDLE
    /// action (a mid-load sensor rise is the bay feed, not a bypass engage).
    void derive_stock_bypass_locked();

    // Callback lifetime management
    helix::AsyncLifetimeGuard lifetime_;

    /// Dispatch a load/unload/swap CR_BOX_* script with proper completion
    /// semantics: ensures the toolhead is homed, sends the gcode, and flips
    /// `system_info_.action` back to IDLE *only when Klipper finishes the
    /// entire script* (success or error). The previous design relied on the
    /// `filament_switch_sensor` flipping to declare "done" — but that sensor
    /// triggers at the toolhead extruder, which is reached at the *end of
    /// CR_BOX_EXTRUDE* (step 2 of 5). The remaining `CR_BOX_WASTE` and
    /// `CR_BOX_FLUSH` (~3 min of nozzle-at-240 °C extrusion) ran while the
    /// UI told the user the load was idle.
    /// Marked virtual so test subclasses can capture the assembled load/swap/
    /// unload script (and the WITH/WITHOUT-material selection that produced it)
    /// without a live Moonraker connection. Private -- test access to call the
    /// real implementation directly goes through the ::CfsTestAccess friend
    /// shim (tests/test_helpers/cfs_test_access.h), not a `using` declaration.
    virtual AmsError dispatch_action_script(std::string gcode);

    /// Undo the derived LOADED stamp, putting back whatever the last parse
    /// wrote there. Caller must hold mutex_. Runs at the TOP of
    /// handle_status_update so check_hardware_event_clear, the lane_data mirror
    /// and apply_overrides all see firmware truth rather than a synthesized
    /// seat; restoring the saved status (rather than assuming AVAILABLE) is
    /// what keeps a bay firmware called EMPTY from acquiring a phantom spool
    /// when the toolhead clears. A no-op when the slot vector was rebuilt
    /// underneath the stamp, since the fresh parse already wrote truth there.
    void clear_seated_slot_stamp_locked();

    /// Re-derive the LOADED stamp from the aggregate pair and apply it. Caller
    /// must hold mutex_. CFS publishes the seated bay across two signals that
    /// arrive on separate frames — the per-unit T{n}.filament letter names the
    /// lane, the toolhead filament_switch_sensor says whether anything reached
    /// the nozzle — so this runs at the END of handle_status_update, after both
    /// branches have had their say, and again after the optimistic current_slot
    /// writes in load_filament()/change_tool().
    ///
    /// The stamp is applied even over an EMPTY bay: a spool pulled while still
    /// threaded leaves filament at the toolhead that the user must be able to
    /// unload, and refusing to stamp there would blank the active-lane
    /// highlight in exactly that case.
    void apply_seated_slot_stamp_locked();

    /// Global index the LOADED stamp currently sits on, and the status the
    /// parse had written there before it was overwritten. -1 / UNKNOWN when no
    /// stamp is outstanding.
    int seated_stamp_slot_ = -1;
    SlotStatus seated_stamp_prev_ = SlotStatus::UNKNOWN;

    /// Layer a configured FilamentSlotOverride for `slot_index` over `slot`,
    /// mutating `slot` in place. Override wins for every non-default field;
    /// default sentinels (empty strings, spoolman_id 0, weights -1, color_rgb 0)
    /// fall through to firmware-reported data. Callers must hold mutex_.
    /// Called from handle_status_update AFTER firmware parse populates the slot
    /// and AFTER check_hardware_event_clear, so the final SlotInfo visible via
    /// get_slot_info reflects the override layer.
    void apply_overrides(SlotInfo& slot, int slot_index);

    /// Hardware-event detection: CFS exposes per-slot RFID material data. The
    /// composite (material_type + color_value) raw RFID strings form a
    /// per-slot fingerprint. When the fingerprint changes between parses, the
    /// physical spool was swapped — clear the stored override so stale
    /// spool_name / spoolman_id / remaining_weight_g from the previous user
    /// don't bleed onto the new spool.
    ///
    /// Empty observed_uid (no tag / sentinel `-1` / `None`) is treated as
    /// "no signal" — never updates the baseline and never clears. First
    /// observation for a slot establishes the baseline and NEVER fires a
    /// clear. Must be called BEFORE apply_overrides so the clear's field
    /// reset isn't masked by a stale override layer.
    ///
    /// CFS-specific field policy on clear: CFS firmware populates
    /// brand/color_name/total_weight_g from its material database via RFID
    /// lookup, so those fields are NOT zeroed here — the parse has already
    /// written firmware-truth for the newly-inserted spool. Only strictly
    /// override-exclusive fields (spool_name / spoolman_id /
    /// spoolman_vendor_id / remaining_weight_g) are reset.
    ///
    /// One fingerprint component pair — color_value, and material_type when a
    /// firmware-observed code for the user's pick exists — is also WRITTEN by
    /// push_slot_identity_to_firmware, so firmware eventually echoes our own
    /// edit back as a fingerprint change. That echo is not a swap. push_
    /// therefore registers the expected post-write fingerprints with
    /// rfid_tracker_, which classifies the echo as OwnWriteEcho and leaves the
    /// override intact.
    ///
    /// Returns true iff the override was cleared, so the caller can skip the
    /// lane_data mirror for this parse (a DELETE and a POST against the same
    /// lane_data key in one pass is a write race — see handle_status_update).
    [[nodiscard]] bool check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                                  const std::string& observed_uid);

    /// Clear a stale auto-mirrored override when firmware reports the bay
    /// EMPTY. CFS has no other ejection path: the RFID fingerprint LATCHES
    /// after a spool is pulled, so check_hardware_event_clear sees Unchanged
    /// forever and the lane_data record would keep advertising a spool that
    /// isn't there (stale color/material published to OrcaSlicer, plus
    /// apply_overrides promoting the empty bay back to AVAILABLE as a ghost
    /// slot).
    ///
    /// User-locked overrides are RETAINED across an empty bay — a deliberate
    /// assignment means "this is what lives in this slot", and a slot that is
    /// merely unloaded must not lose it. Only unlocked records, which by
    /// construction came from the firmware auto-mirror, are erased. Matches
    /// the AD5X IFS policy of retaining the lane->Spoolman override across
    /// empty (#1071).
    ///
    /// Caller must hold mutex_ and must call this BEFORE apply_overrides.
    /// Returns true iff the override was cleared.
    [[nodiscard]] bool clear_stale_override_on_removal_locked(SlotInfo& slot, int slot_index);

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets strictly override-exclusive fields on
    // the provided SlotInfo (spool_name, spoolman_*, remaining_weight_g), and
    // fires clear_async. Brand / color_name / total_weight_g are preserved —
    // firmware populates them from the RFID material database.
    void clear_override_locked(int slot_index, SlotInfo& slot);

    // Persistent per-slot overrides. Writers (on_started bulk load,
    // set_slot_info persist path, check_hardware_event_clear) all hold
    // mutex_. Reads happen inside apply_overrides, which is also called
    // under mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Per-slot last-observed RFID fingerprint (material_type + "|" +
    // color_value, using the raw pre-strip_code strings), plus the pending
    // expected fingerprints for an identity push we issued. Shared with the
    // other RFID-fingerprint backend (Snapmaker). All access under mutex_.
    helix::ams::SlotFingerprintTracker rfid_tracker_;

    // Firmware-observed material_type code vocabulary, harvested from box
    // status by handle_status_update and consulted by
    // push_slot_identity_to_firmware. Keys are 5-char stripped catalog ids /
    // "brand|type" / "type"; values are the FULL 6-char codes exactly as the
    // firmware reported them (brand prefix included) — those full forms are
    // the only values we ever write back. Insert-if-absent: the first code
    // observed for a key wins, so a value stays stable across frames. All
    // access under mutex_.
    std::unordered_map<std::string, std::string> observed_material_id_codes_;
    std::unordered_map<std::string, std::string> observed_material_codes_;
    std::unordered_map<std::string, std::string> observed_material_type_codes_;

    // Sub-phase synthesis: CFS sets system_info_.action=LOADING/UNLOADING once
    // at gcode dispatch and leaves it there through cut/retract/feed/purge.
    // The step indicator therefore parks on the wrong sub-step (#Task #2).
    // We synthesize CUTTING / UNLOADING / LOADING / PURGING transitions from
    // physical signals — filament-sensor edges and extruder-target rises —
    // and overwrite system_info_.action so the UI's existing step mapping
    // shows the correct phase. All access under mutex_.
    struct PhaseTracker {
        bool active = false; // true between dispatch and on_complete/on_error
        // The operation the user actually asked for, latched at dispatch.
        // system_info_.action cannot stand in for this at completion time:
        // apply_synthesized_action_locked() overwrites it with the synthesized
        // sub-phase (CUTTING / PURGING / ...) as physical signals arrive, so by
        // on_complete it no longer says LOADING or UNLOADING.
        AmsAction intent = AmsAction::IDLE;
        bool started_with_filament = false; // filament_detected at op start
        // Latched alongside intent: this UNLOADING is the external/bypass spool,
        // whose end state legitimately still shows filament at the toolhead.
        bool bypass_unload = false;
        bool seen_filament_drop = false;  // true→false transition (cut completed)
        bool seen_filament_rise = false;  // false→true transition after a drop (new filament fed)
        bool reached_target_once = false; // current_temp ever within 5°C of target this op
        bool pending_purge_target = false; // target rose >10°C above baseline (waits for rise)
        bool seen_purge_signal = false;    // pending_purge_target gated by seen_filament_rise
        int baseline_target_deci = 0;      // extruder target when heating first completed
    };
    PhaseTracker phase_tracker_;
    int last_extruder_target_deci_ = 0;
    int last_extruder_temp_deci_ = 0;
    bool last_filament_detected_ = false;
    // False until the toolhead filament switch has published a real boolean.
    // Klipper reports `filament_detected` as null until the sensor takes its
    // first reading, and a printer without the sensor never publishes one at
    // all — in both cases last_filament_detected_ is a default, not an
    // observation, and phase verification must not draw conclusions from it.
    bool filament_sensor_seen_ = false;

    // Track box.filament_useup transitions. Read-only firmware flag (no BOX_*
    // setter). Decoded from a live runout->reload cycle on the K2 Plus
    // (2026-06-18): it is a runout / path-empty signal — 1 when no filament is
    // established at the box gate (pre-load and runout), 0 when loaded and
    // feeding. Coincides with the runout pause, clears on reload. Logged at
    // debug; not yet surfaced to the UI.
    int last_filament_useup_ = -1;

    // Capture op-start state (filament + extruder target). Sets phase_tracker_.active.
    // Caller must hold mutex_.
    void begin_phase_tracking();

    // Reset phase tracker on op completion. Caller must hold mutex_.
    void end_phase_tracking();

    // Body of dispatch_action_script's success callback: verify the operation
    // achieved what it was asked to, then settle to IDLE (or ERROR when it did
    // not). Takes mutex_ itself. Named rather than inline so tests drive the
    // real completion path instead of a copy of it.
    void finish_action();

    // Drive phase machine on signal changes. Caller must hold mutex_.
    void on_filament_transition_locked(bool new_detected);
    void on_extruder_temp_change_locked(int new_temp_deci, int new_target_deci);

    // Recompute system_info_.action from phase_tracker_ state.
    // Caller must hold mutex_.
    void apply_synthesized_action_locked();
};

} // namespace helix::printer

#endif // HELIX_HAS_CFS
