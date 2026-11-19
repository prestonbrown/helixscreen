// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_action_set.cpp
 * @brief The backend-neutral filament action decision and the backend answers
 *        it is built from.
 */

#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "filament_action_set.h"
#include "filament_op_execute.h"

#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::FilamentOpSupport;
using helix::HeadFilamentOps;
using namespace helix::ui;

namespace {

HeadState ready_head(int slot) {
    HeadState head;
    head.source_slot = slot;
    head.unload_slot = slot;
    head.source_present = true;
    head.at_nozzle = false;
    head.has_filament = false;
    return head;
}

HeadState loaded_head(int slot) {
    HeadState head = ready_head(slot);
    head.at_nozzle = true;
    head.has_filament = true;
    return head;
}

HeadState manual_head(int slot) {
    HeadState head = ready_head(slot);
    head.ops.load = FilamentOpSupport::Manual;
    head.ops.unload = FilamentOpSupport::Manual;
    head.ops.load_reason = "by hand (load)";
    head.ops.unload_reason = "by hand (unload)";
    return head;
}

FilamentHeadView view_of(std::vector<HeadState> heads) {
    FilamentHeadView view;
    view.backend_present = true;
    view.heads = std::move(heads);
    return view;
}

/// Four heads, one per lane, as on a Snapmaker U1.
class FourHeadBackend : public helix::AmsBackendSnapmaker {
  public:
    FourHeadBackend() : helix::AmsBackendSnapmaker(nullptr, nullptr) {}
    helix::AmsSystemInfo get_system_info() const override {
        helix::AmsSystemInfo info;
        info.total_slots = 4;
        return info;
    }
    bool can_unload_from_toolhead(int slot_index) const override {
        return slot_index == 1;
    }
    bool slot_filament_parked_in_toolhead(int slot_index) const override {
        return slot_index == 2;
    }
};

} // namespace

TEST_CASE("filament actions: one automatic head shows Load and Unload", "[filament][actions]") {
    const auto actions = decide_filament_actions(view_of({loaded_head(0)}));
    CHECK(actions.load.visible);
    CHECK(actions.unload.visible);
    CHECK(actions.unload.enabled);
    CHECK_FALSE(actions.unload_opens_picker);
    CHECK_FALSE(actions.mount.visible);
    CHECK_FALSE(actions.park.visible);
}

TEST_CASE("filament actions: no backend offers nothing", "[filament][actions]") {
    FilamentHeadView view = view_of({ready_head(0)});
    view.backend_present = false;
    const auto actions = decide_filament_actions(view);
    CHECK_FALSE(actions.load.visible);
    CHECK_FALSE(actions.unload.visible);
}

TEST_CASE("filament actions: Load needs an assigned slot", "[filament][actions]") {
    HeadState head = ready_head(0);
    head.source_slot.reset();
    const auto actions = decide_filament_actions(view_of({head}));
    CHECK(actions.load.visible);
    CHECK_FALSE(actions.load.enabled);
    CHECK_FALSE(actions.load.reason.empty());

    // A source known to be empty, or already fed, leaves nothing to load and
    // says nothing: the slot grid shows both.
    HeadState empty = ready_head(0);
    empty.source_present = false;
    CHECK_FALSE(decide_filament_actions(view_of({empty})).load.enabled);
    CHECK(decide_filament_actions(view_of({empty})).load.reason.empty());
    CHECK_FALSE(decide_filament_actions(view_of({loaded_head(0)})).load.enabled);

    // A source that cannot say whether it holds filament stays loadable.
    HeadState unknown = ready_head(0);
    unknown.source_present.reset();
    unknown.at_nozzle.reset();
    CHECK(decide_filament_actions(view_of({unknown})).load.enabled);
}

TEST_CASE("filament actions: Unload needs filament in some head", "[filament][actions]") {
    CHECK_FALSE(decide_filament_actions(view_of({ready_head(0), ready_head(1)})).unload.enabled);
    CHECK(decide_filament_actions(view_of({ready_head(0), loaded_head(1)})).unload.enabled);

    // A head nothing senses filament in stays unloadable: the backend refuses
    // an empty one itself.
    HeadState unknown = ready_head(0);
    unknown.has_filament.reset();
    CHECK(decide_filament_actions(view_of({unknown})).unload.enabled);
}

