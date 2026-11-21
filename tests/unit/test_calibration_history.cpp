// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_hub_overlay.h"

#include "../lvgl_test_fixture.h"
#include "calibration_config_results.h"
#include "calibration_history_store.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <ctime>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::calibration;
using json = nlohmann::json;

namespace {

History sample_history() {
    History h;
    h["z_offset"] = {"2026-11-20", "-1.245 mm", json{{"z_offset", -1.245}, {"drag_z", 0.1}}};
    h["input_shaper"] = {"2025-02-28", "MZV 52 Hz", json::object()};
    return h;
}

/// Serves one stored document, answers each read when told to, and records writes.
class DatabaseAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;
    void database_get_item(const std::string& ns, const std::string& key,
                           helix::SharedCallback<void(const json&)> on_success,
                           ErrorCallback on_error) override {
        asked = ns + "/" + key;
        pending_success = std::move(on_success);
        pending_error = std::move(on_error);
        if (answer_at_once) {
            answer();
        }
    }
    void database_post_item(const std::string& ns, const std::string& key, const json& value,
                            helix::SharedCallback<void()> on_success, ErrorCallback) override {
        posted = ns + "/" + key;
        ++posts;
        stored = value;
        if (on_success) {
            on_success();
        }
    }
    void answer() {
        if (missing) {
            MoonrakerError err;
            err.message = "Key not found";
            pending_error(err);
        } else {
            pending_success(stored);
        }
    }
    json stored;
    bool missing = false;
    bool answer_at_once = true;
    std::string asked;
    std::string posted;
    int posts = 0;
    helix::SharedCallback<void(const json&)> pending_success;
    ErrorCallback pending_error;
};

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

} // namespace

TEST_CASE("a saved calibration history reads back as it was", "[calibration_history]") {
    const auto j = history_to_json(sample_history());
    CHECK(j["version"] == 1);
    const auto back = history_from_json(j);
    REQUIRE(back.has_value());
    REQUIRE(back->size() == 2);
    const auto& z = back->at("z_offset");
    CHECK(z.date == "2026-11-20");
    CHECK(z.summary == "-1.245 mm");
    CHECK(z.values["z_offset"].get<double>() == Catch::Approx(-1.245));
    CHECK(back->at("input_shaper").values == json::object());
    CHECK(history_from_json(history_to_json(History{}))->empty());
}

TEST_CASE("a damaged or foreign calibration history reads as none", "[calibration_history]") {
    const auto bad = [](void (*damage)(json&)) {
        auto j = history_to_json(sample_history());
        damage(j);
        return !history_from_json(j).has_value();
    };
    CHECK(bad([](json& j) { j["version"] = 2; }));
    CHECK(bad([](json& j) { j.erase("version"); }));
    CHECK(bad([](json& j) { j["version"] = "1"; }));
    CHECK(bad([](json& j) { j.erase("steps"); }));
    CHECK(bad([](json& j) { j["steps"] = json::array(); }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"] = "done"; }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"].erase("date"); }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"]["date"] = 20261120; }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"]["date"] = "yesterday"; }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"].erase("summary"); }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"]["summary"] = 1.2; }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"].erase("values"); }));
    CHECK(bad([](json& j) { j["steps"]["z_offset"]["values"] = json::array({1}); }));
    CHECK_FALSE(history_from_json(json::array()).has_value());
    CHECK_FALSE(history_from_json(json()).has_value());
}

