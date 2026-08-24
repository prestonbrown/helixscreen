// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_realtime_filament_state.cpp
 * @brief Phase 1 of the cross-backend AMS real-time filament-state refactor.
 *
 * Covers:
 *  - AmsBackend per-slot LIVE accessors: the base-class safe defaults
 *    (slot_has_filament_at_toolhead=false, slot_is_actively_loaded derived
 *    from current_slot + filament_loaded) plus the Snapmaker overrides.
 *  - AmsState per-slot LIVE subjects published on sync_from_backend:
 *    get_slot_segment_subject / get_slot_toolhead_present_subject /
 *    get_slot_active_loaded_subject.
 *  - Op-card current-loaded color matches the loaded slot's color (not slot+1).
 *  - filament_loaded cleared on unload (active-lane subject clears).
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;

using json = nlohmann::json;

// Friend-class shim mirroring the one in test_ams_backend_snapmaker.cpp.
class SnapmakerRealtimeTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const json& n) {
        b.handle_status_update(n);
    }
    static void set_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.sensor_filament_present_[slot_index] = present;
    }
};

namespace {
void drain() {
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}
} // namespace

// ============================================================================
// Part 1 — Backend per-slot LIVE accessors
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "AmsBackend base defaults: slot LIVE accessors degrade gracefully",
                 "[ams][realtime][backend]") {
    // The mock backend does NOT override the new virtuals, so it exercises the
    // base-class defaults.
    AmsBackendMock backend;
    backend.set_operation_delay(10); // fast async completion for testing
    backend.start();

    // Default toolhead-present is always false ("no per-slot toolhead sensor").
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(backend.slot_has_filament_at_toolhead(i));
    }

    SECTION("active-loaded default tracks current_slot + filament_loaded") {
        // The mock starts with slot 0 loaded — the default derives the
        // actively-loaded slot from current_slot + filament_loaded.
        REQUIRE(backend.is_filament_loaded());
        REQUIRE(backend.get_current_slot() == 0);
        CHECK(backend.slot_is_actively_loaded(0));
        CHECK_FALSE(backend.slot_is_actively_loaded(1));
        CHECK_FALSE(backend.slot_is_actively_loaded(2));

        // After an unload completes, no slot is actively loaded. The mock
        // finishes the op on its own thread, so poll for it: a fixed sleep is a
        // bet that the thread got scheduled inside the window, and on a
        // saturated box it does not (seen failing at load average 94, passing
        // 11/11 when the box was quiet).
        backend.unload_active_filament();
        REQUIRE(wait_until([&] { return !backend.is_filament_loaded(); }));
        for (int i = 0; i < 4; ++i) {
            CHECK_FALSE(backend.slot_is_actively_loaded(i));
        }

        // Load slot 1 → it becomes the actively-loaded slot.
        backend.load_filament(1);
        REQUIRE(wait_until([&] { return backend.is_filament_loaded(); }));
        REQUIRE(backend.get_current_slot() == 1);
        CHECK(backend.slot_is_actively_loaded(1));
        CHECK_FALSE(backend.slot_is_actively_loaded(0));
        CHECK_FALSE(backend.slot_is_actively_loaded(2));
    }

    backend.stop();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Snapmaker overrides slot LIVE accessors from sensor + LOADED status",
                 "[ams][realtime][snapmaker]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Slot 0 active (LOADED), slots 1 & 3 hold filament (AVAILABLE), slot 2 empty.
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}}, // active tool = slot 0
        {"print_task_config", json{{"filament_exist", json::array({true, true, false, true})}}}};
    SnapmakerRealtimeTestAccess::handle_status(backend, status);

    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(backend.get_slot_info(1).status == SlotStatus::AVAILABLE);

    SECTION("slot_is_actively_loaded true only for the LOADED slot") {
        CHECK(backend.slot_is_actively_loaded(0));
        CHECK_FALSE(backend.slot_is_actively_loaded(1));
        CHECK_FALSE(backend.slot_is_actively_loaded(2));
        CHECK_FALSE(backend.slot_is_actively_loaded(3));
    }

    SECTION("slot_has_filament_at_toolhead reflects the channel_state load latch") {
        // Loaded state now comes from channel_state, NOT the motion sensor
        // (which fails to clear after an unload on current firmware). Latch
        // tools 0 and 1 loaded via load_finish.
        SnapmakerRealtimeTestAccess::handle_status(
            backend, json{{"filament_feed left",
                           json{{"extruder0", json{{"filament_detected", true},
                                                   {"channel_state", "load_finish"}}},
                                {"extruder1", json{{"filament_detected", true},
                                                   {"channel_state", "load_finish"}}}}}});
        CHECK(backend.slot_has_filament_at_toolhead(0));
        CHECK(backend.slot_has_filament_at_toolhead(1));

        // Unload tool 1 (channel_state unload_finish). The latch clears even
        // though filament_detected stays true — the exact firmware condition.
        SnapmakerRealtimeTestAccess::handle_status(
            backend, json{{"filament_feed left",
                           json{{"extruder1", json{{"filament_detected", true},
                                                   {"channel_state", "unload_finish"}}}}}});
        CHECK_FALSE(backend.slot_has_filament_at_toolhead(1));
        // Other tools unaffected.
        CHECK(backend.slot_has_filament_at_toolhead(0));
    }

    SECTION("out-of-range indices are safe") {
        CHECK_FALSE(backend.slot_has_filament_at_toolhead(-1));
        CHECK_FALSE(backend.slot_has_filament_at_toolhead(99));
        CHECK_FALSE(backend.slot_is_actively_loaded(-1));
        CHECK_FALSE(backend.slot_is_actively_loaded(99));
    }
}