TEST_CASE("filament actions: busy and print gating disable everything without a reason",
          "[filament][actions]") {
    for (int which = 0; which < 2; ++which) {
        FilamentHeadView view = view_of({loaded_head(0), ready_head(1)});
        view.heads_dock = true;
        view.supports_park = true;
        view.mounted_head = 0;
        (which == 0 ? view.system_busy : view.print_blocks_op) = true;
        const auto actions = decide_filament_actions(view);
        CHECK(actions.load.visible);
        CHECK_FALSE(actions.load.enabled);
        CHECK(actions.load.reason.empty());
        CHECK_FALSE(actions.unload.enabled);
        CHECK(actions.unload.reason.empty());
        CHECK(actions.park.visible);
        CHECK_FALSE(actions.park.enabled);
    }
}

TEST_CASE("filament actions: a docking machine with no automatic op falls back to Mount and Park",
          "[filament][actions]") {
    FilamentHeadView view = view_of({manual_head(0), manual_head(1), manual_head(2)});
    view.heads_dock = true;
    view.supports_park = true;
    view.mounted_head = 1;
    const auto actions = decide_filament_actions(view);
    CHECK_FALSE(actions.load.visible);
    CHECK_FALSE(actions.unload.visible);
    CHECK(actions.mount.visible);
    CHECK(actions.mount.enabled);
    CHECK(actions.park.visible);
    CHECK(actions.park.enabled);

    // Mount names the selected tool when there is one.
    view.selected_head = 1;
    const auto on_mounted = decide_filament_actions(view);
    CHECK_FALSE(on_mounted.mount.enabled);
    CHECK_FALSE(on_mounted.mount.reason.empty());
    view.selected_head = 2;
    CHECK(decide_filament_actions(view).mount.enabled);

    // A machine whose heads do not dock has nothing to mount: it keeps
    // Load and Unload, disabled with the reason.
    view.heads_dock = false;
    const auto shared = decide_filament_actions(view);
    CHECK(shared.load.visible);
    CHECK_FALSE(shared.load.enabled);
    CHECK(shared.load.reason == "by hand (load)");
    CHECK_FALSE(shared.mount.visible);
}

TEST_CASE("filament actions: Park is hidden with nothing mounted", "[filament][actions]") {
    FilamentHeadView view = view_of({manual_head(0), manual_head(1)});
    view.heads_dock = true;
    view.supports_park = true;
    view.mounted_head = -1;
    CHECK_FALSE(decide_filament_actions(view).park.visible);

    // A feeding changer shows Park beside Load and Unload.
    FilamentHeadView feeding = view_of({ready_head(0), loaded_head(1)});
    feeding.heads_dock = true;
    feeding.supports_park = true;
    feeding.mounted_head = 1;
    const auto actions = decide_filament_actions(feeding);
    CHECK(actions.load.visible);
    CHECK(actions.park.visible);
    CHECK_FALSE(actions.mount.visible);
    feeding.supports_park = false;
    CHECK_FALSE(decide_filament_actions(feeding).park.visible);
}

TEST_CASE("filament actions: a Manual direction is disabled with its reason",
          "[filament][actions]") {
    HeadState head = loaded_head(0);
    head.ops.unload = FilamentOpSupport::Manual;
    head.ops.unload_reason = "cut it";
    const auto actions = decide_filament_actions(view_of({head}));
    CHECK(actions.load.visible);
    CHECK(actions.unload.visible);
    CHECK_FALSE(actions.unload.enabled);
    CHECK(actions.unload.reason == "cut it");
}

TEST_CASE("filament actions: a guided hand load is offered where a plain Manual load is not",
          "[filament][actions][manual_load]") {
    HeadState plain = manual_head(0);
    HeadState guided = manual_head(0);
    guided.ops.guided_hand_load = true;
    // The user pushes the filament in, so no lane sensor has to see it first.
    guided.source_present = false;

    CHECK_FALSE(decide_filament_actions(view_of({plain})).load.enabled);
    CHECK(decide_filament_actions(view_of({plain})).load.reason == "by hand (load)");
    const auto actions = decide_filament_actions(view_of({guided}));
    CHECK(actions.load.enabled);
    CHECK(actions.load.reason.empty());
    CHECK(decide_slot_actions(view_of({guided}), 0).load.enabled);
    CHECK_FALSE(decide_slot_actions(view_of({plain}), 0).load.enabled);
    CHECK(decide_head_picker_rows(view_of({guided}), HeadPickOp::Load)[0].enabled);
    // Filament already at the nozzle has nothing to load, by hand or not.
    guided.at_nozzle = true;
    CHECK_FALSE(head_can_load(guided));
    // A print gates the hand load like any other.
    FilamentHeadView printing = view_of({manual_head(0)});
    printing.heads[0].ops.guided_hand_load = true;
    printing.print_blocks_op = true;
    CHECK_FALSE(decide_slot_actions(printing, 0).load.enabled);
}

