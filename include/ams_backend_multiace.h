// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_SNAPMAKER

#include "ams_backend_snapmaker.h"
#include "multiace_status.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/**
 * @file ams_backend_multiace.h
 * @brief A Snapmaker U1 with multiACE: ACE Pro units upstream of its heads.
 *
 * The U1 stays the printer. Its four heads, `T<n>`, channel_state, pre-print
 * remap, preferences and resume all stay AmsBackendSnapmaker's, on global
 * slots 0..3. Each ACE unit multiACE reports becomes one more unit whose four
 * bays sit at multiace::bay_global(ace, bay); a bay feeds the head its mode
 * routes it to. The engine is driven through its gcode contract only
 * (docs/devel/FILAMENT_BACKEND_MULTIACE.md).
 *
 * Until a frame shows api_version 1 and a multi or head mode, this backend
 * answers exactly as the base does: no ACE units, the stock feed path.
 */
class AmsBackendMultiAce : public AmsBackendSnapmaker {
  public:
    AmsBackendMultiAce(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /// Unit 0 is the heads (PARALLEL). An ACE unit is a HUB in head mode, its
    /// four bays merging into the one head wired to it, and PARALLEL in multi
    /// mode, bay s feeding head s.
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;

    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;

    /// The bay routed to an ACE-fed head is that head lane's spool: the head
    /// shows the bay's filament, not a spool of its own. nullopt everywhere
    /// else, bays included (a bay is where its spool lives).
    [[nodiscard]] std::optional<int> upstream_slot(int slot_index) const override;

    /// Bays number the spools 1-based in global order; a routed head wears
    /// its bay's number so the two lanes count one spool, and the numbers on
    /// the lanes that hold their own spools stay consecutive.
    [[nodiscard]] int spool_display_number(int slot_index) const override;

    /// A bay into a head holding another bay is not a swap for the screen to
    /// sequence: do_load_filament() sends the head's unload ahead of the load.
    [[nodiscard]] bool needs_unload_before_load(const AmsSystemInfo& info,
                                                int target_slot) const override;

    /// A batch load is refused into ACE-fed heads (the engine sequences its
    /// own swaps), so the picker must not offer them as eligible.
    [[nodiscard]] FilamentOpEligibility slot_op_eligibility(int slot_index,
                                                            bool load) const override;

    /// A head slot feeds itself; a bay feeds the head its mode routes it to,
    /// and an unrouted bay feeds none.
    [[nodiscard]] std::optional<int> head_for_slot(int slot) const override;
    /// An ACE-fed head's source is its one routed bay; with several bays wired
    /// to it, the bay loaded into it now, else none. A head the ACE does not
    /// feed is its own source.
    [[nodiscard]] std::optional<int> head_source_slot(int head,
                                                      const AmsSystemInfo& info) const override;

    /// Also drops a bay load waiting behind its head's unload: the restart
    /// ended that unload, so the load's second step must not follow it.
    void on_klipper_restarted() override;

    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot_index) const override;
    [[nodiscard]] bool slot_is_actively_loaded(int slot_index) const override;

