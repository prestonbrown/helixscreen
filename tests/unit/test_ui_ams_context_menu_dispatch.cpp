// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_ams_context_menu_dispatch.cpp
 * @brief Regression tests for the shared AMS context-menu action dispatch
 *
 * AmsPanel and AmsOverviewPanel each wire their own AmsContextMenu action
 * callback. The two switches drifted: Overview handled LOAD/UNLOAD/EDIT/
 * SPOOLMAN/RECOVER_POSITION/SCAN_QR and ended in `case CANCELLED: default:
 * break;`, so EJECT, SELECT_GATE, CHECK_GATE and CLEAR_SPOOL fell into the
 * default arm and were discarded with no toast and no log line. Multi-unit
 * setups render the Overview, so every AFC user with two units (e.g. BoxTurtle
 * + NightOwl) had a dead Eject button (prestonbrown/helixscreen#1258).
 *
 * ams_dispatch_backend_action() now owns those five actions for both panels.
 * These tests pin the contract: the backend-only actions are claimed, and the
 * panel-specific ones are declined so each panel's own switch still sees them.
 */

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_slot_resolver.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using MenuAction = helix::ui::AmsContextMenu::MenuAction;

namespace {

/// Install a 4-slot mock backend so dispatch reaches real backend calls
/// rather than short-circuiting on the "no MFS available" guard.
void install_mock_backend() {
    helix::AmsState::instance().init_subjects(true);
    auto mock = helix::AmsBackend::create_mock(4);
    helix::AmsState::instance().set_backend(std::move(mock));
    helix::AmsState::instance().sync_from_backend();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: claims every backend-only action",
                 "[ui][ams][context_menu][dispatch][1258]") {
    install_mock_backend();

    // These five must be handled centrally. If any one of them stops being
    // claimed here, the Overview panel silently swallows it again — which is
    // exactly the #1258 failure mode.
    const MenuAction shared[] = {MenuAction::EJECT, MenuAction::RECOVER_POSITION,
                                 MenuAction::CHECK_GATE, MenuAction::PRELOAD,
                                 MenuAction::CLEAR_SPOOL};

    for (MenuAction action : shared) {
        INFO("action index = " << static_cast<int>(action));
        CHECK(helix::ui::ams_dispatch_backend_action(action, 0, nullptr));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: declines panel-specific actions",
                 "[ui][ams][context_menu][dispatch][1258]") {
    install_mock_backend();

    // These open panel-owned modals or route through the panel's sidebar, so
    // they must fall through to the caller's own switch. Claiming one here
    // would make it a no-op in BOTH panels.
    const MenuAction panel_owned[] = {MenuAction::LOAD, MenuAction::UNLOAD, MenuAction::EDIT,
                                      MenuAction::SPOOLMAN, MenuAction::SCAN_QR};

    for (MenuAction action : panel_owned) {
        INFO("action index = " << static_cast<int>(action));
        CHECK_FALSE(helix::ui::ams_dispatch_backend_action(action, 0, nullptr));
    }

    // CANCELLED is a dismissal, not an operation — never claimed.
    CHECK_FALSE(helix::ui::ams_dispatch_backend_action(MenuAction::CANCELLED, 0, nullptr));
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: EJECT reaches the backend",
                 "[ui][ams][context_menu][dispatch][1258]") {
    install_mock_backend();

    auto* backend = static_cast<helix::AmsBackendMock*>(helix::AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    helix::SlotInfo info;
    info.slot_index = 1;
    info.material = "PLA";
    info.status = helix::SlotStatus::AVAILABLE;
    helix::test::apply_edit(*backend, 1, info);
    helix::AmsState::instance().sync_from_backend();

    // The whole point of #1258: the tap must actually arrive at the backend,
    // not be consumed by a switch that has no case for it.
    REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::EJECT, 1, nullptr));
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: claims actions even with no backend",
                 "[ui][ams][context_menu][dispatch][1258]") {
    helix::AmsState::instance().init_subjects(true);
    helix::AmsState::instance().set_backend(nullptr);

    // With no MFS the user still gets a warning toast — the action is handled,
    // so the caller must not fall through and double-report it.
    CHECK(helix::ui::ams_dispatch_backend_action(MenuAction::EJECT, 0, nullptr));
    CHECK(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 0, nullptr));

    // Panel-specific actions are still declined regardless of backend state.
    CHECK_FALSE(helix::ui::ams_dispatch_backend_action(MenuAction::EDIT, 0, nullptr));
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: clears the external spool with no backend",
                 "[ui][ams][context_menu][dispatch][clear_spool]") {
    helix::AmsState::instance().init_subjects(true);
    helix::AmsState::instance().set_backend(nullptr);
    helix::SlotInfo ext;
    ext.slot_index = -2;
    ext.global_index = -2;
    ext.spoolman_id = 139;
    ext.material = "ASA";
    helix::AmsState::instance().set_external_spool_info(ext);
    REQUIRE(helix::AmsState::instance().get_external_spool_info().has_value());

    CHECK(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, -2, nullptr));

    const auto after = helix::AmsState::instance().get_external_spool_info();
    CHECK((!after || (after->spoolman_id == 0 && after->material.empty())));
}

