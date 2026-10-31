// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_performance_overlay.cpp
 * @brief XML snapshot test: PerformanceOverlay rebuilds MCU rows when
 *        perf_mcu_names changes.
 *
 * Creates the real performance_overlay XML component, drives perf_mcu_names
 * through PerformanceStateTestAccess::apply_sample, drains the UpdateQueue,
 * and asserts the mcu_card child count matches the MCU count in the sample.
 */

#include "ui_nav_manager.h"
#include "ui_overlay_performance.h"
#include "ui_settings_system.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/performance_state_test_access.h"
#include "../test_helpers/ui_overlay_performance_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "helix_sparkline.h"
#include "performance_state.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::perf::McuStat;
using helix::perf::PerformanceState;
using helix::perf::PerformanceStateTestAccess;
using helix::perf::PerfSample;
using helix::ui::UiOverlayPerformance;
using helix::ui::UiOverlayPerformanceTestAccess;
using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;

// ============================================================================
// Fixture
// ============================================================================

namespace {

/**
 * @brief XMLTestFixture subclass that additionally:
 *   - registers helix_sparkline custom widget (needed by perf_metric_row)
 *   - registers overlay_panel, header_bar, components/perf_metric_row,
 *     and performance_overlay XML components
 *   - inits + deinits PerformanceState subjects
 *   - resets UiOverlayPerformance singleton before/after each test
 */
class PerfOverlayFixture : public XMLTestFixture {
  public:
    PerfOverlayFixture() : XMLTestFixture() {
        // helix_sparkline is a native C++ widget, not an XML file.
        // register_xml_components() (called from application.cpp) handles this
        // in production; tests must do it manually.
        if (!s_perf_xml_registered) {
            helix::ui::register_helix_sparkline_widget();

            // overlay_panel depends on header_bar; register in dependency order.
            register_component("header_bar");
            register_component("overlay_panel");
            register_component("components/perf_metric_row");
            register_component("performance_overlay");

            s_perf_xml_registered = true;
        }

        PerformanceState::instance().init_subjects();

        // Guarantee a clean singleton regardless of prior test order.
        UiOverlayPerformanceTestAccess::reset(UiOverlayPerformance::instance());
    }

    ~PerfOverlayFixture() override {
        // Teardown order matters:
        // 1. Delete the overlay widget (removes all LVGL observers on perf subjects).
        // 2. Reset the singleton (clears cached root/card pointers + mcu_names_observer_).
        // 3. Deinit perf subjects (safe — no observers remain).
        //
        // XMLTestFixture::~XMLTestFixture() runs AFTER this destructor and will
        // delete the test screen. By the time it does so, the overlay widget is
        // already gone (step 1), so the screen deletion is clean.
        lv_obj_t* root = UiOverlayPerformance::instance().root();
        if (root && lv_obj_is_valid(root)) {
            lv_obj_delete(root);
        }
        UiOverlayPerformanceTestAccess::reset(UiOverlayPerformance::instance());

        PerformanceState::instance().deinit_subjects();
    }

    // Non-copyable
    PerfOverlayFixture(const PerfOverlayFixture&) = delete;
    PerfOverlayFixture& operator=(const PerfOverlayFixture&) = delete;

  private:
    // Performance XML components only need to be registered once per process.
    static bool s_perf_xml_registered;
};

bool PerfOverlayFixture::s_perf_xml_registered = false;

void set_mcus(std::initializer_list<const char*> mcu_names) {
    PerfSample s;
    s.host_cpu_pct = 30.0f;
    float load = 0.10f;
    for (const char* n : mcu_names) {
        McuStat m;
        m.name = n;
        m.load = load;
        load += 0.05f;
        s.mcus.push_back(m);
    }
    PerformanceStateTestAccess::apply_sample(PerformanceState::instance(), s);
}

} // namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(PerfOverlayFixture, "PerformanceOverlay renders MCU rows dynamically",
                 "[performance][xml]") {
    // --- initial state: 1 MCU ---
    set_mcus({"mcu"});
    // apply_sample runs synchronously; the observe<const char*> callback for
    // perf_mcu_names defers rebuild_mcu_rows via queue_update.
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    // create() registers the observer on perf_mcu_names and will immediately
    // find mcu_card (initially empty, before the next drain). Because
    // observe<const char*> is non-immediate, the first rebuild fires on the next drain.
    auto* root = UiOverlayPerformance::instance().create(lv_screen_active());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    // Drain: fires the initial observe<const char*> callback triggered by attaching
    // the observer (LVGL calls the cb once on subscription).
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto* card = lv_obj_find_by_name(root, "mcu_card");
    REQUIRE(card != nullptr);
    REQUIRE(lv_obj_get_child_count(card) == 1);

    // --- second sample: 3 MCUs -> overlay must rebuild to 3 rows ---
    set_mcus({"mcu", "mcu sb", "mcu helper"});
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_obj_get_child_count(card) == 3);
}

