// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_firmware_persistence.cpp
 * @brief Firmware spool capabilities across AMS backends
 */

#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

// =============================================================================
// printer_reports_spool_ids() tests
// =============================================================================

TEST_CASE("printer_reports_spool_ids capability", "[ams][capabilities]") {
    CHECK_FALSE(helix::BackendTraits{}.printer_reports_spool_ids);
    auto afc = std::make_unique<helix::AmsBackendAfc>(nullptr, nullptr);
    CHECK(afc->printer_reports_spool_ids());
    auto hh = std::make_unique<helix::AmsBackendHappyHare>(nullptr, nullptr);
    CHECK(hh->printer_reports_spool_ids());
}

// =============================================================================
// apply_user_edit() mapped_tool propagation
//
// Cross-backend regression tests: the slot edit modal calls apply_user_edit() with
// the new mapped_tool. Each backend must handle that consistently — either by
// updating registry/local state and emitting the right G-code (where supported),
// or by leaving live state untouched when the caller passes the default -1.
// =============================================================================

TEST_CASE("AmsBackendMock: apply_user_edit propagates mapped_tool change",
          "[ams][backend][slot-edit-remap]") {
    auto mock = helix::AmsBackendMock::create_mock();

    // Mock seeds slot 0 → T0 by default.
    auto initial = mock->get_slot_info(0);
    REQUIRE(initial.mapped_tool == 0);

    // Remap slot 0 → T2 through the slot edit path.
    helix::SlotInfo info = initial;
    info.mapped_tool = 2;
    REQUIRE(helix::test::apply_edit(*mock, 0, info).result == helix::AmsResult::SUCCESS);

    REQUIRE(mock->get_slot_info(0).mapped_tool == 2);
}

namespace {
// Minimal helper: capture G-code strings without dispatching to a real Moonraker.
class ToolChangerGcodeCapture : public helix::AmsBackendToolChanger {
  public:
    ToolChangerGcodeCapture() : helix::AmsBackendToolChanger(nullptr, nullptr) {}
    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    std::vector<std::string> captured;
};
} // namespace

TEST_CASE("AmsBackendToolChanger: apply_user_edit emits ASSIGN_TOOL on mapped_tool change",
          "[ams][backend][slot-edit-remap]") {
    ToolChangerGcodeCapture backend;
    // set_discovered_tools() calls initialize_tools() which seeds slots — sufficient
    // for apply_user_edit, no live MoonrakerClient (and therefore no start()) needed.
    backend.set_discovered_tools({"tool0", "tool1", "tool2", "tool3"});

    // Backend seeds slot 0 → T0. Remap slot 0 to respond to G-code T2.
    helix::SlotInfo info = backend.get_slot_info(0);
    info.mapped_tool = 2;
    REQUIRE(helix::test::apply_edit(backend, 0, info).result == helix::AmsResult::SUCCESS);

    bool emitted = false;
    for (const auto& g : backend.captured) {
        if (g == "ASSIGN_TOOL TOOL=\"tool tool0\" N=2") {
            emitted = true;
            break;
        }
    }
    REQUIRE(emitted);
    REQUIRE(backend.get_slot_info(0).mapped_tool == 2);
}

TEST_CASE("AmsBackendToolChanger: apply_user_edit ignores default mapped_tool (-1)",
          "[ams][backend][slot-edit-remap]") {
    ToolChangerGcodeCapture backend;
    backend.set_discovered_tools({"tool0", "tool1", "tool2", "tool3"});

    helix::SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";

    REQUIRE(helix::test::apply_edit(backend, 1, info).result == helix::AmsResult::SUCCESS);

    for (const auto& g : backend.captured) {
        REQUIRE(g.rfind("ASSIGN_TOOL ", 0) != 0);
    }
    REQUIRE(backend.get_slot_info(1).mapped_tool == 1);
}

// =============================================================================
// lane_record_is_only_identity(): where Keep Spool Info on Eject applies
// =============================================================================

TEST_CASE("Keep Spool Info on Eject applies where the lane record is the only identity",
          "[ams][capabilities][keep_spool]") {
    CHECK(helix::AmsBackendToolChanger(nullptr, nullptr).lane_record_is_only_identity());
    // Firmware that holds the spool decides an eject itself.
    CHECK_FALSE(helix::AmsBackendAfc(nullptr, nullptr).lane_record_is_only_identity());
    CHECK_FALSE(helix::AmsBackendHappyHare(nullptr, nullptr).lane_record_is_only_identity());
    CHECK_FALSE(helix::AmsBackendAd5xIfs(nullptr, nullptr).lane_record_is_only_identity());
}
