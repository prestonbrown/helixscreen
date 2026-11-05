// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_subscription_guard.h"

#include "ams_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override_store.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lane_echo.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace helix {

/// Base class for AMS backends that use Moonraker subscription-based status updates.
/// Extracts common lifecycle, event, and state query logic from AFC/HappyHare/ToolChanger.
///
/// Derived classes MUST implement:
///   - get_type() - return backend-specific AmsType
///   - handle_status() - parse the backend-specific status object
///   - backend_log_tag() - return log prefix like "[AMS AFC]"
///
/// Derived classes MAY override:
///   - on_started() - post-start initialization (version detection, config loading, etc.)
///   - on_stopping() - pre-stop cleanup
///   - on_stopped() - post-stop work that needs mutex_ or emits
///   - additional_start_checks() - extra preconditions before subscribing
///   - get_system_info() - if they need to build info from SlotRegistry
///   - slot_index_bound_locked() - if the valid index range is not total_slots
class AmsSubscriptionBackend : public AmsBackend {
  public:
    AmsSubscriptionBackend(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsSubscriptionBackend() override;

    // --- Lifecycle (final -- derived classes use hooks instead) ---
    AmsError start() final;
    void stop() final;
    void release_subscriptions() final;
    [[nodiscard]] bool is_running() const final;

    /// Re-read the lane-record store a backend names in lane_record_store()
    /// and file what it holds as vendor-cache readings.
    ///
    /// This refreshes the lane SOURCE MODEL and nothing else. The persisted
    /// override map is loaded once, at init, and is not touched here.
    /// Refreshing that map is tracked separately: a live map replaced wholesale
    /// mid-session has to answer for an edit made or in flight since it loaded.
    ///
    /// Runs only where firmware_publishes_lane_identity() is false. Elsewhere
    /// there is nothing to file, so no request is issued. A backend that
    /// names no store cannot issue the re-read; that skip is logged at warn
    /// on a started backend with an attached API, and is expected and silent
    /// before start or without one.
    void request_resync() override;

    // --- Event system (final) ---
    void set_event_callback(EventCallback callback) final;

    // --- State queries (final) ---
    [[nodiscard]] AmsAction get_current_action() const final;
    void on_klipper_restarted() override;
    [[nodiscard]] int get_current_tool() const final;
    [[nodiscard]] int get_current_slot() const final;
    [[nodiscard]] bool is_filament_loaded() const final;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // --- Filament operations (final -- derived backends implement do_*) ---
    //
    // Non-virtual interface. The public entry point runs the print-active gate,
    // then dispatches to the backend's do_* hook. Invoking the gate used to be
    // opt-in, hand-written at ~24 call sites: 329e731e9 added it to seven
    // backends and missed the eighth, which shipped with no gate at all
    // (180a71c7d). A backend cannot forget it here, because it never writes the
    // entry point.
    //
    // The gate is a CLAIM, not a test: run_filament_op() test-and-sets an
    // in-flight flag in the same critical section as the busy read and holds it
    // across the do_* dispatch, so exactly one op can be in flight at a time.
    // See filament_op_in_flight_ for why holding mutex_ across the busy check —
    // which is what the pre-NVI backends did — never achieved that.
    AmsError load_filament(int slot_index) final;
    AmsError unload_filament(int slot_index) final;
    AmsError select_slot(int slot_index) final;
    AmsError change_tool(int tool_number) final;
    /// Through the same gate and claim as the four above: parking moves the
    /// toolhead. Backends implement do_park_active_tool().
    AmsError park_active_tool() final;

    /// Batch load/unload, through the SAME gate and claim as the per-slot ops
    /// above: one claim covers the whole batch, so a batch cannot start while
    /// any other filament op is in flight, and no per-slot op can start mid
    /// batch. Backends opt in via do_filament_batch(); everyone else inherits
    /// the base not_supported refusal.
    AmsError load_filament_batch(const std::vector<int>& slots) final;

    /// Runs a device action through do_execute_device_action(). Refuses an id
    /// get_device_actions() does not list now or lists disabled, and one it marks moves_machine
    /// while a print holds the printer (refuse_motion_if_printing(), or refuse_if_printing() for
    /// one allowed_while_paused), so no backend can skip the rule.
    AmsError execute_device_action(const std::string& action_id, const std::any& value = {}) final;

    /// execute_device_action()'s gate, then do_run_device_action().
    AmsError run_device_action(const std::string& action_id, const std::any& value,
                               DeviceActionDone on_done) final;

    [[nodiscard]] bool device_action_running() const final {
        return tracked_run_active_.load();
    }

    /// `system_info_.tool_to_slot_map`, read under mutex_. A backend whose
    /// frames fill the forward map needs no override.
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    /// Refuses while a print holds the printer, paused included, then
    /// do_run_edit_follow_up().
    AmsError run_edit_follow_up(int slot_index, const SlotInfo& before,
                                const SlotInfo& after) final;
    AmsError unload_filament_batch(const std::vector<int>& slots) final;

    /// This backend's echo guard. A store other writers co-author can hold
    /// the mirror of the backend's own write, so the resync strips from those
    /// records any field still equal to a standing declaration. A backend
    /// that never writes identity back to firmware never stages into it, and
    /// an empty guard strips nothing.
    ///
    /// The guard stays under this backend's mutex_ discipline; the resync
    /// takes the lock around its consult.
    [[nodiscard]] helix::ams::OwnWriteEchoes* own_write_echoes() {
        return &own_write_echoes_;
    }

    // --- Shared utilities (public for AmsState and tests) ---
    void emit_event(const std::string& event, const std::string& data = "");
    /// Common gating before an AMS action runs.
    /// @param requires_toolhead_motion When true, additionally run
    ///        refuse_if_printing(): always refuse while PRINTING, and refuse
    ///        while PAUSED only when filament_ops_self_home() (AD5X
    ///        `_IFS_REMOVE_CURRENT_PRUTOK` runs a buried G28 that Layer 1 cannot
    ///        see). Pausing to swap filament is the runout recovery workflow, so
    ///        every other backend is allowed through while paused. Pass false for
    ///        no-motion ops (eject_lane, select, unlock/recovery).
    AmsError check_preconditions(bool requires_toolhead_motion = false) const;
    virtual AmsError execute_gcode(const std::string& gcode);
    /// Same as execute_gcode(gcode), but invokes @p on_complete when the gcode
    /// command finishes (Klipper acks the script — i.e. a long macro has fully
    /// run, not merely been queued). The callback runs on the main thread,
    /// queued behind any status frame that arrived before the answer, and is
    /// dropped if the backend is destroyed first. Use this when a macro's
    /// completion is the reliable terminal signal for an operation.
    virtual AmsError execute_gcode(const std::string& gcode,
                                   helix::SharedCallback<void()> on_complete);
    /// Same as execute_gcode(gcode, on_complete), plus @p on_error for callers
    /// that hold state the failure has to unwind — an optimistic AmsAction set
    /// before the send stays set forever if nothing runs on the error path.
    /// Both callbacks run on the main thread, queued the same way. The one
    /// exception is a send refused for want of an IMoonrakerAPI: @p on_error
    /// then runs synchronously, on the caller's stack.
    ///
    /// @p silent suppresses the advisory RPC timeout's toast. Leave it true for
    /// a long macro whose completion is owned by a status signal rather than the
    /// RPC return; pass false when the RPC return is the operation's own answer.
    virtual AmsError execute_gcode(const std::string& gcode,
                                   helix::SharedCallback<void()> on_complete,
                                   helix::SharedCallback<void(const MoonrakerError&)> on_error,
                                   bool silent = true);

    /// Whether the toolhead is homed. Virtual purely as a test seam: fixtures
    /// override it to exercise the homed and unhomed branches of
    /// ensure_homed_then() without a live Moonraker connection or a PrinterState.
    /// Production implementation reads the live homed_axes subject via api_.
    [[nodiscard]] virtual bool toolhead_homed() const;

    /// Query homing status and auto-home (G28) if needed before executing gcode.
    /// Returns immediately — homing and gcode execution happen asynchronously.
    /// @p on_complete (optional) fires when the final gcode command finishes
    /// (Klipper acks the script), on the main thread behind any status frame
    /// that arrived before the answer. Use when the gcode's macro completion is
    /// the terminal signal.
    /// @param on_error   Fires on G28 or payload failure. When null the failure
    ///                   is logged and the action is reset to IDLE, matching
    ///                   the historical behaviour.
    /// @param timeout_ms Payload timeout. G28 always uses kHomingTimeoutMs.
    /// @param skip_homing Dispatch the payload without homing, whatever the
    ///                   toolhead reports. For firmware macros that home
    ///                   themselves (CFS Fork variant).
    /// @param silent     Suppress REQUEST_TIMEOUT toasts on the payload. True
    ///                   matches every backend except CFS, which passes false.
    /// @param caller_surfaces_errors Whether @p on_error shows the user
    ///                   something. Unset means "derive from @p on_error";
    ///                   pass false when the callback only logs and unwinds
    ///                   state, so GcodeErrorRouter keeps the report.
    /// @param on_predispatch_error Used INSTEAD of @p on_error when the failure
    ///                   happens before the payload is sent — today that is the
    ///                   G28, plus the no-API case on the same leg. A caller
    ///                   whose @p on_error unwinds state the payload
    ///                   establishes needs this: nothing was established yet,
    ///                   so the unwind gcode is sent into a printer that will
    ///                   reject it, and Klipper broadcasts that rejection on
    ///                   the `!!` stream where GcodeErrorRouter turns it into a
    ///                   second error toast beside the real one. Null (the
    ///                   default) means "use @p on_error for both", which is
    ///                   every caller that has no such unwind.
    /// @param bypass_busy_gate Send the payload past IMoonrakerAPI's busy gate,
    ///                   which sends a discretionary script (heater, fan)
    ///                   fire-and-forget while a blocking operation runs and
    ///                   drops both callbacks. Only for a caller whose sequence
    ///                   cannot go on without the answer; every other caller
    ///                   keeps the gate.
    ///
    /// @warning Call from the main thread only — checks toolhead_homed(),
    /// which (in the production override) reads an LVGL subject.
    AmsError ensure_homed_then(
        std::string gcode, helix::SharedCallback<void()> on_complete = nullptr,
        helix::SharedCallback<void(const MoonrakerError&)> on_error = nullptr,
        uint32_t timeout_ms = IMoonrakerAPI::kAmsOperationTimeoutMs, bool skip_homing = false,
        bool silent = true, std::optional<bool> caller_surfaces_errors = std::nullopt,
        helix::SharedCallback<void(const MoonrakerError&)> on_predispatch_error = nullptr,
        bool bypass_busy_gate = false);

    /// Repaints the SlotInfo cached_slot_locked() names from its lane, under
    /// mutex_. Call without holding mutex_.
    void repaint_slot_from_lane(int slot_index) override;

    /// Under mutex_, hands the slot identity_slot_locked() names to
    /// write_synced_identity_locked(), then announces kEventSlotChanged for a
    /// write that moved something. A refusal announces nothing.
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) final;

