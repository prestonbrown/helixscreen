// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_SNAPMAKER

#include "ams_subscription_backend.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    /// Whether CARD_UID names a real card: a nonzero integer, or an array with a
    /// nonzero byte. The firmware reports a scalar 0 or an all-zero array for no
    /// card (reader off, untagged spool, an endpoint write). nullopt when the
    /// entry carries no readable CARD_UID.
    std::optional<bool> has_card;
};

class AmsBackendSnapmaker : public AmsSubscriptionBackend {
  public:
    AmsBackendSnapmaker(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    ~AmsBackendSnapmaker() override;

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::SNAPMAKER;
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

  public:
    /// AUTO_FEEDING ... UNLOAD=1 runs the firmware's unload state machine, whose
    /// unload_heating stage heats. The bare INNER_FILAMENT_UNLOAD that
    /// do_unload_filament() falls back to with no slot resolved does not.
    [[nodiscard]] bool unload_heats_hotend(int slot_index) const override;

    // FEED_AUTO homes and sets its own nozzle target from the lane's material:
    // a load reports load_homing then load_heating, an unload unload_homing then
    // unload_heating. A G28 or UI preheat in front of it is a second wait.
    [[nodiscard]] bool delegates_homing_to_printer() const override {
        return true;
    }
    [[nodiscard]] bool supports_auto_heat_on_load() const override {
        return true;
    }

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
    /// Also forgets each channel's last frame and in-flight op, because the
    /// backend outlives the restart: the subscription's first full frame is
    /// then a fresh start rather than a change from firmware that is gone.
    void on_klipper_restarted() override;

    // Resume preparation. A Terminal pause answers RESUME_REQUIRES_RESTART at
    // once. After a runout (sensor_filament_present_ false for the active
    // tool) AUTO_FEEDING refeeds it before the caller dispatches RESUME.
    // Otherwise every used head whose print_task_config record names no
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
        return RemapStrategy::SnapmakerNative;
    }

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

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
    //             Tools absent from `remap` use default_head(t) = (t>=0 && t<=3) ? t : 0.
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

    /// Which lines of a filament record write to send.
    enum class RecordLines { Both, Identity, Colour };

    /// The SET_PRINT_FILAMENT_CONFIG script that writes @p slot's identity into
    /// print_task_config for physical head @p head: one line for vendor, type
    /// and subtype, one for colour (@p lines picks either alone). VENDOR is
    /// "Snapmaker" for a Snapmaker spool and "Generic" otherwise;
    /// FILAMENT_SUBTYPE is the spool's product line when it names one, else
    /// "Basic". Empty when the slot names no material or one
    /// IMoonrakerAPI::is_safe_material_param() refuses; values with a space are
    /// quoted by IMoonrakerAPI::gcode_param_value(). @p force appends FORCE=1
    /// to every line, which a head the firmware calls official requires.
    [[nodiscard]] static std::string
    build_filament_config_gcode(int head, const SlotInfo& slot, bool force = false,
                                RecordLines lines = RecordLines::Both);

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