// The menu publishes "ams_slot_is_loaded" / "ams_slot_can_load" into the
// process-wide XML subject registry, which resolves by name and holds a raw
// pointer. Those subjects used to be instance members deinited in the
// destructor, so every destroyed menu left the two names pointing at storage it
// no longer owned — and three separate owners construct an AmsContextMenu
// (AmsPanel, AmsOverviewPanel, ExternalSpoolMenu), so the first teardown
// poisoned the names for the others.
//
// The nightly ASan run caught the consequence rather than the cause: a later
// lv_xml_create() binding ams_context_menu.xml called
// lv_subject_add_observer_obj() through the stale pointer, read a reused
// allocation's subs_ll.n_size as 0, and memzero'd an observer into a 16-byte
// heap block. The LV_SUBJECT_TYPE_INVALID guard did not catch it precisely
// because the storage was live garbage rather than zeroed.
//
// The assertion is deliberately on the ADDRESS, not on the subject's contents.
// lv_subject_deinit() resets neither `type` nor `subs_ll.n_size` (it only frees
// the observer nodes), so a deinited subject is indistinguishable from a live
// one by inspection — the defect was never "deinited", it was "freed". Reading
// through the stale pointer to prove that would be UB and would only show up
// under a sanitizer. Comparing pointer values is total and deterministic.
TEST_CASE_METHOD(LVGLUITestFixture, "ams context menu: subjects outlive every menu instance",
                 "[ui][ams][context_menu][subjects]") {
    auto menu = std::make_unique<helix::ui::AmsContextMenu>();
    const auto* object_lo = reinterpret_cast<const std::byte*>(menu.get());
    const auto* object_hi = object_lo + sizeof(helix::ui::AmsContextMenu);

    REQUIRE(lv_xml_get_subject(nullptr, "ams_slot_is_loaded") != nullptr);
    lv_subject_t* can_load_before = lv_xml_get_subject(nullptr, "ams_slot_can_load");
    REQUIRE(can_load_before != nullptr);

    menu.reset(); // storage freed — the registry still resolves both names

    for (const char* name : {"ams_slot_is_loaded", "ams_slot_can_load"}) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        INFO("subject: " << name);
        REQUIRE(subject != nullptr);
        const auto* addr = reinterpret_cast<const std::byte*>(subject);
        CHECK_FALSE((addr >= object_lo && addr < object_hi));
    }

    // A second menu must resolve to the SAME storage, not re-point the shared
    // names at its own members — three owners construct these menus, and the
    // survivors keep using whatever the last registration left behind.
    auto second = std::make_unique<helix::ui::AmsContextMenu>();
    CHECK(lv_xml_get_subject(nullptr, "ams_slot_can_load") == can_load_before);
    second.reset();
    CHECK(lv_xml_get_subject(nullptr, "ams_slot_can_load") == can_load_before);
}

// ============================================================================
// Clear Spool vs an active print
// ============================================================================