// ============================================================================
// #1061 regression tests
//
// On 32-bit ARM, rebuilding perf_metric_row widgets (height="content" =
// LV_SIZE_CONTENT) on every perf_mcu_names notification was both wasteful and
// crash-prone: a freshly-created row carries the unresolved LV_COORD_MAX
// sentinel until the next layout pass, and a render that blits it in that
// window overflows int32 fill arithmetic → SIGSEGV. The fix gates rebuilds on
// an actual change to the names string and forces a synchronous layout pass so
// rows never linger with unresolved coordinates. See (prestonbrown/helixscreen#1061).
// ============================================================================

// Test A: identical perf_mcu_names notifications must NOT rebuild the rows.
TEST_CASE_METHOD(PerfOverlayFixture, "PerformanceOverlay gates rebuild on actual name change",
                 "[perf][1061]") {
    // Seed an initial 3-MCU sample so the names subject is non-empty.
    set_mcus({"mcu", "Turtle_1", "EBBCan"});
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto* root = UiOverlayPerformance::instance().create(lv_screen_active());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto* card = lv_obj_find_by_name(root, "mcu_card");
    REQUIRE(card != nullptr);
    REQUIRE(lv_obj_get_child_count(card) == 3);

    // Capture the first row pointer; a gated (skipped) rebuild must leave it untouched.
    lv_obj_t* first_child_before = lv_obj_get_child(card, 0);
    REQUIRE(first_child_before != nullptr);

    // Re-publish the SAME MCU set. Without gating this tears down + recreates
    // every row; with gating it returns early and the pointers are stable.
    set_mcus({"mcu", "Turtle_1", "EBBCan"});
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_obj_get_child_count(card) == 3);
    REQUIRE(lv_obj_get_child(card, 0) == first_child_before);

    // A genuinely different MCU set MUST rebuild to the new count.
    set_mcus({"mcu", "Turtle_1"});
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_obj_get_child_count(card) == 2);

    // A root deleted with its screen reads null, and a new one shows the rows again.
    lv_obj_delete(root);
    CHECK(UiOverlayPerformance::instance().root() == nullptr);
    root = UiOverlayPerformance::instance().create(lv_screen_active());
    REQUIRE(root != nullptr);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    CHECK(lv_obj_get_child_count(lv_obj_find_by_name(root, "mcu_card")) == 2);
}

// Test B: after a rebuild, every row must have resolved (non-sentinel, > 0)
// coordinates — the forced layout pass must have run.
TEST_CASE_METHOD(PerfOverlayFixture, "PerformanceOverlay resolves row layout synchronously",
                 "[perf][1061]") {
    {
        PerfSample s;
        s.host_cpu_pct = 30.0f;
        for (const char* n : {"mcu", "Turtle_1", "EBBCan"}) {
            McuStat m;
            m.name = n;
            m.load = 0.10f;
            s.mcus.push_back(m);
        }
        PerformanceStateTestAccess::apply_sample(PerformanceState::instance(), s);
    }
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto* root = UiOverlayPerformance::instance().create(lv_screen_active());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto* card = lv_obj_find_by_name(root, "mcu_card");
    REQUIRE(card != nullptr);
    REQUIRE(lv_obj_get_child_count(card) == 3);

    uint32_t n = lv_obj_get_child_count(card);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(card, i);
        REQUIRE(row != nullptr);

        int32_t w = lv_obj_get_width(row);
        int32_t h = lv_obj_get_height(row);

        // Unresolved LV_SIZE_CONTENT rows report the LV_COORD_MAX sentinel; a
        // forced layout pass replaces it with a real size. Both dimensions must
        // be concrete and positive.
        REQUIRE(w != LV_COORD_MAX);
        REQUIRE(h != LV_COORD_MAX);
        REQUIRE(w > 0);
        REQUIRE(h > 0);
    }
}