    /// Under mutex_, clear_override_locked() on the slot identity_slot_locked()
    /// names, then after_override_cleared_locked(), then kEventSlotChanged. An
    /// index naming no cached slot is logged and nothing moves, unless
    /// clears_override_without_slot() says the stores are cleared anyway. A
    /// backend whose firmware keeps its own copy of the identity clears that in
    /// an override around this.
    void clear_slot_override(int slot_index) override;

  protected:
    // --- Hooks for derived classes ---

    /// What the current operation step projects to, for a backend whose action
    /// follows its step model; nullopt for one that assigns its action. Both
    /// get_current_action() and the backend's get_system_info() publish it
    /// through project_action(). Caller holds mutex_.
    [[nodiscard]] virtual std::optional<AmsAction> step_action_locked() const {
        return std::nullopt;
    }
    /// system_info_.action as published. Caller holds mutex_.
    [[nodiscard]] AmsAction published_action_locked() const {
        return project_action(system_info_.action, step_action_locked());
    }

    /// Whether @p slot_index names a position this backend has. Zero positions
    /// means none are discovered yet, which is not the caller's mistake, so it
    /// reads as not-connected rather than as a bad number. Caller holds mutex_.
    [[nodiscard]] AmsError validate_slot_index_locked(int slot_index) const;
    /// validate_slot_index_locked() for a caller that does not hold mutex_.
    [[nodiscard]] AmsError validate_slot_index(int slot_index) const;
    /// Exclusive upper bound for a valid slot index. Caller holds mutex_.
    [[nodiscard]] virtual int slot_index_bound_locked() const {
        return system_info_.total_slots;
    }

