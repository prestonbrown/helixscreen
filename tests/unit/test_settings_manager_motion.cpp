// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/settings_manager.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds default, round-trip and clamp to a sane range",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    CHECK(s.get_jog_speed_xy() == 6000);
    CHECK(s.get_jog_speed_z() == 600);
    s.set_jog_speed_z(1500);
    CHECK(s.get_jog_speed_z() == 1500);
    for (const int ask : {0, 999999}) {
        const int expected = ask == 0 ? 60 : 60000;
        s.set_jog_speed_xy(ask);
        CHECK(s.get_jog_speed_xy() == expected);
        s.set_jog_speed_z(ask);
        CHECK(s.get_jog_speed_z() == expected);
    }
}

#include "../../include/ui_panel_motion.h"

TEST_CASE_METHOD(HelixTestFixture, "Jog distances default to today's table", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, false) == Catch::Approx(0.1f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, true) == Catch::Approx(1.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Coarse, false) == Catch::Approx(1.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Coarse, true) == Catch::Approx(10.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, false) == Catch::Approx(10.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(50.0f));
}

TEST_CASE_METHOD(HelixTestFixture, "Jog distances round-trip, clamp and reset",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    s.set_jog_distance(helix::JogMode::Turbo, true, 25.0f);
    CHECK(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(25.0f));
    s.set_jog_distance(helix::JogMode::Turbo, true, 0.0f);
    CHECK(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(0.01f));
    s.set_jog_distance(helix::JogMode::Turbo, true, 999.0f);
    // The persisted value, not get_jog_distance(): the getter clamps on read,
    // which would mask the write clamp.
    CHECK(helix::Config::get_instance()->get<float>(helix::Config::get_instance()->df() +
                                                    "motion/turbo_outer") == Catch::Approx(200.0f));

    s.set_jog_distance(helix::JogMode::Fine, false, 5.0f);
    s.reset_jog_distances();
    CHECK(s.get_jog_distance(helix::JogMode::Fine, false) == Catch::Approx(0.1f));
}

#include "../../include/ui_settings_motion.h"

using helix::settings::Field;
using helix::settings::keypad_bounds;

TEST_CASE_METHOD(HelixTestFixture, "keypad_bounds: each step is bounded by its pair",
                 "[settings_motion]") {
    struct Row {
        const char* name;
        Field field;
        float inner, outer, min, max;
    };
    const Row rows[] = {
        {"outer floor is the paired inner step", Field::TurboOuter, 10.0f, 50.0f, 10.0f,
         helix::settings::kJogDistanceMaxMm},
        {"inner ceiling is the paired outer step", Field::FineInner, 0.1f, 1.0f,
         helix::settings::kJogDistanceMinMm, 1.0f},
        {"speed rows pass the slider range through", Field::JogSpeedZ, 1.0f, 500.0f, 1.0f, 500.0f},
    };
    for (const auto& r : rows) {
        INFO(r.name);
        const auto b = keypad_bounds(r.field, r.inner, r.outer);
        CHECK(b.min == Catch::Approx(r.min));
        CHECK(b.max == Catch::Approx(r.max));
    }
}

TEST_CASE_METHOD(HelixTestFixture, "keypad_bounds: an equal pair still allows either value",
                 "[settings_motion]") {
    // inner == outer: neither keypad collapses to an inverted range.
    const auto outer = keypad_bounds(Field::CoarseOuter, 5.0f, 5.0f);
    CHECK(outer.min == Catch::Approx(5.0f));
    CHECK(outer.min <= outer.max);
    const auto inner = keypad_bounds(Field::CoarseInner, 5.0f, 5.0f);
    CHECK(inner.max == Catch::Approx(5.0f));
    CHECK(inner.min <= inner.max);
}

TEST_CASE_METHOD(HelixTestFixture, "Bed map clearance defaults to 5mm and clamps",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    REQUIRE(s.get_bed_map_clearance_mm() == 5);
    s.set_bed_map_clearance_mm(0);
    CHECK(s.get_bed_map_clearance_mm() == helix::kBedMapClearanceMinMm);
    s.set_bed_map_clearance_mm(999);
    CHECK(s.get_bed_map_clearance_mm() == helix::kBedMapClearanceMaxMm);
    s.set_bed_map_clearance_mm(5);
}

namespace {

// Seeds the config the load path reads, then rebuilds the subject cache the
// way a printer switch does. init_subjects() is one-shot for the process, so
// the rebuild needs the deinit/init pair — and the destructor must put the
// defaults back, or every later init_subjects() call in the binary no-ops and
// reads this test's values (same hazard as test_settings_manager_scoping.cpp).
class LoadClampFixture : public HelixTestFixture {
  protected:
    helix::Config* cfg = helix::Config::get_instance();
    helix::SettingsManager& sm = helix::SettingsManager::instance();

    LoadClampFixture() {
        helix::setup_printer_data(*cfg,
                                  {{"motion", {{"jog_speed_xy", 1}, {"jog_speed_z", 999999}}}});
        reload();
    }

    ~LoadClampFixture() override {
        helix::test::reset_config_singleton();
        reload();
    }

    void reload() {
        sm.deinit_subjects();
        sm.init_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(LoadClampFixture, "Out-of-range motion values load clamped", "[settings_motion]") {
    // The protection against a hand-edited settings.json: the loader clamps
    // what it reads, not just what the setters write.
    CHECK(sm.get_jog_speed_xy() == 60);
    CHECK(sm.get_jog_speed_z() == 60000);
}

TEST_CASE_METHOD(HelixTestFixture, "Coordinate source defaults to commanded and round-trips",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    REQUIRE(s.get_motion_show_actual_position() == false);

    s.set_motion_show_actual_position(true);
    REQUIRE(s.get_motion_show_actual_position() == true);
    REQUIRE(lv_subject_get_int(s.subject_motion_show_actual_position()) == 1);

    // Persisted beside the jog settings in the per-printer motion section.
    auto* cfg = helix::Config::get_instance();
    REQUIRE(cfg->get<bool>(cfg->df() + "motion/show_actual_position", false) == true);

    s.set_motion_show_actual_position(false);
    REQUIRE(s.get_motion_show_actual_position() == false);
    REQUIRE(cfg->get<bool>(cfg->df() + "motion/show_actual_position", true) == false);
}