// ============================================================================
// Part 2 — AmsState per-slot LIVE subjects
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AmsState publishes per-slot LIVE subjects on sync",
                 "[ams][realtime][ams_state]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto backend = std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr);
    auto* backend_ptr = backend.get();
    ams.set_backend(std::move(backend));

    // Drive a known state: slot 0 LOADED, slot 1 AVAILABLE, slot 2 empty.
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, false, false})}}}};
    SnapmakerRealtimeTestAccess::handle_status(*backend_ptr, status);

    ams.sync_from_backend();
    drain();

    SECTION("accessors return non-null subjects in range and null out of range") {
        CHECK(ams.get_slot_segment_subject(0) != nullptr);
        CHECK(ams.get_slot_toolhead_present_subject(0) != nullptr);
        CHECK(ams.get_slot_active_loaded_subject(0) != nullptr);
        CHECK(ams.get_slot_segment_subject(-1) == nullptr);
        CHECK(ams.get_slot_segment_subject(AmsState::MAX_SLOTS) == nullptr);
    }

    SECTION("active-loaded subject reflects backend per-slot loaded state") {
        CHECK(lv_subject_get_int(ams.get_slot_active_loaded_subject(0)) == 1);
        CHECK(lv_subject_get_int(ams.get_slot_active_loaded_subject(1)) == 0);
        CHECK(lv_subject_get_int(ams.get_slot_active_loaded_subject(2)) == 0);
    }

    SECTION("toolhead-present subject reflects the channel_state load latch") {
        // Latch tool 0 loaded via channel_state (the authoritative signal).
        SnapmakerRealtimeTestAccess::handle_status(
            *backend_ptr, json{{"filament_feed left",
                                json{{"extruder0", json{{"filament_detected", true},
                                                        {"channel_state", "load_finish"}}}}}});
        ams.sync_from_backend();
        drain();
        CHECK(lv_subject_get_int(ams.get_slot_toolhead_present_subject(0)) == 1);

        // Unload tool 0 (channel_state unload_finish) → latch clears → subject
        // updates on next sync, even though the motion sensor never dropped.
        SnapmakerRealtimeTestAccess::handle_status(
            *backend_ptr, json{{"filament_feed left",
                                json{{"extruder0", json{{"filament_detected", true},
                                                        {"channel_state", "unload_finish"}}}}}});
        ams.sync_from_backend();
        drain();
        CHECK(lv_subject_get_int(ams.get_slot_toolhead_present_subject(0)) == 0);
    }

    SECTION("segment subject matches the backend per-slot segment") {
        int expected = static_cast<int>(backend_ptr->get_slot_filament_segment(0));
        CHECK(lv_subject_get_int(ams.get_slot_segment_subject(0)) == expected);
    }

    SECTION("SubjectLifetime overload returns the same static subject + empty token") {
        SubjectLifetime lt;
        lv_subject_t* s = ams.get_slot_active_loaded_subject(0, lt);
        CHECK(s == ams.get_slot_active_loaded_subject(0));
        // Static subject → empty (always-alive) token.
        CHECK(lt == nullptr);
    }

    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// Phase 2 — Reactive chain: observers on per-slot LIVE subjects fire on change