    /// Drop what earlier paints left on a persistent SlotInfo, so the paint
    /// that follows takes the lane's current records as its only supplier.
    ///
    /// A backend whose SlotInfo persists across frames (AD5X IFS, Happy Hare)
    /// must clear the identity fields nothing but the lane states before every
    /// paint, or a field outlives the record that stated it. The default does
    /// nothing: a backend that rebuilds its struct from each frame already
    /// satisfies apply_resolved()'s premise. Caller holds mutex_.
    virtual void prepare_lane_repaint_locked(int slot_index, SlotInfo& slot) {
        (void)slot_index;
        (void)slot;
    }

    /// The SlotInfo get_slot_info() copies for @p slot_index, or nullptr when
    /// there is none. The default reads system_info_; a backend keeping its
    /// slots in a SlotRegistry answers from that. Caller holds mutex_.
    [[nodiscard]] virtual const SlotInfo* slot_info_locked(int slot_index) const {
        return system_info_.get_slot_global(slot_index);
    }

    /// Called after subscription is established and running_ is set.
    /// Lock is NOT held. Safe to call emit_event().
    virtual void on_started() {}

    /// Called before stop() releases the subscription.
    /// Lock IS held.
    virtual void on_stopping() {}

    /// Called at the end of stop(), once mutex_ is released and the subscription
    /// is gone. Lock is NOT held: work that takes mutex_, sends gcode or calls
    /// emit_event() belongs here rather than in on_stopping().
    virtual void on_stopped() {}

    /// The SlotInfo get_slot_info() serves for @p slot_index, or nullptr when
    /// there is none to repaint. Lock IS held.
    ///
    /// A backend that paints a stored SlotInfo from the lane only while it
    /// parses a frame names it here, so a lane written from outside the backend
    /// (a person's edit, a Spoolman fetch) shows before the next frame, which an
    /// idle machine may not send for a long time. A backend that re-reads its
    /// slots on a short poll keeps the default.
    virtual SlotInfo* cached_slot_locked(int slot_index) {
        (void)slot_index;
        return nullptr;
    }