TEST_CASE("filament actions: multi-head unload opens the picker", "[filament][actions]") {
    CHECK(decide_filament_actions(view_of({loaded_head(0), ready_head(1)})).unload_opens_picker);
    CHECK_FALSE(decide_filament_actions(view_of({loaded_head(0)})).unload_opens_picker);
}

TEST_CASE("slot actions: Mount targets the tapped head and refuses the mounted one",
          "[filament][actions]") {
    FilamentHeadView view = view_of({manual_head(0), manual_head(1)});
    view.heads_dock = true;
    view.supports_park = true;
    view.mounted_head = 0;

    const auto parked = decide_slot_actions(view, 1);
    CHECK(parked.load.visible);
    CHECK_FALSE(parked.load.enabled);
    CHECK(parked.load.reason == "by hand (load)");
    CHECK(parked.unload.reason == "by hand (unload)");
    CHECK(parked.mount.visible);
    CHECK(parked.mount.enabled);
    CHECK_FALSE(parked.park.visible);

    const auto mounted = decide_slot_actions(view, 0);
    CHECK(mounted.mount.visible);
    CHECK_FALSE(mounted.mount.enabled);
    CHECK_FALSE(mounted.mount.reason.empty());
    CHECK(mounted.park.visible);
    CHECK(mounted.park.enabled);

    // An automatic head keeps its filament actions and offers no Mount.
    FilamentHeadView automatic = view_of({ready_head(0), ready_head(1)});
    automatic.heads_dock = true;
    const auto feeds = decide_slot_actions(automatic, 1);
    CHECK(feeds.load.enabled);
    CHECK(feeds.unload.enabled);
    CHECK_FALSE(feeds.mount.visible);
}

TEST_CASE("slot actions: a slot that feeds no head says so", "[filament][actions]") {
    const auto actions = decide_slot_actions(view_of({ready_head(0)}), -1);
    CHECK(actions.load.visible);
    CHECK_FALSE(actions.load.enabled);
    CHECK_FALSE(actions.load.reason.empty());
    CHECK_FALSE(actions.unload.enabled);
}

TEST_CASE("head picker: rows follow the source slot, preselect the selected or mounted head",
          "[filament][actions]") {
    FilamentHeadView view = view_of({ready_head(5), ready_head(6), loaded_head(7)});
    view.mounted_head = 1;

    auto rows = decide_head_picker_rows(view, HeadPickOp::Load);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].slot == 5);
    CHECK(rows[2].slot == 7);
    CHECK(rows[0].enabled);
    CHECK_FALSE(rows[0].preselected);
    CHECK(rows[1].preselected);

    // A selection outranks the mounted head.
    view.selected_head = 0;
    rows = decide_head_picker_rows(view, HeadPickOp::Load);
    CHECK(rows[0].preselected);
    CHECK_FALSE(rows[1].preselected);

    // A preselection is never forced onto a head with nothing to do.
    view.selected_head = 2;
    rows = decide_head_picker_rows(view, HeadPickOp::Load);
    CHECK(rows[2].enabled);
    CHECK_FALSE(rows[2].preselected);
    rows = decide_head_picker_rows(view, HeadPickOp::Unload);
    CHECK(rows[2].preselected);
    CHECK_FALSE(rows[0].preselected);

    // Mount offers every head but the mounted one.
    view.selected_head = -1;
    rows = decide_head_picker_rows(view, HeadPickOp::Mount);
    CHECK(rows[0].enabled);
    CHECK_FALSE(rows[1].enabled);
    CHECK_FALSE(rows[1].reason.empty());
}

TEST_CASE("head picker: a head with no assigned slot is disabled with a reason",
          "[filament][actions]") {
    HeadState unassigned = ready_head(0);
    unassigned.source_slot.reset();
    HeadState manual = manual_head(1);
    const auto rows =
        decide_head_picker_rows(view_of({unassigned, manual, ready_head(2)}), HeadPickOp::Load);
    CHECK_FALSE(rows[0].enabled);
    CHECK_FALSE(rows[0].reason.empty());
    CHECK(rows[0].slot == -1);
    CHECK_FALSE(rows[1].enabled);
    CHECK(rows[1].reason == "by hand (load)");
    CHECK(rows[2].enabled);
}

