// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_SNAPMAKER

#include "ui_timer_guard.h"

#include "ams_subscription_backend.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_binding.h"
#include "lane_echo.h"
#include "lane_observation.h"
#include "snapmaker_channel_state.h"
#include "snapmaker_nozzle_properties.h"
#include "snapmaker_print_preferences.h"
#include "snapmaker_status_parse.h"

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix {

class SnapmakerTestAccess;
class SnapmakerRealtimeTestAccess;
class FileStartFactsLoader;
struct FileStartFacts;
class RunoutScopeTestAccess;

/**
 * @file ams_backend_snapmaker.h
 * @brief Snapmaker U1 SnapSwap toolchanger backend
 *
 * The Snapmaker U1 is a 4-toolhead printer with custom Klipper extensions.
 * Each extruder has state fields (park_pin, active_pin, activating_move)
 * and RFID tags provide filament info per channel.
 *
 * Klipper Objects:
 * - extruder0..3 with custom fields: state, park_pin, active_pin,
 *   activating_move, extruder_offset, switch_count, retry_count, error_count
 * - filament_detect with info/state per channel and filament_feed left/right
 * - toolchanger, toolhead, print_task_config
 *
 * Path topology is PARALLEL (each tool has its own independent path).
 */

class AmsBackendSnapmaker : public AmsSubscriptionBackend {
  public:
    AmsBackendSnapmaker(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    ~AmsBackendSnapmaker() override;

    /// The status objects the SnapSwap backend pushes: RFID filament, feed modules,
    /// task config, extruder state, and the batch-feed macro when the firmware has it.
    /// Empty unless @p hw detected the Snapmaker filament system.
    [[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::SNAPMAKER;
    }

    /// The U1's own UI names these "Feeder 1".."Feeder 4" (verified against its
    /// firmware UI binary: zero occurrences of Slot, Lane, or capitalised
    /// Channel), never our "slot".
    [[nodiscard]] helix::ui::LaneNoun lane_noun() const override {
        return helix::ui::LaneNoun::Feeder;
    }

    /// The U1's own UI names the printing end "Toolhead 1".."Toolhead 4" - a
    /// different word from lane_noun()'s Feeder, naming a different physical
    /// thing (where filament enters vs. where it prints) despite the 1:1 count.
    [[nodiscard]] helix::ui::LaneNoun tool_noun() const override {
        return helix::ui::LaneNoun::Toolhead;
    }

    // State queries
    /// Four physical heads, up to 32 logical tools: [0,1,2,3,0,0,...]. Verified
    /// live against print_task_config.extruder_map_table on a U1.
    ///
    /// Static because AmsBackendMock answers with it too, and the head count is
    /// exactly the sort of constant that goes stale in a second copy: the
    /// inherited identity default agrees for T0-T3 and diverges silently from T4
    /// up. Same reasoning as preprint_gcode() below.
    [[nodiscard]] static helix::FirmwareRouting default_routing() {
        return helix::FirmwareRouting::fixed_heads(kNumTools, 0);
    }

    [[nodiscard]] helix::FirmwareRouting firmware_default_routing() const override {
        return default_routing();
    }

    /// The routing a caller may act on, from the firmware's own two fields.
    ///
    /// @param extruders_used print_task_config.extruders_used - the firmware's
    ///        own "a task is configured" signal, all-false when idle.
    /// @param extruder_map   print_task_config.extruder_map_table.
    /// @return @p extruder_map when a task is configured, EMPTY otherwise. Empty
    ///         means "no opinion", which is what stops the identity table an idle
    ///         U1 holds from being read as this print's routing.
    ///
    /// Static and pure so the mock gates the same way rather than carrying a
    /// second copy of the rule.
    [[nodiscard]] static std::vector<int> task_routing(const std::vector<bool>& extruders_used,
                                                       const std::vector<int>& extruder_map);

    [[nodiscard]] AmsSystemInfo get_system_info() const override;

    // Operation step bar. The U1 firmware reports a granular channel_state that
    // classify_channel_state maps to a per-direction step index published via the
    // ams_operation_phase subject, so the step model and its driving index live in
    // the backend (the sidebar renders generically). LOAD is a 5-step model
    // (Home -> Select -> Heat -> Feed filament -> Purge); UNLOAD is 4 steps
    // (Home -> Select -> Heat -> Retract). The Heat step shows a live nozzle temp.
    [[nodiscard]] OperationStepModel get_operation_step_model(StepOperationType op) const override;
    [[nodiscard]] lv_subject_t* get_operation_step_index_subject(StepOperationType op) override;

    /// Snapmaker U1 has 4 independent extruders (extruder, extruder1, extruder2,
    /// extruder3), one per tool. Each extrudes from the slot feeding its head.
    [[nodiscard]] std::optional<int> slot_for_extruder(int extruder_idx) const override {
        return head_source_slot(extruder_idx, get_system_info());
    }

    // Path visualization (PARALLEL topology — each tool is independent)
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::PARALLEL;
    }

    /// Constant capability answers; see BackendTraits.
    static constexpr BackendTraits kTraits = [] {
        BackendTraits t;
        // The U1 has no shared tray or housing: spools mount on the left and right
        // of the machine and feed through bowdens into a lane-assist motor unit on
        // each side. The detail view's tray graphic draws a container that is not
        // there.
        t.has_physical_tray = false;
        t.supports_auto_heat_on_load = true;
        // AUTO_FEEDING ... UNLOAD=1 runs the firmware's unload state machine,
        // whose unload_heating stage heats (FILAMENT_BACKEND_SNAPMAKER_U1.md).
        // unload_heats_hotend() excludes the bare INNER_FILAMENT_UNLOAD fallback.
        t.supports_auto_heat_on_unload = true;
        // Snapmaker U1's Resume runs AUTO_FEEDING (loads filament to the nozzle)
        // before RESUME, so Resume alone recovers a runout. The runout dialog uses
        // this to present Resume as primary and demote manual Load/Unload/Purge.
        t.recovers_filament_on_resume = true;
        // The U1 drives load/unload entirely on its own, so an idle lane going empty
        // (a hand-pull, or a lane left unloaded) needs no operator action and the
        // idle runout-guidance modal is just noise. Mid-print runout is a separate
        // path and is unaffected.
        t.should_suppress_idle_runout_modal = true;
        // Snapmaker U1's firmware errors if SET_PRINT_USED_EXTRUDERS /
        // SET_PRINT_EXTRUDER_MAP arrive mid-print, so the config must be sent before
        // PRINT_START. Always-on (even with no remap) to suppress a spurious-feed
        // runout — the slicer auto-feeds heads the print doesn't use → empty head →
        // runout cancel.
        t.requires_preprint_send = true;
        // The sidebar's Reset has nothing to clear here: the firmware's own
        // feed state machine recovers each head.
        t.supports_reset = false;
        // channel_state stays idle between AUTO_FEEDING being accepted and the
        // firmware's first homing step.
        t.holds_optimistic_action = true;
        // Each head has its own lane and AUTO_FEEDING runs one direction, so
        // an unload is never the first half of a swap.
        t.unloads_stand_alone = true;
        return t;
    }();
    [[nodiscard]] BackendTraits traits() const override {
        return kTraits;
    }

    // PARK_EXTRUDER docks every head, so the carriage can be empty even though
    // a load feeds filament rather than mounting a tool.
    [[nodiscard]] bool carriage_can_be_empty() const override {
        return true;
    }

    /// One head per extruder, each fed by the lane of the same index.
    [[nodiscard]] int filament_head_count() const override {
        return kNumTools;
    }
    [[nodiscard]] std::optional<int> head_for_slot(int slot) const override {
        if (slot < 0 || slot >= kNumTools) {
            return std::nullopt;
        }
        return slot;
    }
    [[nodiscard]] std::optional<int> head_source_slot(int head,
                                                      const AmsSystemInfo&) const override {
        return head_for_slot(head);
    }
    /// Loaded to the nozzle, or parked in the toolhead short of it: either way
    /// an unload has filament to act on.
    [[nodiscard]] std::optional<bool> head_has_filament(int head) const override;

    /// The four independent feeders run as one batch: the firmware sequences
    /// per-extruder AUTO_FEEDING itself.
    [[nodiscard]] int max_heads_per_filament_op() const override {
        return kNumTools;
    }

    // FEED_AUTO homes and sets its own nozzle target from the lane's material:
    // a load reports load_homing then load_heating, an unload unload_homing then
    // unload_heating. A G28 or UI preheat in front of it is a second wait.
    [[nodiscard]] bool delegates_homing_to_printer() const override {
        return true;
    }

    // needs_unload_before_load() is answered by the base class: every lane here
    // is PARALLEL, so slot_has_independent_path() is true for all of them and the
    // serial rule never applies. See AmsBackend for why, including the `T{n}`
    // load this backend used to dispatch.
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// Every feeder's port sensor sits at the top of its tube: the lane's
    /// entry sensor, read as PREP. A head fed from upstream has none of its
    /// own (head_is_upstream_fed_locked()).
    [[nodiscard]] bool slot_has_prep_sensor(int slot_index) const override;

    /// Pending while a feeder insert waits on filament_detect.info, then the
    /// reader's verdict (pending_insert_passes_ / insert_verdict_).
    [[nodiscard]] InsertIdentity slot_insert_identity(int slot_index) const override;

    /// The feeder port sensor alone: a tool parking or print_task_config's
    /// filament_exist moves the slot status without a spool going in.
    [[nodiscard]] std::optional<bool> slot_insert_presence(int slot_index) const override;

    // Per-slot "filament is loaded to THIS tool's nozzle". Returns the
    // channel_state latch (loaded_at_toolhead_), driven by filament_feed
    // channel_state transitions: true between load_finish and the next
    // unload_finish/wait_insert/preload_finish. The toolhead switch
    // (e{N}_filament) cannot answer this: a firmware unload retracts the tip
    // only ~70mm, so the switch still reads filament after an unload. It answers
    // slot_filament_parked_in_toolhead() instead.
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot_index) const override;

    // The toolhead switch reads filament while the latch says not loaded: the
    // state after a firmware unload, and after filament is fed to the nozzle by
    // plain extrusion (the firmware sets load_finish only from its own load).
    // Both read identically, so Unload is offered for both.
    [[nodiscard]] bool slot_filament_parked_in_toolhead(int slot_index) const override;

    // Per-tool LOADED status. The U1 has 4 independent toolheads, so each slot's
    // loaded state is tracked separately rather than derived from a single
    // current_slot. Drives the active-lane highlight per tool.
    [[nodiscard]] bool slot_is_actively_loaded(int slot_index) const override;

    /// Raw per-channel feeder fields as the firmware last reported them.
    /// Eligibility answers from these; presence alone cannot distinguish a
    /// lane holding filament from a head that is loaded.
    struct ChannelSnapshot {
        std::string state;        ///< channel_state, e.g. "load_finish"
        std::string action_state; ///< channel_action_state: last op step, kept at rest
        /// An op outcome channel_action_state reported while the channel still
        /// showed a step under way, applied when it settles.
        std::string pending_outcome;
        std::string error{"ok"}; ///< channel_error
        bool filament_detected{false};
        bool module_exist{false};
        bool disable_auto{false};
        /// The head's filament_motion_sensor `enabled` flag. Arrives from the
        /// sensor status objects, not the feeder frame, so a feeder write
        /// carries the previous value forward instead of defaulting it.
        bool sensor_enabled{false};
    };

    [[nodiscard]] ChannelSnapshot channel_snapshot(int slot_index) const;

    /// Eligibility answered from the feeder's own channel fields: only a
    /// settled state on a fault-free, auto-mode channel with its motion
    /// sensor armed can take an operation, and the answer flips with the
    /// requested direction.
    [[nodiscard]] FilamentOpEligibility slot_op_eligibility(int slot_index,
                                                            bool load) const override;

  protected:
    // Operations. Every one of these drives the toolhead: AUTO_FEEDING forwards
    // to FEED_AUTO, which homes before it feeds, and `T{n}` moves the carriage.
    // AmsSubscriptionBackend's NVI wrapper refuses them while a print owns the
    // toolhead; PAUSED still passes (filament_ops_self_home() is false), which
    // is what keeps U1 runout recovery working (#991).
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;

  public:
    /// The AUTO_FEEDING unload heats; the bare INNER_FILAMENT_UNLOAD leaf, sent
    /// when no slot resolves, skips the state machine that does.
    [[nodiscard]] bool unload_heats_hotend(int slot_index) const override;

  protected:
    AmsError do_change_tool(int tool_number) override;
    /// PARK_EXTRUDER: the firmware's "Detach Toolhead", back to its dock.
    AmsError do_park_active_tool() override;

  public:
    [[nodiscard]] bool supports_tool_park() const override {
        return true;
    }

  protected:
    /// Batch load/unload as ONE AUTO_FEEDING script. The firmware serializes
    /// channels itself (FEED_AUTO's process-wide channel_active gate) and
    /// Moonraker holds the script response until every line has run, so the
    /// script needs no sequencer. Klipper aborts the remaining lines when one
    /// raises, so every index is validated before anything is sent.
    AmsError do_filament_batch(const std::vector<int>& slots, bool load) override;

    /// On the U1 a slot select IS a physical tool change — do_select_slot()
    /// forwards to do_change_tool(), which emits `T{n}` and moves the carriage.
    [[nodiscard]] bool select_slot_moves_toolhead() const override {
        return true;
    }

  public:
    // Filament loaded to this tool's nozzle: a present lane with the
    // channel_state load latch set, for every toolhead (active or parked). The
    // base PARALLEL gate's is_present() alone would read true for a lane whose
    // filament the firmware has retracted, since filament_exist stays true.
    // Filament parked in the toolhead is slot_filament_parked_in_toolhead().
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

    // Recovery (not supported)
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;
    /// cancel() has nothing to send on this firmware, so no Abort is offered.
    [[nodiscard]] bool can_cancel_operation() const override {
        return false;
    }
    /// Also forgets each channel's last frame and in-flight op, because the
    /// backend outlives the restart: the subscription's first full frame is
    /// then a fresh start rather than a change from firmware that is gone.
    void on_klipper_restarted() override;

    // Resume preparation. A Terminal pause answers RESUME_REQUIRES_RESTART at
    // once. A head whose last load is unconfirmed holds RESUME: the head
    // resumed on retries its load, any other head is refused. After a runout
    // (sensor_filament_present_ false for the active tool) AUTO_FEEDING refeeds it before the
    // caller dispatches RESUME. Otherwise every used head whose print_task_config record names no
    // filament type while the slot knows its material gets that record
    // written (the firmware refuses RESUME without it). on_ready is always
    // called on the main thread.
    void prepare_for_resume(int slot_index, ResumeReadyCallback on_ready) override;

    // The firmware refuses RESUME while a used extruder still has no material
    // assigned, and raises that refusal oneshot: it reaches us only on the `!!`
    // broadcast, while print_stats.exception keeps whatever paused the print.
    // Left to error_classify::classify(), an uncoded `!!` on a paused printer is
    // handed a Resume button, and this is the one fault that refuses it again on
    // every tap.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line, const helix::ClassifyContext& ctx) const override;

    // True when the motion sensor reports runout but the port sensor still
    // reads filament present — i.e. the encoder is stale (e.g., it never
    // saw the start-of-print purge) but physical filament is in the buffer.
    // Callers (FilamentRunoutHandler) auto-recover silently instead of
    // showing the modal.
    [[nodiscard]] bool is_stuck_motion_sensor_runout(int slot_index) const override;

    // Snapmaker U1 uses firmware-native print_task_config gcode
    // (SET_PRINT_USED_EXTRUDERS / SET_PRINT_EXTRUDER_MAP) emitted before
    // PRINT_START; no gcode-file rewrite is needed.
    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::PrePrintSend;
    }

    // Configuration
    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) override;

    /// The channel's last tag reading. An entry whose CARD_UID names no card
    /// was written over the API, not read off a spool, and is none.
    [[nodiscard]] std::optional<helix::ams::TagReading> tag_reading(int slot_index) const override;
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    void persist_external_identity_impl(int slot_index,
                                        const helix::ams::Observation& spoolman) override;
    [[nodiscard]] AmsError can_set_tool_mapping(int tool_number, int slot_index) const override;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields (spool_name, spoolman_*, remaining_weight_g) on
    // the live SlotInfo, and fires override_store_->clear_async. Brand /
    // color_name / total_weight_g are preserved — on Snapmaker those fields
    // are populated from the RFID tag, so they reflect firmware truth.
    // The hardware-event detector calls this internally once a CARD_UID change
    // confirms a physical swap.
    void clear_slot_override(int slot_index) override;

    // Bypass (not applicable for tool changers)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override {
        return false;
    }

    // Builds the firmware-native pre-print command sequence for print_task_config.
    // tools_used: logical tools the gcode body uses (ParsedGCodeFile::tools_used_indices).
    // remap:      logical tool -> physical head, ONLY for tools the user changed from identity.
    //             Tools absent from `remap` take default_routing().head(t); a tool the
    //             routing gives no head is left out of the map entirely.
    // Returns newline-joined gcode (NO trailing newline), or "" when tools_used is empty.
    // Pure — no api_/network access, trivially unit-testable.
    [[nodiscard]] std::string build_preprint_gcode(const std::set<int>& tools_used,
                                                   const std::map<int, int>& remap) const override;

    /// A start from this screen during a print in progress (a restart whose
    /// cancel and start reach this backend as one unbroken print) begins a
    /// new print's state.
    void note_print_start_from_here() override;

    /// print_task_config.py's reset_print_info() returns extruder_map_table to
    /// identity at every print end (complete, cancel, error).
    [[nodiscard]] bool print_routing_resets_to_identity() const override {
        return true;
    }

    /// SET_PRINT_USED_EXTRUDERS for @p tools_used at their default heads, no
    /// map, plus the per-print unload choice. Only while the firmware's map
    /// still reads its default for every used tool: a map a failed start left
    /// pending would route them elsewhere, and then only the unload choice goes.
    [[nodiscard]] std::string
    build_unrouted_preprint_gcode(const std::set<int>& tools_used) const override;

    /// The builder above, as a free-standing function.
    ///
    /// It reads no member state — the routing is entirely a function of its two
    /// arguments — so the virtual is a one-line forward to this. Exposed because
    /// the mock backend has to emit the SAME bytes when emulating a U1: it
    /// advertises requires_preprint_send() and so promises the controller there
    /// is work to do, and returning nothing there made the whole remap chain a
    /// silent no-op under --test. Calling this keeps the command format in ONE
    /// place; a copy in the mock would teach the wrong shape the moment the
    /// format moved.
    [[nodiscard]] static std::string preprint_gcode(const std::set<int>& tools_used,
                                                    const std::map<int, int>& remap);

    /// `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<heads>`, ascending.
    [[nodiscard]] static std::string used_extruders_line(const std::set<int>& heads);

    /// Which lines of a filament record write to send.
    enum class RecordLines { Both, Identity, Colour };

    /// The SET_PRINT_FILAMENT_CONFIG script that writes @p slot's identity into
    /// print_task_config for physical head @p head: one line for vendor, type
    /// and subtype, one for colour (@p lines picks either alone). VENDOR is
    /// "Snapmaker" for a Snapmaker spool and "Generic" otherwise;
    /// FILAMENT_SUBTYPE is the spool's product line when it names one, else
    /// "Basic". Empty when the slot names no material or one
    /// IMoonrakerAPI::is_safe_material_param() refuses; values with a space are
    /// quoted by IMoonrakerAPI::gcode_param_value(). A soft material adds
    /// SOFT=1 to the identity line. @p force appends FORCE=1 to every line,
    /// which a head the firmware calls official requires.
    [[nodiscard]] static std::string
    build_filament_config_gcode(int head, const SlotInfo& slot, bool force = false,
                                RecordLines lines = RecordLines::Both);

    /// One multi-line feed script covering @p slots, in the order given,
    /// newline-joined with no trailing newline. Empty @p slots yields the
    /// empty string in the per-line shape (a bare START/END pair in the batch
    /// shape); the caller (do_filament_batch) refuses empty before sending.
    ///
    /// @p use_batch_macro selects the shape: false joins one
    /// `AUTO_FEEDING EXTRUDER={n} LOAD=1` (or `UNLOAD=1`) line per slot; true
    /// drives the firmware's AUTO_FEEDING_BATCH state machine, which owns
    /// preheat and target-restore between the START and END sentinels. The
    /// default false is the shape every firmware accepts; do_filament_batch
    /// passes the capability cached in use_batch_macro_.
    ///
    /// Pure, same reasoning as preprint_gcode() above — reads no member state,
    /// so it unit-tests without a backend or connection.
    [[nodiscard]] static std::string batch_feed_gcode(const std::vector<int>& slots, bool load,
                                                      bool use_batch_macro = false);

    /// Caches the batch-macro capability from @p discovery. Must run before
    /// start(): the status parse and the batch dispatch both read
    /// use_batch_macro_ / batch_macro_object_, and PrinterState publishes its
    /// global discovery only after backends have started.
    void set_discovery(const helix::PrinterDiscovery& discovery) override;

    /// The dispatched batch and how far it has verified. heads is in dispatch
    /// order; cursor counts heads that reached the direction's terminal
    /// channel_state. active spans dispatch until every head verified or a
    /// head failed.
    struct BatchPlan {
        std::vector<int> heads; ///< in dispatch order
        bool load{false};
        size_t cursor{0}; ///< how many heads have reached their terminal state
        bool active{false};
        /// Progress-line words, translated at dispatch time: the
        /// cursor-advance parse that renders them only formats them and makes
        /// no LVGL calls.
        std::string direction_label; ///< "Load" / "Unload"
        std::string of_label;        ///< "of", as in "Load 2 of 4"
        /// Identifies THIS dispatch. The deferred RPC-failure recovery
        /// compares it against the live plan, so a failure answered after
        /// the plan completed — or one belonging to an earlier, replaced
        /// plan — cannot act on stale authority.
        uint64_t dispatch_id{0};
    };

    /// Snapshot of the in-flight batch plan (all defaults when none was
    /// dispatched). The failure-recovery path and tests read this.
    [[nodiscard]] BatchPlan batch_plan() const;

    /// True while a batch this process dispatched is still unverified, so
    /// connect-time cleanup can tell its own live batch from an interlock
    /// stranded by an earlier session.
    [[nodiscard]] bool filament_batch_in_flight() const override {
        return batch_plan().active;
    }

    /// Sends AUTO_FEEDING_BATCH ACTION=END. Klipper aborts the rest of a
    /// script when one line raises, so a failed head strands the firmware's
    /// `doing` interlock — which refuses every print start and resume until
    /// cleared. No-op when use_batch_macro_ is false: firmware without the
    /// macro has no interlock, and the command would be bogus there.
    void end_firmware_batch();

    /// Sends a single-head feed command. The macro blocks until the feed is
    /// over, so its answer ends the operation: OK raises @p complete_event and
    /// an error raises kEventError. Either one ends an action the UI is holding
    /// for a feed the firmware declined without moving channel_state. A
    /// timeout raises nothing: the feed may still be running, and
    /// channel_state reports its end. An error or a timeout on a load marks
    /// @p load_head's load unconfirmed; -1 names no head.
    AmsError dispatch_feed(std::string gcode, const char* complete_event, int load_head = -1);

    /// dispatch_feed() with the answer handed to the caller: @p on_ok on OK,
    /// @p on_failed on an error or a timeout. Both run on the main thread.
    AmsError dispatch_feed(std::string gcode, std::function<void()> on_ok,
                           std::function<void(const MoonrakerError&)> on_failed);

    /// Applied logical-tool -> physical-head routing for the CURRENT print.
    ///
    /// The capability question generic code asks; the vendor knowledge (that the
    /// answer is print_task_config.extruder_map_table) stops here. Callers must
    /// not reach for the firmware field themselves.
    ///
    /// Empty until the first status frame carrying the table arrives, which the
    /// caller must read as "no opinion" and fall back on, NOT as "identity".
    ///
    /// Capabilities are deliberately left unsupported/non-editable: this backend
    /// applies remaps through its pre-print send, not the generic
    /// set_tool_mapping() path, and reporting support would pull it into
    /// build_ams_topology() and the mapping save/restore flow it does not use.
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    /// The routing observed while a print task was configured — what a reprint
    /// replays. See the base declaration; the member it reads is
    /// last_task_extruder_map_.
    [[nodiscard]] std::vector<int> last_print_tool_mapping() const override;

    /// What the firmware last reported for its stored print preferences
    /// (print_task_config). Empty until a frame carrying one arrives.
    /// Returns a copy under mutex_: every status frame rewrites the member, so
    /// a reference would hand a caller a torn read.
    [[nodiscard]] snapmaker::PrintPreferences print_preferences() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return print_preferences_;
    }

    /// The Print Behavior section shown on the AMS device-operations
    /// overlay, and the Toolhead section with each head's nozzle. Each is
    /// absent until the firmware has reported what it shows, so the overlay
    /// never offers a setting whose state it does not know.
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;

    /// One DeviceAction per reported preference: toggles, a sensitivity
    /// dropdown, and one end-unload toggle per reported toolhead. Then, per
    /// head, a nozzle diameter and a nozzle flow dropdown for each field the
    /// head reports, unavailable while a print is printing or paused (the
    /// firmware refuses the change then).
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;

    /// Sends the SET_PRINT_PREFERENCES line build_preference_gcode() maps the
    /// action id to, or the nozzle change build_nozzle_change() makes of it.
    /// Unknown ids are reported as not supported.
    AmsError do_execute_device_action(const std::string& action_id, const std::any& value) override;

    /// Each head's nozzle as the firmware last reported it.
    [[nodiscard]] std::array<snapmaker::nozzle::Nozzle, snapmaker::kToolCount>
    reported_nozzles() const;

    /// What a pick on a nozzle row sends.
    struct NozzleChange {
        /// The id names a nozzle row of a head that reports the field, and the
        /// value is one of its options.
        bool valid = false;
        /// The params of the firmware's nozzle_properties call, carrying only
        /// the field that differs from the report; unset when the pick is what
        /// the head already reports, so nothing is sent.
        std::optional<nlohmann::json> params;
    };

    /// The change a nozzle row's pick makes. Separated from
    /// execute_device_action so the mapping is testable without a client.
    [[nodiscard]] NozzleChange build_nozzle_change(const std::string& action_id,
                                                   const std::any& value) const;

    /// The saved unload-at-end list (SettingsManager's end-of-print unload
    /// tools) as one SET_PRINT_PREFERENCES line, sized to the toolheads the
    /// firmware reports, with every soft_heads() head 0; empty until it has
    /// reported them. The firmware clears its list when every print ends, so
    /// build_preprint_gcode() carries it on every start. @p print_running
    /// renders it for a print already under way, which the firmware refuses
    /// without FORCE.
    [[nodiscard]] std::string end_unload_gcode(bool print_running = false) const;

    /// The unload-at-end list for a print some other client started.
    [[nodiscard]] std::string standing_print_gcode() const override;

    /// Per head, whether it holds soft filament: the firmware's filament_soft
    /// flag, or a soft material on the slot. Never the flag alone: the
    /// firmware sets it only for types its own table names.
    [[nodiscard]] std::vector<bool> soft_heads() const;

    /// The command one action produces, or empty when the id is not ours.
    /// Separated from execute_device_action so the mapping is testable without
    /// a Moonraker client.
    [[nodiscard]] std::string build_preference_gcode(const std::string& action_id,
                                                     const std::any& value) const;

    /// Ends a toolhead offset calibration run in progress, so the printer is
    /// not left holding the calibration state with heaters on.
    void before_disconnect() override;

    /// Endless Spool is the firmware's auto-replenish: available once it has
    /// reported the flag, switchable, and firmware-managed (the firmware
    /// chooses the backup head).
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;

  protected:
    /// SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=0|1.
    AmsError apply_endless_spool_enabled(bool enabled) override;

    // Extension points for hardware hung off the U1's heads. Each default is
    // the stock U1's answer, and each is called with mutex_ held.

    /// Whether @p head is fed from upstream of its feeder port, through a
    /// bowden from another unit. Its feed channel's filament_detected is then
    /// that unit's gate, not a spool seated at the head's port: no PREP
    /// sensor, no insert presence, no insert to judge.
    [[nodiscard]] virtual bool head_is_upstream_fed_locked(int head) const {
        (void)head;
        return false;
    }

    /// Whether a channel reaching preload_finish straight from an unload's
    /// in-progress states ends that unload on @p head. A stock unload ends at
    /// unload_finish and only rests at preload_finish.
    [[nodiscard]] virtual bool preload_finish_ends_unload_locked(int head) const {
        (void)head;
        return false;
    }

    /// Reads the extension's own status objects out of one frame, ahead of the
    /// U1's own sections, so they see this frame's answers from the hooks
    /// above. Sets @p changed when it moved anything published; the frame
    /// notifies once either way.
    virtual void apply_extension_locked(const nlohmann::json& status, bool& changed) {
        (void)status;
        (void)changed;
    }

    void on_started() override;
    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[AMS Snapmaker]";
    }
    SlotInfo* cached_slot_locked(int slot_index) override;
    [[nodiscard]] std::optional<AmsAction> step_action_locked() const override;

  private:
    friend class SnapmakerTestAccess;
    friend class SnapmakerRealtimeTestAccess;
    friend class RunoutScopeTestAccess;

    static constexpr int kNumTools = 4;

    /// What one status frame decides, collected under mutex_ and acted on by
    /// dispatch_effects() after it is released, plus the frame-local values the
    /// parse sections hand to each other.
    /// A user edit's record write held by record_writes_held_locked().
    struct PendingRecordWrite {
        SlotInfo info;
        bool material_declared = false;
    };

    /// A refeed whose AUTO_FEEDING answered OK before the feeder's frame
    /// named its outcome.
    struct RefeedWait {
        int slot = -1;
        ResumeReadyCallback on_ready;
    };

    struct FrameEffects {
        bool changed = false;
        /// The active-tool port-present flag changed (#991): published to
        /// AmsState exactly once.
        bool port_present_changed = false;
        /// Lanes that reached "unload_finish".
        std::vector<int> unloaded_lanes;
        /// Channels whose feed-port presence rose with no tag evidence behind
        /// it.
        std::vector<int> unverified_insert_lanes;
        /// The cursor head's *_fail state when the active batch hit one;
        /// end_firmware_batch() sends gcode, which must not run under mutex_.
        int batch_failed_head = -1;
        std::string batch_failed_state;

        /// What this frame physically read off each channel's spool. A default
        /// entry (no UID, read not finished) means the frame carried no
        /// filament_detect.info for that channel, which the insert rule reads
        /// as no signal.
        std::array<helix::ams::SpoolEvidence, kNumTools> observed_evidence{};
        /// The head whose channel reported an in-progress state this frame.
        int in_progress_head = -1;
        /// The batch macro reported doing=false and retired the active plan.
        bool batch_retired = false;
        /// official_flag_repairs_locked() output, sent outside mutex_.
        std::vector<std::pair<int, std::string>> official_repairs;
        /// record_restores_locked() output, sent outside mutex_.
        std::vector<std::pair<int, std::string>> record_restores;
        /// Held record writes this frame released.
        std::vector<std::pair<int, PendingRecordWrite>> record_flushes;
        /// The head a refeed waits on reached a settled state this frame.
        bool refeed_settled = false;
        /// The print's first printing frame: its file's heads are checked for
        /// one the firmware will not load.
        bool check_used_heads = false;
        std::string check_used_heads_file;
        uint32_t check_used_heads_print = 0;
    };

    void dispatch_effects(const FrameEffects& fx);

    // The sections of one status frame, applied under mutex_ in this order.
    // Each reads only its slice of the parsed frame plus the state held from
    // earlier frames.
    void apply_extruders_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    void apply_active_tool_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    void apply_filament_detect_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    void apply_rfid_entry_locked(int slot_index, const SnapmakerRfidInfo& rfid, FrameEffects& fx);
    /// Keep this entry as the channel's tag reading (the vendor-cache @p cache
    /// plus the temperatures and multi-colour the lane model has no field
    /// for), and lay the tag's multi-colour onto
    /// @p slot unless the lane declares a colour.
    void record_tag_reading_locked(int slot_index, const SnapmakerRfidInfo& rfid,
                                   const helix::ams::Observation& cache, SlotInfo& slot);
    void apply_feed_channels_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    void apply_feed_channel_locked(const snapmaker::FeedChannelDelta& channel, FrameEffects& fx);
    /// The feed-port flag: its false -> true edge is an insert into the channel.
    void apply_port_presence_locked(int slot_index, bool detected, FrameEffects& fx);
    /// Step-bar phase, working head and the loaded-at-toolhead latch, all driven
    /// by the channel_state the frame reported.
    void apply_channel_progress_locked(int slot_index, const std::string& state,
                                       const snapmaker::ChannelStateInfo& info, FrameEffects& fx);
    /// Error surfacing and the operation's action lifecycle for the op outcome.
    void apply_channel_outcome_locked(int slot_index, const std::string& op_state,
                                      const snapmaker::ChannelStateInfo& op_info,
                                      const std::string& error, bool outcome_is_new,
                                      FrameEffects& fx);
    /// The task manager's record: stored preferences, routing, and each head's
    /// configured material, brand and colour.
    void apply_print_task_config_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    void apply_task_routing_locked(const snapmaker::PrintTaskConfigDelta& ptc, FrameEffects& fx);
    void apply_task_slot_identity_locked(const snapmaker::PrintTaskConfigDelta& ptc,
                                         FrameEffects& fx);
    /// The per-tool runout sensors: enabled flag and filament present/runout.
    void apply_toolhead_sensors_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    /// The tail of every frame: derives what no single section owns from the
    /// state the sections left, and lays each lane's resolved values on its
    /// slot.
    void converge_locked(FrameEffects& fx);
    /// filament_loaded and the active tool's LOADED status, from the latch and
    /// the toolhead switch.
    void derive_active_tool_loaded_locked(FrameEffects& fx);
    /// A slot whose runout sensor reports no filament is AVAILABLE, not LOADED.
    void demote_runout_slots_locked(FrameEffects& fx);
    /// Override layering and the lane-model ingest, once per slot.
    void converge_lanes_locked(FrameEffects& fx);
    /// The active tool's first-gate (port) presence, flagged for publication
    /// when it changed.
    void track_active_port_present_locked(FrameEffects& fx);
    /// Retires an active batch plan the firmware reports no longer running.
    void apply_batch_state_locked(const snapmaker::StatusDelta& delta, FrameEffects& fx);
    /// The head an operation is working on, from the batch cursor or the
    /// in-progress channel this frame found.
    void apply_working_slot_locked(FrameEffects& fx);
    /// Verifies the batch plan's cursor head against the op outcome.
    void advance_batch_locked(int slot_index, const std::string& op_state,
                              const snapmaker::ChannelStateInfo& op_info, FrameEffects& fx);

    /// RPC timeout budget for ONE batch feed op. AUTO_FEEDING heats from cold +
    /// feeds + flushes; measured ~86s live (see prepare_for_resume), so 150s is
    /// the headroom the resume path already uses. A batch scales this per op.
    static constexpr uint32_t kBatchFeedOpTimeoutMs = 150000;

    /// Firmware routing: logical tool index -> physical head index, mirrored from
    /// print_task_config.extruder_map_table (32 logical entries, 4 heads).
    ///
    /// This is the U1's authoritative answer to "which head prints Tn", not a
    /// guess: klippy routes BOTH tool ranges through it (T0-T3 via the default
    /// A=1 on SWITCH_EXTRUDER_ADVANCED, T4-T31 unconditionally), and its own
    /// runout auto-replenish rewrites this table to redirect a logical tool at a
    /// replacement head. See docs/devel/FILAMENT_BACKEND_SNAPMAKER_U1.md §
    /// "How `Tn` resolves to a physical head".
    ///
    /// Deliberately NOT folded into system_info_.tool_to_slot_map: that field
    /// means physical attachment (which head holds which spool, trivially
    /// identity here) and is read as such by the Load/Unload resolver and the
    /// persisted tool-map ledger. Routing and attachment are different questions
    /// that only coincide when nothing is remapped.
    std::vector<int> extruder_map_table_;

    /// print_task_config.extruders_used — which physical heads the CURRENT task
    /// uses. All-false means no task is configured, which is the gate that makes
    /// extruder_map_table_ readable at all: idle, the table reads a default
    /// identity [0,1,2,3] that is indistinguishable from "this print needs no
    /// remap" and is flatly wrong for a file whose tools do not line up with the
    /// lanes. The firmware sets this only when a task is set up (our pre-print
    /// SET_PRINT_USED_EXTRUDERS, or the Snapmaker screen/cloud doing the same)
    /// and clears it when the print ends. Observed live: idle
    /// [F,F,F,F] + map [0,1,2,3]; mid-print [F,F,T,F] + map [2,1,2,3].
    std::vector<bool> extruders_used_;

    /// extruder_map_table_ as it read while a task WAS configured — the routing
    /// the most recent print actually ran with.
    ///
    /// extruder_map_table_ alone cannot answer that after the fact: the gate that
    /// makes it readable closes when the print ends, and the firmware resets the
    /// table to its default identity, so both a finished crossover and a job that
    /// needed no remap read the same. Captured on every print_task_config frame
    /// that arrives with the gate open, which also means the U1's own runout
    /// auto-replenish rewrite is captured — a reprint should follow the heads the
    /// print finished on, not the ones it was planned with.
    ///
    /// Empty until a configured task has been seen. Written only from
    /// handle_status under mutex_; read by last_print_tool_mapping().
    std::vector<int> last_task_extruder_map_;

    /// What the firmware last reported for its stored print preferences.
    /// Merged across frames: Moonraker sends deltas, so a frame that omits a
    /// setting is silent about it rather than reporting it off.
    ///
    /// Like every other print_task_config field, these are a write surface, not
    /// a sensor — held as told, never filed as a lane observation.
    ///
    /// Written only from handle_status under mutex_; read by
    /// print_preferences().
    snapmaker::PrintPreferences print_preferences_;

    /// Per-extruder cached state
    std::array<ExtruderToolState, kNumTools> extruder_states_;

    /// How long a sent T<n> keeps the carriage from reading empty if the head
    /// never reports; a change that fails on the firmware must not hold it forever.
    static constexpr std::chrono::seconds kToolChangeWindow{20};

    /// Tool of a T<n> sent and not yet seen on the carriage, -1 when none. The
    /// old head docks before the new one reports, so every head briefly reads
    /// PARKED mid-change; this keeps that gap from reading as an empty carriage.
    /// Guarded by mutex_.
    int pending_tool_ = -1;
    std::chrono::steady_clock::time_point pending_until_{};

    /// A head is moving onto the carriage: a T<n> is pending, a head is in its
    /// activating move, or the action is SELECTING. Caller holds mutex_.
    [[nodiscard]] bool tool_change_in_flight_locked() const;

    /// Per-slot filament_motion_sensor "filament_detected" state. True means
    /// filament is currently being fed to the toolhead; false means runout has
    /// fired. Tracks `filament_motion_sensor e0..e3_filament` (and the
    /// matching `filament_switch_sensor` form as a fallback). Defaults to true
    /// so a slot without a configured sensor doesn't render as "runout" — the
    /// flag flips only when Klipper sends an explicit `filament_detected:false`
    /// for that sensor, which only happens on configured runout sensors. Read
    /// by get_filament_segment() / get_slot_filament_segment() to break the
    /// spool→toolhead line when the active tool has run out.
    std::array<bool, kNumTools> sensor_filament_present_{{true, true, true, true}};

    /// Whether sensor_filament_present_[i] came from a real filament_detected
    /// report. Its true default means "no runout", never "filament in the
    /// toolhead", so only a reported reading answers the parked question.
    std::array<bool, kNumTools> toolhead_switch_reported_{{false, false, false, false}};

    /// How far tool i's own lane holds filament. Caller holds mutex_.
    [[nodiscard]] PathSegment slot_segment_locked(int i) const;

    /// Filament in tool i's toolhead short of the nozzle. Caller holds mutex_.
    [[nodiscard]] bool parked_in_toolhead_locked(int i) const {
        return !loaded_at_toolhead_[i] && toolhead_switch_reported_[i] &&
               sensor_filament_present_[i];
    }

    /// Per-slot port/buffer sensor state — the filament_feed left/right
    /// .extruder{N}.filament_detected flag. Reads the physical-presence
    /// sensor at the spool/buffer side, NOT the encoder-based motion sensor.
    /// Used together with sensor_filament_present_ to differentiate real
    /// runouts (both false) from stale motion sensor false positives
    /// (motion=false, port=true). Defaults to false so a slot we've never
    /// seen filament_feed data for doesn't generate a "stuck sensor" false
    /// positive of its own — the auto-recover path requires the port sensor
    /// to explicitly report present.
    std::array<bool, kNumTools> port_sensor_filament_present_{{false, false, false, false}};

    /// Last value published to AmsState::set_active_tool_port_present for the
    /// active tool (#991). Tracks the active-tool port flag so we only push to
    /// the UI subject on an actual change. -1 = nothing published yet. Written
    /// only from handle_status (the single writer).
    int last_published_port_present_ = -1;

    /// Per-slot "filament is loaded to THIS tool's nozzle" latch, driven
    /// purely from filament_feed.channel_state transitions (NOT the toolhead
    /// switch). The switch (e{N}_filament) stays true after an unload: the tip
    /// is retracted from the melt zone only and still sits in the toolhead.
    /// channel_state is the authoritative load signal (verified live on a U1,
    /// firmware 20260608): load_finish means loaded, unload_finish / wait_insert /
    /// preload_finish mean not-loaded. Mirrors the firmware's own persisted
    /// config['load_finish'] flag. Set true on load_finish; cleared on
    /// unload_finish / wait_insert / preload_finish; left unchanged on every
    /// transient / in-progress / fail state. Defaults false: a lane we've never
    /// seen a channel_state for is treated as not-loaded. Read by
    /// slot_has_filament_at_toolhead() and can_unload_from_toolhead(). The
    /// motion sensor (sensor_filament_present_) still owns mid-print runout —
    /// a different question ("did the ACTIVE lane run out during extrusion").
    std::array<bool, kNumTools> loaded_at_toolhead_{{false, false, false, false}};

    /// Per-slot "filament_feed has reported this lane" — set the first time a
    /// frame carries a boolean filament_detected or a channel_state for the
    /// lane, and never cleared. Until it is set, the convergence-point presence
    /// ingest stays silent: the port/latch arrays rest on their defaults
    /// ("no reading yet", not "no filament"), and declaring those defaults
    /// would empty a lane whose only signal so far is the toolhead pin state.
    std::array<bool, kNumTools> feed_presence_seen_{{false, false, false, false}};

    /// Per-channel pending insert: parse passes held since the feed-port
    /// presence rose, 0 = none pending. The port edge fires the moment
    /// filament seats in the bay, but the tag reader's answer for that spool
    /// lands a frame or two later in filament_detect.info, so the edge itself
    /// cannot tell a tagged spool from an untagged one. The next info entry
    /// for the channel decides: tag evidence verifies the insert, an entry
    /// with none (no UID, MAIN_TYPE NONE) is the reader saying there is no tag
    /// behind it and asks. No entry at all within kSnapPendingInsertPasses
    /// parses (reader disabled, read never landed) asks too. Presence dropping
    /// cancels it: the spool left before any read. A feed the firmware itself
    /// drives (tool-change load/unload, one of our batch ops) never arms it.
    /// Written only from handle_status (the single writer).
    std::array<int, kNumTools> pending_insert_passes_{{0, 0, 0, 0}};

    /// The reader's verdict on each channel's last judged insert: FromTag,
    /// NoTag, or NotRead once a new insert arms or the reader never judged one.
    /// Written only from handle_status, beside pending_insert_passes_.
    std::array<InsertIdentity, kNumTools> insert_verdict_{};

    /// What each channel's firmware last said about the spool's identity:
    /// whether filament_detect.info named one, and print_task_config's vendor
    /// and official flag. Unset until the first frame carrying each.
    std::array<std::optional<bool>, kNumTools> tag_names_identity_{};
    std::array<std::optional<std::string>, kNumTools> task_vendor_{};
    std::array<std::optional<bool>, kNumTools> task_official_{};

    /// print_task_config.filament_type per head, as the firmware spells it
    /// ("NONE" or "" when nothing is set). SlotInfo::material cannot answer
    /// this: the user's own declaration is laid over it. Unset until the first
    /// frame carrying it.
    std::array<std::optional<std::string>, kNumTools> task_type_{};

    /// print_task_config.filament_color_rgba per head, decoded. Unset until the
    /// first frame carrying a colour that decodes.
    std::array<std::optional<uint32_t>, kNumTools> task_color_{};

    /// print_task_config.filament_sub_type per head, as the firmware spells it.
    /// Unset until the first frame carrying it.
    std::array<std::optional<std::string>, kNumTools> task_subtype_{};

    /// Per head: an official-flag repair has been sent this session. Under
    /// mutex_.
    std::array<bool, kNumTools> official_repair_sent_{};

    /// Per head: the lane's declaration has been written back since the
    /// firmware record last named a type. Under mutex_.
    std::array<bool, kNumTools> record_restore_sent_{};

    /// Per channel: a spool went into the feeder port after the lane's
    /// material was declared, so the declaration may name a spool that has
    /// left. The next user edit declares the spool in the port. Under mutex_.
    std::array<bool, kNumTools> insert_since_declaration_{};

    /// Per channel: the tag reader has shown a real card this session. Under
    /// mutex_.
    std::array<bool, kNumTools> card_seen_{};

    /// print_stats.state from this backend's own status frames; unset until a
    /// frame carries it. Under mutex_.
    std::optional<std::string> print_state_;

    /// Per head: a load during the current print ended without the feeder
    /// reporting load_finish with channel_error ok. The head presence switch
    /// reads "present" for filament that reached the toolhead and stopped
    /// short of the nozzle, so it cannot vouch for a load; until the feeder
    /// does, Resume retries the load instead of sending RESUME. Cleared by a
    /// confirmed load, the print ending, and a disconnect. Under mutex_.
    std::array<bool, kNumTools> load_unconfirmed_{};

    /// The current print's used heads have been checked: once per print,
    /// reset when the print ends. Under mutex_.
    bool used_heads_checked_ = false;

    /// A restart began while a print was in progress: its check waits for a
    /// frame naming the new file. A restart of the same file names none and
    /// goes unchecked. Under mutex_.
    bool awaiting_restart_file_ = false;

    /// How long a refeed's OK waits for the head's next settled frame.
    static constexpr std::chrono::milliseconds kRefeedFrameWait{3000};

    /// The refeed waiting on its head's frame. Under mutex_.
    std::optional<RefeedWait> refeed_wait_;

    /// A refeed was sent and has not been judged. Under mutex_.
    bool refeed_in_flight_ = false;

    /// Ends refeed_wait_ at kRefeedFrameWait. Main thread.
    helix::ui::LvglTimerGuard refeed_wait_timer_;

    /// Counts prints: bumped each time print_stats.state enters or leaves a
    /// print, so an answer about one print's file is dropped once it ends.
    /// Under mutex_.
    uint32_t print_generation_ = 0;

    /// print_stats.filename from this backend's own frames. Under mutex_.
    std::string print_filename_;

    /// Reads the running print's file for its tools. Created on first use;
    /// main thread only.
    std::unique_ptr<FileStartFactsLoader> file_facts_;

    /// Answers the tools @p filename uses (empty when unreadable); tests
    /// replace the file read with it. Main thread only.
    std::function<void(const std::string& filename, std::function<void(std::set<int>)> on_tools)>
        file_tools_probe_;

    /// machine_state_manager.main_state from this backend's own status frames;
    /// unset until a frame carries it. Under mutex_.
    std::optional<int> main_state_;

    /// Per head, the latest held record write; dropped with the head's
    /// override and at on_started(). Under mutex_.
    std::array<std::optional<PendingRecordWrite>, kNumTools> pending_record_writes_{};

    /// The temperatures and spool weight the last tag reading on each channel
    /// wrote onto SlotInfo, which no lane record carries. Unset until a tag is
    /// read, and again once retract_tag_reading_locked() takes them back.
    struct TagSlotValues {
        int nozzle_temp_min = 0;
        int nozzle_temp_max = 0;
        int bed_temp = 0;
        float total_weight_g = 0;
    };
    std::array<std::optional<TagSlotValues>, kNumTools> tag_slot_values_{};

    /// The last tag reading on each channel, as tag_reading() reports it. Unset
    /// when the channel's entry names no tag, or a CARD_UID naming no card.
    /// Written by handle_status, retract_tag_reading_locked() and
    /// on_started(); all access under mutex_.
    std::array<std::optional<helix::ams::TagReading>, kNumTools> tag_readings_{};

    /// Whether filament_detect.info last showed a real card on each channel: a
    /// CARD_UID with a nonzero byte. Unset until the first entry for it.
    std::array<std::optional<bool>, kNumTools> tag_has_card_{};

    /// How a SET_PRINT_FILAMENT_CONFIG may reach @p head: false plain, true
    /// with FORCE=1 (the firmware calls the head official but no card backs
    /// it), nullopt not at all (a real tag's record, or no card reading yet
    /// beside an official flag). Caller holds mutex_.
    [[nodiscard]] std::optional<bool> record_write_needs_force_locked(int head) const;

    /// Write a user edit into the head's print_task_config record, the one the
    /// firmware's RESUME gate, the stock screen and slicers read: the identity
    /// line when the record names a different type, vendor or subtype, the
    /// colour line when it names a different colour, nothing when it already
    /// agrees (every line runs FLOW_RESET_K on the head), both lines with
    /// FORCE=1 on an official head with no card. A record not seen yet is
    /// written only for a declared material, @p material_declared. While
    /// record_writes_held_locked() the edit is kept in pending_record_writes_
    /// and written by the first frame for which heads_free_locked(). Main
    /// thread.
    void write_task_filament_record(int head, const SlotInfo& info, bool material_declared);

    /// Whether a user edit's record write must wait: the offset calibration
    /// holds the heads, this backend's own print_stats.state reads printing,
    /// or, before its frames have named one, the app's print job state reads
    /// PRINTING or is not initialized. Caller holds mutex_.
    [[nodiscard]] bool record_writes_held_locked() const;

    /// FORCE=1 rewrites for heads whose record is flagged official while the
    /// tag entry is filled with no card behind it, the channel has shown no
    /// card this session, and the record holds exactly what this backend's
    /// stored edit says. A filament_detect/set write (Extended Firmware) leaves
    /// that flag, and the firmware then refuses plain writes from us and from
    /// the stock screen. Nothing is returned until this backend has read a
    /// print_stats.state that is neither printing nor paused. Marks each head
    /// returned sent. Caller holds mutex_.
    [[nodiscard]] std::vector<std::pair<int, std::string>> official_flag_repairs_locked();

    /// Forget @p head's held record write, if it has one. Caller holds mutex_.
    void drop_pending_record_write_locked(int head);

    /// Whether the toolhead offset calibration holds the machine state: it
    /// parks, heats and probes the heads between its steps. Caller holds mutex_.
    [[nodiscard]] bool calibration_holds_heads_locked() const;

    /// Whether this backend's own print_stats.state is known and holds no head:
    /// neither printing nor paused, and no offset calibration holds the
    /// machine state. Caller holds mutex_.
    [[nodiscard]] bool heads_free_locked() const;

    /// The firmware forgets a head's record whenever filament leaves its
    /// toolhead (an unload, unload-at-end, a runout), though the spool still
    /// sits in the feeder. One SET_PRINT_FILAMENT_CONFIG script per head whose
    /// record names no type while its lane declares a material, a spool is in
    /// the port or at the nozzle, no spool has gone in since the declaration,
    /// no tag read is pending on it, no feed is moving the head, and the heads
    /// are free. Marks each head
    /// returned sent until its record next names a type. Caller holds mutex_.
    [[nodiscard]] std::vector<std::pair<int, std::string>> record_restores_locked();

    /// The script that refills @p head's record from its slot: empty unless
    /// the record is known to name no type, a write may reach the head, and
    /// the slot names a material. Caller holds mutex_.
    [[nodiscard]] std::string unset_record_restore_locked(int head) const;

    /// Send each script in @p writes as its own request, logging @p why per
    /// head. Main thread, outside mutex_.
    void dispatch_record_scripts(const std::vector<std::pair<int, std::string>>& writes,
                                 const char* why);

    /// One SET_PRINT_FILAMENT_CONFIG script per head the paused task uses whose
    /// firmware record names no type while the slot knows its material: the
    /// record the firmware's RESUME gate checks. The used heads are the union
    /// of extruders_used, the heads the task's map routes to, and
    /// @p active_slot. Caller holds mutex_.
    [[nodiscard]] std::vector<std::pair<int, std::string>>
    resume_record_repairs_locked(int active_slot) const;

    /// Send each of @p repairs as its own request, in order, then run @p next
    /// on the main thread, whatever each answered: a refused head leaves RESUME
    /// to report the firmware's own refusal. Runs @p next at once when there
    /// is nothing to send.
    void send_resume_repairs_then(std::vector<std::pair<int, std::string>> repairs,
                                  std::function<void()> next);

    /// prepare_for_resume after a runout on @p slot, or with @p slot's last
    /// load unconfirmed: AUTO_FEEDING refeeds it, and on_ready reports success
    /// only when the feeder then reads load_finish with channel_error ok.
    /// FEED_AUTO answers OK without feeding when the port reads empty under
    /// PRINTING=1, so the command's own answer proves nothing.
    void run_runout_refeed(int slot, ResumeReadyCallback on_ready);

    /// The feeder's own word that @p head is loaded to the nozzle: the success
    /// test FEED_AUTO applies before it raises 525.
    [[nodiscard]] static bool load_confirmed(const ChannelSnapshot& snap) {
        return snap.state == "load_finish" && snap.error == "ok";
    }

    /// A channel_error that is a fault. The firmware reports "no_filament"
    /// for any empty feeder and ""/"none" when a channel has nothing to say,
    /// none of which is one.
    [[nodiscard]] static bool channel_error_is_fault(const std::string& error) {
        return error != "ok" && !error.empty() && error != "none" && error != "no_filament";
    }

    /// The feeder's own word that the head holds filament it loaded: a load
    /// finished at the nozzle, or a hand feed finished, either with no error.
    [[nodiscard]] static bool feeder_reports_loaded(const ChannelSnapshot& snap) {
        return load_confirmed(snap) || (snap.state == "manual_sta_finish" && snap.error == "ok");
    }

    /// The feeder reports this channel failed: a *_fail state, or a
    /// channel_error that is a fault.
    [[nodiscard]] static bool feed_failure_reported(const ChannelSnapshot& snap) {
        return snapmaker::classify_channel_state(snap.state).is_fail ||
               channel_error_is_fault(snap.error);
    }

    /// An automatic feeder serves this head, so its channel_state says
    /// whether filament is loaded to the nozzle.
    [[nodiscard]] static bool feeder_serves_head(const ChannelSnapshot& snap) {
        return snap.module_exist && !snap.disable_auto && !snap.state.empty();
    }

    /// After a refeed's OK: judge it now when the feeder already reads
    /// load_finish, else on the head's next settled frame or at
    /// kRefeedFrameWait. Main thread.
    void await_refeed_outcome(int slot, ResumeReadyCallback on_ready);
    /// End a pending refeed wait, judging it on what the feeder reads now.
    void finish_refeed_wait();
    /// Judge a refeed on the head's snapshot and answer @p on_ready.
    void settle_refeed(int slot, ResumeReadyCallback on_ready);

    /// The head whose unconfirmed load holds Resume: @p preferred when it is
    /// one, else the lowest the print is known to use, else -1. Caller holds
    /// mutex_.
    [[nodiscard]] int unconfirmed_load_head_locked(int preferred) const;

    /// Resume refused because @p head's last load is unconfirmed. The detail
    /// line offers Resume as the retry only when @p retry_on_resume.
    [[nodiscard]] AmsError unconfirmed_load_error(int head, std::string technical,
                                                  bool retry_on_resume = true) const;

    /// print_stats.state names a print in progress (printing or paused).
    [[nodiscard]] bool print_in_progress_locked() const;

    /// Sends a nozzle change (@p params from build_nozzle_change()) and reads
    /// the firmware's reply. A refusal, an unexpected reply or a failed send
    /// goes to nozzle_change_failed().
    AmsError send_nozzle_change(nlohmann::json params);

    /// Main thread: tells the user the nozzle was not changed, and why when
    /// @p reason says (the firmware's or the transport's words), then
    /// re-announces the backend's state, so a row showing the refused pick
    /// goes back to the reported value.
    void nozzle_change_failed(const std::string& reason);

    /// Apply this frame's print_stats.state; a print that ends forgets the
    /// unconfirmed loads and the task judgement. Caller holds mutex_.
    void apply_print_state_locked(const std::string& state);

    /// True once per print, on its first printing frame that names its state
    /// or file (only its file after a restart that never left the print): the
    /// moment its used heads are checked. Caller holds mutex_.
    [[nodiscard]] bool used_heads_check_due_locked(bool frame_names_state, bool frame_names_file);

    /// The heads @p tools print from: the task's map when the firmware holds a
    /// configured task, else the default routing. Caller holds mutex_.
    [[nodiscard]] std::set<int> heads_for_tools_locked(const std::set<int>& tools) const;

    /// Read @p filename's tools and warn, once per head, about each head the
    /// print uses that its feeder reports not loaded and the firmware will not
    /// load itself (not in extruders_used), while print @p print still runs.
    /// Silent when the tools cannot be read.
    void warn_unloaded_used_heads(const std::string& filename, uint32_t print);

    /// On a channel whose firmware states no identity at all, drop the brand
    /// and product line the lane only remembers. Caller holds mutex_.
    void retract_unbacked_identity_locked(int slot_index, SlotInfo& slot);

    /// On a channel whose tag reader names no tag, drop the lane's vendor-cache
    /// record and take what it stated off @p slot, falling back to what
    /// print_task_config last said. Returns whether a record was dropped.
    /// Caller holds mutex_.
    bool retract_tag_reading_locked(int slot_index, SlotInfo& slot);

    /// Last filament_feed frame's raw per-channel fields (channel_state,
    /// channel_error, filament_detected, module_exist, disable_auto), written
    /// by handle_status before classification. Each write replaces the
    /// whole entry, defaulting any feeder field that frame omitted;
    /// sensor_enabled arrives from the motion-sensor objects instead and is
    /// carried across a feeder write. Read by channel_snapshot().
    std::array<ChannelSnapshot, kNumTools> channel_snapshots_{};

    /// Per channel: the last in-progress channel_state was a preload_*. A
    /// preload ends at preload_finish; any other op merely rests there (an
    /// unload heats with the channel still at preload_finish), so only this
    /// lets preload_finish resolve the op. Under mutex_.
    std::array<bool, kNumTools> preload_in_flight_{};

    /// Per channel: the last in-progress channel_state was an unload_*. Lets
    /// preload_finish_ends_unload_locked() tell an unload's end from the rest
    /// state an unload starts from. Under mutex_.
    std::array<bool, kNumTools> unload_in_flight_{};

    /// Layer a configured FilamentSlotOverride for `slot_index` over `slot`,
    /// mutating `slot` in place. Override wins for every non-default field.
    /// Callers must hold mutex_. Called from the tail of handle_status
    /// AFTER firmware data has been populated and BEFORE event emission, so
    /// the very next get_slot_info() reflects the overridden values.

  protected:
    /// Hardware-event detection under the insert rule
    /// (prestonbrown/helixscreen#1710): each RFID reading becomes a tracker
    /// fingerprint built from exactly the fields the rule judges - the UID
    /// when the read produced one, else the tag's material and colour, else
    /// "-" for a finished read that found no tag. A fingerprint change runs
    /// classify_insert() against the reading the previous fingerprint named,
    /// and only a different-spool verdict clears the stored override. Same
    /// spool or too little read to say: the record stands (there is no insert
    /// edge on this backend to hang the "same spool?" notice on).
    ///
    /// First observation for a slot establishes the baseline and NEVER fires
    /// a clear. Must be called BEFORE apply_resolved_lane so the clear's field
    /// reset isn't masked by a stale declaration.
    ///
    /// Snapmaker registers no expect() value with rfid_tracker_: CARD_UID is
    /// a hardware identifier the user cannot set via the UI, so user edits
    /// don't race the baseline. (CFS shares the tracker but DOES need that
    /// guard - it writes color_value back to the box, which is half of its
    /// fingerprint.)
    ///
    /// Protected: AmsBackendMultiAce feeds its bays' engine identity through
    /// the same rule and the same tracker.
    void check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                    const helix::ams::SpoolEvidence& observed);

    /// Record that a load on @p head ended unconfirmed. Only during a print
    /// (printing or paused); outside one there is no Resume to hold.
    /// Caller holds mutex_. Protected: AmsBackendMultiAce records the
    /// engine's own load_failed through it.
    void mark_load_unconfirmed_locked(int head, std::string_view why);

    /// Whether Resume may load @p head itself with AUTO_FEEDING before RESUME,
    /// when the feeder has not confirmed it. False refuses Resume and leaves
    /// the load to the user. Caller holds mutex_.
    [[nodiscard]] virtual bool resume_may_refeed_locked(int head) const {
        (void)head;
        return true;
    }

  private:
    /// Brand / total_weight_g are kept: firmware populates them from the RFID
    /// tag. spool_name goes, and each clear restates the tag's product line.
    /// The tag has no colour-name field, so color_name is override-exclusive
    /// and goes.
    void clear_override_fields(SlotInfo& slot) const override;

    /// Whether the connected firmware ships the AUTO_FEEDING_BATCH macro,
    /// cached from the discovery set_discovery() handed over before start().
    /// Selects the script shape do_filament_batch() builds. All access under
    /// mutex_.
    bool use_batch_macro_ = false;

    /// The status-object key the batch macro publishes under ("gcode_macro "
    /// + the config-case macro name; empty when the firmware lacks the
    /// macro). The subscription and the doing-parse must agree on this
    /// spelling: Klipper preserves the config's case in object keys. All
    /// access under mutex_.
    std::string batch_macro_object_;

    /// The firmware ships the multi-toolhead offset calibration macros
    /// (snapmaker::nozzle_offset::supported), cached by set_discovery(). All
    /// access under mutex_.
    bool nozzle_offset_supported_ = false;

    /// The batch do_filament_batch() dispatched, verified head-by-head in
    /// handle_status's channel_state parse. All access under mutex_.
    BatchPlan batch_;

    /// Source of BatchPlan::dispatch_id; monotonic per backend. Under mutex_.
    uint64_t next_batch_dispatch_id_ = 1;

    /// The shared lane_data namespace this backend co-authors. request_resync()
    /// re-reads it only where firmware states no identity of its own.
    helix::ams::FilamentSlotOverrideStore* lane_record_store() override {
        return override_store_.get();
    }

    // Per-slot last-observed RFID CARD_UID. Shared with the other
    // RFID-fingerprint backend (CFS). Snapmaker never calls expect() — nothing
    // here writes a UID back to firmware, so every change is external. All
    // access under mutex_.
    helix::ams::SlotFingerprintTracker rfid_tracker_;

    // The base's own_write_echoes_ stays unarmed on this backend: a user edit
    // reaches firmware only through SET_PRINT_FILAMENT_CONFIG, which writes
    // print_task_config, and that struct files no lane observation. Nothing
    // this backend sends comes back through filament_detect.info.
};

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