    /// The SlotInfo an outside identity write or an override clear lands on,
    /// or nullptr when @p slot_index names none. The default reads
    /// system_info_; a backend keeping its slots in a SlotRegistry answers
    /// from that. Caller holds mutex_.
    [[nodiscard]] virtual SlotInfo* identity_slot_locked(int slot_index) {
        return system_info_.get_slot_global(slot_index);
    }

    /// What one sync_external_identity() did to the slot.
    struct IdentityWrite {
        AmsError err = AmsErrorHelper::success();
        /// kEventSlotChanged once the lock drops. False for a write that kept
        /// nothing, so a backend with no store for the value announces nothing.
        bool emit = true;
    };

    /// sync_external_identity()'s write, under mutex_. @p slot is what
    /// identity_slot_locked() found, nullptr when @p slot_index names none.
    /// The default refuses a missing slot as out of range and assigns the
    /// filament fields; a backend whose tool-map registry, firmware mirror or
    /// stored record also has to move does that here.
    virtual IdentityWrite write_synced_identity_locked(int slot_index, SlotInfo* slot,
                                                       const SlotInfo& info);

    /// clear_slot_override()'s vendor half, once clear_override_locked() has
    /// run on @p slot, still under mutex_. Default does nothing.
    virtual void after_override_cleared_locked(int slot_index, SlotInfo& slot) {
        (void)slot_index;
        (void)slot;
    }

    /// Whether clear_slot_override() still erases the stored override, resets
    /// the lane and announces the slot when identity_slot_locked() finds no
    /// SlotInfo for the index: a slot not cached yet, before the first frame or
    /// after a topology change. Default false: the clear is logged and nothing
    /// moves.
    [[nodiscard]] virtual bool clears_override_without_slot() const {
        return false;
    }

    /// Extra checks before subscribing (e.g., ToolChanger requires tools discovered).
    /// Return error to abort start. Lock IS held.
    virtual AmsError additional_start_checks() {
        return AmsErrorHelper::success();
    }

    /// Unwrap a notify_status_update frame (`{"params": [{status}, eventtime]}`)
    /// and hand its status object to handle_status(). Any other shape is ignored.
    void handle_status_update(const nlohmann::json& notification);

    /// Parse one status object, `{<object_name>: <fields>, ...}`, as a notify
    /// frame or a printer.objects.query result carries it. Runs on the main
    /// thread.
    virtual void handle_status(const nlohmann::json& status) = 0;

    /// Return log tag like "[AMS AFC]" for log messages.
    virtual const char* backend_log_tag() const = 0;

    // --- Filament operation hooks ---

    /// Backend implementation of the matching public operation. Reached only
    /// after the gate has passed, so these must NOT call check_preconditions()
    /// or refuse_if_printing() again — one authority, in the base.
    ///
    /// Called WITHOUT mutex_ held, and with the in-flight claim held for the
    /// whole call. Take mutex_ for the state you need; do not hold it across the
    /// gcode send. A hook must not re-enter a PUBLIC filament op on the same
    /// backend — it would be refused by its own claim. Forward to the sibling
    /// do_* instead, which is what the delegating backends already do.
    ///
    /// When one op is really another (ACE loads to select, a Snapmaker select
    /// IS a tool change), forward to the sibling do_* rather than the public
    /// entry point, so the gate runs once.
    virtual AmsError do_load_filament(int slot_index) = 0;
    virtual AmsError do_unload_filament(int slot_index) = 0;
    virtual AmsError do_select_slot(int slot_index) = 0;
    virtual AmsError do_change_tool(int tool_number) = 0;
    /// Backend implementation of park_active_tool(), same contract as the hooks
    /// above. Default refuses, matching AmsBackend::supports_tool_park().
    virtual AmsError do_park_active_tool() {
        return AmsErrorHelper::not_supported("Tool park");
    }

    /// Backend implementation of a batch load (@p load true) or unload, reached
    /// only after the gate has passed — same contract as the per-slot do_*
    /// hooks above. Default refuses: a backend that has not opted in answers
    /// not_supported even through the shared gate.
    virtual AmsError do_filament_batch(const std::vector<int>& slots, bool load) {
        (void)slots;
        (void)load;
        return AmsErrorHelper::not_supported("Batch filament ops");
    }

    /// Does a slot SELECT move the toolhead on this backend?
    ///
    /// Load, unload and tool change are toolhead motion on every backend and are
    /// not negotiable — see op_moves_toolhead(). select_slot is the one
    /// genuinely per-backend answer, and re-deciding it once per backend is how
    /// it gets got wrong: Happy Hare's MMU_SELECT drives the gate selector and
    /// never touches the carriage, while on a Snapmaker U1 a slot select IS a
    /// physical tool change (do_select_slot forwards to do_change_tool, which
    /// emits `T{n}`). Override to true when yours moves the toolhead.
    ///
    /// This can only ADD motion, never remove it. There is deliberately no hook
    /// for declaring an operation gate-free.
    [[nodiscard]] virtual bool select_slot_moves_toolhead() const {
        return false;
    }

