// SPDX-License-Identifier: GPL-3.0-or-later

// What a finished calibration offers, and what each choice says it does.

#include "ui_calibration_result.h"
#include "ui_calibration_time.h"

#include "../test_fixtures.h"
#include "calibration_result_choices.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "subject_managed_panel.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::calibration;
using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

PrinterDiscovery kalico() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array());
    hw.apply_printer_info(json{{"app", "Kalico"}, {"software_version", "v2025.1"}});
    return hw;
}

const json kPid = {{"extruder", {{"control", "pid"}, {"pid_kp", "22.1"}}}};
const json kMpc = {{"extruder", {{"control", "mpc"}, {"block_heat_capacity", "18.4"}}}};

bool extruder(const std::string& section) {
    return section == "extruder";
}

} // namespace

TEST_CASE("A staged result that is not in use yet saves with a restart", "[calibration_result]") {
    const ResultChoices c = result_choices(staged_result_facts(PrinterDiscovery{}, kPid, nullptr));
    CHECK(c.offer_save);
    CHECK(c.offer_keep);
    CHECK(c.keep == KeepMeaning::NotYetInUse);
    CHECK(c.discard == DiscardMeaning::RestartDropsIt);
    CHECK(c.save_restarts);
}

TEST_CASE("A result the firmware already uses is in use now and saves without a restart",
          "[calibration_result]") {
    const ResultChoices pid = result_choices(staged_result_facts(kalico(), kPid, extruder));
    CHECK(pid.keep == KeepMeaning::InUse);
    CHECK_FALSE(pid.save_restarts);
    CHECK(pid.discard == DiscardMeaning::RestartDropsIt);

    // MPC values take effect only on a restart, Kalico or not.
    const ResultChoices mpc = result_choices(staged_result_facts(kalico(), kMpc, extruder));
    CHECK(mpc.keep == KeepMeaning::NotYetInUse);
    CHECK(mpc.save_restarts);
}

TEST_CASE("A result after a control switch can only be saved", "[calibration_result]") {
    ResultFacts facts = staged_result_facts(PrinterDiscovery{}, kMpc, nullptr);
    facts.control_switched = true;
    const ResultChoices c = result_choices(facts);
    CHECK(c.offer_save);
    CHECK_FALSE(c.offer_keep);
    CHECK_FALSE(c.offer_discard);
    CHECK(c.must_save);
}

TEST_CASE("Other staged results are named, and only this result decides in use",
          "[calibration_result]") {
    const json both = {{"extruder", {{"control", "pid"}, {"pid_kp", "22.1"}}},
                       {"probe", {{"z_offset", "1.234"}}}};
    const auto is_extruder = [](const std::string& s) { return s == "extruder"; };
    const ResultFacts facts = staged_result_facts(kalico(), both, is_extruder);
    REQUIRE(facts.also_staged == std::vector<std::string>{"probe"});
    // The probe's offset needs a restart, the extruder's PID is live on Kalico.
    CHECK(facts.live);
    CHECK(facts.save_restarts);
    CHECK(result_choices(facts).also_staged == std::vector<std::string>{"probe"});
    CHECK(helix::calibration::result_save_line(result_choices(facts)).find("probe") !=
          std::string::npos);
}

TEST_CASE("Nothing staged leaves nothing to save or drop", "[calibration_result]") {
    const ResultChoices c =
        result_choices(staged_result_facts(PrinterDiscovery{}, json::object(), nullptr));
    CHECK_FALSE(c.offer_save);
    CHECK(c.offer_keep);
    CHECK(c.discard == DiscardMeaning::NothingToUndo);

    // An unreadable answer counts as staged.
    const ResultChoices unknown =
        result_choices(staged_result_facts(PrinterDiscovery{}, json(), nullptr));
    CHECK(unknown.offer_save);
    CHECK(unknown.discard == DiscardMeaning::RestartDropsIt);
}

