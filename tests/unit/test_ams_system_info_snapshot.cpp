// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_system_info_snapshot.cpp
 * @brief One slots_version bump costs one backend read, whoever is listening
 *        (prestonbrown/helixscreen#1777).
 *
 * AmsState::system_info() hands every reader the same reading of a backend's
 * AmsSystemInfo until the backend changes. A sync that bumps slots_version takes
 * that reading itself, so the observers it wakes read nothing more.
 */

#include "ui_ams_mini_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/observer_factory.h"
#include "core/update_queue.h"

#include <chrono>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A mock that counts its full system-info copies. Moving `tint` changes slot 0's
/// color without raising an event, so only an explicit sync can see it. A
/// nonzero `unit_size` regroups the slots into units of that many, each with a
/// climate reading, the shape of a multi-box fleet.
class CountingMock : public AmsBackendMock {
  public:
    explicit CountingMock(int slots, int unit_size = 0)
        : AmsBackendMock(slots), unit_size(unit_size) {}

    AmsSystemInfo get_system_info() const override {
        ++reads;
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (unit_size > 0) {
            std::vector<SlotInfo> all;
            for (const auto& unit : info.units)
                all.insert(all.end(), unit.slots.begin(), unit.slots.end());
            info.units.clear();
            for (size_t first = 0; first < all.size(); first += unit_size) {
                AmsUnit unit;
                unit.unit_index = static_cast<int>(info.units.size());
                unit.name = "unit_" + std::to_string(unit.unit_index);
                unit.display_name = "Box " + unit.name;
                unit.first_slot_global_index = static_cast<int>(first);
                EnvironmentData env;
                env.temperature_c = 21.0f;
                env.humidity_pct = 35.0f;
                env.has_humidity = true;
                unit.environment = env;
                for (size_t i = first; i < all.size() && i < first + unit_size; ++i)
                    unit.slots.push_back(all[i]);
                unit.slot_count = static_cast<int>(unit.slots.size());
                info.units.push_back(unit);
            }
        }
        if (SlotInfo* slot = info.get_slot_global(0)) {
            slot->color_rgb = 0x100000 + tint;
        }
        return info;
    }

    mutable int reads = 0;
    uint32_t tint = 0;
    const size_t unit_size;
};

class SnapshotFixture : public LVGLUITestFixture {
  public:
    explicit SnapshotFixture(int slots = 4, int unit_size = 0) {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<CountingMock>(slots, unit_size);
        mock = owned.get();
        ams.set_backend(std::move(owned));
        REQUIRE(mock->start().success());
        ams.sync_from_backend();
        drain();
    }

    ~SnapshotFixture() override {
        drain();
        mock->stop();
        AmsState::instance().clear_backends();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    static int slots_version() {
        return lv_subject_get_int(AmsState::instance().get_slots_version_subject());
    }

    /// Change slot 0 behind AmsState's back, sync, and let every observer run.
    /// @return the backend reads that took
    int sync_a_change() {
        ++mock->tint;
        const int version = slots_version();
        const int before = mock->reads;
        AmsState::instance().sync_from_backend();
        drain();
        REQUIRE(slots_version() != version);
        return mock->reads - before;
    }

    CountingMock* mock = nullptr;
};

/// Twelve boxes of four, the OpenAMS mock fleet's shape.
struct BigFleet : SnapshotFixture {
    BigFleet() : SnapshotFixture(48, 4) {}
};

} // namespace

TEST_CASE_METHOD(SnapshotFixture, "AMS sync: a slot change costs one backend read", "[ams][1777]") {
    CHECK(sync_a_change() == 1);
}

TEST_CASE_METHOD(SnapshotFixture, "AMS system_info: one reading until the backend changes",
                 "[ams][1777]") {
    auto& ams = AmsState::instance();
    const auto first = ams.system_info();
    REQUIRE(first != nullptr);
    const int reads = mock->reads;

    SECTION("readers share the reading") {
        CHECK(ams.system_info() == first);
        CHECK(ams.system_info(0) == first);
        CHECK(mock->reads == reads);
    }

    SECTION("a slots_version bump drops it") {
        ams.bump_slots_version();
        const auto next = ams.system_info();
        CHECK(next != first);
        CHECK(mock->reads == reads + 1);
        drain();
    }

    SECTION("a backend event drops it") {
        // Before the queued sync runs: the reading must not outlive the event.
        REQUIRE(mock->select_slot(2).success());
        const auto next = ams.system_info();
        REQUIRE(next != nullptr);
        CHECK(next != first);
        CHECK(next->current_slot == 2);
        CHECK(mock->reads == reads + 1);
        drain();
    }

    SECTION("a sync replaces it, and a held reading keeps its values") {
        const uint32_t held_color = first->get_slot_global(0)->color_rgb;
        ++mock->tint;
        ams.sync_from_backend();
        const auto next = ams.system_info();
        REQUIRE(next != nullptr);
        CHECK(next != first);
        CHECK(next->get_slot_global(0)->color_rgb == held_color + 1);
        CHECK(first->get_slot_global(0)->color_rgb == held_color);
        CHECK(mock->reads == reads + 1);
    }

    SECTION("no backend at the index reads as null") {
        CHECK(ams.system_info(1) == nullptr);
        CHECK(ams.system_info(-1) == nullptr);
    }

    SECTION("clearing the backends drops it") {
        mock->stop();
        ams.clear_backends();
        CHECK(ams.system_info() == nullptr);
        mock = nullptr;
        auto owned = std::make_unique<CountingMock>(4);
        mock = owned.get();
        ams.set_backend(std::move(owned));
        REQUIRE(mock->start().success());
        const auto fresh = ams.system_info();
        REQUIRE(fresh != nullptr);
        CHECK(fresh != first);
        CHECK(ams.system_info() == fresh);
    }
}

TEST_CASE_METHOD(SnapshotFixture, "AMS sync: the filament-mapping readers read nothing more",
                 "[ams][1777]") {
    // PrintSelectDetailView and PrintStatusPanel answer a slots_version bump with
    // these two; each would otherwise copy every backend's system info.
    auto& ams = AmsState::instance();
    ++mock->tint;
    ams.sync_from_backend();
    const int reads = mock->reads;

    const auto slots = ams.collect_available_slots();
    REQUIRE_FALSE(slots.empty());
    CHECK(slots.front().color_rgb == 0x100000 + mock->tint);
    (void)ams.routed_tool_colors();
    CHECK(mock->reads == reads);
}

TEST_CASE_METHOD(SnapshotFixture, "AMS sync: the mini status reads nothing more",
                 "[ams][1777][ams_mini]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    REQUIRE(w != nullptr);
    ui_ams_mini_status_set_width(w, 260);
    drain();

    CHECK(sync_a_change() == 1);
    lv_obj_delete(w);
}

TEST_CASE_METHOD(BigFleet, "AMS slots_version observers: cost per synced change bench",
                 "[.][bench][ams][1777]") {
    // The per-bump work of the mini status, the print-select detail view (three
    // collect_available_slots) and the print-status preview (routed_tool_colors).
    auto& ams = AmsState::instance();
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 260);
    drain();
    int listener = 0;
    ObserverGuard guard = helix::ui::observe<int>(
        ams.get_slots_version_subject(), &listener,
        [](int*, int) {
            auto& state = AmsState::instance();
            for (int i = 0; i < 3; ++i)
                (void)state.collect_available_slots();
            (void)state.routed_tool_colors();
        },
        ams.get_subjects_lifetime());

    constexpr int kRuns = 1000;
    const auto c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i)
        (void)mock->get_system_info();
    const auto c1 = std::chrono::steady_clock::now();
    const double copy_us = std::chrono::duration<double, std::micro>(c1 - c0).count() / kRuns;
    WARN("BENCH one system info copy, 12x4 fleet: " << copy_us << " us");

    for (int i = 0; i < 50; ++i)
        sync_a_change();
    const int reads = mock->reads;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i)
        sync_a_change();
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kRuns;
    WARN("BENCH synced change, 12x4 fleet: " << us << " us/tick, backend reads/tick "
                                             << (mock->reads - reads) / double(kRuns));
    guard.reset();
    lv_obj_delete(w);
}
