// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_tool_mapping.cpp
 * @brief Unit tests for tool mapping interface across AMS backends
 *
 * Tests for tool mapping feature:
 * - owns_tool_mapping_table() virtual method
 * - get_remap_strategy() / remap_ready(), asked through helix::printer::can_remap
 * - get_tool_mapping() virtual method
 * - set_tool_mapping() method
 * - Backend-specific implementations (Mock, AFC, Happy Hare, ACE, ToolChanger)
 */

#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

// =============================================================================
// Base Class Interface Tests
// =============================================================================

TEST_CASE("AmsBackend base class has tool mapping virtual methods",
          "[ams][tool_mapping][interface]") {
    // This test verifies the interface exists by using the mock
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("the mock declares the shape of a filament system by default") {
        // Native, ready, and owning its own tool->slot table — what AFC, CFS,
        // Happy Hare and QIDI all declare.
        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
        CHECK(backend.remap_ready());
        CHECK(helix::printer::can_remap(backend));
        CHECK(backend.owns_tool_mapping_table());
    }

    SECTION("get_tool_mapping returns vector of mappings") {
        auto mapping = backend.get_tool_mapping();

        // Mock with 4 slots should return 4 tool mappings
        REQUIRE(mapping.size() == 4);

        // Default mock should have 1:1 tool-to-slot mapping
        for (size_t i = 0; i < mapping.size(); ++i) {
            CHECK(mapping[i] == static_cast<int>(i));
        }
    }

    SECTION("set_tool_mapping returns AmsError") {
        auto result = backend.set_tool_mapping(0, 2);

        // Mock should succeed
        CHECK(result);
        CHECK(result.technical_msg.empty());
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - Filament System Mode
// =============================================================================

TEST_CASE("Mock backend tool mapping - filament system mode", "[ams][tool_mapping][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("the default mode writes a persistent mapping table") {
        CHECK(helix::printer::remap_is_persistent(backend.get_remap_strategy()));
        CHECK(backend.remap_ready());
        CHECK(backend.owns_tool_mapping_table());
    }

    SECTION("get_tool_mapping returns default 1:1 mapping") {
        auto mapping = backend.get_tool_mapping();

        REQUIRE(mapping.size() == 4);
        CHECK(mapping[0] == 0);
        CHECK(mapping[1] == 1);
        CHECK(mapping[2] == 2);
        CHECK(mapping[3] == 3);
    }

    SECTION("set_tool_mapping updates mapping") {
        auto result = backend.set_tool_mapping(0, 2); // Tool 0 -> Slot 2
        REQUIRE(result);

        auto mapping = backend.get_tool_mapping();
        CHECK(mapping[0] == 2);
        // Other mappings should remain unchanged
        CHECK(mapping[1] == 1);
        CHECK(mapping[2] == 2);
        CHECK(mapping[3] == 3);
    }

    SECTION("set_tool_mapping can remap multiple tools") {
        backend.set_tool_mapping(0, 3);
        backend.set_tool_mapping(1, 2);
        backend.set_tool_mapping(2, 1);
        backend.set_tool_mapping(3, 0);

        auto mapping = backend.get_tool_mapping();
        CHECK(mapping[0] == 3);
        CHECK(mapping[1] == 2);
        CHECK(mapping[2] == 1);
        CHECK(mapping[3] == 0);
    }

    SECTION("set_tool_mapping validates tool number") {
        // Invalid tool number (too high)
        auto result = backend.set_tool_mapping(99, 0);
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::INVALID_TOOL);
    }

    SECTION("set_tool_mapping validates slot number") {
        // Invalid slot number (too high)
        auto result = backend.set_tool_mapping(0, 99);
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::INVALID_SLOT);
    }

    SECTION("set_tool_mapping rejects negative values") {
        auto result1 = backend.set_tool_mapping(-1, 0);
        CHECK_FALSE(result1);

        auto result2 = backend.set_tool_mapping(0, -1);
        CHECK_FALSE(result2);
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - Tool Changer Mode
// =============================================================================

TEST_CASE("Mock backend tool mapping - tool changer mode",
          "[ams][tool_mapping][mock][toolchanger]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.set_tool_changer_mode(true);
    REQUIRE(backend.start());

    SECTION("tool changer mode declares what the real tool changer declares") {
        // It used to declare the opposite: RemapStrategy::None and "not
        // supported", while AmsBackendToolChanger declares Native and remaps via
        // ASSIGN_TOOL. Its table is identity — tools ARE slots — but identity is
        // still a real table the firmware owns and rewrites.
        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
        CHECK(helix::printer::can_remap(backend));
        CHECK(backend.owns_tool_mapping_table());
    }

    backend.stop();
}

// =============================================================================
// Note: AFC and Happy Hare backend tests are covered in their respective
// test files (test_ams_backend_afc.cpp, test_ams_backend_happy_hare.cpp)
// which have proper test helper classes with friend access.
// This file focuses on Mock backend and interface tests.
// =============================================================================

// =============================================================================
// Edge Cases and Integration
// =============================================================================

TEST_CASE("Tool mapping edge cases", "[ams][tool_mapping][edge]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("multiple tools can map to same slot") {
        // This is valid - e.g., T0 and T1 both use slot 0
        backend.set_tool_mapping(0, 0);
        backend.set_tool_mapping(1, 0);

        auto mapping = backend.get_tool_mapping();
        CHECK(mapping[0] == 0);
        CHECK(mapping[1] == 0);
    }

    SECTION("tool mapping affects system_info") {
        backend.set_tool_mapping(0, 3);

        auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() == 4);
        CHECK(info.tool_to_slot_map[0] == 3);
    }

    backend.stop();
}

