// SPDX-License-Identifier: GPL-3.0-or-later

// Object picks made in print details reach the print: captured at the Print
// tap, sent only once Moonraker confirms the start, refused when they cover
// every object, and dropped (with a toast) when the tap queues the file.

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/registered_backend.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "moonraker_api_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

const char* kPlate = "; HEADER\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n"
                     "G28\n";

/// A real panel over the mock API with a three-object file's details open.
class PickStartFixture : private helix::PrintSelectGlobalStateReset,
                         public helix::PrintSelectPanelFixture {
  public:
    PickStartFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered,
                                         helix::PrintSelectVisit::Immediate,
                                         helix::PrintSelectApi::Mock) {
        set_moonraker_api(api_.get());
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"exclude_object", "extruder"});
        get_printer_state().set_hardware(hw);
        // A halted Klipper refuses G-code, and the reset state reads halted.
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        helix::ui::set_test_notification_info_hook(
            [this](const std::string& m) { infos.push_back(m); });
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& m) { warnings.push_back(m); });

        panel_->refresh_files(/*force=*/true);
        drain();
        REQUIRE(panel_->select_file_by_name(file_.name()));
        drain();
        detail = PrintSelectPanelTestAccess::detail_view(*panel_);
        REQUIRE(detail != nullptr);
        REQUIRE(detail->exclude_objects().get_defined_objects().size() == 3);
        mock_client_.clear_gcode_script_history();
    }

    ~PickStartFixture() override {
        drop_print_status();
        helix::ui::set_test_notification_info_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        ps.capabilities_state().set_job_queue_available(false);
        ps.set_hardware(helix::PrinterDiscovery{});
        set_moonraker_api(nullptr);
        drain();
    }

    std::vector<std::string> exclusions() const {
        std::vector<std::string> out;
        for (const auto& s : mock_client_.gcode_script_history()) {
            if (s.rfind("EXCLUDE_OBJECT NAME=", 0) == 0) {
                out.push_back(s);
            }
        }
        return out;
    }

    /// Hold printer.print.start's answer until the test releases it.
    void hold_print_start() {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "printer.print.start",
            [this](MoonrakerClientMock*, const json&, std::function<void(const json&)> success_cb,
                   std::function<void(const MoonrakerError&)> error_cb) -> bool {
                held_start = std::move(success_cb);
                held_error = std::move(error_cb);
                return true;
            });
    }

    /// The print status tree is process-wide: leave none behind.
    void drop_print_status() {
        lv_obj_t* status = PrintStatusPanel::get_cached_overlay();
        if (!status) {
            return;
        }
        auto& nav = NavigationManager::instance();
        auto stack = NavigationManagerTestAccess::panel_stack(nav);
        stack.erase(std::remove(stack.begin(), stack.end(), status), stack.end());
        NavigationManagerTestAccess::set_panel_stack(nav, stack);
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        drain();
    }

    /// Start the open file through the panel's controller, past initiate().
    /// @p navigate keeps the panel's own navigation: details hides and print
    /// status opens, as a real start does. Without it details stays open.
    helix::ui::PrintStartController& start_held(bool navigate) {
        auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
        REQUIRE(controller != nullptr);
        if (!navigate) {
            controller->set_navigate_to_print_status(nullptr);
        }
        controller->set_file(file_.name(), "", {}, "");
        controller->set_exclude_picks(detail->exclude_picks());
        hold_print_start();
        PrintStartControllerTestAccess::execute(*controller);
        drain();
        lv_timer_handler(); // the close callback runs on the next tick
        REQUIRE(held_start);
        return *controller;
    }

    static int pick_count() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "detail_exclude_pick_count"));
    }

    static bool contains(const std::vector<std::string>& msgs, const std::string& needle) {
        for (const auto& m : msgs) {
            if (m.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    helix::PlantedGcode file_{"pick_start.gcode", "", kPlate};
    helix::ui::PrintSelectDetailView* detail = nullptr;
    std::function<void(const json&)> held_start;
    std::function<void(const MoonrakerError&)> held_error;
    std::vector<std::string> infos;
    std::vector<std::string> warnings;
};

} // namespace

TEST_CASE_METHOD(PickStartFixture, "Picks are sent only after Moonraker confirms the start",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    auto& controller = start_held(/*navigate=*/false);
    CHECK(PrintStartControllerTestAccess::exclude_picks(controller).empty()); // consumed
    CHECK(exclusions().empty()); // nothing before Moonraker answers

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
}

TEST_CASE_METHOD(PickStartFixture,
                 "Opening another file while the start is in flight keeps the started file's picks",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cone_id_0");
    start_held(/*navigate=*/false);

    PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    lv_timer_handler(); // the close callback runs on the next tick
    helix::PlantedGcode other("other_file.gcode", "", kPlate);
    panel_->refresh_files(/*force=*/true);
    drain();
    REQUIRE(panel_->select_file_by_name(other.name()));
    drain();
    CHECK(detail->exclude_picks().empty());
    detail->toggle_exclude_pick("Cube_id_1");

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cone_id_0"});
    // The confirmation belongs to the started file, not the one now open.
    CHECK(detail->exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(PickStartFixture, "A failed start comes back to details with the picks intact",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    drain();
    REQUIRE(pick_count() == 1);
    start_held(/*navigate=*/true);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    CHECK(detail->picks_held_for_start());

    REQUIRE(NavigationManager::instance().is_panel_on_top(PrintStatusPanel::get_cached_overlay()));

    held_error(MoonrakerError::unknown("Klipper refused the print", "printer.print.start"));
    drain();
    lv_timer_handler();
    drain();

    // The controller popped print status and re-showed details itself.
    REQUIRE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    CHECK(detail->exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    CHECK(pick_count() == 1);
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(PickStartFixture, "A confirmed start spends the picks and releases the hold",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    start_held(/*navigate=*/true);
    REQUIRE(detail->picks_held_for_start());

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(detail->exclude_picks().empty());
    CHECK_FALSE(detail->picks_held_for_start());

    PrintSelectPanelTestAccess::show_detail_view(*panel_); // the same file again
    drain();
    CHECK(detail->exclude_picks().empty());
    CHECK(pick_count() == 0);
}

TEST_CASE_METHOD(PickStartFixture, "The Print tap hands the picks it saw to the start controller",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cylinder_id_2");
    // A running print makes initiate() refuse after set_file(), so the hand-off
    // is read without driving a real start.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    panel_->start_print(/*force=*/true);
    drain();
    auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
    REQUIRE(controller != nullptr);
    CHECK(PrintStartControllerTestAccess::exclude_picks(*controller) ==
          std::vector<std::string>{"Cylinder_id_2"});
}

TEST_CASE_METHOD(PickStartFixture, "Every object picked refuses the start before anything is sent",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cone_id_0");
    detail->toggle_exclude_pick("Cube_id_1");
    detail->toggle_exclude_pick("Cylinder_id_2");
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(contains(warnings, "Every object is set to skip"));
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());
    CHECK(exclusions().empty());
}

namespace {

/// The queue's state while a print runs: the Print tap queues the file.
struct QueueMode {
    JobQueueState jqs;
    JobQueueState* previous = get_job_queue_state();
    QueueMode(PickStartFixture& f, IMoonrakerAPI* api, MoonrakerClientMock& client)
        : jqs(api, &client) {
        set_job_queue_state(&jqs);
        auto& ps = get_printer_state();
        // The button re-decides on print state, not on queue availability.
        ps.capabilities_state().set_job_queue_available(true);
        set_wire_state(ps, PrintJobState::PRINTING);
        f.drain();
        REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_button_mode")) == 1);
    }
    ~QueueMode() {
        set_job_queue_state(previous);
    }
};

} // namespace

TEST_CASE_METHOD(PickStartFixture, "A queued start drops the picks and says so",
                 "[print_select][pre_start_exclude][start][job_queue]") {
    QueueMode queue(*this, api_.get(), mock_client_);
    detail->toggle_exclude_pick("Cube_id_1");
    panel_->start_print();
    drain();

    REQUIRE(
        PrintSelectPanelTestAccess::controller_file(*panel_).first.empty()); // queued, not started
    CHECK(contains(infos, "Object picks apply only to prints started now"));
    CHECK(detail->exclude_picks().empty());
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(PickStartFixture, "A refused queue add keeps the picks",
                 "[print_select][pre_start_exclude][start][job_queue]") {
    QueueMode queue(*this, api_.get(), mock_client_);
    bool asked = false;
    helix::MoonrakerClientMockTestAccess::set_method_handler(
        mock_client_, "server.job_queue.post_job",
        [&asked](MoonrakerClientMock*, const json&, std::function<void(const json&)>,
                 std::function<void(const MoonrakerError&)> error_cb) -> bool {
            asked = true;
            error_cb(MoonrakerError::unknown("refused", "server.job_queue.post_job"));
            return true;
        });
    detail->toggle_exclude_pick("Cube_id_1");
    panel_->start_print();
    drain();

    REQUIRE(asked);
    CHECK_FALSE(contains(infos, "Object picks apply only to prints started now"));
    CHECK(detail->exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(PickStartFixture, "A queue add answered after another file opened keeps its picks",
                 "[print_select][pre_start_exclude][start][job_queue]") {
    QueueMode queue(*this, api_.get(), mock_client_);
    std::function<void(const json&)> held_add;
    helix::MoonrakerClientMockTestAccess::set_method_handler(
        mock_client_, "server.job_queue.post_job",
        [&held_add](MoonrakerClientMock*, const json&, std::function<void(const json&)> success_cb,
                    std::function<void(const MoonrakerError&)>) -> bool {
            held_add = std::move(success_cb);
            return true;
        });
    detail->toggle_exclude_pick("Cube_id_1");
    panel_->start_print();
    drain();
    REQUIRE(held_add);

    PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    lv_timer_handler(); // the close callback runs on the next tick
    helix::PlantedGcode other("other_file.gcode", "", kPlate);
    panel_->refresh_files(/*force=*/true);
    drain();
    REQUIRE(panel_->select_file_by_name(other.name()));
    REQUIRE(wait_until([&] {
        drain();
        return detail->exclude_objects().get_defined_objects().size() == 3;
    }));
    detail->toggle_exclude_pick("Cone_id_0");
    REQUIRE(detail->exclude_picks() == std::vector<std::string>{"Cone_id_0"});

    json result;
    result["queue_state"] = "ready";
    result["queued_jobs"] = json::array({{{"job_id", "0001"},
                                          {"filename", file_.name()},
                                          {"time_added", 0.0},
                                          {"time_in_queue", 0.0}}});
    held_add(json{{"result", result}});
    drain();

    CHECK(detail->exclude_picks() == std::vector<std::string>{"Cone_id_0"});
    CHECK_FALSE(contains(infos, "Object picks apply only to prints started now"));
}

namespace {

/// A tool changer whose remap rewrites the job file, mapping tool 0 to head 1.
struct RewriteRemap {
    helix::test::RegisteredBackend<helix::AmsBackendMock> ams{2};
    std::vector<helix::ToolMapping> mappings;
    RewriteRemap() {
        ams->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
        helix::ToolMapping m;
        m.tool_index = 0;
        m.mapped_slot = 1;
        mappings.push_back(m);
    }
};

} // namespace

TEST_CASE_METHOD(PickStartFixture,
                 "A remap that changes nothing starts the original and carries the picks",
                 "[print_select][pre_start_exclude][start][remap]") {
    RewriteRemap remap;
    detail->toggle_exclude_pick("Cube_id_1");
    hold_print_start(); // the plate has no tool change, so the original prints
    PrintSelectPanelTestAccess::apply_remap(*panel_, remap.mappings);
    drain();
    REQUIRE(held_start);
    CHECK(exclusions().empty());

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(detail->exclude_picks().empty());
    CHECK(PrintStatusPanel::get_cached_overlay() != nullptr); // the start navigated
}

namespace {

const char* kToolPlate = "; HEADER\n"
                         "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4\n"
                         "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
                         "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n"
                         "T0\n"
                         "G28\n"
                         "T1\n";

} // namespace

TEST_CASE_METHOD(PickStartFixture, "A rewritten-remap start carries the picks after confirmation",
                 "[print_select][pre_start_exclude][start][remap]") {
    RewriteRemap remap;
    helix::PlantedGcode tools("tool_plate.gcode", "", kToolPlate);
    PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    lv_timer_handler(); // the close callback runs on the next tick
    panel_->refresh_files(/*force=*/true);
    drain();
    REQUIRE(panel_->select_file_by_name(tools.name()));
    REQUIRE(wait_until([&] {
        drain();
        return detail->exclude_objects().get_defined_objects().size() == 3;
    }));
    detail->toggle_exclude_pick("Cube_id_1");
    auto& transfers = static_cast<MoonrakerAPIMock&>(*api_).transfers_mock();
    auto& jobs = static_cast<MoonrakerAPIMock&>(*api_).job_mock();
    transfers.mock_hold_path_uploads();

    PrintSelectPanelTestAccess::apply_remap(*panel_, remap.mappings);
    drain();
    REQUIRE(transfers.path_uploads().size() == 1); // the rewritten copy
    CHECK(jobs.modified_prints().empty());
    CHECK(exclusions().empty());

    SECTION("the open file") {
        transfers.release_held_path_uploads();
        drain();
        REQUIRE(jobs.modified_prints().size() == 1);
        CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
        CHECK(detail->exclude_picks().empty());
    }

    SECTION("another file opened during the rewrite: only the started file's picks go out") {
        PrintSelectPanelTestAccess::hide_detail_view(*panel_);
        drain();
        lv_timer_handler();
        REQUIRE(panel_->select_file_by_name(file_.name()));
        REQUIRE(wait_until([&] {
            drain();
            return detail->exclude_objects().get_defined_objects().size() == 3;
        }));
        detail->toggle_exclude_pick("Cone_id_0");
        REQUIRE(detail->exclude_picks() == std::vector<std::string>{"Cone_id_0"});
        transfers.release_held_path_uploads();
        drain();
        REQUIRE(jobs.modified_prints().size() == 1);
        // Print status opening over details is leaving that file too, which
        // clears its picks; none of them reach this print.
        CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    }
}

TEST_CASE_METHOD(PickStartFixture, "A rewritten-remap start is refused with every object picked",
                 "[print_select][pre_start_exclude][start][remap]") {
    RewriteRemap remap;
    detail->toggle_exclude_pick("Cone_id_0");
    detail->toggle_exclude_pick("Cube_id_1");
    detail->toggle_exclude_pick("Cylinder_id_2");
    hold_print_start();
    PrintSelectPanelTestAccess::apply_remap(*panel_, remap.mappings);
    drain();
    CHECK(contains(warnings, "Every object is set to skip"));
    CHECK_FALSE(held_start);
}
