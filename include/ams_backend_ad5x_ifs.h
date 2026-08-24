// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_IFS

#include "ams_subscription_backend.h"
#include "error_event.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "slot_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Ad5xIfsTestAccess;

/// AMS backend for FlashForge Adventurer 5X IFS (Intelligent Filament Switching).
///
/// IFS is a 4-lane filament switching system controlled by a separate STM32 MCU,
/// driven through ZMOD's zmod_ifs.py Klipper module.
///
/// === Stock zMod vs plugin variants (IMPORTANT for Moonraker visibility) ===
///
/// Stock zMod owns two Klipper objects, `zmod_ifs` and `zmod_color`, that hold
/// the authoritative per-channel state:
///   - zmod_ifs.ifs_data.get_port(port)         -> per-channel HUB presence switch
///   - zmod_ifs.get_ifs_sensor(port)            -> per-channel motion/stall sensor
///                                                 (located INSIDE the IFS, just
///                                                 after the hub — NOT at the toolhead)
///   - zmod_ifs.get_extruder_sensor()           -> toolhead filament switch
///   - zmod_ifs.get_prutok_type_from_config(p)  -> per-channel material string
///   - zmod_color.get_current_channel()         -> active channel (1-based)
///   - zmod_color.get_printer_data_detail()     -> hasMatlStation, indepMatlInfo, ...
///
/// These are `printer.lookup_object()`-only Python APIs. They are NOT exposed via
/// `get_status()`, so Moonraker (and therefore HelixScreen) cannot subscribe to
/// them directly. Stock zMod only gives Moonraker:
///   - filament_motion_sensor ifs_motion_sensor   (single boolean, post-hub)
///   - filament_switch_sensor head_switch_sensor  (toolhead)
///   - Adventurer5M.json                          (polled via Moonraker file API)
///
/// The lessWaste / bambufy plugins close this gap. They are effectively a
/// Moonraker exporter for zmod_ifs/zmod_color, publishing:
///   - filament_switch_sensor _ifs_port_sensor_{1-4}  per-port HUB presence
///     (wraps zmod_ifs.ifs_data.get_port)
///   - save_variables with <prefix>_colors, _types, _tools, _current_tool,
///     _external   (prefix = "less_waste" or "bambufy"; schema identical)
///   - _IFS_VARS gcode macro for atomic writes of the above
///
/// Plugin delta over stock zMod (via Moonraker):
///   (1) per-channel HUB presence as 4 separate booleans
///   (2) live tool->port mapping (16 slots)
///   (3) active tool index with push notifications
///   (4) bypass/external flag
///   (5) atomic, subscribable color+material updates
/// Everything else — including the toolhead switch — is shared with stock zMod.
///
/// === Sensor -> PathSegment mapping ===
///
///   head_switch_sensor        -> TOOLHEAD / NOZZLE (at toolhead)
///   _ifs_port_sensor_{1..4}   -> HUB               (per-channel, plugin only)
///   ifs_motion_sensor         -> OUTPUT            (post-hub, NOT toolhead;
///                                                   single boolean on stock zMod)
///
/// NOTE: `parse_head_sensor()` currently conflates `ifs_motion_sensor` with the
/// toolhead switch. That is a known simplification - motion at the hub does not
/// mean filament has reached the nozzle. Fixing this requires splitting a
/// hub_output presence from head_filament and updating
/// `system_info_.filament_loaded` + `detect_load_unload_completion()` accordingly.
/// Until then, `head_filament_ == false` is NOT evidence the toolhead is empty:
/// the motion sensor reads `filament_detected=false` on a lane that is loaded but
/// idle. Anything that needs an authoritative empty head must use the switch pair
/// `head_switch_seen_ && !head_switch_present_` instead - that is what the #1065
/// row 28 seated head-gate and the runout detector below both do.
///
/// Ports are 1-based (1-4), slots are 0-based (0-3).
/// slot_to_port = slot + 1, port_to_slot = port - 1.
class AmsBackendAd5xIfs : public AmsSubscriptionBackend {
  public:
    AmsBackendAd5xIfs(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendAd5xIfs() override;

    static constexpr int NUM_PORTS = 4;
    static constexpr int TOOL_MAP_SIZE = 16;
    static constexpr int UNMAPPED_PORT = 5;

    /// Which auto-switchover macro package is driving the IFS, if any.
    ///
    /// `None` is stock zMod, whose own `ANALOG_PRUTOK` (zmod_ifs.py) handles
    /// runout-triggered switchover with no plugin required — zmod's user-facing
    /// name is "Infinite Spool Mode". `get_endless_spool_capabilities()`
    /// reports it as `Available`/`FirmwareManaged`/`provider="zmod"`/always-on,
    /// NOT through any AD5X-specific subject. `LessWaste`/`Bambufy` add a
    /// `variable_backup` toggle (default off / on respectively) on top of the
    /// same type+colour+present rule, surfaced as `PluginReadOnly`.
    enum class IfsPlugin : int {
        None = 0,      ///< Stock zMod - no _IFS_VARS macro; ANALOG_PRUTOK runs switchover
        LessWaste = 1, ///< Hrybmo/lessWaste (`less_waste_*` save_variables)
        Bambufy = 2    ///< function3d/bambufy (`bambufy_*` save_variables)
    };

    /// The live switchover state as a tri-state, returned by
    /// backup_state_locked(). Stock zMod is always ON (ANALOG_PRUTOK has no
    /// toggle); the plugin path is the variable_backup value, with UNKNOWN
    /// covering "macro exists but the key was never carried" — and only a
    /// definite OFF justifies telling the user switchover will not happen.
    /// Maps onto helix::printer::EndlessSpoolEnabled in
    /// get_endless_spool_capabilities().
    static constexpr int BACKUP_UNKNOWN = -1;
    static constexpr int BACKUP_OFF = 0;
    static constexpr int BACKUP_ON = 1;

    /// Cadence of the Adventurer5M.json freshness poll. Slower while printing:
    /// FFMInfo holds only per-slot colour/type labels, and each poll is a
    /// loopback HTTP GET on a 2-core board that is also feeding the MCU step
    /// queue. PAUSED keeps the fast cadence — a pause is when a user actually
    /// swaps a spool and relabels it.
    static constexpr std::chrono::seconds JSON_POLL_IDLE{5};
    static constexpr std::chrono::seconds JSON_POLL_PRINTING{30};

    /// Should the JSON freshness poll fire now? (public for testing)
    ///
    /// True when the printing->not-printing edge was just crossed — so the
    /// firmware's post-print FFMInfo revert (#965) is seen without waiting out
    /// the slow interval — or when the cadence for the current state elapsed.
    static bool should_poll_json(bool printing_now, bool was_printing,
                                 std::chrono::steady_clock::duration since_last);

    /**
     * @brief Stock AD5X firmware material whitelist.
     *
     * Sending anything outside this list makes the firmware reject the command with
     * "Invalid material type: X. Valid: PLA, PLA-CF, SILK, TPU, ABS, PETG, PETG-CF".
     * get_supported_materials() seeds its result from here and appends user-defined
     * types; the invariant and catalog-selector tests derive their expectations from
     * this array rather than re-typing it, so a change here propagates to its guards.
     */
    static constexpr std::array<const char*, 7> STOCK_WHITELIST{"PLA", "PLA-CF", "SILK",   "TPU",
                                                                "ABS", "PETG",   "PETG-CF"};

    /**
     * @brief Bare filament-sensor names AD5X IFS owns.
     *
     * Native ZMOD post-hub motion sensor ifs_motion_sensor; toolhead
     * head_switch_sensor; lessWaste per-port _ifs_port_sensor_N; older ZMOD
     * _ifs_motion_sensor_N. Static and discovery-free; @p discovery is accepted
     * for signature uniformity. See AmsBackend::sensor_belongs_to_backend (#1054).
     */
    static bool owns_filament_sensor(const std::string& bare_name,
                                     const helix::PrinterDiscovery& discovery);

    // --- AmsBackend interface ---
    [[nodiscard]] AmsType get_type() const override {
        return AmsType::AD5X_IFS;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::LINEAR;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] bool is_bypass_active() const override;

    // Unload-action gate (also suppresses Load for the same slot). Keeps the
    // firmware's active slot unloadable even when a runout has cleared the head
    // sensor and dropped the slot's display status below LOADED — that runout is
    // precisely when the user needs Unload (#995). The same flag keeps Load
    // hidden on the active slot, which is intended: don't offer Load on a slot
    // the firmware still considers seated.
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

    /// update_slot_from_state() stamps SlotStatus::LOADED on the seated lane
    /// from the firmware's own active-lane pointer plus the head sensor — the
    /// same two inputs system_info_.filament_loaded is assigned from — and it
    /// re-runs on every path that moves either one, including the
    /// FFMInfo.channel adoption in parse_adventurer_json. Reading that stamp
    /// therefore never contradicts the aggregate pair, and it survives the #995
    /// runout that drops a lane's port sensor while its filament is still at the
    /// toolhead (prestonbrown/helixscreen#1199).
    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

    /// AD5X IFS is the one backend whose filament macros home inside firmware.
    ///
    /// Both toolhead macros open with `_G28`: `_IFS_REMOVE_CURRENT_PRUTOK` (the
    /// unload HelixScreen dispatches for a loaded toolhead) and
    /// `_INSERT_PRUTOK_IFS` (behind the `INSERT_PRUTOK_IFS` load), the latter
    /// being `_G28` -> heat -> feed -> purge.
    ///
    /// `_G28` is CONDITIONAL, not an unconditional home. Its whole body is
    /// `{% if "xyz" not in printer.toolhead.homed_axes %} _HOME {% endif %}`
    /// (ZMOD 1.7.1 `mod/_mod/translate/*/base.cfg`, identical in all 12 language
    /// copies). So it homes an unhomed toolhead and no-ops on a homed one, and
    /// pairing it with ensure_homed_then() does NOT home twice: our `G28`
    /// (itself `_HOME`, via ZMOD's `G28` override in the same base.cfg) leaves
    /// `homed_axes` == "xyz", and the macro's `_G28` then falls through. See
    /// prestonbrown/helixscreen#1248, which read the macro as an unconditional
    /// home and reported a double home that does not occur.
    ///
    /// What this flag is about is the rest of the macro, not the home. The AD5X
    /// has a loadcell Z, and the macros drive the toolhead across the bed on
    /// their own authority (`_GOTO_TRASH`, `_SBROS_TRASH`, `_CLEAR_REZINA`
    /// nozzle wipe) with a `_G28` in front that WILL fire whenever `homed_axes`
    /// has been cleared - a Klipper error, an `M84`, a cold resume. Issued while
    /// a job owns the toolhead, that motion reaches the part, tripping ZMOD's
    /// ZCONTROL_AUTO force trip and shutting Klipper down - recoverable only by
    /// a firmware restart (bundle XWPBR2DX, commit 329e731e9). Layer 1
    /// (reject_homing_during_active_print) cannot help: the `_G28` is buried in
    /// the firmware macro and never crosses our gcode API.
    ///
    /// So this backend keeps refusing load/unload/change_tool while PAUSED as
    /// well as while PRINTING. That protection was earned on a real shutdown and
    /// must not be relaxed on the strength of the `homed_axes` guard alone.
    [[nodiscard]] bool filament_ops_self_home() const override {
        return true;
    }

    // Seated-channel-aware: a non-seated lane cold-ejects, so the menu reads
    // "Eject" even when the firmware dropped its active pointer. Mirrors
    // unload_filament()'s eject-vs-toolhead routing (drift-guarded by test).
    [[nodiscard]] bool slot_unloads_to_toolhead(int slot_index, bool loaded_hint) const override;

  protected:
    // Gated by AmsSubscriptionBackend's NVI wrapper. filament_ops_self_home()
    // above is what makes that gate refuse while PAUSED too.
    // select_slot_moves_toolhead() stays false: SET_EXTRUDER_SLOT only points
    // the IFS at a port.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

  public:
    // ZMOD's INSERT_PRUTOK_IFS self-swaps: _INSERT_PRUTOK_IFS runs
    // IFS_REMOVE_CURRENT_PRUTOK first, which no-ops on an empty head sensor and
    // otherwise heats to the seated lane's configured temp before backing it
    // out (zmod_ifs.py cmd_IFS_REMOVE_CURRENT_PRUTOK). A helix-side
    // unload-before-load is therefore redundant — and the sidebar's swap path
    // routed it to eject_lane(-1) when the head read empty, silently dropping
    // the load (Vger1700, bundle Z5V4K3NL). Load straight through the macro,
    // exactly like the stock screen (zmod_color.py).
    [[nodiscard]] bool needs_unload_before_load(const AmsSystemInfo& info,
                                                int target_slot) const override {
        (void)info;
        (void)target_slot;
        return false;
    }

    // INSERT_PRUTOK_IFS resolves the target lane's configured material temp
    // (get_prutok_config(prutok)['temp']) and _INSERT_PRUTOK_IFS does its own
    // M104 + TEMPERATURE_WAIT, so the UI preheat poll is unnecessary. The
    // backend phase tracker synthesizes the Heat-nozzle step from extruder temp
    // frames, so heat progress still renders in the sidebar.
    [[nodiscard]] bool supports_auto_heat_on_load() const override {
        return true;
    }

    // Cold per-lane eject / recover (#996). Issues `IFS_F11 PRUTOK={port}
    // CHECK=0` — a cold retract that drives one idle lane's feed motor backward
    // toward the spool. It does NOT heat the hotend and (CHECK=0) ignores the
    // lane presence/runout sensor, so it recovers a snapped chunk stuck in an
    // idle lane. Refuses the slot currently loaded to the toolhead (use Unload
    // first). Not routed through ensure_homed_then() — no toolhead move.
    AmsError eject_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_eject() const override {
        return true;
    }
    [[nodiscard]] bool supports_force_eject() const override {
        return true;
    }

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    /// Pre-print unaccounted gate: the SWITCH pair only, never head_filament_
    /// alone (motion-sensor false negatives, see the block comment above).
    /// nullopt until the switch has ever published a reading.
    [[nodiscard]] std::optional<bool> toolhead_filament_unaccounted() const override;

    /// Which auto-switchover plugin the live printer has, from the same two
    /// signals set_slot_info()/parse_save_variables() already trust: the
    /// detected variable prefix and the `gcode_macro _ifs_vars` existence latch.
    /// Returns IfsPlugin::None whenever has_ifs_vars_ is false - stale
    /// `less_waste_*` rows left behind by an uninstalled plugin must not read as
    /// "installed", which is exactly what the latch exists to prevent.
    [[nodiscard]] IfsPlugin get_plugin() const;

    /// The plugin's `variable_backup` setting, or nullopt when there is no
    /// plugin (stock zMod) or the macro's get_status() dict never carried the
    /// key (a version that does not declare it). Distinct from
    /// backup_state_locked(): stock zMod returns nullopt here but backup_state
    /// reports ON, because ANALOG_PRUTOK is always-on regardless of any plugin.
    /// nullopt means UNKNOWN for the plugin path and must never be reported as
    /// "off" - see BACKUP_UNKNOWN.
    [[nodiscard]] std::optional<bool> plugin_backup_enabled() const;

    /// True while the backend is holding an unattended-runout fault (see
    /// evaluate_runout_locked). Distinct from `action == ERROR`, which the
    /// operation-timeout backstop also produces.
    [[nodiscard]] bool runout_active() const;

    // === Endless spool ===
    //
    // The cross-backend view of what get_plugin() / plugin_backup_enabled()
    // already know, and the ONLY path this state takes to the UI. The
    // AD5X-specific `ams_ifs_plugin` / `ams_ifs_backup_enabled` subjects are
    // gone: AmsState now publishes `ams_endless_state` / `ams_endless_text` from
    // these capabilities for every backend, so a per-firmware subject could only
    // ever have described one printer's answer.

    /**
     * @brief IFS auto-switchover as a shared capability.
     *
     * Three modes, all `Available` — switchover exists in stock zMod too:
     *   - stock zMod (`!has_ifs_vars_`): `ANALOG_PRUTOK` runs unconditionally
     *     on head runout. `FirmwareManaged` / `provider="zmod"` / always-on.
     *   - bambufy: `variable_backup` (default on) gates `_RUNOUT_HEAD`.
     *     `PluginReadOnly` / `provider="bambufy"`.
     *   - lessWaste: `variable_backup` (default off) gates `_RUNOUT_HEAD`.
     *     `PluginReadOnly` / `provider="lessWaste"`.
     * `enabled` mirrors `variable_backup` including its genuine Unknown on the
     * two plugin paths; stock zMod has no toggle, so it reports a definite On.
     *
     * Deliberately **read-only** rather than editable: `backup` is never written
     * today, and `write_ifs_var()` rides the same `_IFS_VARS` unknown-command
     * latch that has already been seen to drop out from under us mid-session -
     * an editable toggle would silently stop working with no way to tell.
     *
     * get_endless_spool_config() is deliberately NOT overridden either: the
     * firmware computes the match at runout time (find_backup_slot_locked) and
     * stores no per-slot relation, so the base's empty relation is the honest
     * answer. endless_spool_backup_eligibility() is where the rule is exposed.
     *
     * @note Takes `mutex_`; callers must NOT hold it.
     */
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;

    /**
     * @brief The AD5X switchover rule, not the generic material-compatibility one.
     *
     * Exact material AND exact colour AND the port reporting filament present -
     * the same three conditions find_backup_slot_locked() applies, sharing one
     * implementation so the advertised rule and the enforced rule cannot drift.
     *
     * @note Takes `mutex_`; callers must NOT hold it.
     */
    [[nodiscard]] helix::printer::BackupEligibility
    endless_spool_backup_eligibility(int slot_index, int backup_slot) const override;

  protected:
    /// The recovery buttons for whichever fault is currently latched.
    ///
    /// Two shapes: the operation-timeout fault keeps the historical lone
    /// "Recover" (IFS_UNLOCK), and an unattended runout gets the Resume / Purge
    /// / Recover set (see the implementation for why there is deliberately no
    /// "Load" button). Caller holds mutex_ (the base declares that contract;
    /// mutex_ is non-recursive, so this must not lock).
    [[nodiscard]] std::vector<helix::RecoveryAction> build_recovery_actions() const override;

  public:
    // Backend-driven step model for the right-side vertical operation tracker.
    // The AD5X synthesizes 3 firmware phases (HEATING→CUTTING→UNLOADING /
    // HEATING→LOADING→PURGING) from extruder temp + head sensor in
    // apply_phase_action_locked(); these expose them as labelled steps + the
    // current-step subject so the tracker advances instead of falling back to
    // the legacy coarse AmsAction model.
    [[nodiscard]] OperationStepModel get_operation_step_model(StepOperationType op) const override;
    [[nodiscard]] lv_subject_t* get_operation_step_index_subject(StepOperationType op) override;

    // User-initiated state refresh. Re-reads Adventurer5M.json (the JSON poll
    // is the primary truth source on both old and new zmod) and schedules a
    // GET_ZCOLOR SILENT=1 follow-up to refresh the active-slot view. Both
    // calls are debounced/coalesced internally so this is safe to invoke
    // from screen-activation hooks.
    void request_resync() override;

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    // Weight-only persist: updates remaining/total weight in the override store
    // and NEVER rewrites Adventurer5M.json / _IFS_VARS or re-locks material —
    // the firmware-facing writers in set_slot_info() are what reverted the
    // user's material on every 60 s consumption persist (#981).
    void update_slot_weight(int slot_index, float remaining_weight_g, float total_weight_g,
                            bool persist) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Tool reassignment is only persistable when the lessWaste/bambufy plugin
    // is loaded (has_ifs_vars_): `_IFS_VARS tools=...` writes save_variables
    // that the plugin replays at print start. On native ZMOD the macro is
    // absent and set_tool_mapping() can only mutate local state, which the
    // firmware ignores — surface that as caps={false,false} so the print
    // start path doesn't silently drop user-supplied remaps.
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and kicks off
    // override_store_->clear_async so the Moonraker lane_data entry is deleted.
    // The eject path (parse_adventurer_json detecting an empty ffmColor while
    // port_presence was true) shares this routine so the field-reset policy
    // stays in one place.
    void clear_slot_override(int slot_index) override;

    /// Publish the external spool as lane{N+1} in the lane_data namespace.
    /// ZMOD never writes lane_data — our mirror owns the namespace entirely,
    /// so unlike AFC/HH there is no firmware writer to collide with.
    void publish_external_spool_lane(const SlotInfo* spool) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;

    // IFS firmware persists color + material type but NOT spoolman_id,
    // so ToolState must handle spool assignment persistence via Moonraker DB.
    [[nodiscard]] bool has_firmware_spool_persistence() const override {
        return false;
    }

    // Match the AFC/Happy Hare pattern: HelixScreen must NOT auto-call
    // server.spoolman.post_spool_id for AD5X. AD5X has no per-spool identity
    // (no RFID/color sensing) so the lane->spool link can be stale after a
    // physical swap; auto-firing set_active_spool against a stale link bumped
    // "last used" on the wrong spool (#1071, symptom A). AD5X has no native
    // active-spool mechanism either, so this simply disables auto active-spool
    // tracking for this printer. All Spoolman writes become explicit-user-only.
    [[nodiscard]] bool manages_active_spool() const override {
        return true;
    }

    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }

    // IFS retracts filament from the extruder at end-of-print by default, so
    // the toolhead is expected to be empty at the next print-start. Suppresses
    // the pre-print runout warning modal.
    [[nodiscard]] bool auto_unloads_after_print() const override {
        return true;
    }

    // AD5X IFS firmware (ZMOD) validates material against a fixed whitelist
    // and rejects anything outside it with "Invalid material type: X. Valid: ...".
    // The UI dropdown is filtered to this list and outgoing values are normalized
    // via normalize_material() before being sent to firmware.
    [[nodiscard]] std::optional<std::vector<std::string>> get_supported_materials() const override;

    // Firmware-specific aliases for the shared normalize_material() pipeline.
    // AD5X treats SILK as distinct from PLA, but the shared filament DB
    // groups silk variants under compat_group "PLA" (most printers don't
    // make that distinction), so without these aliases "Silk PLA" would
    // collapse to "PLA" instead of "SILK".
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    get_material_aliases() const override;

    // Parse `[zmod_ifs] filament_<NAME>: <TEMP>` lines out of a user.cfg body.
    // Returns the NAME tokens (filament type tags as written, e.g. "PLA+").
    // Stateless + public so tests and external callers can drive it directly.
    static std::vector<std::string> parse_user_cfg_filament_types(const std::string& body);