    // Static parsers (public for testing)
    static ExtruderToolState parse_extruder_state(const nlohmann::json& json);
    static SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json);

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS Snapmaker]";
    }

  private:
    friend class ::SnapmakerTestAccess;
    friend class ::SnapmakerRealtimeTestAccess;
    friend class ::RunoutScopeTestAccess;

    static constexpr int NUM_TOOLS = 4;

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

    /// print_task_config.filament_type per head, as the firmware spells it
    /// ("NONE" or "" when nothing is set). SlotInfo::material cannot answer
    /// this: the user's own override is laid over it. Unset until the first
    /// frame carrying it.
    std::array<std::optional<std::string>, NUM_TOOLS> task_type_{};

    /// print_task_config.filament_official per head. Unset until the first
    /// frame carrying it.
    std::array<std::optional<bool>, NUM_TOOLS> task_official_{};

    /// Whether filament_detect.info last showed a real card on each channel
    /// (SnapmakerRfidInfo::has_card). Unset until the first entry for it.
    std::array<std::optional<bool>, NUM_TOOLS> tag_has_card_{};

    /// print_task_config.filament_vendor per head, as the firmware spells it.
    /// Unset until the first frame carrying it.
    std::array<std::optional<std::string>, NUM_TOOLS> task_vendor_{};

    /// print_task_config.filament_color_rgba per head, decoded. Unset until the
    /// first frame carrying a colour that decodes.
    std::array<std::optional<uint32_t>, NUM_TOOLS> task_color_{};

    /// print_task_config.filament_sub_type per head, as the firmware spells it.
    /// Unset until the first frame carrying it.
    std::array<std::optional<std::string>, NUM_TOOLS> task_subtype_{};

    /// Whether each channel's filament_detect entry names a tag: MAIN_TYPE or
    /// SUB_TYPE other than NONE. Unset until the first entry for the channel.
    std::array<std::optional<bool>, NUM_TOOLS> tag_names_identity_{};

    /// Per head: an official-flag repair has been sent this session. Under
    /// mutex_.
    std::array<bool, NUM_TOOLS> official_repair_sent_{};

    /// Per channel: the tag reader has shown a real card this session. Under
    /// mutex_.
    std::array<bool, NUM_TOOLS> card_seen_{};

    /// print_stats.state from this backend's own status frames; unset until a
    /// frame carries it. Under mutex_.
    std::optional<std::string> print_state_;

    /// A user edit's record write held by record_writes_held_locked().
    struct PendingRecordWrite {
        SlotInfo info;
        bool material_declared = false;
    };

    /// Per head, the latest held record write; dropped with the head's
    /// override and at on_started(). Under mutex_.
    std::array<std::optional<PendingRecordWrite>, NUM_TOOLS> pending_record_writes_{};

    /// What the last tag reading on each channel wrote onto SlotInfo (an empty
    /// brand or spool_name, or no colour: the tag stated none). Unset until a
    /// tag is read, and again once retract_tag_reading_locked() takes it back.
    struct TagReading {
        std::string material;
        std::string brand;
        std::optional<uint32_t> color_rgb;
        std::string spool_name;
        int nozzle_temp_min = 0;
        int nozzle_temp_max = 0;
        int bed_temp = 0;
        float total_weight_g = 0;
    };
    std::array<std::optional<TagReading>, NUM_TOOLS> tag_reading_{};

    /// On a channel whose tag reader names no tag, take what the last tag
    /// reading wrote off @p slot, falling back to what print_task_config last
    /// said. A slot the user declared or bound to a spool keeps its fields.
    /// Returns whether there was a reading to take back. Caller holds mutex_.
    bool retract_tag_reading_locked(int slot_index, SlotInfo& slot);

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

    /// Whether a user edit's record write must wait: this backend's own
    /// print_stats.state reads printing, or, before its frames have named one,
    /// the app's print job state reads PRINTING or is not initialized. Caller
    /// holds mutex_.
    [[nodiscard]] bool record_writes_held_locked() const;

    /// Forget @p head's held record write, if it has one. Caller holds mutex_.
    void drop_pending_record_write_locked(int head);

    /// Whether this backend's own print_stats.state is known and holds no head:
    /// neither printing nor paused. Caller holds mutex_.
    [[nodiscard]] bool heads_free_locked() const;

    /// FORCE=1 rewrites for heads whose record is flagged official while the
    /// tag entry is filled with no card behind it, the channel has shown no
    /// card this session, and the record holds exactly what this backend's
    /// stored edit says. A filament_detect/set write (Extended Firmware) leaves
    /// that flag, and the firmware then refuses plain writes from us and from
    /// the stock screen. Nothing is returned until this backend has read a
    /// print_stats.state that is neither printing nor paused. Marks each head
    /// returned sent. Caller holds mutex_.
    [[nodiscard]] std::vector<std::pair<int, std::string>> official_flag_repairs_locked();

    /// Send official_flag_repairs_locked() output. Main thread, outside mutex_.
    void dispatch_official_flag_repairs(const std::vector<std::pair<int, std::string>>& repairs);

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

    /// prepare_for_resume after a runout on @p slot: AUTO_FEEDING refeeds it
    /// before the caller dispatches RESUME.
    void run_runout_refeed(int slot, ResumeReadyCallback on_ready);

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

    /// Per channel: the last channel_state and channel_action_state a frame
    /// carried. Status frames are deltas, and an op that settles in one frame
    /// reports its outcome only as a channel_action_state change while the
    /// channel rests. Under mutex_.
    std::array<std::string, NUM_TOOLS> held_channel_state_{};
    std::array<std::string, NUM_TOOLS> held_channel_action_state_{};
    /// Per channel: an op outcome channel_action_state reported while the
    /// channel still showed a step under way, applied when it settles. Under
    /// mutex_.
    std::array<std::string, NUM_TOOLS> held_op_outcome_{};

    /// Per channel: the last in-progress channel_state was a preload_*. A
    /// preload ends at preload_finish; any other op merely rests there (an
    /// unload heats with the channel still at preload_finish), so only this
    /// lets preload_finish resolve the op. Under mutex_.
    std::array<bool, NUM_TOOLS> preload_in_flight_{};

    /// Validate slot index is within range
    AmsError validate_slot_index(int slot_index) const;

    /// Layer a configured FilamentSlotOverride for `slot_index` over `slot`,
    /// mutating `slot` in place. Override wins for every non-default field.
    /// Callers must hold mutex_. Called from the tail of handle_status_update
    /// AFTER firmware data has been populated and BEFORE event emission, so
    /// the very next get_slot_info() reflects the overridden values.
    void apply_overrides(SlotInfo& slot, int slot_index);

    /// Hardware-event detection: if the RFID CARD_UID changes between parses,
    /// the user physically swapped the spool. Clears the stored override so
    /// stale brand / spool_name / spoolman_id from the previous user don't
    /// bleed onto the new spool. Empty observed_uid (no tag / unread) is
    /// treated as "no signal" — never updates the baseline and never clears.
    /// First observation for a slot establishes the baseline and NEVER fires
    /// a clear. Must be called BEFORE apply_overrides so the clear's field
    /// reset isn't masked by a stale override layer.
    ///
    /// Unlike the AD5X IFS implementation (which uses color as the event
    /// signal and needs a self-wipe guard in set_slot_info), Snapmaker uses
    /// the RFID UID — a hardware identifier the user cannot set via the UI.
    /// So set_slot_info registers no expected-echo value with rfid_tracker_;
    /// the baseline stays at whatever firmware last reported and user edits
    /// don't race. (CFS shares the tracker but DOES need that guard — it
    /// writes color_value back to the box, which is half of its fingerprint.)
    void check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                    const std::string& observed_uid);

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets strictly override-exclusive fields on
    // the provided SlotInfo (spool_name, spoolman_*, remaining_weight_g), and
    // fires clear_async. Brand / color_name / total_weight_g are preserved —
    // firmware populates them from the RFID tag.
    void clear_override_locked(int slot_index, SlotInfo& slot);

    // Persistent per-slot overrides. Writers (on_started bulk load,
    // set_slot_info persist path, check_hardware_event_clear) all hold mutex_.
    // Reads happen inside apply_overrides which is also under mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Per-slot last-observed RFID CARD_UID. Shared with the other
    // RFID-fingerprint backend (CFS). Snapmaker never calls expect() — nothing
    // here writes a UID back to firmware, so every change is external. All
    // access under mutex_.
    helix::ams::SlotFingerprintTracker rfid_tracker_;
};

#endif // HELIX_HAS_SNAPMAKER