    /// Which gate the public filament operations run. Neither value means
    /// "no gate" — the print-active refusal applies either way.
    enum class FilamentOpGate {
        /// check_preconditions(): backend started, AMS not busy, and — for a
        /// toolhead-motion op — no print owning the toolhead.
        Standard,
        /// refuse_if_printing() only, skipping the running_/busy half. For a
        /// backend that does not drive its filament ops off those at all; see
        /// AmsBackendQidi, which has never consulted them.
        PrintActiveOnly,
    };
    [[nodiscard]] virtual FilamentOpGate filament_op_gate() const {
        return FilamentOpGate::Standard;
    }

    /// Refuse a toolhead-motion filament op while a print owns the toolhead.
    ///
    /// Returns AmsErrorHelper::print_active() (with a spdlog::warn) when the
    /// print-job state is PRINTING, or when it is PAUSED **and**
    /// filament_ops_self_home() is true. A PAUSED job on a backend that does not
    /// self-home is allowed through — pause-then-swap is the runout /
    /// colour-change recovery workflow, and Layer 1
    /// (helix::api::reject_homing_during_active_print) still refuses any
    /// app-emitted G28 in that state. Success when api_ is null (unit tests /
    /// cold-boot: print state is unknown, don't block).
    ///
    /// Exposed to derived backends that gate motion ops WITHOUT the running_/busy
    /// checks in check_preconditions() (e.g. QIDI Box).
    ///
    /// @param paused_blocks Refuse a PAUSED job too, on a backend that would
    ///        otherwise allow it: for an op that leaves nothing on the carriage,
    ///        which the job cannot resume without.
    AmsError refuse_if_printing(bool paused_blocks = false) const;

    /// refuse_if_printing() for a LOAD: print_blocks_filament_load() is the rule,
    /// so a paused job on a backend whose buried home is skipped once homed
    /// (self_home_skipped_when_homed()) loads when toolhead_homed() reads "xyz"
    /// and the load unloads nothing first (load_unloads_toolhead_now()).
    AmsError refuse_load_if_printing() const;

    /// Refuse anything that moves the printer while a print holds it, paused or
    /// not: what a `moves_machine` control greys for. AmsErrorHelper::busy_with_print()
    /// when refused; success when api_ is null (print state unknown).
    AmsError refuse_motion_if_printing() const;

    /// A print holds the printer, paused included: the predicate
    /// refuse_motion_if_printing() refuses on. False without an API (print
    /// state unknown). Virtual so tests can print without a printer.
    [[nodiscard]] virtual bool print_holds_printer() const;

    /// Send an Endless Spool on/off command. A rejection Klipper reports after
    /// the send emits kEventEndlessSpoolRefused, so a switch the user flipped
    /// returns to the backend's state. @p on_sent runs once Klipper has run it.
    AmsError send_endless_spool_switch(const std::string& gcode,
                                       std::function<void()> on_sent = {});

    /// Backend implementation of execute_device_action(), reached only after the
    /// moves_machine gate has passed. Default refuses.
    virtual AmsError do_execute_device_action(const std::string& action_id, const std::any& value) {
        (void)value;
        return AmsErrorHelper::not_supported("Device action " + action_id);
    }

    /// Backend implementation of run_device_action(), reached only after the same
    /// gate. Default runs do_execute_device_action() and, when that succeeds,
    /// reports success through @p on_done at once.
    virtual AmsError do_run_device_action(const std::string& action_id, const std::any& value,
                                          DeviceActionDone on_done);

    /// Backend implementation of run_edit_follow_up(), reached only when no
    /// print holds the printer. Default refuses.
    virtual AmsError do_run_edit_follow_up(int slot_index, const SlotInfo& before,
                                           const SlotInfo& after) {
        (void)slot_index;
        (void)before;
        (void)after;
        return AmsErrorHelper::not_supported("Edit follow-up");
    }

    /// The mapping verb of a backend that has none: can_set_tool_mapping()'s
    /// verdict is the whole answer, nothing is sent. A backend with a verb
    /// overrides this.
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    /// Mark a tracked device action as sent; false when one already is. A
    /// backend whose do_run_device_action() answers later calls this before
    /// the send and end_tracked_run() on both of its answers and on a failed
    /// send.
    [[nodiscard]] bool begin_tracked_run();
    void end_tracked_run();

    /// Why @p action_id may not run now (not listed, listed disabled, moving
    /// the machine while a print holds it or while a tracked run is in flight),
    /// or success when it may.
    [[nodiscard]] AmsError device_action_refusal(const std::string& action_id) const;