    // Result of parsing a GET_ZCOLOR SILENT=1 response. Public so tests can
    // construct instances via Ad5xIfsTestAccess.
    struct ZColorSlot {
        std::string material;
        std::string hex; // Empty for old-format zmod responses (no /HEX).
    };

    struct ZColorSilentResult {
        bool is_prompt_fallback = false; // Response was an action:prompt dialog
        bool is_old_format = false;      // Slot lines had no /HEX segment
        bool ifs_active = false;         // "IFS: True" in summary line
        bool saw_valid_response = false; // Matched at least one summary or slot line
        // True when a genuine GET_ZCOLOR summary or slot line was parsed (as
        // opposed to ONLY the IFS_STATUS JSON). Proves SILENT actually works on
        // this device, which retires the false prompt-demotion (#981).
        bool saw_silent_content = false;
        // True only when the GET_ZCOLOR "// Extruder: ..." summary line was parsed
        // (the line that feeds extruder_slot). Slot lines also set
        // saw_silent_content but carry no Extruder field, so this distinguishes a
        // definitive head reading (extruder_slot reflects "N" or "None") from a
        // frame that simply lacked the summary line — without it a slot-line-only
        // frame would have extruder_slot == nullopt and falsely clear head state.
        bool saw_extruder_summary = false;
        std::optional<int> current_channel;
        std::optional<int> extruder_slot; // 0-based, absent when "None"
        // Seated/engaged channel from IFS_STATUS "Chan" (1-based, 0 = none).
        // Distinct from current_channel (the stale "(N)" paren form, unused) and
        // from extruder_slot (the live "Extruder:" feed view, which reads "None"
        // while loaded-idle). Chan persists at the physically seated port, so it
        // is the seated-channel authority for active_tool_/current_slot.
        std::optional<int> ifs_chan;
        // Per-port presence from IFS_STATUS "Ports" (RS-485 silk sensors, 1 entry
        // per port, index 0 = port 1). The sensor-backed presence truth — present
        // whenever IFS_STATUS returns clean JSON, independent of the GET_ZCOLOR
        // SILENT slot lines and of the persisted ffmColor cache. When set, it is
        // the presence authority (apply_zcolor_result), which makes empty-channel
        // resurrection from a stale ffmColor structurally impossible (#981).
        std::optional<std::array<bool, NUM_PORTS>> ifs_ports;
        std::array<std::optional<ZColorSlot>, NUM_PORTS> slots;
    };