namespace {

/// Pin the print lifecycle for one case and restore a machine-free state on
/// the way out, even when an assertion throws. The lifecycle subject outlives
/// every test in this binary (the fixture resets plain data, not subject
/// values), and CLEAR_SPOOL's behaviour depends on it: a case that died with
/// Printing latched would change what any later test's clear is allowed to do.
struct LifecycleGuard {
    explicit LifecycleGuard(PrintState state) {
        if (state == PrintState::Preparing) {
            // Preparing is phase-derived, not a print_stats state, so the wire
            // driver cannot name it, so the enum value is the sanctioned route
            // (print_state_test_drivers.h).
            lv_subject_set_int(get_printer_state().print_state().get_print_lifecycle_subject(),
                               static_cast<int>(PrintState::Preparing));
        } else {
            helix::test::set_wire_state(get_printer_state(), wire_job_state(state));
        }
    }
    ~LifecycleGuard() {
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
    }

  private:
    static helix::PrintJobState wire_job_state(PrintState state) {
        switch (state) {
        case PrintState::Printing:
            return helix::PrintJobState::PRINTING;
        case PrintState::Paused:
            return helix::PrintJobState::PAUSED;
        case PrintState::Idle:
        case PrintState::Complete:
        case PrintState::Cancelled:
        case PrintState::Error:
        case PrintState::Preparing:
            break;
        }
        return helix::PrintJobState::STANDBY;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams dispatch: Clear Spool refuses on the lane feeding an active print",
                 "[ui][ams][context_menu][dispatch][1661]") {
    install_mock_backend();
    auto* backend = static_cast<helix::AmsBackendMock*>(helix::AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    // The warning hook is the seam the commit-slot tests use for this same
    // dispatch function; severity is pinned by which hook the toast lands in
    // (an info-severity toast would reach the info hook and this stays empty).
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&](const std::string& msg) { warnings.push_back(msg); });

    // create_mock() seeds slot 0 as the loaded, current lane, the one a job
    // draws from. Everything the clear would erase is still there afterwards.
    REQUIRE(backend->slot_is_actively_loaded(0));

    size_t warnings_index = 0;
    for (const PrintState busy :
         {PrintState::Preparing, PrintState::Printing, PrintState::Paused}) {
        INFO("lifecycle = " << static_cast<int>(busy));
        LifecycleGuard hold(busy);

        const helix::SlotInfo before = backend->get_slot_info(0);
        REQUIRE_FALSE(before.material.empty()); // something to refuse erasing

        // Handled (true) with a refusal toast, not silently swallowed.
        REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 0, nullptr));
        // Exactly one refusal per lifecycle state, so a state that stops
        // refusing fails here rather than being averaged into the final count.
        CHECK(warnings.size() == warnings_index + 1);
        ++warnings_index;

        const helix::SlotInfo after = backend->get_slot_info(0);
        CHECK(after.material == before.material);
        CHECK(after.color_rgb == before.color_rgb);
        CHECK(after.color_name == before.color_name);
        CHECK(after.spoolman_id == before.spoolman_id);
        CHECK(after.remaining_weight_g == before.remaining_weight_g);
    }

    helix::ui::set_test_notification_warning_hook(nullptr);

    REQUIRE(warnings.size() == 3);
    for (const std::string& message : warnings) {
        // The mock speaks Happy Hare, whose positions are gates (1-based).
        CHECK(message.find("Gate 1") != std::string::npos);
        CHECK(message.find("feeding the current print") != std::string::npos);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams dispatch: Clear Spool still clears other lanes mid-print and any lane idle",
                 "[ui][ams][context_menu][dispatch][1661]") {
    install_mock_backend();
    auto* backend = static_cast<helix::AmsBackendMock*>(helix::AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    SECTION("a lane the job is not drawing from clears mid-print") {
        LifecycleGuard hold(PrintState::Printing);
        REQUIRE_FALSE(backend->slot_is_actively_loaded(1));
        REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 1, nullptr));

        const helix::SlotInfo after = backend->get_slot_info(1);
        CHECK(after.material.empty());
        CHECK(after.spoolman_id == 0);
    }

    SECTION("the loaded lane clears once the machine is free") {
        LifecycleGuard hold(PrintState::Idle);
        REQUIRE(backend->slot_is_actively_loaded(0));
        REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 0, nullptr));

        const helix::SlotInfo after = backend->get_slot_info(0);
        CHECK(after.material.empty());
        CHECK(after.spoolman_id == 0);
    }
}