// ============================================================================
// L077 regression: the overlay's observer on perf_mcu_names must carry
// PerformanceState's subject lifetime token.
//
// perf_mcu_names is NOT a never-freed singleton. PerformanceState::deinit_subjects()
// frees it (subjects_.deinit_all()) and init_subjects() recreates it on reconnect,
// printer switch, and between tests. mcu_names_observer_ lives on the overlay
// SINGLETON (not the widget), so it can outlive the subject. If it was created
// without subjects_lifetime(), tearing it down after the subject is freed calls
// lv_observer_remove() on freed memory → UAF/SEGV (lv_observer.c:584) — the same
// class of bug that 056bb40e9 fixed for HelixSparkline on perf_history_tick.
//
// This test frees the perf subjects out from under a live observer, then tears
// the observer down. Without the lifetime token the reset() below SEGVs; with it,
// ObserverGuard::reset() detects the dead subject and releases instead of removing.
// ============================================================================
TEST_CASE_METHOD(PerfOverlayFixture,
                 "PerformanceOverlay perf_mcu_names observer survives subject teardown",
                 "[performance][uaf][L077]") {
    // Seed the perf subjects so perf_mcu_names is live and non-empty.
    {
        PerfSample s;
        s.host_cpu_pct = 30.0f;
        McuStat a;
        a.name = "mcu";
        a.load = 0.10f;
        s.mcus = {a};
        PerformanceStateTestAccess::apply_sample(PerformanceState::instance(), s);
    }
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    // create() registers mcu_names_observer_ on perf_mcu_names. The observer is a
    // member of the singleton, NOT tied to the widget lifetime.
    auto* root = UiOverlayPerformance::instance().create(lv_screen_active());
    REQUIRE(root != nullptr);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    // Delete the widget tree while the perf subjects are still alive so the
    // per-MCU XML binding observers unbind cleanly. mcu_names_observer_ survives
    // this (it lives on the singleton, not the widget).
    lv_obj_delete(root);

    // Free the perf subjects — the reconnect / printer-switch path. This calls
    // lv_subject_deinit(&s_mcu_names_); mcu_names_observer_ now references freed
    // memory.
    PerformanceState::instance().deinit_subjects();

    // Tear the observer down. This is the UAF site: ObserverGuard::reset() runs on
    // an observer whose subject was just freed. With subjects_lifetime() passed to
    // observe<const char*>, reset() sees the dead token and skips lv_observer_remove();
    // without it, this line SEGVs.
    UiOverlayPerformanceTestAccess::reset(UiOverlayPerformance::instance());

    // Reaching here without a crash is the regression assertion.
    REQUIRE(UiOverlayPerformance::instance().root() == nullptr);
}

// ============================================================================
// Overlay lifecycle registration
//
// The System settings Performance row pushed its overlay straight onto the nav
// stack without register_overlay_instance(). NavigationManager resolved no
// lifecycle for it, so every open was filed as "unreg" in panel telemetry and in
// the crash breadcrumb ring — and a genuine missing registration elsewhere became
// indistinguishable from this known-benign one in user logs. Bundle 3Q2GB74K
// carried the resulting warning pair from a live printer.
//
// UiOverlayPerformance implements no IPanelLifecycle, so a null lifecycle is the
// correct registration: it marks the overlay as deliberately lifecycle-less.
// Tests run with strict registration, which aborts on an unregistered push — so
// completing this test at all is half the assertion.
// ============================================================================
TEST_CASE_METHOD(PerfOverlayFixture, "PerformanceOverlay registers with the nav stack when pushed",
                 "[performance][navigation][overlay]") {
    auto& nav = NavigationManager::instance();

    // panel_stack_[0] must hold a root panel — overlay code reads panel_stack_.back()
    // to find what sits beneath it.
    std::array<lv_obj_t*, static_cast<size_t>(kUiPanelCount)> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(lv_screen_active());
    }
    nav.set_panels(panels.data());
    nav.set_active(helix::PanelId::Settings);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE_FALSE(nav.has_open_overlays());

    // The real production path behind the System settings Performance row.
    helix::settings::get_system_settings_overlay().open_performance();

    // push_overlay() queues its whole body through UpdateQueue.
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(UiOverlayPerformance::instance().root() != nullptr);
    REQUIRE(nav.has_open_overlays());
}