  protected:
    void on_started() override;
    void on_stopping() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS AD5X-IFS]";
    }

    /// load_filament()/unload_filament() arm the phase tracker (HEATING +
    /// begin_phase_tracking_locked()) BEFORE calling ensure_homed_then() --
    /// on decline, the base class's IDLE reset alone leaves the tracker
    /// active, and apply_phase_action_locked() has no `!= IDLE` guard, so the
    /// very next extruder-temp frame re-arms HEATING. Unwind the tracker too.
    void on_home_confirmation_declined() override;

  private:
    friend class Ad5xIfsTestAccess;
    friend class Ad5xPerSlotLoadedHelper;

    void parse_save_variables(const nlohmann::json& vars);
    void parse_port_sensor(int port_1based, bool detected);
    void parse_head_sensor(bool detected);
    // Belt-and-suspenders head-loaded derivation from GET_ZCOLOR's "Extruder:"
    // summary, for native Z-Mod where the head switch sensor may not stream
    // under the stock filament_*_sensor sections (BUG-B, #1065). Asserts loaded
    // on "Extruder: N" and clears on "Extruder: None" ONLY when physical presence
    // corroborates an empty head — never strands seated filament on a firmware
    // that drops the extruder pointer post-runout (C1/#995) nor clobbers a real
    // head switch sensor whose lane still reads present (C2). Returns true if
    // head_filament_ changed. Caller MUST hold mutex_.
    bool derive_head_loaded_from_summary_locked(const ZColorSilentResult& result);
    // A COMMANDED unload that reached its terminal state empties the toolhead, so
    // clear head-loaded directly. Unlike a passive "Extruder: None" (runout/print-
    // end, #995), a tracked unload is unambiguous and must NOT wait on the lane-
    // presence corroboration in derive_head_loaded_from_summary_locked(): filament
    // often parks in the lane after an unload, leaving the silk sensor present, so
    // that path would leave the slot stuck LOADED. Caller MUST hold mutex_.
    void clear_head_loaded_after_unload_locked();
    /// Is the toolhead empty, for the purpose of choosing between the heated
    /// toolhead unload and a cold per-lane eject?
    ///
    /// Single source of truth for three sites that MUST agree —
    /// do_unload_filament()'s router, its context-menu mirror
    /// slot_unloads_to_toolhead(), and eject_lane()'s seated-lane refusal. If
    /// they disagreed, "Unload" could route to eject_lane() only for eject_lane()
    /// to refuse it with "Unload from toolhead first."
    ///
    /// The SWITCH pair is the authority when it has ever reported, never the
    /// conflated head_filament_: parse_head_sensor() writes head_filament_ from
    /// BOTH the switch AND ifs_motion_sensor, and the motion sensor is
    /// device-confirmed to read filament_detected=false on a lane that is loaded
    /// but idle (class-header NOTE). Claiming "empty" off that false negative
    /// sends seated, un-cut filament to a cold eject, which grinds it (raza616
    /// #981, bundle 5HR3HHS6). The opposite error - claiming loaded when empty -
    /// only wastes a firmware no-op, so the predicate is deliberately biased
    /// toward "loaded."
    ///
    /// On motion-only firmware head_switch_seen_ stays false and this falls back
    /// to !head_filament_ - exactly the historical behaviour, no silent change.
    ///
    /// PROXY, not the firmware's own gate. cmd_IFS_REMOVE_CURRENT_PRUTOK
    /// early-returns on get_extruder_sensor() (zmod_ifs.py:1149), which reads an
    /// ADC - `temperature_sensor filamentValue`, result = value >= 0.72 when
    /// value > 0.3, and True otherwise, i.e. a missing reading counts as loaded
    /// (zmod_ifs.py:353-361). HelixScreen does not subscribe to `filamentValue`
    /// anywhere. Subscribing to it is the proper fix and would make this
    /// predicate exact; it needs a real AD5X to confirm the object is actually
    /// published, and we have neither hardware nor an `ad5x` mock profile.
    /// Caller MUST hold mutex_.
    [[nodiscard]] bool head_empty_for_unload_routing_locked() const;
    // One-shot fetch of /mod_data/user.cfg. Parses the [zmod_ifs] section for
    // `filament_<NAME>: <TEMP>` entries — zmod's mechanism for user-defined
    // material types beyond the AD5X firmware whitelist (e.g., PLA+, RPLA,
    // HELIX). 404 → no-op (not a zmod printer or no user.cfg present).
    void fetch_user_cfg_materials();
    // One-shot fetch of /mod_data/filament.json — zmod's per-filament-type
    // table holding the unload tube length (filament_tube_length) and feed
    // speed (filament_ifs_speed) that eject_lane() needs for a real, full lane
    // retract. Mirrors read_adventurer_json's download_file + tok.defer
    // threading discipline. 404 → flip filament_json_supported_ false for the
    // session (not a zmod printer). filament.json changes rarely so this is a
    // one-shot fetch at startup/reconnect, NOT a periodic poll.
    void fetch_filament_json();
    // Pure parse helper (no IO) — populates filament_eject_params_ +
    // filament_eject_default_ from a filament.json body. Static-shaped but
    // mutates members under mutex_; exposed to tests via Ad5xIfsTestAccess.
    // Tolerant of malformed JSON (logs + leaves cache untouched).
    void parse_filament_json(const std::string& content);
    void update_slot_from_state(int slot_index);
    // Layer any configured FilamentSlotOverride for `slot_index` over `slot`,
    // mutating `slot` in place. Override wins for every non-default field;
    // default values (empty string, 0, -1.0 weights) fall through to the parsed
    // firmware data untouched. Called from update_slot_from_state so every
    // parse path (save_variables, Adventurer5M.json, GET_ZCOLOR SILENT=1) picks
    // up the override before the SlotInfo is exposed via events.
    void apply_overrides(SlotInfo& slot, int slot_index);
    // External-edit sync: if the firmware-reported color for `slot_index`
    // differs from the previously observed value, treat it as an external
    // color/material edit (Mainsail console, AD5X LCD, native zmod dialog,
    // CHANGE_ZCOLOR from any non-Helix path) and refresh the stored override
    // so Moonraker DB lane_data stays in sync with zmod truth. This is what
    // OrcaSlicer's MoonrakerPrinterAgent reads. The sync ONLY writes to
    // Moonraker DB via override_store_->save_async — it does NOT issue
    // CHANGE_ZCOLOR or write Adventurer5M.json, so it cannot trigger zmod's
    // material-confirmation popup.
    //
    // Sync policy (NOT clear — color change alone is NOT a physical-swap
    // signal, and treating it as one wiped lane_data every time the user
    // retitled a color via the printer LCD; bug surfaced via compulsivejohnny
    // on Discord):
    //   - Existing override: update color_rgb + material in place, fire save_async.
    //     brand/spool_name/spoolman_*/weights/color_name are PRESERVED.
    //   - No existing override: create a minimal one carrying just color +
    //     material (so lane_data has a record for Orca even when the user has
    //     never edited the slot via Helix), fire save_async.
    //   - Slot is empty (no filament — placeholder #808080 read from JSON):
    //     update baseline only, no sync. Eject is handled separately by
    //     parse_adventurer_json calling clear_override_locked directly.
    //
    // Called from update_slot_from_state BEFORE apply_overrides, so the check
    // sees firmware-truth (not the override-masked value). First observation
    // on a given slot is a baseline and never fires a sync.
    //
    // `observed_color = nullopt` is the explicit "no color reading" signal —
    // empty colors_[idx] (parse hasn't filled yet, transient JSON race). It
    // is ignored: never updates the baseline, never fires a sync. Pure black
    // (#000000 = 0x000000) is a legitimate user color and MUST be passed as
    // `std::optional<uint32_t>{0}`, not nullopt — the previous "0 = no signal"
    // sentinel silently dropped genuine black filament from external-edit
    // detection.
    //
    // Returns true if a sync was actually triggered (color delta detected,
    // slot has filament). Caller uses this to decide whether to push the
    // updated colors_/materials_ snapshot through _IFS_VARS so the
    // lessWaste/bambufy plugin's private save_variables track zmod truth.
    bool check_external_color_change(int slot_index, std::optional<uint32_t> observed_color,
                                     bool slot_has_filament);
    // Material counterpart to check_external_color_change. A firmware TYPE
    // change that leaves the color unchanged (user picks a new material on the
    // zmod COLOR menu / LCD, or an external CHANGE_ZCOLOR ... TYPE=) never
    // trips the color detector, so a non-locked override's baked material used
    // to go stale and mask firmware truth forever — color updated, type stuck
    // (raza616, prestonbrown/helixscreen#981/#1065). Same contract as the
    // color detector: called BEFORE apply_overrides, first observation is a
    // baseline, empty material is the "no reading" signal (ignored), and on a
    // real delta it fires sync_override_to_firmware_locked() which refreshes
    // the override's material via the OverwriteAlways mirror — user-locked
    // materials (#965) are still skipped there. Returns true if a sync fired.
    bool check_external_type_change(int slot_index, const std::string& observed_material,
                                    std::optional<uint32_t> observed_color, bool slot_has_filament);
    // Sync helper used by check_external_color_change. Caller must hold mutex_.
    // Updates an existing override's color_rgb + material, or creates a
    // minimal one if none exists. Fires save_async to push the result to the
    // Moonraker DB lane_data namespace. Returns true if anything actually
    // changed (i.e. save_async was issued); false on the in-sync short-circuit.
    bool sync_override_to_firmware_locked(int slot_index, uint32_t firmware_color,
                                          const std::string& firmware_material);
    // Shared helper for every override-clear path (eject detected in
    // parse_adventurer_json and explicit user request via clear_slot_override).
    // Caller must hold mutex_. Erases overrides_[slot_index], resets
    // override-exclusive fields on the provided SlotInfo (brand, spool_name,
    // spoolman_*, weights, color_name), and fires clear_async on the override
    // store. Firmware-sourced fields are left untouched.
    void clear_override_locked(int slot_index, SlotInfo& slot);
    // External-CHANGE_ZCOLOR counterpart to clear_override_locked. Caller must
    // hold mutex_. An external CHANGE_ZCOLOR is a deliberate firmware edit of
    // color/material — firmware truth must win for THOSE fields — but brand /
    // spool_name / spoolman_id / spoolman_vendor_id / weights / color_name are
    // HelixScreen-only metadata the firmware CANNOT carry. A routine AD5X LCD
    // load emits a bare `CHANGE_ZCOLOR SLOT=N TYPE=<material>` (no brand), so a
    // full clear_override_locked() would silently drop the user's saved vendor
    // on every physical load (Bug B / #981 regression). This helper instead
    // releases the color/material user-locks and strips the firmware-carryable
    // override fields (color_set/color_rgb/color_name/material) so
    // apply_overrides lets firmware truth through, while RETAINING the identity
    // metadata — mirroring the #1071 insert/eject retention. If nothing but
    // firmware fields were in the override (no identity to keep), it falls back
    // to a full clear_override_locked() erase so the pre-existing #981 tests
    // (locked-but-no-brand overrides) still see a clean wipe.
    void release_locked_override_keep_identity_locked(int slot_index, SlotInfo& slot);
    // Called on the empty->present (physical insert) edge for a lane. Drops the
    // color/material user-lock flags on an AUTO-TRACKED override (one with no
    // real Spoolman binding) so firmware truth for the freshly inserted spool
    // refreshes through the OverwriteAlways auto-mirror. A physical insert emits
    // no CHANGE_ZCOLOR, so the #981 external-edit clear never fires here — the
    // insert itself is the "this lane's contents changed" signal. A lane with a
    // deliberate Spoolman binding (spoolman_id > 0) is left untouched: #1071
    // retains it across an eject/insert cycle. brand/spool_name/spoolman_id/
    // weights are never modified — only the two lock flags. Caller holds mutex_.
    // See docs/devel/FILAMENT_MANAGEMENT.md § "AD5X IFS material/color reconcile".
    void unlock_auto_tracked_override_on_insert_locked(int slot_index);
    void parse_adventurer_json(const std::string& content);
    void read_adventurer_json();
    void register_zcolor_listener();
    // Listener body — extracted so tests can drive it directly without a live
    // MoonrakerClient. Returns true if the line was buffered as part of an
    // in-flight GET_ZCOLOR response (i.e., it was NOT treated as an external
    // change trigger). Buffering-and-suppressing our own response avoids the
    // self-feedback spam loop that hit v0.99.51 (zmod's GET_ZCOLOR macro body
    // echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens which would otherwise re-arm
    // schedule_zcolor_query() at ~2-4 Hz).
    bool on_gcode_response_line(const std::string& line);
    // Apply a zmod COLOR-menu per-slot row as firmware truth. The root "Select
    // print materials" dialog renders one row per slot carrying that slot's
    // CURRENT color/material:
    //   // action:prompt_button 1: SILK|RUN_ZCOLOR SLOT=1 HEX=F330F9 TYPE=SILK|primary|F330F9
    // zmod re-renders it after every edit, so the row lands ~100ms after the
    // user's tap — well ahead of the debounced GET_ZCOLOR, which can race the
    // firmware write and return the pre-edit value (#1065, bundle 482NB943:
    // a type change stayed stale on screen for 40s). Returns true if a row was
    // recognised and applied. Caller must NOT hold mutex_.
    bool apply_color_menu_slot_row(const std::string& line);
    void register_klippy_ready_listener();
    // Re-query `gcode_macro _ifs_vars` and update the latch + has_ifs_vars_.
    // Fired from notify_klippy_ready so a FIRMWARE_RESTART that adds or
    // removes the lessWaste/bambufy plugin macro doesn't leave us caching
    // the wrong has_ifs_vars_ for the rest of the helixscreen session.
    void recheck_ifs_vars_macro();
    void unregister_moonraker_listeners();
    void schedule_json_reread();
    // True when `content` differs from the last observed Adventurer5M.json
    // body. Updates last_json_content_ on change. Single source of truth for
    // the "did the JSON change?" decision used by both the initial read and
    // the periodic poll.
    bool note_json_content(const std::string& content);
    // Lightweight HTTP poll that downloads Adventurer5M.json and only fires
    // schedule_zcolor_query() when content actually changed. Replaces the old
    // unconditional 15s GET_ZCOLOR backstop — the JSON download is invisible
    // to the gcode console, so polling here costs nothing user-visible while
    // still catching native-dialog edits zmod makes outside our gcode path.
    void poll_adventurer_json();

    // GET_ZCOLOR SILENT=1 primary-truth query. zmod's Adventurer5M.json
    // is a stale last-known-colors cache; SILENT=1 emits one line per
    // physically loaded slot (filtered by live per-port sensors) plus a
    // summary line. See project_ifs_data_sources.md for rationale.
    void query_zcolor_silent();
    void schedule_zcolor_query(const char* reason = "unknown");
    void finalize_zcolor_response();
    void apply_zcolor_result(const ZColorSilentResult& result);
    static ZColorSilentResult parse_zcolor_silent(const std::vector<std::string>& lines,
                                                  const char* reason);

    std::string build_color_list_value() const;
    std::string build_type_list_value() const;
    /// Shared shape logic for the `_IFS_VARS colors=` / `types=` payloads.
    /// The two plugins index these arrays differently: bambufy keeps 4-entry
    /// PORT-indexed lists, while lessWaste keeps TOOL_MAP_SIZE-entry
    /// TOOL-indexed lists projected through tool_map_ (`variable_tools` — its
    /// `_RUNOUT_HEAD` backup scan iterates tool slots, not ports, and a
    /// port-indexed 4-entry payload truncates the arrays wholesale, #1247).
    /// `colors` selects which per-port array supplies each entry.
    std::string build_ifs_list_value(bool colors) const;
    std::string build_tool_map_value() const;
    /// Push a correctly-shaped `colors=`/`types=` pair into `_IFS_VARS` after
    /// parse_save_variables() observed a truncated lessWaste array (the
    /// persisted damage from the #1247 bug — SAVE_VARIABLE keeps it across
    /// reboots). Called from handle_status_update() with mutex_ released
    /// because execute_gcode() blocks.
    void dispatch_ifs_vars_repair();
    AmsError write_ifs_var(const std::string& key, const std::string& value);
    AmsError write_adventurer_json(int slot_index);
    // Direct filesystem write to the resolved AD5X-stock-ZMOD config path. Used
    // when helix-screen runs on the same host as Moonraker AND the canonical
    // config path is present + writable; bypasses Moonraker's HTTP upload (which
    // does an os.rename across mount points on AD5X stock-ZMOD and corrupts the
    // file via EXDEV on the symlinked /usr/prog/config target). Returns
    // command_failed if the path isn't set or the read-modify-write fails.
    AmsError write_adventurer_json_local(int slot_index);
    // Resolve the on-disk Adventurer5M.json path when running on the same host
    // as Moonraker. Sets local_adventurer_json_path_ to the realpath of the
    // file if it exists and is regular; otherwise leaves it empty so we fall
    // back to the Moonraker upload path.
    void detect_local_adventurer_json_path();
    void detect_load_unload_completion(bool head_detected);

    // === Unattended runout detection (#1250, reported as #1247) ===
    //
    // The hole this fills: detect_load_unload_completion() only reacts to a head
    // transition while action is LOADING or UNLOADING, and check_action_timeout()
    // only runs while an operation phase is active. A head drop at IDLE with no
    // phase tracking therefore produced nothing at all - the print sat paused with
    // an empty toolhead and HelixScreen said nothing.
    //
    // The authority is the SWITCH pair (head_switch_seen_ && !head_switch_present_),
    // never head_filament_: the motion sensor also writes head_filament_ and reads
    // false on a loaded-but-idle lane, so gating on it would fire on healthy idle.

    /// Record a toolhead SWITCH reading and maintain the runout edge state.
    /// Arms head_empty_since_ only on a genuine present->absent transition, and
    /// clears both the stamp and any latched runout when filament returns.
    /// Caller must hold mutex_.
    void note_head_switch_reading_locked(bool detected);

    /// Stamp "a filament-moving command was just dispatched". Any head-empty
    /// window that opens within RUNOUT_OP_SUPPRESSION of the stamp belongs to
    /// that operation, not to a runout. Covers the paths that leave the backend
    /// IDLE and armless: eject_lane() and the three early returns in
    /// do_unload_filament() that route to it. Caller must hold mutex_.
    void note_filament_op_dispatch_locked();

    /// Evaluate the runout predicate and latch/clear the fault. Returns true when
    /// it changed observable state (so the caller emits EVENT_STATE_CHANGED).
    /// Caller must hold mutex_.
    bool evaluate_runout_locked();

    /// Drop the latched runout back to IDLE. Caller must hold mutex_.
    void clear_runout_locked(const char* why);

    /// Compose the user-facing runout text, including the plugin sentence that
    /// answers "will the printer switch to a backup spool by itself?".
    /// Caller must hold mutex_.
    [[nodiscard]] std::string build_runout_detail_locked() const;

    /// Strict backup-spool match for @p runout_slot: a port qualifies only when
    /// its filament TYPE and COLOUR both equal the ran-out lane's and its own
    /// port sensor reads filament present. Returns a 0-based slot index, or -1
    /// when nothing qualifies. Deliberately strict - this is what the hint text
    /// promises, and promising more than the plugin delivers is the #1247
    /// misexpectation. Caller must hold mutex_.
    [[nodiscard]] int find_backup_slot_locked(int runout_slot) const;

    /// The single-pair form of find_backup_slot_locked()'s rule: could
    /// @p candidate stand in for @p slot? Both that scan and the public
    /// endless_spool_backup_eligibility() run through here, so what the UI would
    /// offer and what the firmware would pick cannot diverge. Caller holds mutex_.
    [[nodiscard]] bool backup_eligible_locked(int slot, int candidate) const;

    /// How long the toolhead must read authoritatively empty, while paused and
    /// idle, before the fault is raised. Longer when a plugin with backup
    /// switching is installed: that plugin's own recovery pauses, unloads and
    /// loads a replacement lane, which takes minutes and must not be
    /// interrupted by us declaring a runout on top of it. Caller holds mutex_.
    [[nodiscard]] std::chrono::seconds runout_confirm_delay_locked() const;

    /// Read the `variable_*` payload out of a `gcode_macro _ifs_vars`
    /// get_status() dict. Returns true when something we publish changed.
    /// Caller must hold mutex_.
    bool parse_ifs_vars_macro_locked(const nlohmann::json& macro_status);

    /// The `variable_backup` tri-state (BACKUP_*). Feeds both the runout warning
    /// log and get_endless_spool_capabilities()' `enabled` axis, so the number in
    /// the log and the sentence on screen cannot disagree. Caller holds mutex_.
    [[nodiscard]] int backup_state_locked() const;

    // === Live load/unload progress phase tracker ===
    //
    // Between the moment WE start a load/unload (load_filament / unload_filament)
    // and the moment the operation finalizes (head transition completes, or the
    // action-timeout backstop fires), this tracker synthesizes the firmware's
    // internal phases from primary signals (extruder temp/target + head sensor)
    // and a secondary, corroborating parse of RESPOND lines. It overwrites
    // system_info_.action so the UI's existing step mapping advances correctly,
    // and sets a dynamic system_info_.operation_detail string.
    //
    // Sequences:
    //   Unload: HEATING → (temp ≥ target) CUTTING → (head drop) UNLOADING → IDLE
    //   Load:   HEATING → (temp ≥ target) LOADING → (head rise)  PURGING   → IDLE
    //
    // active gates the new behavior: when INACTIVE (legacy/external/firmware-
    // initiated action changes), detect_load_unload_completion preserves the
    // historical snap-to-IDLE on a head transition.
    struct IfsPhaseTracker {
        bool active = false;              // true between begin and finalize
        bool is_unload = false;           // unload vs load direction
        bool reached_target_once = false; // current temp ever within ~0.5°C of target
        bool seen_head_drop = false;      // head sensor true→false (cut/retract started)
        bool seen_head_rise = false;      // head sensor false→true (filament reached nozzle)
        int target_deci = 0;              // heat target in deci-degrees (×10), 0 = unknown
        // Set when load_filament() dispatches while another lane is currently
        // seated (seated_chan_ != target). INSERT_PRUTOK_IFS then runs an
        // IMPLICIT UNLOAD of the seated lane before the actual load, which
        // easily runs past the 90s LOADING budget (bundle NJB2U558: ch4 seated
        // → load ch2 took ~2min total, timed out at 90s mid-swap). The flag
        // extends LOADING to SWAP_LOADING_TIMEOUT_SECONDS in check_action_timeout.
        // Cleared by end_phase_tracking_locked on op completion.
        bool swap_expected = false;
    };
    IfsPhaseTracker phase_tracker_;
    int last_extruder_temp_deci_ = 0;   // deci-degrees (×10)
    int last_extruder_target_deci_ = 0; // deci-degrees (×10)

    // Capture op-start state + set active. Caller must hold mutex_.
    void begin_phase_tracking_locked(bool is_unload);
    // Reset tracker (clears active). Caller must hold mutex_.
    void end_phase_tracking_locked();
    // Drive the phase machine on an extruder temp/target frame. Caller holds mutex_.
    void on_extruder_temp_locked(int temp_deci, int target_deci);
    // Drive the phase machine on a head-sensor transition. Caller holds mutex_.
    void on_head_transition_locked(bool detected);
    // Recompute system_info_.action + operation_detail + operation_phase from
    // tracker state. Caller must hold mutex_. Returns true when
    // system_info_.action actually changed — the caller releases mutex_ and
    // then emits EVENT_STATE_CHANGED (the lock contract requires emit_event with
    // the lock NOT held; see ams_subscription_backend.h). NOT emitting here is
    // what left the busy state unpublished until the next ~1.4s status frame.
    bool apply_phase_action_locked();
    // Set system_info_.operation_detail. Caller must hold mutex_.
    void set_operation_detail_locked(std::string detail);
    // Finalize a phased load/unload to IDLE when its zmod macro completes (the
    // gcode ack). The reliable terminal signal for the synthesized Retract /
    // Purge phases, which have no sensor event of their own — without it the op
    // sticks until the 90s timeout flips to ERROR (raza616 stuck-on-Retract /
    // stuck-on-Purging). @p is_unload selects which op this ack belongs to. Takes
    // mutex_ internally; no-op if the op already finalized or a different op is
    // now in flight.
    void finalize_op_after_macro(bool is_unload);

    int find_first_tool_for_port(int port_1based) const;

    // Map active_tool_ -> system_info_.current_slot via tool_map_. Single source
    // of truth shared by handle_status_update and apply_zcolor_result so the
    // seated slot updates immediately when IFS_STATUS reports a new Chan instead
    // of waiting for the next status frame. Caller must hold mutex_.
    void recompute_current_slot_locked();

    // Persist the remembered seated lane to the Moonraker "lane_data" DB so it
    // survives a power cycle (the firmware forgets Chan across a reboot, #1065).
    // slot0 >= 0 writes the lane index; slot0 < 0 clears the key. Fire-and-forget
    // (no-op when there is no override_store_, e.g. in unit tests). Caller holds
    // mutex_; the store call dispatches asynchronously and does not block.
    void persist_seated_slot_locked(int slot0);

    // Belt-and-suspenders for #1065 row 28: the firmware does NOT blank
    // FFMInfo.channel / IFS_STATUS Chan when a lane is ejected (same stickiness as
    // ffmColor/ffmType), so a stale seated pointer at the just-ejected lane would
    // keep offering Unload until the next head-gated poll. If the seated channel
    // or FFMInfo.channel points at the ejected lane, zero both and recompute so
    // the affordance dies immediately. Returns true if it changed state. The lane
    // can't be the genuinely-seated one — eject_lane() refuses that when the head
    // is loaded. Caller holds mutex_.
    bool clear_seated_if_ejected_locked(int slot_index);

    // Debug trace of the seated-authority state (#1065 field confirmation): dumps
    // ffm_channel_ / seated_chan_ / current_slot / head_filament_ / switch
    // authority / port presence so a debug bundle can confirm whether
    // FFMInfo.channel stays stale through an eject->poll and whether the head-gate
    // fired. Debug level, off the hot path. Caller holds mutex_.
    void log_seated_state_locked(const char* where) const;

  private:
    bool validate_slot_index(int slot_index) const;
    void check_action_timeout();
    // Reset the indeterminate ("Working…") no-progress clock. Called on every
    // genuine load/unload progress signal. Caller must hold mutex_.
    void note_phase_progress_locked();

    // Cached state from save_variables
    // Variable prefix: "less_waste" (lessWaste/zmod) or "bambufy" — auto-detected from
    // whichever save_variables are present on the printer.
    std::string var_prefix_ = "less_waste";
    std::array<std::string, NUM_PORTS> colors_;    // Hex strings: "FF0000"
    std::array<std::string, NUM_PORTS> materials_; // Material names: "PLA"

    // Per-filament-type eject parameters parsed from /mod_data/filament.json:
    // material name -> {filament_tube_length (LEN), filament_ifs_speed (SPEED)}.
    // Consumed by eject_lane() to drive a full per-material lane retract via
    // IFS_F11 LEN=.. SPEED=.. instead of the firmware default. Guarded by
    // mutex_. filament_eject_default_ holds the file's "default" entry (or the
    // hardcoded {1000, 1200} when absent) used when a lane's material is empty
    // or not present in the table.
    std::map<std::string, std::pair<int, int>> filament_eject_params_;
    std::pair<int, int> filament_eject_default_{1000, 1200};
    // filament.json availability latch (mirrors json_poll_supported_): starts
    // true, flips false permanently on a 404 so non-zmod printers stop fetching.
    std::atomic<bool> filament_json_supported_{true};
    // User-defined material types extending the firmware whitelist. Two
    // sources, both surfaced via get_supported_materials():
    //   - bambufy_custom_types in save_variables (when bambufy is/was active);
    //     parse_save_variables() populates this regardless of has_ifs_vars_
    //     because user-defined types are orthogonal to plugin activation.
    //   - [zmod_ifs] filament_<NAME>: <TEMP> in /mod_data/user.cfg (zmod's
    //     own mechanism); fetched once via fetch_user_cfg_materials() at
    //     backend start.
    // Guarded by its own mutex (NOT mutex_) so get_supported_materials() —
    // called from normalize_material() inside set_slot_info(), which already
    // holds mutex_ — doesn't deadlock. Both writers (parse_save_variables
    // and fetch_user_cfg_materials) currently take mutex_ AND
    // custom_types_mutex_; lock order is mutex_ → custom_types_mutex_.
    mutable std::mutex custom_types_mutex_;
    std::vector<std::string> custom_material_types_;
    std::array<int, TOOL_MAP_SIZE> tool_map_;   // tool_map_[tool] = port (1-4, 5=unmapped)
    std::array<bool, NUM_PORTS> port_presence_; // Per-port filament sensor state
    // Per-port instant of the last optimistic eject clear. On the constrained
    // AD5X the RS-485 silk sensor lags ~1s after IFS_F11 cold-retracts a lane, so
    // the eject follow-up IFS_STATUS/GET_ZCOLOR can still read the just-ejected
    // lane present and resurrect it. Within EJECT_PRESENCE_SUPPRESSION of the
    // stamp, a false->true presence transition for that lane is ignored so the
    // optimistic clear survives the settling window (#1065 — the last-ejected
    // lane had no later query to re-correct it and kept offering Unload). A
    // present->absent transition and any transition after the window still apply.
    std::array<std::chrono::steady_clock::time_point, NUM_PORTS> last_eject_time_{};
    static constexpr std::chrono::milliseconds EJECT_PRESENCE_SUPPRESSION{4000};
    int active_tool_ = -1; // Current tool (-1 = none)
    // Physically seated port from IFS_STATUS "Chan" (1-4; 0 = none). Persists at
    // the seated port while loaded-idle (when GET_ZCOLOR's "Extruder:" reads
    // None), so it is the seated-channel authority for unload routing. Stored
    // unconditionally — independent of has_ifs_vars_ / tool_map_ — because the
    // tool_map_-derived current_slot can disagree with it on the plugin path.
    int seated_chan_ = 0;
    // Firmware's own record of the seated toolhead lane, parsed from
    // Adventurer5M.json "FFMInfo.channel" (1-based; 0 = none/absent). This is the
    // field the firmware's _IFS_REMOVE_CURRENT_PRUTOK unload macro resolves the
    // seated channel from, and it stays put while idle — unlike IFS_STATUS "Chan",
    // which tracks the last lane the switching mechanism touched, including a
    // zmod COLOR-menu slot SELECTION that moves no filament (#1065 Bug 3, bundle
    // ZT8Y9WPM: editing lane 3 made Chan=3 while FFMInfo.channel stayed 2). When
    // >0 it is the seated authority, overriding a divergent Chan in
    // apply_zcolor_result. 0 (nothing seated, or forgotten across a reboot) falls
    // back to the persisted-lane floor / Chan.
    int ffm_channel_ = 0;
    // Last lane (0-based slot index) we saw loaded to the toolhead, persisted to
    // the Moonraker "lane_data" DB (sibling "seated" key) so it survives a power
    // cycle. The firmware forgets the seated channel across a reboot — IFS_STATUS
    // "Chan" comes back 0 even with a lane physically at the head (bundle
    // CGR6C7PA, #1065). On cold boot, when head_filament_ is true but Chan==0 and
    // this lane's port still reads present, it is restored as the seated channel
    // so the Unload/Eject menu labels correctly. nullopt = nothing remembered.
    std::optional<int> persisted_seated_slot_;
    // Gates the cold-boot seated-lane restore to genuine power-cycle amnesia.
    // False until we observe a definitive seated signal this session — a real
    // IFS_STATUS Chan>0, or a confirmed-empty head (Chan==0 with head_filament_
    // false). While false, a Chan==0 reported with the head still loaded is the
    // post-reboot "firmware forgot which lane" case and the remembered lane is
    // restored. Once true, a Chan==0 is a genuine "nothing seated" and clears the
    // loaded slot as before (#1065).
    bool seated_resolved_since_boot_ = false;
    // Latches true the first time IFS_STATUS "Ports" is observed. Once the
    // RS-485 silk-sensor presence truth is available, (1) the legacy
    // Adventurer5M.json ffmColor presence inference must NEVER run — that
    // inference resurrects an emptied channel from its persisted colour when
    // GET_ZCOLOR SILENT gets (even falsely) demoted; and (2) we keep firing
    // IFS_STATUS even after a SILENT demotion so Ports presence stays live
    // (query_zcolor_silent / schedule_zcolor_query) (#981, bundle EE5L8LY2).
    // Atomic: written under mutex_ in apply_zcolor_result, read unlocked in the
    // schedule/query gates.
    std::atomic<bool> ifs_status_ports_seen_{false};
    bool external_mode_ = false; // Bypass/external spool mode
    bool head_filament_ = false; // Head sensor state
    // Toolhead SWITCH-sensor authority, tracked separately from the conflated
    // head_filament_. parse_head_sensor() writes head_filament_ from BOTH the
    // switch AND the ifs_motion_sensor, and the motion sensor is device-confirmed
    // to read filament_detected=false while a lane is loaded-but-idle (header NOTE
    // above). So head_filament_==false is NOT trustworthy on its own. The
    // FFMInfo.channel / IFS_STATUS Chan head-gate (#1065 row 28) must only reject a
    // sticky seated channel when an AUTHORITATIVE empty-head reading exists — i.e.
    // the switch sensor itself says empty — never on a motion-only false-negative.
    // head_switch_seen_ latches true once the filament_switch_sensor /
    // zmod_ifs_switch_sensor head_switch_sensor namespace reports; head_switch_present_
    // holds the switch's last reading. The gate authority is
    // (head_switch_seen_ && !head_switch_present_). When no switch is published
    // (motion-only firmware), head_switch_seen_ stays false and the gate never
    // fires — the seated lane is preserved (fall back to prior behaviour).
    bool head_switch_seen_ = false;
    bool head_switch_present_ = false;

    // --- Unattended runout detection (#1250 / #1247) ---
    // Set on a genuine head-switch present->absent EDGE; nullopt otherwise. An
    // edge, not a level, on purpose: a printer that boots already paused with an
    // empty toolhead has no runout to report, and a level test would invent one.
    std::optional<std::chrono::steady_clock::time_point> head_empty_since_;
    // Instant of the last filament-moving dispatch (load / unload / change tool /
    // eject). Default-constructed = the epoch, so a fresh backend is never
    // suppressed. See note_filament_op_dispatch_locked().
    std::chrono::steady_clock::time_point last_filament_op_dispatch_{};
    // True while system_info_.action == ERROR *because of* a runout rather than
    // an operation timeout. current_error() and build_recovery_actions() branch
    // on it; recover()/reset()/cancel() clear it alongside the ERROR itself.
    bool runout_active_ = false;
    // 0-based lane that was seated when the toolhead emptied (-1 = unknown). The
    // firmware routinely drops its active pointer on a runout, so this is
    // captured at raise time from whichever authority still had it.
    int runout_slot_ = -1;
    // Head must read authoritatively empty this long, paused and idle, before the
    // fault is raised. Long enough to ride out a status-frame hiccup and to give
    // a firmware-driven sequence a chance to put filament back, short enough that
    // the user is not left staring at a stopped print with no explanation.
    static constexpr std::chrono::seconds RUNOUT_CONFIRM_DELAY{30};
    // The lessWaste backup switchover is itself an unload + load, minutes long,
    // during which the toolhead is legitimately empty on a paused print. Wait it
    // out before claiming the runout is unattended.
    static constexpr std::chrono::seconds RUNOUT_CONFIRM_DELAY_WITH_BACKUP{180};
    // A head-empty window opening within this long after a filament-moving
    // dispatch is attributed to that operation. Covers eject_lane() and
    // do_unload_filament()'s early returns, which leave action IDLE.
    static constexpr std::chrono::seconds RUNOUT_OP_SUPPRESSION{30};

    std::array<bool, NUM_PORTS> dirty_{}; // Per-slot dirty flag to prevent stale overwrites

    helix::printer::SlotRegistry slots_;

    // Native ZMOD IFS has no per-port sensors — infer port presence from active
    // tool + head sensor state so the UI doesn't show all slots as EMPTY.
    bool has_per_port_sensors_ = false;

    // True if _IFS_VARS macro is available (lessWaste or bambufy plugin).
    // False for native ZMOD, which stores color/type in Adventurer5M.json
    // (read/written via Moonraker HTTP file API).
    bool has_ifs_vars_ = false;

    // Latch: starts TRUE (pessimistic) — cleared when a `gcode_macro
    // _ifs_vars` query returns a non-empty variables dict (real macro
    // present). Prevents the race where a notify_status_update with
    // save_variables arrives between subscription registration and the
    // initial query callback, which would set has_ifs_vars_ = true before
    // we've verified the macro is loaded. Re-evaluated on every
    // notify_klippy_ready via recheck_ifs_vars_macro() so a FIRMWARE_RESTART
    // that adds/removes the macro takes effect without restarting
    // helixscreen — also forces has_ifs_vars_ = false when the macro goes
    // missing after a restart, and on Unknown-command responses to our own
    // _IFS_VARS writes (self-heal). Note: Klipper/Kalico return `{}` for
    // missing objects rather than erroring the query, so empty-vs-non-empty
    // is the discriminator, not key presence.
    bool ifs_macro_confirmed_missing_ = true;

    // `variable_backup` from the `gcode_macro _ifs_vars` get_status() dict:
    // lessWaste's "auto-switch to a matching backup spool on runout" toggle
    // (docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md § lessWaste-Specific
    // Variables; a real dump in printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md
    // shows `variable_backup: 0`, i.e. the feature exists but ships OFF).
    // nullopt = the dict never carried the key, which is NOT the same as off and
    // must be reported as unknown. Never observed on a device by us - the value
    // is read and displayed, nothing branches on it except the wording and the
    // longer runout confirm delay.
    std::optional<bool> ifs_backup_variable_;

    // `variable_ifs_unlock_after_boot` from the same dict: lessWaste's own
    // post-boot `IFS_F18` workaround for the IFS lane clamps sticking after a
    // power cycle (the "stock screen glitch" its README names). Log-only
    // visibility — the plugin runs its `_UNLOCK_IFS` delayed gcode itself when
    // this is on, and HelixScreen must not send hardware motion unprompted.
    // Recorded because "a plain reboot may itself change IFS behavior" is the
    // confounder that muddied #1247's screen A/B test; a debug bundle should
    // answer whether the plugin's unlock was armed (#1247).
    std::optional<bool> ifs_unlock_after_boot_;

    // Set by parse_save_variables() when a lessWaste `<prefix>_colors` or
    // `_types` save_variables array arrives with fewer than TOOL_MAP_SIZE
    // entries — the truncation signature of the #1247 mirror bug. Consumed
    // (read + cleared) by handle_status_update() under the same lock hold,
    // then dispatched after unlock. Guarded by mutex_.
    bool ifs_vars_repair_staged_ = false;

    std::atomic<bool> reread_pending_{false};

    // Main-thread-only: counts external color-change detections in the gcode
    // stream since the last coalesced re-read fired. zmod re-emits CHANGE_ZCOLOR
    // on every edit, so a single user action produces a burst of trigger lines
    // (24 in a 3s window in bundle UQG4RNUA). Rather than log one line each, the
    // count is folded into a single consolidated line when reread_apply runs.
    // Both the increment (on_gcode_response_line) and the read/reset
    // (reread_apply) run on the main thread via the UpdateQueue, so no atomic is
    // needed.
    int external_change_burst_count_ = 0;

    // Signature (count + per-slot color/material) of the slots parsed from the
    // last Adventurer5M.json read. Native ZMOD re-reads the file on every sensor
    // change / ~5s poll cycle; comparing against this lets the "Loaded N slots"
    // line log at INFO only when the parsed set actually changed.
    std::string last_parsed_signature_;

    // GET_ZCOLOR SILENT=1 query state.
    // zcolor_silent_supported_ starts optimistic; a prompt-style response
    // flips it false for the session (not retried).
    std::atomic<bool> zcolor_query_active_{false};
    std::atomic<bool> zcolor_query_pending_{false};
    // Coalesce-gate for schedule_zcolor_query(): true while a debounce worker is
    // in flight. zmod re-emits the "Select print materials" prompt on every
    // CHANGE_ZCOLOR, so a single color edit produces a burst of trigger lines —
    // without this gate each one submitted its own fast()-pool worker (20+ in a
    // 40ms window seen in bundle ACJRZBXJ), every one holding a pool slot through
    // its 500ms sleep while only a single query ever fired. zcolor_query_pending_
    // carries the "refresh wanted" signal, so later callers just set it and
    // return. Mirrors reread_pending_ on schedule_json_reread().
    std::atomic<bool> zcolor_schedule_armed_{false};
    std::atomic<bool> zcolor_silent_supported_{true};
    // Latches true the first time GET_ZCOLOR SILENT=1 returns genuine silent
    // content (a summary or slot line — NOT just the IFS_STATUS JSON). Once
    // confirmed, a later prompt dialog is the user's own interactive zmod colour
    // menu colliding with our in-flight query, NOT our query degrading, so we no
    // longer demote zcolor_silent_supported_ on it (#981 false-latch, EE5L8LY2).
    std::atomic<bool> zcolor_silent_confirmed_{false};
    std::mutex zcolor_buffer_mutex_;
    std::vector<std::string> zcolor_response_buffer_;
    // Diagnostic counter — incremented on every schedule_zcolor_query() call.
    // Exposed via Ad5xIfsTestAccess so the listener-feedback regression test
    // can assert that buffered response lines never re-arm a query.
    std::atomic<uint32_t> zcolor_schedule_count_{0};
    // Diagnostic counter — incremented only when a debounce worker is actually
    // submitted (past the zcolor_schedule_armed_ gate). Lets the coalescing test
    // assert that a burst of triggers spawns a single worker, not one each.
    std::atomic<uint32_t> zcolor_worker_submit_count_{0};
    // Diagnostic-only: which operation triggered the next/current GET_ZCOLOR +
    // IFS_STATUS query, threaded into the IFS_STATUS Chan log line for field
    // diagnostics. const char* to string literals (no allocation). _pending_ is
    // set by schedule_zcolor_query(reason); _active_ is promoted from it when
    // query_zcolor_silent() actually fires. Main-thread only; not synchronized.
    const char* zcolor_query_reason_pending_ = "unknown";
    const char* zcolor_query_reason_active_ = "unknown";

    // JSON poll state: download Adventurer5M.json on a slow tick and compare
    // to last-seen content. Hash-by-equality is fine here — file is a few
    // hundred bytes and changes are rare. json_poll_supported_ flips false
    // permanently on a 404 so non-zmod printers stop trying.
    std::atomic<bool> json_poll_in_flight_{false};
    std::atomic<bool> json_poll_supported_{true};
    std::string last_json_content_; // protected by mutex_

    // Action timeout tracking. action_start_time_ is reset on every phase
    // transition (apply_phase_action_locked) so each phase gets its own window.
    // HEATING needs a longer budget: a real AD5X cold-start unload heats
    // ~26°C→230°C in ~158s (longer for high-temp materials approaching 300°C),
    // which far exceeds the 90s general timeout. 300s is a UI backstop only —
    // Klipper's own verify_heater aborts a genuinely stuck heater within ~1-2
    // min and the macro errors out, so this never gates real functionality.
    static constexpr int ACTION_TIMEOUT_SECONDS = 90;
    static constexpr int HEATING_TIMEOUT_SECONDS = 300;
    // A real purge runs far longer than the generic 90 s phase window (raza616:
    // ~3 min whole-op from cold; Vger1700 hit the 90 s ERROR twice mid-purge,
    // #1065). PURGING gets its own budget AND its clock is reset on
    // ifs_motion_sensor activity (see handle_status_update), so the budget is
    // effectively "time since filament last moved" — a long-but-healthy purge is
    // never falsely failed, a genuinely stalled one still surfaces ERROR.
    static constexpr int PURGING_TIMEOUT_SECONDS = 240;
    // INSERT_PRUTOK_IFS with another lane currently seated runs an implicit
    // UNLOAD first (heat → cut → retract, ~50-90s) before the actual load
    // begins. The 90s LOADING budget fires mid-swap on bundle NJB2U558
    // (load ch2 while ch4 seated → "Loading error, feeding filament to nozzle
    // (timed out)" popup even though the op completed). 180s covers the full
    // swap with margin; the head-drop reset in on_head_transition_locked is
    // the primary defence, this is the belt-and-suspenders backstop for
    // firmware variants where the head sensor doesn't transition reliably.
    static constexpr int SWAP_LOADING_TIMEOUT_SECONDS = 180;
    std::chrono::steady_clock::time_point action_start_time_;

    // Indeterminate ("Working…") detector (#1065 row 14). Distinct from the
    // coarse ERROR budgets above: those flip a stalled op to ERROR after minutes,
    // this flips a SHORT ~8s no-progress window into a busy indicator so the
    // frozen live-temp number ("Heat 225/230") doesn't read as a hang while the
    // shared main-thread status feed is starved on the constrained box.
    // last_phase_progress_time_ is reset on every genuine progress signal
    // (temp-VALUE change, head transition, motion, phase change, op start);
    // when it goes stale past the threshold check_action_timeout raises
    // system_info_.operation_indeterminate. last_progress_temp_deci_ gates the
    // temp reset on a value change (not every frame) so a frozen subject — which
    // stops changing value — lets the clock elapse. Both under mutex_.
    static constexpr int INDETERMINATE_THRESHOLD_SECONDS = 8;
    std::chrono::steady_clock::time_point last_phase_progress_time_;
    int last_progress_temp_deci_ = 0; // deci-degrees of the last progress-noting temp frame

    // Rate-limit gate for the JSON-content poll. handle_status_update kicks
    // poll_adventurer_json() if at least kJsonPollInterval has elapsed since
    // the last kick — replaces the old 15s unconditional GET_ZCOLOR backstop.
    // Default-constructed time_point is the epoch, so the first status update
    // after backend start fires a poll immediately.
    std::chrono::steady_clock::time_point last_json_poll_kick_{};

    // RAW_PRINT_STATE_OK: names the wire state this member caches.
    // Was the printer in PrintJobState::PRINTING at the previous status update?
    // Used to spot the printing->done edge and force an off-cadence poll there,
    // so the slower in-print interval never delays seeing the firmware's
    // post-print FFMInfo revert (#965).
    bool json_poll_was_printing_ = false;

    // User-provided per-slot metadata (brand, spool name, spoolman IDs, remaining
    // weight, etc.) layered over firmware-reported state.
    //
    // Write paths (both hold mutex_):
    //   - on_started(): initial bulk load from Moonraker DB lane_data.
    //     Swap happens under mutex_ so a concurrent status notification can
    //     never see a torn map.
    //   - set_slot_info(persist=true): user edit staged into overrides_
    //     BEFORE update_slot_from_state() is called, so apply_overrides on
    //     the very same call applies the new values rather than the old
    //     pre-edit override.
    //
    // Read: in apply_overrides() during the parse path, which always runs
    // under mutex_ (via update_slot_from_state).
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Resolved on-disk path of Adventurer5M.json when helix-screen runs on the
    // same host as Moonraker. Empty string means "fall back to Moonraker HTTP
    // upload" — either we're remote, the file isn't where we expect it, or the
    // path isn't writable. Set once during on_started() via
    // detect_local_adventurer_json_path(); never mutated thereafter.
    std::string local_adventurer_json_path_;

    // Per-slot previous firmware color (NOT the override-masked value).
    // Used to detect external color/material edits (Mainsail console, AD5X
    // LCD, native zmod dialog) so we can refresh the Moonraker DB lane_data
    // entry that OrcaSlicer reads. Empty = first observation (baseline,
    // never triggers a sync). observed_color == 0 is ignored as "no reading"
    // and does not update the baseline.
    //
    // Startup safety: on_started() loads overrides_ from Moonraker DB BEFORE
    // any firmware parse runs, and last_firmware_color_ stays empty until the
    // first parse — so the startup window can't flag the initial observation
    // as an external edit. set_slot_info() also pre-updates this map with the
    // user's chosen color before calling update_slot_from_state() so a Helix-
    // initiated color edit isn't misread as a foreign one on the same call.
    //
    // Access is always under mutex_ (written/read from update_slot_from_state
    // -> check_external_color_change and from set_slot_info's pre-update, all
    // of which run under the lock).
    std::unordered_map<int, uint32_t> last_firmware_color_;
    // Per-slot previous firmware MATERIAL, mirroring last_firmware_color_.
    // Drives check_external_type_change so a type-only firmware edit refreshes
    // a non-locked override. Same lock discipline and baseline semantics as
    // last_firmware_color_; empty string = first observation / no reading.
    std::unordered_map<int, std::string> last_firmware_material_;

    // Bumped by sync_override_to_firmware_locked on every accepted external
    // edit (color or material delta detected for a present slot, lane_data
    // save_async issued). parse_adventurer_json snapshots the count around
    // its per-slot loop and uses the delta to decide whether to also mirror
    // colors_/materials_ into the lessWaste/bambufy plugin's _IFS_VARS
    // save_variables — those don't self-sync against zmod's
    // Adventurer5M.json, so without the mirror the plugin's runout-recovery
    // and smart-purge logic operate on stale data. Wraps on overflow which
    // is fine — the comparison is `>`, not equality. Always accessed under
    // mutex_.
    size_t external_sync_count_ = 0;

    // Note: uses inherited lifetime_ from AmsSubscriptionBackend (not shadowed).
};

#endif // HELIX_HAS_IFS