    /// Unit 0 has no dryer; every ACE unit has the ACE Pro's.
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;
    AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1,
                           int unit = 0) override;
    [[nodiscard]] std::vector<DryingPreset> get_drying_presets() const override;

    /// multiACE raises its own faults coded (id 525, a 200-band code, the head
    /// as index); they are filed as a filament error on that head. Everything
    /// else is the base's.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line, const helix::ClassifyContext& ctx) const override;

    /// A bay edit lands on the bay's own SlotInfo and the shared override
    /// store, keyed by the global slot index; nothing is written to the engine,
    /// whose own spool table never sees the edit (phase 4). A head keeps the
    /// base's path, which also writes the head's firmware record.
    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    /// A head's clear is Snapmaker's; a bay has no head firmware record to
    /// drop and no tag product line to restate.
    void after_override_cleared_locked(int slot_index, SlotInfo& slot) override;

  protected:
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    /// Only the four heads are tools; the bay indices beyond them are not.
    AmsError do_change_tool(int tool_number) override;
    AmsError do_park_active_tool() override;
    AmsError do_filament_batch(const std::vector<int>& slots, bool load) override;

    [[nodiscard]] bool head_is_upstream_fed_locked(int head) const override;
    [[nodiscard]] bool preload_finish_ends_unload_locked(int head) const override;
    /// While the engine is active, Resume never sends the stock AUTO_FEEDING load.
    [[nodiscard]] bool resume_may_refeed_locked(int head) const override;
    void apply_extension_locked(const nlohmann::json& status, bool& changed) override;

    /// The base frame parse, then the deferred bay load it may release.
    void handle_status(const nlohmann::json& status) override;

    const char* backend_log_tag() const override {
        return "[AMS multiACE]";
    }

  private:
    static constexpr int kHeads = multiace::kHeadCount;

    /// The engine contract is in force: api_version 1 in multi or head mode.
    /// Caller holds mutex_.
    [[nodiscard]] bool active_locked() const;

    /// Head @p head is fed by an ACE. Caller holds mutex_.
    [[nodiscard]] bool ace_fed_locked(int head) const;

    /// The bay routed to @p head, when one is and its load did not fail.
    /// Caller holds mutex_.
    [[nodiscard]] std::optional<std::pair<int, int>> routed_bay_locked(int head) const;

    /// The head bay @p global feeds, when @p global names a bay of a unit
    /// this backend shows. Caller holds mutex_.
    [[nodiscard]] std::optional<int> head_for_global_locked(int global) const;

    /// Position in units_ of the unit whose idx is @p ace, or -1.
    [[nodiscard]] int unit_position_locked(int ace) const;

    /// The ACE unit an AmsUnit index names, or nullptr for unit 0 and indices
    /// beyond the units shown. Caller holds mutex_.
    [[nodiscard]] const multiace::Unit* unit_for_index_locked(int unit_index) const;

    /// Replaces every unit after unit 0 from the held engine state, and
    /// total_slots with them. Caller holds mutex_.
    void rebuild_units_locked();

    /// A bay load waiting on its head's unload. ACE_LOAD_HEAD feeds nothing
    /// into a head that still holds filament, so the load is sent only once a
    /// frame shows the head empty: head_source[head] null and the load latch
    /// clear. An OK from the unload is not that proof; it starts a grace
    /// window, and a head still occupied when the window closes drops the load.
    struct PendingBayLoad {
        int head = -1;
        std::pair<int, int> bay{-1, -1};
        uint64_t id = 0;
        bool unload_answered = false;
        std::chrono::steady_clock::time_point unload_answered_at{};
    };

    /// How long after the unload's answer the frames have to show the head
    /// empty. Moonraker batches status, so the frame can trail the answer.
    static constexpr std::chrono::seconds kPendingLoadGrace{15};

    /// Drops a pending bay load: another filament op owns the heads now.
    void cancel_pending_load(const char* why);

    /// Sends the pending bay load when this frame shows its head empty, and
    /// drops it, raising kEventError, once its grace window has closed, the
    /// engine stops driving the units, or a print holds the heads. Main
    /// thread, mutex_ not held.
    void advance_pending_load();

    /// Why head @p head cannot take a lane load now, or nullopt: filament
    /// already at the head, or for a head the ACE does not feed, the U1's own
    /// feeder preconditions. Takes mutex_ itself.
    [[nodiscard]] std::optional<AmsError> head_load_refusal(int head, bool ace_head) const;

    // Engine state, merged across delta frames. All under mutex_.
    std::optional<int> api_version_;
    multiace::Mode mode_ = multiace::Mode::UNKNOWN;
    std::array<int, kHeads> head_ace_{0, 1, 2, 3};
    std::array<bool, kHeads> head_feeder_{};
    std::array<bool, kHeads> head_manual_{};
    std::array<std::optional<multiace::HeadSource>, kHeads> head_source_{};
    std::vector<multiace::Unit> units_;
    bool swap_in_progress_ = false;
    bool unload_all_active_ = false;
    std::string swap_phase_;
    multiace::LastSwap last_swap_;
    int64_t event_seq_ = -1;
    /// api_version mismatch already logged, so a stream of frames logs once.
    bool api_mismatch_logged_ = false;

    /// Under mutex_.
    std::optional<PendingBayLoad> pending_load_;
    uint64_t next_pending_id_ = 1;

    /// head_source load-failure notices staged under mutex_ by
    /// apply_extension_locked() and emitted by handle_status() outside it.
    /// Under mutex_.
    std::vector<std::string> pending_load_failed_errors_;

  protected:
    /// kPendingLoadGrace, held per backend so a test can close the window.
    /// Under mutex_.
    std::chrono::milliseconds pending_load_grace_{kPendingLoadGrace};
};

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
