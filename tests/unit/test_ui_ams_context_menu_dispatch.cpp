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
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <cstddef>
#include <memory>

#include "../catch_amalgamated.hpp"

using MenuAction = helix::ui::AmsContextMenu::MenuAction;

namespace {

/// Install a 4-slot mock backend so dispatch reaches real backend calls
/// rather than short-circuiting on the "no MFS available" guard.
void install_mock_backend() {
    AmsState::instance().init_subjects(false);
    auto mock = AmsBackend::create_mock(4);
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().sync_from_backend();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: claims every backend-only action",
                 "[ui][ams][context_menu][dispatch][1258]") {
    install_mock_backend();

    // These five must be handled centrally. If any one of them stops being
    // claimed here, the Overview panel silently swallows it again — which is
    // exactly the #1258 failure mode.
    const MenuAction shared[] = {MenuAction::EJECT, MenuAction::RECOVER_POSITION,
                                 MenuAction::SELECT_GATE, MenuAction::CHECK_GATE,
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

    auto* backend = static_cast<AmsBackendMock*>(AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    SlotInfo info;
    info.slot_index = 1;
    info.material = "PLA";
    info.status = SlotStatus::AVAILABLE;
    backend->set_slot_info(1, info);
    AmsState::instance().sync_from_backend();

    // The whole point of #1258: the tap must actually arrive at the backend,
    // not be consumed by a switch that has no case for it.
    REQUIRE(helix::ui::ams_dispatch_backend_action(MenuAction::EJECT, 1, nullptr));
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams dispatch: claims actions even with no backend",
                 "[ui][ams][context_menu][dispatch][1258]") {
    AmsState::instance().init_subjects(false);
    AmsState::instance().set_backend(nullptr);

    // With no MFS the user still gets a warning toast — the action is handled,
    // so the caller must not fall through and double-report it.
    CHECK(helix::ui::ams_dispatch_backend_action(MenuAction::EJECT, 0, nullptr));
    CHECK(helix::ui::ams_dispatch_backend_action(MenuAction::CLEAR_SPOOL, 0, nullptr));

    // Panel-specific actions are still declined regardless of backend state.
    CHECK_FALSE(helix::ui::ams_dispatch_backend_action(MenuAction::EDIT, 0, nullptr));
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
