// SPDX-License-Identifier: GPL-3.0-or-later

// Rules behind choosing objects to skip before a print starts: when the
// option exists, how a tapped name maps to a defined object (upper-case, as
// Klipper stores names), when every object is picked, and how the file scan's
// list and the full parse's list combine.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "gcode_parser.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "pre_start_exclude.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;
using helix::gcode::GCodeObject;
using helix::gcode::ParsedGCodeFile;

namespace {
GCodeObject object(const std::string& name, glm::vec2 center = {0.0f, 0.0f},
                   std::vector<glm::vec2> polygon = {}) {
    GCodeObject o;
    o.name = name;
    o.center = center;
    o.polygon = std::move(polygon);
    return o;
}
} // namespace

TEST_CASE("The skip button needs [exclude_object], two objects and a G-code file",
          "[pre_start_exclude]") {
    CHECK(pre_start_exclude_available(true, false, 2));
    CHECK(pre_start_exclude_available(true, false, 7));
    CHECK_FALSE(pre_start_exclude_available(false, false, 3));
    CHECK_FALSE(pre_start_exclude_available(true, false, 1));
    CHECK_FALSE(pre_start_exclude_available(true, false, 0));
    CHECK_FALSE(pre_start_exclude_available(true, true, 3));
}

TEST_CASE("A tapped name maps to its defined spelling, compared upper-case",
          "[pre_start_exclude]") {
    const std::vector<std::string> defined = {"Cone_id_0", "Cube_id_1"};
    CHECK(canonical_object_name(defined, "Cube_id_1") == "Cube_id_1");
    CHECK(canonical_object_name(defined, "CUBE_ID_1") == "Cube_id_1");
    CHECK(canonical_object_name(defined, "cone_id_0") == "Cone_id_0");
    CHECK(canonical_object_name(defined, "Cylinder") == "");
    CHECK(canonical_object_name({}, "Cube_id_1") == "");
}

TEST_CASE("Every object picked, as Klipper would count them", "[pre_start_exclude]") {
    CHECK_FALSE(every_object_picked({}, {}));
    CHECK_FALSE(every_object_picked({"A", "B"}, {"A"}));
    CHECK(every_object_picked({"A", "B"}, {"A", "B"}));
    // Klipper upper-cases names, so "part" and "PART" are one object to it.
    CHECK(every_object_picked({"part", "PART"}, {"part"}));
}

TEST_CASE("The parsed file adds what the scan missed, keeping the scan's order",
          "[pre_start_exclude]") {
    ParsedGCodeFile parsed;
    parsed.objects["Alpha"] = object("Alpha", {1, 1}, {{0, 0}, {2, 0}, {2, 2}});
    parsed.objects["late"] = object("late", {9, 9});
    parsed.objects["zed"] = object("zed", {5, 5}); // the scan's "Zed", other case

    const auto merged =
        merge_defined_objects({object("Zed", {3, 3}), object("Alpha", {1, 1})}, &parsed);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].name == "Zed");
    CHECK(merged[0].center == glm::vec2(3.0f, 3.0f)); // the scan's entry wins
    CHECK(merged[1].name == "Alpha");
    CHECK(merged[2].name == "late");

    CHECK(merge_defined_objects({object("Zed")}, nullptr).size() == 1);
}

TEST_CASE("object_infos_from treats a zero centre as no centre", "[pre_start_exclude]") {
    const auto infos = object_infos_from({object("A", {4, 5}, {{0, 0}, {8, 10}}), object("B")});
    REQUIRE(infos.size() == 2);
    CHECK(infos[0].has_center);
    CHECK(infos[0].has_bbox);
    CHECK(infos[0].bbox_max == glm::vec2(8.0f, 10.0f));
    CHECK_FALSE(infos[1].has_center);
    CHECK_FALSE(infos[1].has_bbox);
}

namespace {

/// The mock printer behind get_moonraker_api(), with error toasts captured.
class SendFixture : private helix::PrintSelectGlobalStateReset, public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::vector<std::string> errors;

    SendFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        set_moonraker_api(api.get());
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& m) { errors.push_back(m); });
        client.clear_gcode_script_history();
    }
    ~SendFixture() override {
        helix::ui::set_test_notification_error_hook(nullptr);
        set_moonraker_api(nullptr);
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
        settle();
    }
    void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    std::vector<std::string> exclusions() const {
        std::vector<std::string> out;
        for (const auto& s : client.gcode_script_history()) {
            if (s.rfind("EXCLUDE_OBJECT NAME=", 0) == 0) {
                out.push_back(s);
            }
        }
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(SendFixture, "Picks go out only once the start is confirmed",
                 "[pre_start_exclude][send]") {
    int confirmed = 0;
    auto on_confirmed = with_pre_start_exclusions([&] { ++confirmed; }, {"Cone_id_0", "Cube_id_1"});
    settle();
    CHECK(exclusions().empty());

    on_confirmed();
    CHECK(confirmed == 1);
    settle();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cone_id_0",
                                                   "EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(errors.empty());
}

TEST_CASE_METHOD(SendFixture, "No picks leave the confirmation callback as it was",
                 "[pre_start_exclude][send]") {
    int confirmed = 0;
    with_pre_start_exclusions([&] { ++confirmed; }, {})();
    settle();
    CHECK(confirmed == 1);
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(SendFixture,
                 "A failed exclusion is named in one toast; a timeout is not a failure",
                 "[pre_start_exclude][send]") {
    client.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST, "Klippy Disconnected",
                                  "NAME=Cube_id_1");
    send_pre_start_exclusions(api.get(), {"Cone_id_0", "Cube_id_1"});
    settle();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Cube_id_1") != std::string::npos);
    CHECK(errors[0].find("Cone_id_0") == std::string::npos);

    errors.clear();
    client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "timed out", "NAME=Cone_id_0");
    send_pre_start_exclusions(api.get(), {"Cone_id_0"});
    settle();
    CHECK(errors.empty());
}

TEST_CASE_METHOD(SendFixture, "A failure after the print was cancelled raises no toast",
                 "[pre_start_exclude][send]") {
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::CANCELLED);
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Unknown object",
                                  "NAME=Cube_id_1");
    send_pre_start_exclusions(api.get(), {"Cube_id_1"});
    settle();
    CHECK(errors.empty());
}

TEST_CASE_METHOD(
    SendFixture,
    "A confirmation that lands with no printer API reports the picks it could not send",
    "[pre_start_exclude][send]") {
    auto on_confirmed = with_pre_start_exclusions([] {}, {"Cube_id_1"});
    set_moonraker_api(nullptr); // printer switched away before the start was confirmed
    on_confirmed();
    settle();
    CHECK(exclusions().empty());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Cube_id_1") != std::string::npos);
}
