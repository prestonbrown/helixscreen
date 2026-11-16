// SPDX-License-Identifier: GPL-3.0-or-later

// The multiACE mock hardware (HELIX_MOCK_AMS=multiace) emits what the engine's
// get_status() returns: this pins the mock's `ace` frame to the real parser
// and the real backend, so the mock cannot drift from the shape
// AmsBackendMultiAce reads. Parity here is what makes a --test run of the
// multiACE UI meaningful.

#include "../helix_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_multiace.h"
#include "moonraker_client_mock.h"
#include "multiace_status.h"
#include "test_helpers/backend_harness.h"

#include <cstdlib>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

/// HELIX_MOCK_AMS=multiace (plus the MULTIACE_* knobs), restored on teardown.
class MultiaceMockEnv : public HelixTestFixture {
  public:
    MultiaceMockEnv(const char* mode = nullptr, const char* units = nullptr) {
        set("HELIX_MOCK_AMS", "multiace", ams_);
        set("HELIX_MOCK_MULTIACE_MODE", mode, mode_);
        set("HELIX_MOCK_MULTIACE_UNITS", units, units_);
    }
    ~MultiaceMockEnv() override {
        restore("HELIX_MOCK_AMS", ams_);
        restore("HELIX_MOCK_MULTIACE_MODE", mode_);
        restore("HELIX_MOCK_MULTIACE_UNITS", units_);
    }

