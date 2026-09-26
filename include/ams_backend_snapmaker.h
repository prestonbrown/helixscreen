// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_SNAPMAKER

#include "ams_subscription_backend.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_binding.h"
#include "lane_echo.h"
#include "lane_observation.h"
#include "snapmaker_print_preferences.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {

class SnapmakerTestAccess;
class SnapmakerRealtimeTestAccess;
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

/// Per-extruder tool state from Snapmaker custom Klipper fields
struct ExtruderToolState {
    std::string state;                                ///< e.g., "PARKED", "ACTIVE", "ACTIVATING"
    bool park_pin = false;                            ///< Tool is in park position
    bool active_pin = false;                          ///< Tool is in active position
    bool activating_move = false;                     ///< Tool change move in progress
    std::array<float, 3> extruder_offset = {0, 0, 0}; ///< XYZ offset
    int switch_count = 0;                             ///< Total tool changes for this extruder
    int retry_count = 0;                              ///< Tool change retries
    int error_count = 0;                              ///< Tool change errors
};

/// RFID tag data parsed from filament_detect info
struct SnapmakerRfidInfo {
    std::string main_type;         ///< e.g., "PLA", "PETG"
    std::string sub_type;          ///< e.g., "SnapSpeed", "Basic"
    std::string manufacturer;      ///< e.g., "Polymaker"
    std::string vendor;            ///< e.g., "Snapmaker"
    uint32_t color_rgb = 0x808080; ///< RGB color (ARGB masked to 0x00FFFFFF)
    int hotend_min_temp = 0;
    int hotend_max_temp = 0;
    int bed_temp = 0;
    int weight_g = 0; ///< Spool weight in grams
    /// Canonical string form of CARD_UID (e.g. "144,32,196,2"). Empty when no
    /// tag is present, the RFID reader is disabled, or the field is missing.
    /// Used by the override system as the hardware-event signal: a change
    /// means the physical spool was swapped.
    std::string uid;
};

class AmsBackendSnapmaker : public AmsSubscriptionBackend {
  public:
    AmsBackendSnapmaker(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    ~AmsBackendSnapmaker() override;

    /// The resync files stored records through this backend's echo guard, the
    /// same one its status responses consult.
    [[nodiscard]] helix::ams::OwnWriteEchoes* own_write_echoes() override {
        return &own_write_echoes_;
    }

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
        return helix::FirmwareRouting::fixed_heads(NUM_TOOLS, 0);
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
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Operation step bar. The U1 firmware reports a granular channel_state that
    // classify_channel_state maps to a per-direction step index published via the
    // ams_operation_phase subject, so the step model and its driving index live in
    // the backend (the sidebar renders generically). LOAD is a 5-step model
    // (Home -> Select -> Heat -> Feed filament -> Purge); UNLOAD is 4 steps
    // (Home -> Select -> Heat -> Retract). The Heat step shows a live nozzle temp.
    [[nodiscard]] OperationStepModel get_operation_step_model(StepOperationType op) const override;
    [[nodiscard]] lv_subject_t* get_operation_step_index_subject(StepOperationType op) override;

    /// Snapmaker U1 has 4 independent extruders (extruder, extruder1, extruder2,
    /// extruder3), one per tool. Tool N sources slot N directly — identity mapping.
    [[nodiscard]] std::optional<int> slot_for_extruder(int extruder_idx) const override {
        if (extruder_idx < 0 || extruder_idx >= static_cast<int>(get_system_info().total_slots)) {
            return std::nullopt;
        }
        return extruder_idx;
    }

    // Path visualization (PARALLEL topology — each tool is independent)
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::PARALLEL;
    }

    // The U1 has no shared tray or housing: spools mount on the left and right
    // of the machine and feed through bowdens into a lane-assist motor unit on
    // each side. The detail view's tray graphic draws a container that is not
    // there.
    [[nodiscard]] bool has_physical_tray() const override {
        return false;
    }