namespace {

/// A backend whose firmware refuses part of the clear, the way Happy Hare's
/// Spoolman pull mode and its mid-print backstop do.
class PartialClearMock : public helix::AmsBackendMock {
  public:
    using helix::AmsBackendMock::AmsBackendMock;
    helix::AmsError apply_user_edit(int slot_index, const helix::SlotInfo& info,
                                    const helix::ams::Observation& declared) override {
        (void)helix::AmsBackendMock::apply_user_edit(slot_index, info, declared);
        helix::AmsError partial(helix::AmsResult::COMMAND_FAILED, "firmware refused",
                                "Couldn't clear", "HelixScreen cleared its own copy.");
        partial.partially_applied = true;
        return partial;
    }
    void clear_slot_override(int slot_index) override {
        cleared_slots.push_back(slot_index);
        helix::AmsBackendMock::clear_slot_override(slot_index);
    }
    std::vector<int> cleared_slots;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams dispatch: a partially applied Clear Spool still drops the local record",
                 "[ui][ams][context_menu][dispatch][1661]") {
    helix::AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<PartialClearMock>(4);
    PartialClearMock* backend = owned.get();
    helix::AmsState::instance().set_backend(std::move(owned));
    helix::AmsState::instance().sync_from_backend();

    REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 1, nullptr));

    // The error says HelixScreen cleared its own copy; that has to be true.
    REQUIRE(backend->cleared_slots.size() == 1);
    CHECK(backend->cleared_slots.front() == 1);
}

// ============================================================================
// An insert the hardware read nothing about (prestonbrown/helixscreen#1710)
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams insert: an unverified insert offers Clear, which runs Clear Spool",
                 "[ui][ams][dispatch][insert_rule]") {
    install_mock_backend();
    auto* backend = static_cast<helix::AmsBackendMock*>(helix::AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });
    helix::ui::reset_insert_offers_for_test();

    SECTION("a flapping gate asks once, not on every edge") {
        LifecycleGuard hold(PrintState::Idle);
        for (int edge = 0; edge < 5; ++edge) {
            helix::ui::offer_clear_after_unverified_insert(0);
        }
        CHECK(toasts.size() == 1);

        // Another lane is its own question.
        helix::ui::offer_clear_after_unverified_insert(2);
        CHECK(toasts.size() == 2);

        // New details on the lane are a new question.
        helix::SlotInfo edited = backend->get_slot_info(0);
        edited.material = "ASA";
        REQUIRE(helix::test::apply_edit(*backend, 0, edited).success());
        helix::ui::offer_clear_after_unverified_insert(0);
        CHECK(toasts.size() == 3);
    }

    SECTION("offered on a free machine, and the button clears the lane") {
        LifecycleGuard hold(PrintState::Idle);
        REQUIRE_FALSE(backend->get_slot_info(0).material.empty());

        helix::ui::offer_clear_after_unverified_insert(0);
        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0].first == ToastSeverity::INFO);
        CHECK(toasts[0].second.find("Gate 1") != std::string::npos);
        // Asking changes nothing on its own.
        CHECK_FALSE(backend->get_slot_info(0).material.empty());

        REQUIRE(helix::ui::fire_last_toast_action());
        CHECK(backend->get_slot_info(0).material.empty());
        CHECK(backend->get_slot_info(0).spoolman_id == 0);
    }

    SECTION("Clear does nothing once the lane changed since the notice") {
        LifecycleGuard hold(PrintState::Idle);
        helix::ui::offer_clear_after_unverified_insert(0);
        REQUIRE(toasts.size() == 1);

        helix::SlotInfo edited = backend->get_slot_info(0);
        edited.material = "ASA";
        REQUIRE(helix::test::apply_edit(*backend, 0, edited).success());
        REQUIRE(backend->get_slot_info(0).material == "ASA");

        REQUIRE(helix::ui::fire_last_toast_action());
        CHECK(backend->get_slot_info(0).material == "ASA");
    }

    SECTION("not offered on a lane with nothing to clear") {
        LifecycleGuard hold(PrintState::Idle);
        REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 1, nullptr));
        toasts.clear();
        helix::ui::offer_clear_after_unverified_insert(1);
        CHECK(toasts.empty());
    }

    SECTION("not offered on the lane feeding the print") {
        LifecycleGuard hold(PrintState::Printing);
        REQUIRE(backend->slot_is_actively_loaded(0));
        helix::ui::offer_clear_after_unverified_insert(0);
        CHECK(toasts.empty());
        CHECK_FALSE(helix::ui::fire_last_toast_action());
    }

    SECTION("offered on another lane mid-print") {
        LifecycleGuard hold(PrintState::Printing);
        REQUIRE_FALSE(backend->slot_is_actively_loaded(1));
        helix::ui::offer_clear_after_unverified_insert(1);
        CHECK(toasts.size() == 1);
    }

    helix::ui::set_test_toast_hook(nullptr);
}