  private:
    struct EnvValue {
        bool present = false;
        std::string value;
    };
    void set(const char* name, const char* value, EnvValue& prev) {
        if (const char* old = std::getenv(name)) {
            prev = {true, old};
        }
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }
    void restore(const char* name, const EnvValue& prev) {
        if (prev.present) {
            setenv(name, prev.value.c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
    EnvValue ams_, mode_, units_;
};

} // namespace

TEST_CASE_METHOD(MultiaceMockEnv, "mock multiace frame parses as head mode", "[multiace][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);
    const json ace = client.multiace_status_json();

    const multiace::StatusDelta d = multiace::parse_ace_status(ace);
    REQUIRE(d.api_version == std::optional<int>{1});
    REQUIRE(d.mode == std::optional<multiace::Mode>{multiace::Mode::HEAD});
    REQUIRE(d.aces.has_value());
    CHECK(d.aces->size() == 1);

    // The resting route: head 1 to ACE 1 bay 1, a red PLA.
    REQUIRE(d.head_source.has_value());
    REQUIRE(d.head_source->at(0).has_value());
    CHECK(d.head_source->at(0)->ace == 0);
    CHECK(d.head_source->at(0)->slot == 0);
    CHECK(d.head_source->at(0)->color_hex == "FF0000");
    CHECK_FALSE(d.head_source->at(1).has_value());

    // One unit wires one head; the heads past it are side-feeder heads, the
    // engine's own 1:1 rule.
    REQUIRE(d.head_ace.has_value());
    CHECK((*d.head_ace)[0] == 0);
    for (int h = 1; h < 4; ++h) {
        CHECK((*d.head_ace)[static_cast<size_t>(h)] == -1);
    }
    REQUIRE(d.head_feeder.has_value());
    for (int h = 1; h < 4; ++h) {
        CHECK((*d.head_feeder)[static_cast<size_t>(h)]);
    }
    REQUIRE(d.ace_heads.has_value());
    CHECK(*d.ace_heads == std::vector<int>{0});

    // Head mode: head h loads the four bays of the unit wired to it.
    const std::array<multiace::HeadFeed, 4> feed{};
    REQUIRE(d.head_ace.has_value());
    const auto bays = multiace::candidate_bays(multiace::Mode::HEAD, 0, *d.head_ace, feed, 1);
    CHECK(bays == std::vector<std::pair<int, int>>{{0, 0}, {0, 1}, {0, 2}, {0, 3}});
}

namespace {
struct MultiaceTwoUnitEnv : MultiaceMockEnv {
    MultiaceTwoUnitEnv() : MultiaceMockEnv("multi", "2") {}
};
} // namespace

TEST_CASE_METHOD(MultiaceTwoUnitEnv, "mock multiace knobs reach the frame", "[multiace][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);
    const json ace = client.multiace_status_json();

    const multiace::StatusDelta d = multiace::parse_ace_status(ace);
    REQUIRE(d.mode == std::optional<multiace::Mode>{multiace::Mode::MULTI});
    REQUIRE(d.aces.has_value());
    CHECK(d.aces->size() == 2);

    // Multi mode: bay s of every unit feeds head s.
    REQUIRE(d.head_ace.has_value());
    CHECK(multiace::head_for_bay(multiace::Mode::MULTI, 1, 2, *d.head_ace, {}) ==
          std::optional<int>{2});
}

TEST_CASE_METHOD(MultiaceMockEnv, "the real backend reads the mock's frame", "[multiace][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);

    helix::test::StartedBackend<AmsBackendMultiAce> backend;
    backend.fire_status(json{{"ace", client.multiace_status_json()}});

    const AmsSystemInfo info = backend.get_system_info();
    REQUIRE(info.units.size() == 2); // the heads plus ACE 1
    CHECK(info.total_slots == 8);
    CHECK(backend.get_unit_topology(1) == PathTopology::HUB);
    // The resting route names head 1: bays map to it and share its extruder.
    CHECK(backend.get_slot_info(multiace::bay_global(0, 0)).mapped_tool == 0);
    CHECK(backend.get_slot_info(multiace::bay_global(0, 0)).extruder_name == "extruder");
    CHECK(backend.upstream_slot(0) == std::optional<int>{multiace::bay_global(0, 0)});
    CHECK(backend.spool_display_number(0) ==
          backend.spool_display_number(multiace::bay_global(0, 0)));
}

TEST_CASE_METHOD(MultiaceMockEnv, "mock multiace clamps out-of-range ACE and SLOT",
                 "[multiace][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);
    // A malformed command must not reach the route table or the bay seeds:
    // ACE 99 clamps to the one unit, SLOT 99 to the last bay. Head 1 is the
    // one head the single unit wires, and its resting route has to go first.
    REQUIRE(client.gcode_script("ACE_UNLOAD_HEAD HEAD=0") == 0);
    REQUIRE(client.gcode_script("ACE_LOAD_HEAD HEAD=0 ACE=99 SLOT=99") == 0);

    const multiace::StatusDelta d = multiace::parse_ace_status(client.multiace_status_json());
    REQUIRE(d.head_source.has_value());
    REQUIRE(d.head_source->at(0).has_value());
    CHECK(d.head_source->at(0)->ace == 0);
    CHECK(d.head_source->at(0)->slot == 3);
    CHECK(d.head_source->at(0)->type == "PLA");
}

TEST_CASE_METHOD(MultiaceMockEnv, "mock multiace feeds a feeder head from its side port",
                 "[multiace][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);

    // A feeder head loads and unloads through the engine like any head, but
    // no bay is routed to it and head_source stays null.
    REQUIRE(client.gcode_script("ACE_LOAD_HEAD HEAD=2") == 0);
    multiace::StatusDelta d = multiace::parse_ace_status(client.multiace_status_json());
    REQUIRE(d.head_source.has_value());
    CHECK_FALSE(d.head_source->at(2).has_value());

    // The engine's unload-all walks every head whose sensor sees filament,
    // feeder heads included.
    LogCapture log;
    REQUIRE(client.gcode_script("ACE_UNLOAD_ALL_HEADS") == 0);
    CHECK(log.count_containing("unload head 2") > 0);
}

namespace {
struct MultiaceBadModeEnv : MultiaceMockEnv {
    MultiaceBadModeEnv() : MultiaceMockEnv("banana", nullptr) {}
};
} // namespace

TEST_CASE_METHOD(MultiaceBadModeEnv, "mock multiace rejects a mode word it does not know",
                 "[multiace][mock]") {
    LogCapture log;
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);

    // An unknown word falls back to the default with a line saying so, and the
    // frame still carries a real mode.
    CHECK(log.count_containing("HELIX_MOCK_MULTIACE_MODE") > 0);
    const multiace::StatusDelta d = multiace::parse_ace_status(client.multiace_status_json());
    CHECK(d.mode == std::optional<multiace::Mode>{multiace::Mode::HEAD});
}