TEST_CASE("only a real calendar date in YYYY-MM-DD form is a history date",
          "[calibration_history]") {
    const auto d = parse_iso_date("2026-11-20");
    REQUIRE(d.has_value());
    CHECK(d->tm_year == 126);
    CHECK(d->tm_mon == 10);
    CHECK(d->tm_mday == 20);
    CHECK(parse_iso_date("2024-02-29").has_value());
    CHECK(parse_iso_date("2000-02-29").has_value());
    CHECK_FALSE(parse_iso_date("2026-02-29").has_value());
    CHECK_FALSE(parse_iso_date("1900-02-29").has_value());
    CHECK_FALSE(parse_iso_date("2026-13-01").has_value());
    CHECK_FALSE(parse_iso_date("2026-00-10").has_value());
    CHECK_FALSE(parse_iso_date("2026-04-31").has_value());
    CHECK_FALSE(parse_iso_date("2026-11-00").has_value());
    CHECK_FALSE(parse_iso_date("2026-1-20").has_value());
    CHECK_FALSE(parse_iso_date("2026/11/20").has_value());
    CHECK_FALSE(parse_iso_date("2026-11-2x").has_value());
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = 0;
    tm.tm_mday = 5;
    tm.tm_hour = 12;
    CHECK(iso_date(std::mktime(&tm)) == "2026-01-05");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the history cache serves the stored results and records new ones",
                 "[calibration_history]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    DatabaseAPI api(client, state);
    CalibrationHistory history;

    api.stored = history_to_json(sample_history());
    history.load(api);
    drain();
    CHECK(api.asked == "helix-screen/calibration_history");
    REQUIRE(history.entry("z_offset") != nullptr);
    CHECK(history.entry("z_offset")->summary == "-1.245 mm");
    CHECK(history.entry("bed_mesh") == nullptr);

    history.record(api, "bed_mesh", "0.18 mm range", json{{"range", 0.18}});
    CHECK(api.posted == "helix-screen/calibration_history");
    const auto saved = history_from_json(api.stored);
    REQUIRE(saved.has_value());
    CHECK(saved->size() == 3);
    CHECK(saved->at("bed_mesh").date == iso_date(std::time(nullptr)));
    CHECK(saved->at("bed_mesh").values["range"].get<double>() == Catch::Approx(0.18));

    api.stored = json{{"version", 1}};
    history.load(api);
    drain();
    CHECK(history.entries().empty());

    api.missing = true;
    history.record(api, "heaters", "MPC", json::object());
    history.load(api);
    drain();
    CHECK(history.entries().empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a result recorded while the history loads keeps the stored ones",
                 "[calibration_history]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    DatabaseAPI api(client, state);
    CalibrationHistory history;

    api.stored = history_to_json(sample_history());
    api.answer_at_once = false;
    history.load(api);
    history.record(api, "z_offset", "-1.300 mm", json{{"z_offset", -1.3}});
    CHECK(api.posts == 0);
    CHECK(history.entry("z_offset")->summary == "-1.300 mm");

    api.answer();
    drain();
    REQUIRE(api.posts == 1);
    const auto saved = history_from_json(api.stored);
    REQUIRE(saved.has_value());
    CHECK(saved->size() == 2);
    CHECK(saved->at("z_offset").summary == "-1.300 mm");
    CHECK(saved->at("input_shaper").summary == "MZV 52 Hz");
    CHECK(history.entry("z_offset")->summary == "-1.300 mm");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a Calibration tile says when a step last ran, else how much it matters",
                 "[calibration_history]") {
    StepInfo step;
    step.tier = Tier::Required;
    const HistoryEntry done{"2026-11-20", "MPC 60 W", json::object()};
    auto s = helix::ui::tile_status(step, &done, nullptr);
    CHECK(s.text == "Done Nov 20 · MPC 60 W");
    CHECK(s.tone == helix::ui::TileTone::Done);
    const HistoryEntry bare{"2026-03-09", "", json::object()};
    CHECK(helix::ui::tile_status(step, &bare, nullptr).text == "Done Mar 9");

    using helix::ui::TileTone;
    const std::pair<Tier, std::pair<const char*, TileTone>> never[] = {
        {Tier::Required, {"Never run · required", TileTone::Due}},
        {Tier::Recommended, {"Never run", TileTone::Due}},
        {Tier::Optional, {"Never run · optional", TileTone::Idle}},
        {Tier::OnRequest, {"Run when needed", TileTone::Idle}},
    };
    for (const auto& [tier, want] : never) {
        step.tier = tier;
        s = helix::ui::tile_status(step, nullptr, nullptr);
        CHECK(s.text == want.first);
        CHECK(s.tone == want.second);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a Calibration tile with no history shows what the printer's config holds",
                 "[calibration_history]") {
    StepInfo step;
    step.tier = Tier::Recommended;
    const std::string in_config = "-1.250 mm";
    auto s = helix::ui::tile_status(step, nullptr, &in_config);
    CHECK(s.text == "In config · -1.250 mm");
    CHECK(s.tone == helix::ui::TileTone::Idle);

    // HelixScreen's own record has a date and wins.
    const HistoryEntry done{"2026-11-20", "-1.300 mm", json::object()};
    s = helix::ui::tile_status(step, &done, &in_config);
    CHECK(s.text == "Done Nov 20 · -1.300 mm");
    CHECK(s.tone == helix::ui::TileTone::Done);
}

TEST_CASE("results_in_config reads each step's tuned value from configfile.settings",
          "[calibration_history]") {
    const json settings = {
        {"probe", {{"z_offset", -1.25}, {"x_offset", 0.0}}},
        {"stepper_z", {{"position_endstop", 0.5}}},
        {"extruder", {{"control", "pid"}, {"pid_kp", 22.0}}},
        {"bed_mesh", {{"probe_count", json::array({5, 5})}}},
        {"bed_mesh hot", {{"version", 1}}},
        {"bed_mesh default", {{"version", 1}}},
        {"input_shaper",
         {{"shaper_type", "mzv"},
          {"shaper_type_x", "mzv"},
          {"shaper_freq_x", 36.7},
          {"shaper_type_y", "ei"},
          {"shaper_freq_y", 47.6}}},
    };
    const ConfigResults r = results_in_config(settings);
    CHECK(r.at("z_offset") == "-1.250 mm");
    CHECK(r.at("heaters") == "PID");
    CHECK(r.at("bed_mesh") == "profile default");
    CHECK(r.at("input_shaper") == "MZV 36.7 · EI 47.6 Hz");

    SECTION("without a probe, Z offset is stepper_z's endstop") {
        json s = settings;
        s.erase("probe");
        CHECK(results_in_config(s).at("z_offset") == "0.500 mm");
    }
    SECTION("BTT Eddy keeps z_offset in its probe_eddy_current section") {
        json s = settings;
        s.erase("probe");
        s["probe_eddy_current btt_eddy"] = {{"z_offset", 1.5}};
        CHECK(results_in_config(s).at("z_offset") == "1.500 mm");
    }
    SECTION("Beacon's offset is its active model's model_offset") {
        json s = settings;
        s.erase("probe");
        s["beacon"] = {{"x_offset", 0.0}};
        CHECK(results_in_config(s).count("z_offset") == 0);
        s["beacon model default"] = {{"model_offset", 0.02}};
        s["beacon model hot"] = {{"model_offset", -0.04}};
        CHECK(results_in_config(s).at("z_offset") == "0.020 mm");
        s["beacon"]["default_model_name"] = "hot";
        CHECK(results_in_config(s).at("z_offset") == "-0.040 mm");
    }
    SECTION("Cartographer's offset is its touch model's, else its scan model's") {
        json s = settings;
        s.erase("probe");
        s["cartographer"] = {{"x_offset", 0.0}};
        s["cartographer scan_model default"] = {{"z_offset", 0.1}};
        CHECK(results_in_config(s).at("z_offset") == "0.100 mm");
        s["cartographer touch_model default"] = {{"z_offset", -0.06}};
        CHECK(results_in_config(s).at("z_offset") == "-0.060 mm");
    }
    SECTION("MPC counts; watermark has nothing tuned") {
        json s = settings;
        s["extruder"]["control"] = "mpc";
        CHECK(results_in_config(s).at("heaters") == "MPC");
        s["extruder"]["control"] = "watermark";
        CHECK(results_in_config(s).count("heaters") == 0);
    }
    SECTION("the [bed_mesh] section alone is not a saved profile") {
        json s = settings;
        s.erase("bed_mesh default");
        s.erase("bed_mesh hot");
        CHECK(results_in_config(s).count("bed_mesh") == 0);
        s["bed_mesh hot"] = {{"version", 1}};
        CHECK(results_in_config(s).at("bed_mesh") == "profile hot");
    }
    SECTION("an axis at the default frequency 0 is not shaped") {
        json s = settings;
        s["input_shaper"]["shaper_freq_x"] = 0.0;
        CHECK(results_in_config(s).at("input_shaper") == "EI 47.6 Hz");
        s["input_shaper"]["shaper_freq_y"] = 0.0;
        CHECK(results_in_config(s).count("input_shaper") == 0);
    }
    CHECK(results_in_config(json::array()).empty());
}