    /// Whether this backend's firmware publishes lane identity of its own,
    /// which it files as a VendorCache reading on every status frame.
    ///
    /// A lane with such a producer must not also receive the persisted record.
    /// Both are the same source and ingest() replaces a source's record whole,
    /// so the two arrival orders say different things and both are reachable:
    /// a resync landing after a frame overwrites a fresh firmware reading with
    /// a stored one. Nor is there a field to be gained by accepting that risk.
    /// Whole-record replacement means the next frame retracts whatever the
    /// persisted record carried beyond what firmware reports, so filing it
    /// buys a window between two frames rather than a value a lane keeps.
    ///
    /// Defaults to true, so a backend that forgets to answer loses a re-read
    /// rather than gaining a race.
    ///
    /// @warning Answering false is what turns request_resync()'s reload path
    ///          on, and that path refreshes the LANE only. A backend's own
    ///          `overrides_` map keeps whatever it loaded at start. Today that
    ///          is harmless because the backends answering false (ToolChanger,
    ///          OpenAMS) touch `overrides_` only on the user-edit and
    ///          persistence paths.
    ///          A backend that reads `overrides_` to DECIDE something, as CFS
    ///          does in three places, would start deciding from a map the
    ///          resync did not refresh. Check that before changing this answer
    ///          (prestonbrown/helixscreen#1629).
    [[nodiscard]] virtual bool firmware_publishes_lane_identity() const {
        return true;
    }

    /// This backend's lane_data-shaped record store, or nullptr when it has
    /// none. request_resync() re-reads it so a lane's shared record is not
    /// frozen at whatever it said when the backend started.
    ///
    /// Only a store on a namespace OTHER writers co-author belongs here. A
    /// namespace HelixScreen alone writes has no drift for a re-read to
    /// correct, and re-filing its records would put a second producer on the
    /// vendor-cache slot the backend's own firmware readings occupy.
    virtual helix::ams::FilamentSlotOverrideStore* lane_record_store() {
        return nullptr;
    }

    /// File @p obs, read out of @p entry on a re-read of the lane-record store,
    /// as another tool's edit on @p lane when it is one and newer than what
    /// stands there (file_outside_edit_if_newer()). Returns true when it filed.
    ///
    /// A co-authored namespace carries the mirror of this backend's own write,
    /// and firmware mirrors a write back with none of our authorship keys, so a
    /// foreign-looking record can still be that echo: @p obs first loses what
    /// the slot's standing declaration explains, and while that declaration
    /// stands our unconfirmed write is the newer statement and nothing files.
    /// A stored record is not a live producer, so a value that differs from the
    /// declaration releases nothing. Takes mutex_.
    bool file_reread_outside_edit(helix::ams::LaneId lane, int slot_index,
                                  const helix::ams::LaneDataRecord& entry,
                                  helix::ams::Observation& obs);

    /// Re-read lane_data once a burst of firmware changes settles. A tool that
    /// changes a slot from outside (an NFC tag reader, a spool-binding macro)
    /// writes its lane record beside the firmware change, and nothing else
    /// reads lane_data after start (prestonbrown/helixscreen#1632). Debounced,
    /// so a burst issues one read. Safe under mutex_.
    void schedule_lane_record_reread();

    /// When the store's load got no answer, read until one lands, then take the
    /// printer's records as the start of this session would have. Nothing is
    /// written over them until then. A failed read is retried from the status
    /// frames, no sooner than kUnreadLaneRecordsRetry. Main thread.
    void read_unread_lane_records();
    /// A read is out, and when the last one started. Main thread.
    bool unread_read_in_flight_ = false;
    std::chrono::steady_clock::time_point unread_read_started_{};

    /// Remember each slot's lane_data document as the store loaded it, so the
    /// first re-read files only what another tool changed since. Caller holds
    /// mutex_, after installing override_store_.
    void seed_lane_records_seen_locked();

    /// read_unread_lane_records() got its answer: @p records replace what the
    /// failed load left in overrides_, and the lanes and slots repaint.
    void adopt_unread_lane_records(helix::ams::FilamentSlotOverrideStore* store,
                                   std::unordered_map<int, helix::ams::LaneDataRecord> records);

    /// File each record that changed since it was last read as another tool's
    /// edit when it is one and newer (file_reread_outside_edit()), and move the
    /// slot's override onto it: a rebinding record starts the override afresh,
    /// anything else amends it. Main thread.
    void
    apply_reread_lane_records(const std::unordered_map<int, helix::ams::LaneDataRecord>& records);

    /// A re-read was scheduled and has not yet run. Called under the caller's
    /// locks, from schedule_lane_record_reread().
    virtual void on_lane_reread_scheduled() {}

    /// A re-read finished: @p taken holds the slots that took another tool's
    /// record. A read that failed took none. Main thread; mutex_ not held.
    virtual void on_lane_reread_finished(const std::unordered_set<int>& /*taken*/) {}