TEST_CASE("Tool mapping with system_info integration", "[ams][tool_mapping][integration]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("tool_to_slot_map is published for a table-owning backend") {
        // AmsSystemInfo::supports_tool_mapping used to carry a second copy of
        // owns_tool_mapping_table()'s answer and was pinned here to agree with
        // it. The field is gone; what a consumer actually needs from
        // system_info is the table itself.
        REQUIRE(backend.owns_tool_mapping_table());
        CHECK_FALSE(backend.get_system_info().tool_to_slot_map.empty());
    }

    SECTION("tool changer mode publishes an identity table") {
        // owns_tool_mapping_table() answers !snapmaker_mode_, so asserting it
        // alone would pass with or without the line above. What the mode
        // actually changes is the table's SHAPE: tools ARE slots, so it is
        // identity, and that is what a consumer reads.
        backend.set_tool_changer_mode(true);
        REQUIRE(backend.start());

        const auto& map = backend.get_system_info().tool_to_slot_map;
        REQUIRE(map.size() == 4);
        for (size_t i = 0; i < map.size(); ++i) {
            CHECK(map[i] == static_cast<int>(i));
        }
    }

    backend.stop();
}

// =============================================================================
// Backend Comparison Tests (Mock backend variations)
// =============================================================================

TEST_CASE("Tool mapping capabilities vary by backend mode", "[ams][tool_mapping][comparison]") {
    SECTION("Mock filament system writes a persistent mapping") {
        AmsBackendMock mock(4);
        mock.set_operation_delay(0);
        REQUIRE(mock.start());

        CHECK(helix::printer::can_remap(mock));
        CHECK(helix::printer::remap_is_persistent(mock.get_remap_strategy()));

        mock.stop();
    }

    SECTION("Mock in Snapmaker mode remaps without writing a table") {
        // The mode that genuinely differs: SnapmakerNative reaches the printer
        // through a pre-print send, so the pick is honored and nothing persists.
        AmsBackendMock mock(4);
        mock.set_operation_delay(0);
        mock.set_snapmaker_mode(true);
        REQUIRE(mock.start());

        CHECK(helix::printer::can_remap(mock));
        CHECK_FALSE(helix::printer::remap_is_persistent(mock.get_remap_strategy()));
        CHECK_FALSE(mock.owns_tool_mapping_table());

        mock.stop();
    }
}
