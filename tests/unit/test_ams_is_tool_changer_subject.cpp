// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_is_tool_changer_subject.cpp
 * @brief The sidebar must hide filament controls for EVERY tool changer.
 *
 * ams_sidebar.xml gated btn_unload and btn_reset on `ams_type == 4`, the raw
 * value of AmsType::TOOL_CHANGER. That is one of three tool-changer types:
 * SNAPMAKER (7) and any later addition read as "not a tool changer" to XML and
 * get Unload and Reset buttons for a filament path they do not have.
 *
 * is_tool_changer() already answers this in C++. ams_is_tool_changer publishes
 * that answer so XML binds to the predicate instead of re-encoding the enum.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

namespace {

/// A mock that reports an arbitrary AmsType. sync_from_backend() reads
/// get_system_info().type, so the type must be stamped there, not only on
/// get_type().
class TypedBackend : public helix::AmsBackendMock {
  public:
    TypedBackend(helix::AmsType type, int slots) : helix::AmsBackendMock(slots), type_(type) {}

    [[nodiscard]] helix::AmsType get_type() const override {
        return type_;
    }

    [[nodiscard]] helix::AmsSystemInfo get_system_info() const override {
        helix::AmsSystemInfo info = helix::AmsBackendMock::get_system_info();
        info.type = type_;
        return info;
    }

  private:
    helix::AmsType type_;
};

lv_subject_t* install(helix::AmsType type) {
    auto mock = std::make_unique<TypedBackend>(type, 4);
    REQUIRE(mock->start().success());
    helix::AmsState::instance().set_backend(std::move(mock));
    helix::AmsState::instance().init_subjects(true);
    helix::AmsState::instance().sync_from_backend();

    lv_subject_t* subject = helix::AmsState::instance().get_is_tool_changer_subject();
    REQUIRE(subject != nullptr);
    return subject;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ams_is_tool_changer follows is_tool_changer(), not type 4",
                 "[ams][toolchanger][subject]") {
    SECTION("klipper-toolchanger reads as a tool changer") {
        CHECK(lv_subject_get_int(install(helix::AmsType::TOOL_CHANGER)) == 1);
    }

    SECTION("Snapmaker U1 reads as a tool changer") {
        // The regression: type 7 against a hardcoded ref_value="4" read as 0,
        // so a U1 showed Unload and Reset.
        CHECK(lv_subject_get_int(install(helix::AmsType::SNAPMAKER)) == 1);
    }

    SECTION("a filament system does not") {
        CHECK(lv_subject_get_int(install(helix::AmsType::AFC)) == 0);
    }

    SECTION("no AMS does not") {
        CHECK(lv_subject_get_int(install(helix::AmsType::NONE)) == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ams_is_tool_changer agrees with the predicate for every type",
                 "[ams][toolchanger][subject]") {
    // Pin the subject to is_tool_changer() itself rather than a list of types,
    // so a new tool-changer AmsType cannot be added without the subject following.
    for (int raw = 0; raw <= static_cast<int>(helix::AmsType::OPENAMS); ++raw) {
        auto type = static_cast<helix::AmsType>(raw);
        CAPTURE(raw, helix::ams_type_to_string(type));
        CHECK(lv_subject_get_int(install(type)) == (helix::is_tool_changer(type) ? 1 : 0));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ams_is_tool_changer clears when the backends go away",
                 "[ams][toolchanger][subject]") {
    lv_subject_t* subject = install(helix::AmsType::TOOL_CHANGER);
    REQUIRE(lv_subject_get_int(subject) == 1);

    // A stale 1 after teardown keeps the filament controls hidden on whatever
    // connects next.
    helix::AmsState::instance().clear_backends();
    CHECK(lv_subject_get_int(subject) == 0);
}

// ============================================================================
// Tray graphic
// ============================================================================

TEST_CASE("No tool changer draws a shared tray", "[ams][toolchanger][tray]") {
    // ams_detail_update_tray() hides the tray on !has_physical_tray(). The U1
    // mounts its spools on the left and right of the machine, feeding through
    // bowdens into a lane-assist motor unit per side, so there is no container
    // to draw -- but AmsBackendSnapmaker inherited the default `true` and drew
    // one anyway.
    helix::AmsBackendToolChanger tc(nullptr, nullptr);
    CHECK_FALSE(tc.has_physical_tray());

    helix::AmsBackendSnapmaker sm(nullptr, nullptr);
    CHECK_FALSE(sm.has_physical_tray());

    // A hub/selector system keeps its tray: the spools really do sit in a case.
    helix::AmsBackendMock hub(4);
    CHECK(hub.has_physical_tray());
}