    [[nodiscard]] bool lane_reread_pending() const {
        return lane_reread_pending_.load();
    }

    /// Clear @p slot_index's user override from every store holding it: the
    /// in-memory map, the lane's records, the echo guard and the persisted
    /// record. @p slot, when non-null, also loses clear_override_fields().
    /// Caller holds mutex_.
    void clear_override_locked(int slot_index, SlotInfo* slot);

    /// Blank on @p slot the fields a user override supplies and this
    /// backend's firmware never restates, so a clear shows in the very next
    /// get_slot_info(). Fields firmware reports are left for the parse.
    ///
    /// The default fits firmware that reports colour and material but no
    /// brand, spool name, weights, colour name, Spoolman link or catalog pick.
    /// The catalog pick is override-exclusive on every backend: no AMS
    /// firmware carries a branded product id.
    virtual void clear_override_fields(SlotInfo& slot) const;

    /// commit_user_edit()'s refusal hooks: a dispatch that failed outright
    /// wrote nothing firmware can echo, so the staging it created cancels
    /// rather than withholding the next genuine reading, and the armed
    /// predecessor that staging suspended stands again.
    std::uint64_t own_write_echo_sequence(int slot_index) override;
    void abandon_own_write_echoes(int slot_index, std::uint64_t staged_sequence) override;

    // --- Protected state for derived classes ---
    IMoonrakerAPI* api_;
    helix::IMoonrakerClient* client_;
    mutable std::mutex mutex_;
    AmsSystemInfo system_info_;
    std::atomic<bool> running_{false};

    /// Lifetime guard for async callback safety. Tokens captured in the
    /// subscription lambda are expired when the backend is destroyed, preventing
    /// use-after-free when WebSocket dispatch races with clear_backends() (#621).
    helix::AsyncLifetimeGuard lifetime_;

    /// The user's per-slot overrides and the Moonraker-DB store that persists
    /// them, keyed by global slot index. Null store until the backend builds
    /// it (it needs api_). Both under mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    /// What the user's own write-back declared per slot, so firmware echoing
    /// it is not filed as a reading. Under mutex_.
    helix::ams::OwnWriteEchoes own_write_echoes_;

    /// Wakes every debounce worker the moment the backend is destroyed. Each
    /// debounce sleeps on a fast-lane worker, and a backend that is gone has
    /// nothing left for the worker to do, so it hands the lane back at once.
    /// Shared with the workers, which outlive the backend.
    struct DebounceCancel {
        std::mutex mutex;
        std::condition_variable cv;
        bool cancelled = false;
    };
    std::shared_ptr<DebounceCancel> debounce_cancel_ = std::make_shared<DebounceCancel>();
    /// BG THREAD: sleep @p window on a debounce worker. False when the backend
    /// was destroyed first, and the worker must return without touching it.
    static bool debounce_wait(const std::shared_ptr<DebounceCancel>& cancel,
                              std::chrono::milliseconds window);

    /// A debounced lane_data re-read is waiting to run.
    std::atomic<bool> lane_reread_pending_{false};

    /// Read the lane-record store now; the debounce lands here.
    void reread_lane_records();

  private:
    /// Each slot's lane_data document as last read, so a re-read files only a
    /// record that changed since: an unchanged document says nothing new, and
    /// an outside record carries no stamp to order it by. Under mutex_.
    std::unordered_map<int, nlohmann::json> lane_wire_seen_;

    /// The gated operations, so motion can be classified per METHOD in one
    /// place instead of per backend at every call site.
    enum class FilamentOp { Load, Unload, SelectSlot, ChangeTool, ParkTool };

    /// Motion classification, stated ONCE for every backend.
    [[nodiscard]] bool op_moves_toolhead(FilamentOp op) const;

    /// The started/busy half of check_preconditions(), with no locking of its
    /// own. check_preconditions() keeps its historical no-internal-lock contract
    /// because roughly half its ~15 callers already hold mutex_ when they call
    /// it (AFC eject_lane/reset/enable_bypass, AD5X eject_lane, ...) and would
    /// self-deadlock on a non-recursive re-acquire. claim_filament_op() calls
    /// this WITH mutex_ held, which is what makes the gate's read of
    /// system_info_.action race-free on the filament-op path.
    [[nodiscard]] AmsError state_preconditions_unlocked() const;

    /// Test-and-set the in-flight claim, in the SAME critical section as the
    /// started/busy read it has to be atomic with. Returns the refusal when the
    /// claim is denied; on success the caller owns the claim and must release it
    /// (run_filament_op() does so via FilamentOpClaim).
    [[nodiscard]] AmsError claim_filament_op(FilamentOp op, bool check_state);
    void release_filament_op_claim();

