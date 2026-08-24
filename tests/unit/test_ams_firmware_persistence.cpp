// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_firmware_persistence.cpp
 * @brief Tests for has_firmware_spool_persistence() across AMS backends
 */

#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

// =============================================================================
// has_firmware_spool_persistence() tests
// =============================================================================

TEST_CASE("AmsBackendMock: no firmware spool persistence by default",
          "[ams][backend][spool-persistence]") {
    auto mock = AmsBackendMock::create_mock();
    REQUIRE_FALSE(mock->has_firmware_spool_persistence());
}

TEST_CASE("AmsBackendHappyHare: has firmware spool persistence",
          "[ams][backend][spool-persistence]") {
    // Construct directly with nullptr — constructor sets capability flags
    auto backend = std::make_unique<AmsBackendHappyHare>(nullptr, nullptr);
    REQUIRE(backend->has_firmware_spool_persistence());
}

TEST_CASE("AmsBackendAfc: has firmware spool persistence", "[ams][backend][spool-persistence]") {
    auto backend = std::make_unique<AmsBackendAfc>(nullptr, nullptr);
    REQUIRE(backend->has_firmware_spool_persistence());
}

TEST_CASE("AmsBackendToolChanger: no firmware spool persistence",
          "[ams][backend][spool-persistence]") {
    auto backend = std::make_unique<AmsBackendToolChanger>(nullptr, nullptr);
    REQUIRE_FALSE(backend->has_firmware_spool_persistence());
}

TEST_CASE("AmsBackendAd5xIfs: no firmware spool persistence", "[ams][backend][spool-persistence]") {
    // IFS firmware persists color + material type but NOT spoolman_id,
    // so ToolState handles spool assignment persistence via Moonraker DB.
    auto backend = std::make_unique<AmsBackendAd5xIfs>(nullptr, nullptr);
    REQUIRE_FALSE(backend->has_firmware_spool_persistence());
}

// =============================================================================
// printer_reports_spool_ids() tests
// =============================================================================

TEST_CASE("printer_reports_spool_ids capability", "[ams][capabilities]") {
    // Qualified call pins the BASE default (false), not the AFC override.
    auto afc = std::make_unique<AmsBackendAfc>(nullptr, nullptr);
    CHECK_FALSE(afc->AmsBackend::printer_reports_spool_ids());
    CHECK(afc->printer_reports_spool_ids());
    auto hh = std::make_unique<AmsBackendHappyHare>(nullptr, nullptr);
    CHECK(hh->printer_reports_spool_ids());
}

// =============================================================================
// set_slot_info() mapped_tool propagation
//
// Cross-backend regression tests: the slot edit modal calls set_slot_info() with
// the new mapped_tool. Each backend must handle that consistently — either by
// updating registry/local state and emitting the right G-code (where supported),
// or by leaving live state untouched when the caller passes the default -1.
// =============================================================================

TEST_CASE("AmsBackendMock: set_slot_info propagates mapped_tool change",
          "[ams][backend][slot-edit-remap]") {
    auto mock = AmsBackendMock::create_mock();

    // Mock seeds slot 0 → T0 by default.
    auto initial = mock->get_slot_info(0);
    REQUIRE(initial.mapped_tool == 0);

    // Remap slot 0 → T2 through the slot edit path.
    SlotInfo info = initial;
    info.mapped_tool = 2;
    REQUIRE(mock->set_slot_info(0, info, /*persist=*/true).result == AmsResult::SUCCESS);

    REQUIRE(mock->get_slot_info(0).mapped_tool == 2);
}

TEST_CASE("AmsBackendMock: set_slot_info ignores default mapped_tool (-1)",
          "[ams][backend][slot-edit-remap]") {
    auto mock = AmsBackendMock::create_mock();

    // Spoolman polling builds a default-constructed SlotInfo; live mapping must survive.
    SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";

    REQUIRE(mock->set_slot_info(2, info, /*persist=*/false).result == AmsResult::SUCCESS);
    REQUIRE(mock->get_slot_info(2).mapped_tool == 2);
}

namespace {
// Minimal helper: capture G-code strings without dispatching to a real Moonraker.
class ToolChangerGcodeCapture : public AmsBackendToolChanger {
  public:
    ToolChangerGcodeCapture() : AmsBackendToolChanger(nullptr, nullptr) {}
    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
    std::vector<std::string> captured;
};
} // namespace

TEST_CASE("AmsBackendToolChanger: set_slot_info emits ASSIGN_TOOL on mapped_tool change",
          "[ams][backend][slot-edit-remap]") {
    ToolChangerGcodeCapture backend;
    // set_discovered_tools() calls initialize_tools() which seeds slots — sufficient
    // for set_slot_info, no live MoonrakerClient (and therefore no start()) needed.
    backend.set_discovered_tools({"tool0", "tool1", "tool2", "tool3"});

    // Backend seeds slot 0 → T0. Remap slot 0 to respond to G-code T2.
    SlotInfo info = backend.get_slot_info(0);
    info.mapped_tool = 2;
    REQUIRE(backend.set_slot_info(0, info, /*persist=*/true).result == AmsResult::SUCCESS);

    bool emitted = false;
    for (const auto& g : backend.captured) {
        if (g == "ASSIGN_TOOL TOOL=tool0 N=2") {
            emitted = true;
            break;
        }
    }
    REQUIRE(emitted);
    REQUIRE(backend.get_slot_info(0).mapped_tool == 2);
}

TEST_CASE("AmsBackendToolChanger: set_slot_info ignores default mapped_tool (-1)",
          "[ams][backend][slot-edit-remap]") {
    ToolChangerGcodeCapture backend;
    backend.set_discovered_tools({"tool0", "tool1", "tool2", "tool3"});

    SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";

    REQUIRE(backend.set_slot_info(1, info, /*persist=*/true).result == AmsResult::SUCCESS);

    for (const auto& g : backend.captured) {
        REQUIRE(g.rfind("ASSIGN_TOOL ", 0) != 0);
    }
    REQUIRE(backend.get_slot_info(1).mapped_tool == 1);
}
