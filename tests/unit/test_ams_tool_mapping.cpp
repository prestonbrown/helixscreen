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

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
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

// =============================================================================
// Routing provenance
// =============================================================================
//
// A routing that sends every routed tool to one lane reads two ways and the
// numbers cannot separate them: a half-published firmware table degrades to it,
// and a user pointing several tools at one spool builds it on purpose. What
// stands behind the table is the only thing that tells them apart, so the base
// class records the assignments it is asked to make and reports what it knows.

namespace {

/// A backend that echoes its tool table back from the printer, the way AFC,
/// Happy Hare and the non-K1 CFS forks do.
class EchoingBackend : public AmsBackendMock {
  public:
    explicit EchoingBackend(int slots) : AmsBackendMock(slots) {}

    [[nodiscard]] bool reports_firmware_tool_mapping() const override {
        return true;
    }
    [[nodiscard]] uint64_t firmware_tool_mapping_generation() const override {
        return generation;
    }

    uint64_t generation = 0;
};

/// A backend whose table arrives from somewhere else entirely, the way a
/// subscription parse or a plugin's save_variables row delivers one.
class SeededBackend : public AmsBackendMock {
  public:
    explicit SeededBackend(int slots) : AmsBackendMock(slots) {}

    [[nodiscard]] std::vector<int> get_tool_mapping() const override {
        return seeded;
    }

    std::vector<int> seeded;
};

/// A backend whose mapping verb is refused, the way ACE and an idle U1 refuse.
class RefusingBackend : public AmsBackendMock {
  public:
    explicit RefusingBackend(int slots) : AmsBackendMock(slots) {}

  protected:
    AmsError set_tool_mapping_impl(int, int) override {
        return AmsErrorHelper::not_supported("no mapping on this backend");
    }
};

} // namespace

TEST_CASE("Routing provenance: a table nobody has vouched for stays unvouched",
          "[ams][tool_mapping][provenance][1422]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    // A backend that neither echoes its table nor has been driven from our UI
    // has nothing to say about where its routing came from.
    CHECK(backend.tool_mapping_origin() == ToolMappingOrigin::Unvouched);

    backend.stop();
}

TEST_CASE("Routing provenance: aiming a tool at a lane vouches for the routing",
          "[ams][tool_mapping][provenance][1422]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("a share our UI built is the user's own answer") {
        // T1 onto T0's lane — the many-to-one our context menu offers with a
        // warning rather than a refusal.
        REQUIRE(backend.set_tool_mapping(1, 0));
        CHECK(backend.get_tool_mapping()[0] == 0);
        CHECK(backend.get_tool_mapping()[1] == 0);
        CHECK(backend.tool_mapping_origin() == ToolMappingOrigin::Deliberate);
    }

    SECTION("a table that has moved off the choice speaks for itself again") {
        REQUIRE(backend.set_tool_mapping(1, 0));
        REQUIRE(backend.tool_mapping_origin() == ToolMappingOrigin::Deliberate);

        // The routing changes underneath us, so the recorded choice no longer
        // describes what the backend reports.
        REQUIRE(backend.set_tool_mapping(1, 1));
        REQUIRE(backend.set_tool_mapping(0, 2));
        auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map[1] == 1);

        AmsBackendMock fresh(4);
        fresh.set_operation_delay(0);
        REQUIRE(fresh.start());
        CHECK(fresh.tool_mapping_origin() == ToolMappingOrigin::Unvouched);
        fresh.stop();
    }

    SECTION("a refused write vouches for nothing") {
        RefusingBackend refusing(4);
        refusing.set_operation_delay(0);
        REQUIRE(refusing.start());
        CHECK_FALSE(refusing.set_tool_mapping(1, 0));
        CHECK(refusing.tool_mapping_origin() == ToolMappingOrigin::Unvouched);
        refusing.stop();
    }

    backend.stop();
}

TEST_CASE("Routing provenance: a printer publishing its own table has already answered",
          "[ams][tool_mapping][provenance][1422]") {
    EchoingBackend backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("claiming to echo is not enough — the echo has to have arrived") {
        backend.generation = 0;
        CHECK(backend.tool_mapping_origin() == ToolMappingOrigin::Unvouched);
    }

    SECTION("a table the firmware has stated needs no second opinion from us") {
        backend.generation = 1;
        CHECK(backend.tool_mapping_origin() == ToolMappingOrigin::Deliberate);
    }

    backend.stop();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Routing provenance reaches the preview through AmsState",
                 "[ams][tool_mapping][provenance][1422]") {
    // The composition the print-status preview actually calls, not a hand-rolled
    // mirror of it: the backend's routing and its provenance meeting in
    // AmsState::routed_tool_colors().
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<AmsBackendMock>(4);
    auto* backend = owned.get();
    backend->set_operation_delay(0);
    REQUIRE(backend->start());
    ams.set_backend(std::move(owned));

    SECTION("a share the user built publishes the lane they all print from") {
        // Every tool aimed at lane 0 from our UI. The print comes out one solid
        // colour and the preview has to say so.
        for (int tool = 0; tool < 4; ++tool) {
            REQUIRE(backend->set_tool_mapping(tool, 0));
        }
        REQUIRE(backend->tool_mapping_origin() == ToolMappingOrigin::Deliberate);

        const uint32_t lane0 = backend->get_slot_info(0).color_rgb;
        const auto colors = ams.routed_tool_colors();
        REQUIRE(colors.size() == 4);
        CHECK(colors[0] == lane0);
        CHECK(colors[1] == lane0);
        CHECK(colors[2] == lane0);
        CHECK(colors[3] == lane0);
    }

    ams.clear_backends();
    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A collapsed table nobody authored leaves the preview palette alone",
                 "[ams][tool_mapping][provenance][1422]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<SeededBackend>(4);
    auto* backend = owned.get();
    backend->set_operation_delay(0);
    REQUIRE(backend->start());
    // The same routing the user's own share produces, arriving instead as a
    // table that names one lane for every tool. Nothing chose it, so the file's
    // own palette outranks a four-colour model painted in one lane's colour.
    backend->seeded = {0, 0, 0, 0};
    ams.set_backend(std::move(owned));

    REQUIRE(backend->tool_mapping_origin() == ToolMappingOrigin::Unvouched);
    CHECK(ams.routed_tool_colors().empty());

    ams.clear_backends();
    ams.deinit_subjects();
}