    /// RAII release for claim_filament_op(). Only run_filament_op() builds one,
    /// so a backend cannot hold, leak, or skip the claim.
    class FilamentOpClaim {
      public:
        FilamentOpClaim() = default;
        explicit FilamentOpClaim(AmsSubscriptionBackend* owner) : owner_(owner) {}
        ~FilamentOpClaim() {
            if (owner_ != nullptr) {
                owner_->release_filament_op_claim();
            }
        }
        FilamentOpClaim(const FilamentOpClaim&) = delete;
        FilamentOpClaim& operator=(const FilamentOpClaim&) = delete;

      private:
        AmsSubscriptionBackend* owner_ = nullptr;
    };

    /// Gate, claim, then dispatch. The public entry points are one line
    /// each on top of this, so there is exactly one place the gate can run.
    /// @param batch When non-null, dispatch goes to do_filament_batch() instead
    ///        of the per-slot hook; @p arg is ignored. Only the batch entry
    ///        points pass it, and they pass FilamentOp::Load / Unload.
    AmsError run_filament_op(FilamentOp op, int arg, const std::vector<int>* batch = nullptr);

    /// True from the instant a filament op wins the gate until its do_* hook
    /// returns. Guarded by mutex_.
    ///
    /// This is what makes the gate a claim rather than a test. The state the
    /// gate reads — system_info_.action — is NOT set by the op itself: every
    /// backend that publishes an optimistic action does it inside the do_* hook,
    /// in a LATER critical section (AFC/ToolChanger begin_dispatch_locked, ACE,
    /// CFS, AD5X), and Happy Hare, Snapmaker and QIDI never set it at all and
    /// wait for firmware to echo. So "check busy, then act" was two steps with a
    /// window between them long before the NVI refactor moved the check out of
    /// the hook's lock — holding mutex_ across the check never closed it,
    /// because the write that closes it happens after the lock is dropped.
    /// The claim closes the check→dispatch half of that window outright, for
    /// every backend, without extending mutex_ across the gcode send.
    bool filament_op_in_flight_ = false;

    /// The action the in-flight op stands for, so a refused second op names it
    /// the same way an AMS-reported busy state would.
    AmsAction filament_op_claimed_action_ = AmsAction::IDLE;

    /// A tracked device action is on the printer (begin_tracked_run()).
    std::atomic<bool> tracked_run_active_{false};

    EventCallback event_callback_;
    SubscriptionGuard subscription_;

    /// Send the payload gcode, honouring the 1-arg/2-arg execute_gcode split.
    /// ~20 test fixtures override ONLY the 1-arg form; calling the 2-arg form
    /// with a null callback would fall through to the base and stop capturing.
    ///
    /// When @p on_error, @p timeout_ms, and @p silent are all left at their
    /// ensure_homed_then() defaults, dispatch stays on the same two virtuals
    /// referenced above — required for fixture compatibility. Any non-default
    /// combination needs a live @p api_: it talks to IMoonrakerAPI directly (the
    /// hardcoded virtuals can't carry a caller's own error/timeout/toast
    /// policy). On every path @p on_complete
    /// runs on the main thread, queued behind any status frame that arrived
    /// before the answer, and so does @p on_error once the send went out.
    ///
    /// @param caller_surfaces_errors Whether @p on_error shows the user
    ///        something. Left unset it is derived from @p on_error being
    ///        non-null, which is right for the callers that pass none. Set it
    ///        explicitly to false when the callback exists but only logs and
    ///        unwinds state — see include/rpc_error_policy.h.
    /// @param bypass_busy_gate See ensure_homed_then(). With a live @p api_ it
    ///        takes the direct path, since the virtuals carry no such flag.
    AmsError dispatch_payload(std::string gcode, helix::SharedCallback<void()> on_complete,
                              helix::SharedCallback<void(const MoonrakerError&)> on_error = nullptr,
                              uint32_t timeout_ms = IMoonrakerAPI::kAmsOperationTimeoutMs,
                              bool silent = true,
                              std::optional<bool> caller_surfaces_errors = std::nullopt,
                              bool bypass_busy_gate = false);

    /// Report a gcode failure through @p on_error when set, else fall back to
    /// the historical behaviour: log at error level and reset the action to
    /// IDLE. Callers on a background thread must already have marshalled to
    /// main (see dispatch_payload()/ensure_homed_then()'s token.defer() calls)
    /// before reaching this -- it touches system_info_ under mutex_ directly.
    void handle_dispatch_error(const MoonrakerError& err,
                               const helix::SharedCallback<void(const MoonrakerError&)>& on_error);

    /// The dispatch leg every sender shares when there is no IMoonrakerAPI to
    /// send through: deliver the same CONNECTION_LOST the wire would have
    /// reported to @p on_error, and tell the caller the send was refused. A
    /// caller that set an optimistic AmsAction before dispatching unwinds it
    /// in on_error, so skipping the callback strands the action forever
    /// (prestonbrown/helixscreen#1720).
    AmsError
    refuse_dispatch_no_api(const helix::SharedCallback<void(const MoonrakerError&)>& on_error);
};

} // namespace helix