TEST_CASE("head picker: an unload row a sensor reports empty is not tickable",
          "[filament][actions]") {
    // The not-known-to-be-empty rule the sidebar already follows, on the rows
    // a multi-head picker offers: a head whose own sensor says EMPTY has
    // nothing for an unload to act on, whatever the firmware would do with
    // the command.
    // ready_head() is a ready-to-LOAD head: its sensor already says empty.
    HeadState unknown = ready_head(4);
    unknown.has_filament.reset();
    const auto rows = decide_head_picker_rows(view_of({ready_head(3), unknown, loaded_head(5)}),
                                              HeadPickOp::Unload);
    CHECK_FALSE(rows[0].enabled);
    CHECK(rows[0].reason == "Empty");
    CHECK_FALSE(rows[0].preselected);
    // Nothing senses the second head: it stays tickable, not assumed empty.
    CHECK(rows[1].enabled);
    CHECK(rows[1].reason.empty());
    CHECK(rows[2].enabled);
}

TEST_CASE("backend filament ops: defaults answer one automatic head", "[ams][filament_ops]") {
    // The plain U1 shape stands in for "any backend" on the base-class
    // answers it does not override.
    struct Plain : public helix::AmsBackendSnapmaker {
        Plain() : helix::AmsBackendSnapmaker(nullptr, nullptr) {}
    } plain;
    const HeadFilamentOps ops = plain.AmsBackend::head_filament_ops(0);
    CHECK(ops.load == FilamentOpSupport::Automatic);
    CHECK(ops.unload == FilamentOpSupport::Automatic);
    CHECK(ops.load_reason.empty());
    CHECK(plain.AmsBackend::filament_head_count() == 1);
    CHECK(plain.AmsBackend::max_heads_per_filament_op() == 1);
}

TEST_CASE("backend filament ops: Snapmaker answers four heads, batch of four",
          "[ams][filament_ops]") {
    FourHeadBackend backend;
    CHECK(backend.filament_head_count() == 4);
    CHECK(backend.max_heads_per_filament_op() == 4);
    CHECK(backend.head_for_slot(3) == 3);
    CHECK_FALSE(backend.head_for_slot(4).has_value());
    CHECK(backend.head_source_slot(2, backend.get_system_info()) == 2);
    CHECK_FALSE(backend.supports_reset());
    // Loaded to the nozzle and parked short of it both hold filament.
    CHECK(backend.head_has_filament(0) == false);
    CHECK(backend.head_has_filament(1) == true);
    CHECK(backend.head_has_filament(2) == true);
    CHECK_FALSE(backend.head_has_filament(4).has_value());
}

TEST_CASE("runout target: the runout's lane names the head and its source",
          "[filament][actions][runout]") {
    // Bays 4..7 feed head lane-4; head 1's Assigned Tool slot is bay 5.
    class BayBackend : public helix::AmsBackendMock {
      public:
        BayBackend() : helix::AmsBackendMock(8) {}
        std::optional<int> head_for_slot(int slot) const override {
            return slot >= 4 ? std::optional<int>(slot - 4) : std::optional<int>(slot);
        }
        std::optional<int> head_source_slot(int head, const helix::AmsSystemInfo&) const override {
            return head == 1 ? std::optional<int>(5) : std::nullopt;
        }
    } backend;

    // Load feeds the head's source; Unload names what ran out.
    CHECK(runout_target_slot(&backend, 1, /*load=*/true) == 5);
    CHECK(runout_target_slot(&backend, 1, /*load=*/false) == 1);
    // No source assigned: the lane itself.
    CHECK(runout_target_slot(&backend, 2, /*load=*/true) == 2);
    // A runout naming no lane falls back to the active slot.
    CHECK(runout_target_slot(&backend, -1, /*load=*/true) == backend.get_current_slot());
    CHECK(runout_target_slot(&backend, std::nullopt, /*load=*/false) == backend.get_current_slot());
    CHECK(runout_target_slot(nullptr, 1, /*load=*/true) == -1);
}

TEST_CASE("head picker: seat by hand offers every tool, none chosen", "[filament][actions]") {
    FilamentHeadView view =
        view_of({loaded_head(0), loaded_head(1), ready_head(2), manual_head(3)});
    view.mounted_head = 1;
    view.selected_head = 1;
    const auto rows = decide_head_picker_rows(view, HeadPickOp::SeatByHand);
    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) {
        INFO("head " << row.head);
        CHECK(row.enabled);
        CHECK_FALSE(row.preselected);
        CHECK(row.slot == -1);
    }
    CHECK(single_select(HeadPickOp::SeatByHand));
    CHECK(single_select(HeadPickOp::Mount));
    CHECK_FALSE(single_select(HeadPickOp::Load));
}