// ============================================================================
// Clear error: a fault that outlived its dialog (prestonbrown/helixscreen#2049)
// ============================================================================

namespace {

/// A backend latching a fault on one lane, the way AFC keeps error_state after
/// the recovery that closed its dialog.
class LatchedFaultMock : public helix::AmsBackendMock {
  public:
    using helix::AmsBackendMock::AmsBackendMock;
    bool has_clearable_fault(int slot_index) const override {
        return slot_index == faulted_slot;
    }
    helix::AmsError clear_fault(int slot_index) override {
        cleared_slots.push_back(slot_index);
        return helix::AmsErrorHelper::success();
    }
    int faulted_slot = 2;
    std::vector<int> cleared_slots;
};

LatchedFaultMock* install_latched_fault_mock() {
    helix::AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<LatchedFaultMock>(4);
    LatchedFaultMock* backend = owned.get();
    helix::AmsState::instance().set_backend(std::move(owned));
    helix::AmsState::instance().sync_from_backend();
    return backend;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: Clear error reaches the backend's clear_fault",
                 "[ui][ams][context_menu][dispatch][stale_fault]") {
    LatchedFaultMock* backend = install_latched_fault_mock();

    REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_FAULT, 2, nullptr));
    REQUIRE(backend->cleared_slots == std::vector<int>{2});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams context menu: Clear error shows only on a lane with a clearable fault",
                 "[ui][ams][context_menu][stale_fault]") {
    LatchedFaultMock* backend = install_latched_fault_mock();
    // ams_context_menu.xml is registered lazily by the AMS panels.
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    { helix::ui::AmsContextMenu registers_subjects; }
    lv_subject_t* clear_error = lv_xml_get_subject(nullptr, "ams_slot_clear_error");
    REQUIRE(clear_error != nullptr);

    SECTION("the faulted lane") {
        helix::ui::AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                      /*is_loaded=*/false, backend));
        CHECK(lv_subject_get_int(clear_error) == 2);
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_clear_error");
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));
    }

    SECTION("another lane, after the faulted one was shown") {
        {
            helix::ui::AmsContextMenu first;
            REQUIRE(first.show_near_widget(test_screen(), 2, test_screen(), false, backend));
            REQUIRE(lv_subject_get_int(clear_error) == 2);
        }
        helix::ui::AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/0, test_screen(),
                                      /*is_loaded=*/false, backend));
        CHECK(lv_subject_get_int(clear_error) == 0);
    }

    SECTION("the external spool menu never offers it") {
        lv_subject_set_int(clear_error, 2);
        helix::ui::AmsContextMenu menu;
        REQUIRE(menu.show_for_external_spool(test_screen(), test_screen(), true));
        CHECK(lv_subject_get_int(clear_error) == 0);
    }
}