// ============================================================================
//
// The MULTI-FILAMENT panel (path canvas) and the per-lane active badge OBSERVE
// these subjects. This pins the observable contract those panels depend on: a
// sensor change → sync → the subject value changes AND a registered observer
// fires. If this breaks, the panel silently stops redrawing on push/pull.

TEST_CASE_METHOD(LVGLTestFixture, "Per-slot LIVE subjects notify observers on sensor change",
                 "[ams][realtime][reactive]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto backend = std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr);
    auto* backend_ptr = backend.get();
    ams.set_backend(std::move(backend));

    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}}, // active tool = slot 0
        {"print_task_config", json{{"filament_exist", json::array({true, true, false, false})}}}};
    SnapmakerRealtimeTestAccess::handle_status(*backend_ptr, status);
    // Latch tool 1 loaded to its toolhead via channel_state (the source that
    // now drives the toolhead-present subject).
    SnapmakerRealtimeTestAccess::handle_status(
        *backend_ptr,
        json{{"filament_feed left", json{{"extruder1", json{{"filament_detected", true},
                                                            {"channel_state", "load_finish"}}}}}});
    ams.sync_from_backend();
    drain();

    // Tool 1 is loaded to its toolhead.
    REQUIRE(lv_subject_get_int(ams.get_slot_toolhead_present_subject(1)) == 1);

    // Attach an observer to slot 1's toolhead-present subject (mirrors how the
    // panel observes it to redraw the lane path).
    int fire_count = 0;
    int last_value = -1;
    struct Ctx {
        int* count;
        int* value;
    } ctx{&fire_count, &last_value};
    lv_observer_t* obs = lv_subject_add_observer(
        ams.get_slot_toolhead_present_subject(1),
        [](lv_observer_t* o, lv_subject_t* s) {
            auto* c = static_cast<Ctx*>(lv_observer_get_user_data(o));
            (*c->count)++;
            *c->value = lv_subject_get_int(s);
        },
        &ctx);
    REQUIRE(obs != nullptr);
    int baseline = fire_count;

    // Unload tool 1 (channel_state unload_finish) → the load latch clears.
    SnapmakerRealtimeTestAccess::handle_status(
        *backend_ptr, json{{"filament_feed left",
                            json{{"extruder1", json{{"filament_detected", true},
                                                    {"channel_state", "unload_finish"}}}}}});
    ams.sync_from_backend();
    drain();

    CHECK(lv_subject_get_int(ams.get_slot_toolhead_present_subject(1)) == 0);
    CHECK(fire_count > baseline); // observer fired → panel would redraw the lane
    CHECK(last_value == 0);

    lv_observer_remove(obs);
    ams.clear_backends();
    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "Active-loaded subject is the single highlight source on unload",
                 "[ams][realtime][reactive][badge]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto backend = std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr);
    auto* backend_ptr = backend.get();
    ams.set_backend(std::move(backend));

    json loaded = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, false, false, false})}}}};
    SnapmakerRealtimeTestAccess::handle_status(*backend_ptr, loaded);
    ams.sync_from_backend();
    drain();
    REQUIRE(lv_subject_get_int(ams.get_slot_active_loaded_subject(0)) == 1);

    // Observe slot 0's active-loaded subject (the SINGLE source the bottom
    // T-badge now binds to). It must fire and drop to 0 on unload — previously
    // the badge read current_slot + filament_loaded separately and stayed lit.
    int fire_count = 0;
    int last_value = -1;
    struct Ctx {
        int* count;
        int* value;
    } ctx{&fire_count, &last_value};
    lv_observer_t* obs = lv_subject_add_observer(
        ams.get_slot_active_loaded_subject(0),
        [](lv_observer_t* o, lv_subject_t* s) {
            auto* c = static_cast<Ctx*>(lv_observer_get_user_data(o));
            (*c->count)++;
            *c->value = lv_subject_get_int(s);
        },
        &ctx);
    REQUIRE(obs != nullptr);
    int baseline = fire_count;

    json unloaded =
        json{{"filament_feed left", json{{"extruder0", json{{"filament_detected", true},
                                                            {"channel_state", "unload_finish"}}}}}};
    SnapmakerRealtimeTestAccess::handle_status(*backend_ptr, unloaded);
    ams.sync_from_backend();
    drain();

    CHECK(lv_subject_get_int(ams.get_slot_active_loaded_subject(0)) == 0);
    CHECK(fire_count > baseline); // badge observer fired → highlight drops
    CHECK(last_value == 0);

    lv_observer_remove(obs);
    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// Part 2b — Per-slot FILL subject: sync writes the canonical display_fill_pct
