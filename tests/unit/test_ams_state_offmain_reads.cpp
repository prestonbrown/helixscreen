// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_offmain_reads.cpp
 * @brief AmsState queries that FilamentSensorManager::update_from_status makes
 *        from the WebSocket thread while the main thread mutates AmsState.
 *
 * Plain builds rarely fail these; they exist to go red under ASAN/TSAN.
 */

#include "../../src/printer/ams_state_internal.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/trait_probe_backend.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "ui/ui_widget_helpers.h"

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int kCycles = 300;

struct OffMainFixture : LVGLTestFixture {
    OffMainFixture() {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.init_subjects(false);
    }
    ~OffMainFixture() override {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.set_action(AmsAction::IDLE);
    }
};
} // namespace

TEST_CASE_METHOD(OffMainFixture, "AmsState::is_filament_operation_active is safe off-main",
                 "[ams][threading]") {
    auto& ams = AmsState::instance();
    std::atomic<bool> stop{false};

    std::thread ws([&] {
        while (!stop.load()) {
            (void)ams.is_filament_operation_active();
        }
    });

    for (int i = 0; i < kCycles; ++i) {
        ams.set_action(i % 2 ? AmsAction::LOADING : AmsAction::IDLE);
    }
    stop = true;
    ws.join();

    ams.set_action(AmsAction::LOADING);
    CHECK(ams.is_filament_operation_active());
    ams.set_action(AmsAction::IDLE);
    CHECK_FALSE(ams.is_filament_operation_active());
}

TEST_CASE_METHOD(OffMainFixture, "AmsState backend type query is safe against clear_backends",
                 "[ams][threading]") {
    auto& ams = AmsState::instance();
    std::atomic<bool> stop{false};

    std::thread ws([&] {
        while (!stop.load()) {
            (void)ams.primary_type();
        }
    });

    for (int i = 0; i < kCycles; ++i) {
        ams.clear_backends();
        ams.add_backend(std::make_unique<AmsBackendMock>(4));
    }
    stop = true;
    ws.join();

    CHECK(ams.primary_type() == ams.get_backend()->get_type());
    ams.clear_backends();
    CHECK_FALSE(ams.primary_type().has_value());
}

TEST_CASE_METHOD(OffMainFixture, "primary_traits is empty with no backend and safe off-main",
                 "[ams][threading]") {
    auto& ams = AmsState::instance();
    CHECK_FALSE(ams.primary_traits().has_value());

    BackendTraits owns;
    owns.raises_runout_fault = true;
    owns.parks_filament_off_toolhead_when_idle = true;
    ams.add_backend(std::make_unique<helix::test::TraitProbeBackend>(owns));
    REQUIRE(ams.primary_traits().has_value());
    CHECK(*ams.primary_traits() == owns);

    std::atomic<bool> stop{false};
    std::thread ws([&] {
        while (!stop.load()) {
            (void)ams.primary_traits();
        }
    });
    for (int i = 0; i < kCycles; ++i) {
        ams.clear_backends();
        ams.add_backend(std::make_unique<helix::test::TraitProbeBackend>(owns));
    }
    stop = true;
    ws.join();

    ams.clear_backends();
    CHECK_FALSE(ams.primary_traits().has_value());
}

// Every query the WebSocket thread makes, hammered together while main runs
// the two mutations that race them: the action edge and a reconnect's
// clear_backends()/add_backend(). Snapmaker's status handler also stamps
// unload times from that thread, so the writer side is off-main here too.
TEST_CASE_METHOD(OffMainFixture, "AmsState off-main queries survive action and backend churn",
                 "[ams][threading]") {
    auto& ams = AmsState::instance();
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};

    std::thread ws([&] {
        while (!stop.load()) {
            (void)ams.is_filament_operation_active();
            (void)ams.primary_type();
            (void)ams.primary_traits();
            (void)ams.any_filament_batch_in_flight();
            (void)ams.post_unload_runout_grace_armed();
            ams.mark_slot_unloaded(1);
            (void)ams.was_slot_recently_unloaded(1);
            reads.fetch_add(1);
        }
    });

    for (int i = 0; i < kCycles; ++i) {
        ams.set_action(i % 2 ? AmsAction::UNLOADING : AmsAction::IDLE);
        ams.clear_backends();
        ams.add_backend(std::make_unique<AmsBackendMock>(4));
    }
    while (reads.load() == 0) {
        std::this_thread::yield();
    }
    stop = true;
    ws.join();

    REQUIRE(ams.backend_count() == 1);
    CHECK_FALSE(ams.any_filament_batch_in_flight());
    ams.mark_slot_unloaded(1);
    CHECK(ams.was_slot_recently_unloaded(1));
    ams.clear_backends();
    CHECK_FALSE(ams.primary_type().has_value());
    CHECK_FALSE(ams.was_slot_recently_unloaded(1));
}

// Off the test build, an off-main call into main-thread state must not take
// the printer down: it reports once and carries on.
TEST_CASE_METHOD(OffMainFixture,
                 "lenient checks report an off-main AmsState call instead of aborting",
                 "[ams][threading]") {
    helix::ui::set_strict_ui_checks(false);
    {
        ExclusiveLogCapture log;
        for (int i = 0; i < 2; ++i) {
            std::thread([] { ams_state_detail::assert_main_thread("offmain_probe"); }).join();
        }
        ams_state_detail::assert_main_thread("onmain_probe");
        CHECK(log.count_containing("offmain_probe called off the main thread") == 1);
        CHECK(log.count_containing("onmain_probe") == 0);
    }
    helix::ui::set_strict_ui_checks(true);
}