    // needs_unload_before_load() is answered by the base class: every lane here
    // is PARALLEL, so slot_has_independent_path() is true for all of them and the
    // serial rule never applies. See AmsBackend for why, including the `T{n}`
    // load this backend used to dispatch.
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // Per-slot "filament is loaded to THIS tool's toolhead". Returns the
    // channel_state latch (loaded_at_toolhead_), driven by filament_feed
    // channel_state transitions — true between load_finish and the next
    // unload_finish/wait_insert/preload_finish. The per-tool motion sensor
    // (e{N}_filament) is NOT used here: on current firmware it fails to drop to
    // false after an unload, so it can't answer "is this lane loaded".
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot_index) const override;

    // Per-tool LOADED status. The U1 has 4 independent toolheads, so each slot's
    // loaded state is tracked separately rather than derived from a single
    // current_slot. Drives the active-lane highlight per tool.
    [[nodiscard]] bool slot_is_actively_loaded(int slot_index) const override;

    /// Raw per-channel feeder fields as the firmware last reported them.
    /// Eligibility answers from these; presence alone cannot distinguish a
    /// lane holding filament from a head that is loaded.
    struct ChannelSnapshot {
        std::string state;       ///< channel_state, e.g. "load_finish"
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
    AmsError do_change_tool(int tool_number) override;

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
    // The base PARALLEL gate offers Unload for any tool with filament in its
    // buffer (is_present()). On the U1 that keeps offering Unload after a tool
    // is already unloaded — the firmware retracts the filament to the buffer
    // (channel_state preload_finish/unload_finish) but filament_exist stays
    // true, so the slot remains AVAILABLE. Override to additionally require the
    // channel_state load latch (loaded_at_toolhead_), which is true only while
    // filament is loaded at the toolhead (between load_finish and the next
    // unload_finish). The motion sensor was tried first but fails to clear after
    // an unload on current firmware; channel_state is the authoritative signal
    // (u1_channel_state_reference.md). Still offers Unload for every toolhead
    // physically loaded (active or parked), preserving the per-tool unload fix.
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

    // Recovery (not supported)
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // Resume preparation. Snapmaker latches a motion-sensor runout exception
    // in Klipper that RESUME alone can't clear when the printer's
    // extruders_used flags are all false. When sensor_filament_present_ is
    // false for the active tool, run a single gcode chain: disable the runout
    // sensor, heat to the slot's recorded nozzle min temp (fallback 200°C),
    // extrude 30 mm to push past the encoder, re-enable the sensor. Otherwise
    // invoke the callback immediately so the caller dispatches RESUME without
    // delay. on_ready is always called on the main thread.
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

    // Snapmaker U1's Resume runs AUTO_FEEDING (loads filament to the nozzle)
    // before RESUME, so Resume alone recovers a runout. The runout dialog uses
    // this to present Resume as primary and demote manual Load/Unload/Purge.
    [[nodiscard]] bool recovers_filament_on_resume() const override {
        return true;
    }

    // The U1 drives load/unload entirely on its own, so an idle lane going empty
    // (a hand-pull, or a lane left unloaded) needs no operator action and the
    // idle runout-guidance modal is just noise. Mid-print runout is a separate
    // path and is unaffected.
    [[nodiscard]] bool should_suppress_idle_runout_modal() const override {
        return true;
    }

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
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    void persist_external_identity_impl(int slot_index,
                                        const helix::ams::Observation& spoolman) override;
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

    // Snapmaker U1's firmware errors if SET_PRINT_USED_EXTRUDERS /
    // SET_PRINT_EXTRUDER_MAP arrive mid-print, so the config must be sent before
    // PRINT_START. Always-on (even with no remap) to suppress a spurious-feed
    // runout — the slicer auto-feeds heads the print doesn't use → empty head →
    // runout cancel.
    [[nodiscard]] bool requires_preprint_send() const override {
        return true;
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

    /// The U1's four independent feeders can be driven as one batch: the
    /// firmware sequences per-extruder AUTO_FEEDING itself. Gates the batch
    /// Load/Unload affordance in the UI.
    [[nodiscard]] bool supports_batch_filament_ops() const override {
        return true;
    }

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
        /// Progress-line words, translated at dispatch time (main thread): the
        /// cursor-advance parse that renders them runs on the WebSocket
        /// thread, which must not call lv_tr into LVGL's pack list.
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
    /// Returns a copy under mutex_: the member is written on the WebSocket
    /// thread, so a reference would hand a UI-thread caller a torn read.
    [[nodiscard]] snapmaker::PrintPreferences print_preferences() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return print_preferences_;
    }

    /// The Print Behaviour section shown on the AMS device-operations
    /// overlay. Absent until the firmware has reported a preference, so the
    /// overlay never offers a setting whose state it does not know.
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;

    /// One DeviceAction per reported preference: toggles, a sensitivity
    /// dropdown, and one end-unload toggle per reported toolhead.
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;

    /// Sends the SET_PRINT_PREFERENCES line build_preference_gcode() maps the
    /// action id to. Unknown ids are reported as not supported.
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

    /// The command one action produces, or empty when the id is not ours.
    /// Separated from execute_device_action so the mapping is testable without
    /// a Moonraker client.
    [[nodiscard]] std::string build_preference_gcode(const std::string& action_id,
                                                     const std::any& value) const;

    // Static parsers (public for testing)
    static ExtruderToolState parse_extruder_state(const nlohmann::json& json);
    static SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json);

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS Snapmaker]";
    }
    SlotInfo* cached_slot_locked(int slot_index) override;

  private:
    friend class SnapmakerTestAccess;
    friend class SnapmakerRealtimeTestAccess;
    friend class RunoutScopeTestAccess;

    static constexpr int NUM_TOOLS = 4;

    /// RPC timeout budget for ONE batch feed op. AUTO_FEEDING heats from cold +
    /// feeds + flushes; measured ~86s live (see prepare_for_resume), so 150s is
    /// the headroom the resume path already uses. A batch scales this per op.
    static constexpr uint32_t BATCH_FEED_OP_TIMEOUT_MS = 150000;

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
    /// handle_status_update under mutex_; read by last_print_tool_mapping().
    std::vector<int> last_task_extruder_map_;

    /// What the firmware last reported for its stored print preferences.
    /// Merged across frames: Moonraker sends deltas, so a frame that omits a
    /// setting is silent about it rather than reporting it off.
    ///
    /// Like every other print_task_config field, these are a write surface, not
    /// a sensor — held as told, never filed as a lane observation.
    ///
    /// Written only from handle_status_update under mutex_; read by
    /// print_preferences().
    snapmaker::PrintPreferences print_preferences_;

    /// Per-extruder cached state
    std::array<ExtruderToolState, NUM_TOOLS> extruder_states_;

    /// Per-slot filament_motion_sensor "filament_detected" state. True means
    /// filament is currently being fed to the toolhead; false means runout has
    /// fired. Tracks `filament_motion_sensor e0..e3_filament` (and the
    /// matching `filament_switch_sensor` form as a fallback). Defaults to true
    /// so a slot without a configured sensor doesn't render as "runout" — the
    /// flag flips only when Klipper sends an explicit `filament_detected:false`
    /// for that sensor, which only happens on configured runout sensors. Read
    /// by get_filament_segment() / get_slot_filament_segment() to break the
    /// spool→toolhead line when the active tool has run out.
    std::array<bool, NUM_TOOLS> sensor_filament_present_{{true, true, true, true}};

    /// Per-slot port/buffer sensor state — the filament_feed left/right
    /// .extruder{N}.filament_detected flag. Reads the physical-presence
    /// sensor at the spool/buffer side, NOT the encoder-based motion sensor.
    /// Used together with sensor_filament_present_ to differentiate real
    /// runouts (both false) from stale motion sensor false positives
    /// (motion=false, port=true). Defaults to false so a slot we've never
    /// seen filament_feed data for doesn't generate a "stuck sensor" false
    /// positive of its own — the auto-recover path requires the port sensor
    /// to explicitly report present.
    std::array<bool, NUM_TOOLS> port_sensor_filament_present_{{false, false, false, false}};

    /// Last value published to AmsState::set_active_tool_port_present for the
    /// active tool (#991). Tracks the active-tool port flag so we only push to
    /// the UI subject on an actual change. -1 = nothing published yet. Written
    /// only from handle_status_update (the single WS-thread writer).
    int last_published_port_present_ = -1;

    /// Per-slot "filament is loaded to THIS tool's toolhead" latch, driven
    /// purely from filament_feed.channel_state transitions (NOT the motion
    /// sensor). The per-tool motion sensor (e{N}_filament) does not reliably
    /// drop to false after an unload on current firmware — a freshly-unloaded
    /// lane still reads filament_detected=true (tip retracted from the melt
    /// zone only, filament parked past the toolhead sensor). channel_state is
    /// the authoritative load signal (verified live on a U1, firmware
    /// 20260608): load_finish means loaded, unload_finish / wait_insert /
    /// preload_finish mean not-loaded. Mirrors the firmware's own persisted
    /// config['load_finish'] flag. Set true on load_finish; cleared on
    /// unload_finish / wait_insert / preload_finish; left unchanged on every
    /// transient / in-progress / fail state. Defaults false: a lane we've never
    /// seen a channel_state for is treated as not-loaded. Read by
    /// slot_has_filament_at_toolhead() and can_unload_from_toolhead(). The
    /// motion sensor (sensor_filament_present_) still owns mid-print runout —
    /// a different question ("did the ACTIVE lane run out during extrusion").
    std::array<bool, NUM_TOOLS> loaded_at_toolhead_{{false, false, false, false}};

    /// Per-slot "filament_feed has reported this lane" — set the first time a
    /// frame carries a boolean filament_detected or a channel_state for the
    /// lane, and never cleared. Until it is set, the convergence-point presence
    /// ingest stays silent: the port/latch arrays rest on their defaults
    /// ("no reading yet", not "no filament"), and declaring those defaults
    /// would empty a lane whose only signal so far is the toolhead pin state.
    std::array<bool, NUM_TOOLS> feed_presence_seen_{{false, false, false, false}};

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
    /// Written only from handle_status_update (the single WS-thread writer).
    std::array<int, NUM_TOOLS> pending_insert_passes_{{0, 0, 0, 0}};

    /// Last filament_feed frame's raw per-channel fields (channel_state,
    /// channel_error, filament_detected, module_exist, disable_auto), written
    /// by handle_status_update before classification. Each write replaces the
    /// whole entry, defaulting any feeder field that frame omitted;
    /// sensor_enabled arrives from the motion-sensor objects instead and is
    /// carried across a feeder write. Read by channel_snapshot().
    std::array<ChannelSnapshot, NUM_TOOLS> channel_snapshots_{};

    /// Layer a configured FilamentSlotOverride for `slot_index` over `slot`,
    /// mutating `slot` in place. Override wins for every non-default field.
    /// Callers must hold mutex_. Called from the tail of handle_status_update
    /// AFTER firmware data has been populated and BEFORE event emission, so
    /// the very next get_slot_info() reflects the overridden values.

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
    void check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                    const helix::ams::SpoolEvidence& observed);

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets strictly override-exclusive fields on
    // the provided SlotInfo (spool_name, spoolman_*, remaining_weight_g), and
    // fires clear_async. Brand / color_name / total_weight_g are preserved —
    // firmware populates them from the RFID tag.
    void clear_override_locked(int slot_index, SlotInfo& slot);

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

    /// The batch do_filament_batch() dispatched, verified head-by-head in
    /// handle_status_update's channel_state parse. All access under mutex_.
    BatchPlan batch_;

    /// Source of BatchPlan::dispatch_id; monotonic per backend. Under mutex_.
    uint64_t next_batch_dispatch_id_ = 1;

    // Persistent per-slot overrides. Writers (on_started bulk load,
    // apply_user_edit, check_hardware_event_clear) all hold mutex_.
    // Reads happen inside the parse path's lane_data mirror and the clear
    // helpers, all of which also hold mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

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

    /// What the user declared in their last edit of a channel, out of the
    /// fields apply_user_edit actually sent to /printer/filament_detect/set.
    ///
    /// The write target is filament_detect.info, the same object
    /// parse_rfid_info reads, and VENDOR / MAIN_TYPE / SUB_TYPE are spelled
    /// identically on both sides, so firmware reports a user's declaration
    /// back through the RFID path where it is indistinguishable by value from
    /// a tag reading.
    ///
    /// The boundary is the tag's CARD_UID, taken from rfid_tracker_: a
    /// hardware identifier the edit UI cannot set, so a change is
    /// unambiguously a different physical spool. It is the same signal
    /// check_hardware_event_clear() trusts to delete the user's override, so
    /// the suppression and the clear end together and neither can expose what
    /// the other still holds. All access under mutex_.
    helix::ams::OwnWriteEchoes own_write_echoes_;
};

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