// ============================================================================
//
// The fill subject is the structural fix for spool fill levels: written here by
// sync_from_backend, observed inside the ams_slot widget, so every panel renders
// fill from state (the AmsOverviewPanel bug where unit-detail spools showed 100%
// full came from only AmsPanel ever pushing fill imperatively).

TEST_CASE_METHOD(LVGLTestFixture, "AmsState publishes per-slot fill subject on sync",
                 "[ams][fill][ams_state]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto mock = AmsBackend::create_mock(4);
    auto* mock_ptr = static_cast<AmsBackendMock*>(mock.get());

    // slot 0: present, real weights 500/1000 → 50%.
    mock_ptr->force_slot_status(0, SlotStatus::AVAILABLE);
    SlotInfo s0;
    s0.slot_index = 0;
    s0.material = "PLA";
    s0.color_rgb = 0x00FF00;
    s0.remaining_weight_g = 500.0f;
    s0.total_weight_g = 1000.0f;
    mock_ptr->set_slot_info(0, s0);

    // slot 1: empty lane → 0 (not present).
    mock_ptr->force_slot_status(1, SlotStatus::EMPTY);

    // slot 2: present, metadata only (no usable weights) → full fallback.
    mock_ptr->force_slot_status(2, SlotStatus::AVAILABLE);
    SlotInfo s2;
    s2.slot_index = 2;
    s2.material = "PETG";
    s2.color_rgb = 0x0000FF;
    mock_ptr->set_slot_info(2, s2); // remaining/total stay -1 (unknown)

    // slot 3: present but zero metadata AND no weights → no data (-1).
    mock_ptr->force_slot_status(3, SlotStatus::AVAILABLE);
    SlotInfo s3;
    s3.slot_index = 3; // material empty, default color, weights -1
    mock_ptr->set_slot_info(3, s3);

    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    drain();

    CHECK(lv_subject_get_int(ams.get_slot_fill_subject(0)) == 50);
    CHECK(lv_subject_get_int(ams.get_slot_fill_subject(1)) == 0);
    CHECK(lv_subject_get_int(ams.get_slot_fill_subject(2)) == 100);
    CHECK(lv_subject_get_int(ams.get_slot_fill_subject(3)) == -1);

    // Out-of-range → nullptr.
    CHECK(ams.get_slot_fill_subject(-1) == nullptr);
    CHECK(ams.get_slot_fill_subject(AmsState::MAX_SLOTS) == nullptr);

    // Token'd overload: backend 0 is a static subject → same pointer, empty token.
    SubjectLifetime lt;
    lv_subject_t* fs = ams.get_slot_fill_subject(0, 0, lt);
    CHECK(fs == ams.get_slot_fill_subject(0));
    CHECK(lt == nullptr);

    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// Part 3a — Op-card current-loaded color matches the loaded slot (not slot+1)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AMS op-card color matches the loaded slot's color",
                 "[ams][realtime][opcard]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<AmsBackendMock>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(10); // fast async completion for testing
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    // Give each slot a distinct, unambiguous color so an off-by-one would be
    // visible (slot N's color != slot N+1's color).
    const uint32_t colors[4] = {0x111111, 0x222222, 0x333333, 0x444444};
    for (int i = 0; i < 4; ++i) {
        auto slot = mock_ptr->get_slot_info(i);
        slot.color_rgb = colors[i];
        mock_ptr->set_slot_info(i, slot);
    }

    // Load slot 1 (a non-zero, non-first slot to expose +1 indexing). The mock
    // constructs with slot 0 already loaded, so is_filament_loaded() is true
    // before this load even starts -- polling on it returns on the first
    // evaluation and leaves the assertion below racing the operation thread,
    // which has not yet published the new slot. Join the thread and read the
    // settled state instead.
    mock_ptr->load_filament(1);
    mock_ptr->wait_for_operation_thread();
    REQUIRE(mock_ptr->is_filament_loaded());
    REQUIRE(mock_ptr->get_current_slot() == 1);

    ams.sync_from_backend();
    drain();

    int current_color = lv_subject_get_int(ams.get_current_color_subject());
    // The op-card color MUST be slot 1's color, NOT slot 2's (the off-by-one).
    CHECK(current_color == static_cast<int>(colors[1]));
    CHECK(current_color != static_cast<int>(colors[2]));

    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// Part 3b — filament_loaded cleared on unload (active-lane subject clears)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AMS clears filament_loaded after unload completes",
                 "[ams][realtime][unload]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto backend = std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr);
    auto* backend_ptr = backend.get();
    ams.set_backend(std::move(backend));

    // Load slot 0: active tool = extruder0, filament present.
    json loaded = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, false, false, false})}}}};
    SnapmakerRealtimeTestAccess::handle_status(*backend_ptr, loaded);
    ams.sync_from_backend();
    drain();

    REQUIRE(backend_ptr->is_filament_loaded());
    REQUIRE(lv_subject_get_int(ams.get_filament_loaded_subject()) == 1);
    REQUIRE(lv_subject_get_int(ams.get_slot_active_loaded_subject(0)) == 1);

    // Unload completes: the firmware retracts to the buffer and reports the
    // channel state as unload_finish for that tool.
    json unloaded =
        json{{"filament_feed left", json{{"extruder0", json{{"filament_detected", true},
                                                            {"channel_state", "unload_finish"}}}}}};
    SnapmakerRealtimeTestAccess::handle_status(*backend_ptr, unloaded);
    ams.sync_from_backend();
    drain();

    // filament_loaded must be cleared so the active-lane highlight drops.
    CHECK_FALSE(backend_ptr->is_filament_loaded());
    CHECK(lv_subject_get_int(ams.get_filament_loaded_subject()) == 0);
    // No slot is actively loaded after unload.
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(backend_ptr->slot_is_actively_loaded(i));
        CHECK(lv_subject_get_int(ams.get_slot_active_loaded_subject(i)) == 0);
    }

    ams.clear_backends();
    ams.deinit_subjects();
}