TEST_CASE("A calibration's time reads the same on every screen", "[calibration_time]") {
    using helix::calibration::AwayKind;
    using helix::calibration::time_away_line;
    using helix::calibration::time_you_line;
    // The minutes show, never below one.
    CHECK(time_away_line(AwayKind::Runs, 14).find("14") != std::string::npos);
    CHECK(time_away_line(AwayKind::Runs, 0).find("1") != std::string::npos);
    // A printed test piece and a routine say different things.
    CHECK(time_away_line(AwayKind::Prints, 14) != time_away_line(AwayKind::Runs, 14));
    // The owner's part names the task when there is one.
    CHECK(time_you_line(2, "pick the band").find("pick the band") != std::string::npos);
    CHECK(time_you_line(2).find("2") != std::string::npos);
}

TEST_CASE("A long calibration's time reads in hours and minutes", "[calibration_time]") {
    using helix::calibration::time_duration_text;
    // An hour or more names the hours: 102 min is 1 h and 42 min, not "102".
    const std::string long_run = time_duration_text(102);
    CHECK(long_run.find("102") == std::string::npos);
    CHECK(long_run.find("1") != std::string::npos);
    CHECK(long_run.find("42") != std::string::npos);
    CHECK(time_duration_text(120).find("2") != std::string::npos);
    CHECK(time_duration_text(120).find("0") == std::string::npos);
    CHECK(time_duration_text(59).find("59") != std::string::npos);
}

TEST_CASE_METHOD(XMLTestFixture, "A result page shows its rows and only the choices it offers",
                 "[calibration_result][xml]") {
    SubjectManager subjects;
    helix::calibration::CalibrationResultView view("test_result");
    view.init_subjects(subjects);
    REQUIRE(register_component("wizard_frame_footer"));
    REQUIRE(register_component("components/calibration_run_result_row"));
    REQUIRE(register_component("components/calibration_result"));
    const char* attrs[] = {"prefix", "test_result", nullptr};
    lv_obj_t* page = create_component("calibration_result", attrs);
    REQUIRE(page != nullptr);
    lv_obj_t* rows = lv_obj_find_by_name(page, "cal_result_rows");
    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_get_child_count(rows) == 4);

    ResultChoices supervised;
    supervised.offer_save = false;
    view.publish("Nozzle heater tuned", "", {{"Kp", "20.000", "22.865"}, {"Ki", "", "1.292"}},
                 supervised);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK_FALSE(lv_obj_is_hidden(lv_obj_get_child(rows, 0)));
    CHECK_FALSE(lv_obj_is_hidden(lv_obj_get_child(rows, 1)));
    CHECK(lv_obj_is_hidden(lv_obj_get_child(rows, 2)));
    CHECK(lv_obj_is_hidden(lv_obj_get_child(rows, 3)));
    lv_obj_t* name = lv_obj_find_by_name(lv_obj_get_child(rows, 0), "result_name");
    REQUIRE(name != nullptr);
    CHECK(std::string(lv_label_get_text(name)) == "Kp");
    lv_obj_t* after = lv_obj_find_by_name(lv_obj_get_child(rows, 0), "result_after");
    REQUIRE(after != nullptr);
    CHECK(std::string(lv_label_get_text(after)) == "22.865");

    lv_obj_t* save = lv_obj_find_by_name(page, "btn_result_save");
    lv_obj_t* keep = lv_obj_find_by_name(page, "btn_result_keep");
    REQUIRE(save != nullptr);
    REQUIRE(keep != nullptr);
    CHECK(lv_obj_is_hidden(save));
    CHECK_FALSE(lv_obj_is_hidden(keep));

    view.publish("Nozzle heater tuned", "", {{"Kp", "20.000", "22.865"}}, ResultChoices{});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(lv_obj_is_hidden(save));
    CHECK(lv_obj_is_hidden(lv_obj_get_child(rows, 1)));

    lv_obj_delete(page);
}
