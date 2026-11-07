// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"
#include "gcode_preview_setup.h"
#include "gcode_render_mode_policy.h"
#include "runtime_config.h"
#include "screensaver_registry.h"
#include "settings_manager.h"
#include "timezone_env.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// DisplaySettingsManager Tests
// ============================================================================

// Pure policy: software-rotated displays (fbdev + rotation) repaint through a
// per-frame CPU rotate that makes transition animations jerky, so the DEFAULT
// for animations_enabled is forced off there regardless of platform tier
// (#986). Not-rotated displays follow the platform capability. This is only a
// default — an explicit user setting is applied ahead of it in init_subjects().
TEST_CASE("DisplaySettingsManager::animations_default policy", "[display_settings]") {
    // Software rotation forces the default off, even on a capable platform.
    REQUIRE(DisplaySettingsManager::animations_default(true, true) == false);
    REQUIRE(DisplaySettingsManager::animations_default(false, true) == false);
    // No software rotation: follow the platform capability verbatim.
    REQUIRE(DisplaySettingsManager::animations_default(true, false) == true);
    REQUIRE(DisplaySettingsManager::animations_default(false, false) == false);
}

// Without a dimmable backlight the dim timeout is what starts the screensaver, so
// the row stays reachable under that name; with neither, it would set nothing.
TEST_CASE("DisplaySettingsManager::dim_row_kind policy", "[display_settings]") {
    using Kind = DisplaySettingsManager::DimRowKind;
    CHECK(DisplaySettingsManager::dim_row_kind(true, true) == Kind::ScreenDim);
    CHECK(DisplaySettingsManager::dim_row_kind(true, false) == Kind::ScreenDim);
    CHECK(DisplaySettingsManager::dim_row_kind(false, true) == Kind::ScreensaverAfter);
    CHECK(DisplaySettingsManager::dim_row_kind(false, false) == Kind::Hidden);
    // settings_display_overlay.xml binds the two rows to these values.
    CHECK(static_cast<int>(Kind::Hidden) == 0);
    CHECK(static_cast<int>(Kind::ScreenDim) == 1);
    CHECK(static_cast<int>(Kind::ScreensaverAfter) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager default values after init",
                 "[display_settings]") {
    // Reset config to defaults so prior tests' set_*() calls don't pollute
    Config::get_instance()->set<int>("/display/sleep_sec", 1200);
    Config::get_instance()->set<int>("/display/dim_sec", 600);
    Config::get_instance()->set<int>("/display/brightness", 80);
    Config::get_instance()->set<bool>("/display/animations_enabled", true);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("dark_mode defaults to true (dark)") {
        REQUIRE(DisplaySettingsManager::instance().get_dark_mode() == true);
    }

    SECTION("dark_mode_available defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().is_dark_mode_available() == true);
    }

    SECTION("display_dim defaults to 600 seconds") {
        REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 600);
    }

    SECTION("display_sleep defaults to 1200 seconds") {
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 1200);
    }

    SECTION("brightness defaults to 80") {
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 80);
    }

    SECTION("sleep_while_printing defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().get_sleep_while_printing() == true);
    }

    SECTION("screensaver_print_progress defaults to true and persists") {
        auto& dsm = DisplaySettingsManager::instance();
        REQUIRE(dsm.get_screensaver_print_progress() == true);
        dsm.set_screensaver_print_progress(false);
        CHECK(lv_subject_get_int(dsm.subject_screensaver_print_progress()) == 0);
        CHECK(helix::Config::get_instance()->get<bool>("/display/screensaver_print_progress",
                                                       true) == false);
        dsm.set_screensaver_print_progress(true);
    }

    SECTION("animations_enabled defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().get_animations_enabled() == true);
    }

    SECTION("bed_mesh_render_mode defaults to 0 (Auto)") {
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 0);
    }

    SECTION("gcode_render_mode defaults to 0 (Auto)") {
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 0);
    }

    SECTION("time_format defaults to HOUR_12") {
        REQUIRE(DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12);
    }

    SECTION("bed_mesh_show_zero_plane defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_show_zero_plane() == true);
    }

    SECTION("printer_image defaults to empty string") {
        REQUIRE(DisplaySettingsManager::instance().get_printer_image().empty());
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager set/get round trips",
                 "[display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("brightness set/get") {
        DisplaySettingsManager::instance().set_brightness(75);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 75);

        DisplaySettingsManager::instance().set_brightness(10);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);

        DisplaySettingsManager::instance().set_brightness(100);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 100);
    }

    SECTION("brightness clamping - below 10 clamps to 10") {
        DisplaySettingsManager::instance().set_brightness(5);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);

        DisplaySettingsManager::instance().set_brightness(0);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);

        DisplaySettingsManager::instance().set_brightness(-10);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);
    }

    SECTION("brightness clamping - above 100 clamps to 100") {
        DisplaySettingsManager::instance().set_brightness(200);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 100);
    }

    SECTION("animations_enabled set/get") {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        REQUIRE(DisplaySettingsManager::instance().get_animations_enabled() == false);

        DisplaySettingsManager::instance().set_animations_enabled(true);
        REQUIRE(DisplaySettingsManager::instance().get_animations_enabled() == true);
    }

    SECTION("keep_navbar_visible set/get") {
        DisplaySettingsManager::instance().set_keep_navbar_visible(true);
        REQUIRE(DisplaySettingsManager::instance().get_keep_navbar_visible() == true);

        DisplaySettingsManager::instance().set_keep_navbar_visible(false);
        REQUIRE(DisplaySettingsManager::instance().get_keep_navbar_visible() == false);
    }

    SECTION("display_dim set/get") {
        DisplaySettingsManager::instance().set_display_dim_sec(60);
        REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 60);

        DisplaySettingsManager::instance().set_display_dim_sec(0);
        REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 0);
    }

    SECTION("display_sleep set/get") {
        DisplaySettingsManager::instance().set_display_sleep_sec(600);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 600);

        DisplaySettingsManager::instance().set_display_sleep_sec(0);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 0);
    }

    SECTION("time_format set/get") {
        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_24);
        REQUIRE(DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24);

        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_12);
        REQUIRE(DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12);
    }

    SECTION("sleep_while_printing set/get") {
        DisplaySettingsManager::instance().set_sleep_while_printing(false);
        REQUIRE(DisplaySettingsManager::instance().get_sleep_while_printing() == false);

        DisplaySettingsManager::instance().set_sleep_while_printing(true);
        REQUIRE(DisplaySettingsManager::instance().get_sleep_while_printing() == true);
    }

    SECTION("bed_mesh_render_mode set/get") {
        DisplaySettingsManager::instance().set_bed_mesh_render_mode(1);
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 1);

        DisplaySettingsManager::instance().set_bed_mesh_render_mode(2);
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 2);

        DisplaySettingsManager::instance().set_bed_mesh_render_mode(0);
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 0);
    }

    SECTION("gcode_render_mode set/get") {
        DisplaySettingsManager::instance().set_gcode_render_mode(2);
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 2);

        DisplaySettingsManager::instance().set_gcode_render_mode(1);
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 1);

        DisplaySettingsManager::instance().set_gcode_render_mode(0);
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 0);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

// The preview asks one question before it fetches anything: is the G-code viewer
// used at all. Thumbnail Only answers no, and that answer has to reach the live
// reader, because the whole G-code pipeline - download, layer index, background
// render pass - hangs off it.
TEST_CASE_METHOD(LVGLTestFixture, "preview_viewer_enabled follows the persisted render mode",
                 "[display_settings][render_mode]") {
    DisplaySettingsManager::instance().init_subjects();

    // The ladder puts a command-line mode above the setting, so this test only
    // says anything about the settings tier while nothing is pinned above it.
    const RuntimeConfig* runtime = get_runtime_config();
    REQUIRE((runtime == nullptr || runtime->gcode_render_mode < 0));
    REQUIRE(std::getenv("HELIX_GCODE_MODE") == nullptr);

    DisplaySettingsManager::instance().set_gcode_render_mode(
        helix::gcode_viewer::kRenderModeThumbnailOnly);
    CHECK_FALSE(helix::ui::preview_viewer_enabled());

    DisplaySettingsManager::instance().set_gcode_render_mode(2);
    CHECK(helix::ui::preview_viewer_enabled());

    DisplaySettingsManager::instance().set_gcode_render_mode(0);
    CHECK(helix::ui::preview_viewer_enabled());

    DisplaySettingsManager::instance().deinit_subjects();
}

// An explicit render-mode pick is a clear "retry GPU features" signal, so it must
// clear BOTH persistent crash-loop blocks (3D GLES viewer + 2D backdrop blur) so
// a user isn't stuck on the CPU paths forever after one prior driver crash.
TEST_CASE_METHOD(LVGLTestFixture,
                 "DisplaySettingsManager set_gcode_render_mode clears GPU crash-loop blocks",
                 "[display_settings]") {
    DisplaySettingsManager::instance().init_subjects();

    Config* config = Config::get_instance();
    config->set<bool>("/display/gpu_3d_blocked", true);
    config->set<bool>("/display/gpu_blur_blocked", true);

    DisplaySettingsManager::instance().set_gcode_render_mode(1);

    REQUIRE(config->get<bool>("/display/gpu_3d_blocked", true) == false);
    REQUIRE(config->get<bool>("/display/gpu_blur_blocked", true) == false);

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager dim seconds to index conversion",
                 "[display_settings]") {
    // dim_seconds_to_index: 0=Never, 30=30sec, 60=1min, 120=2min, 300=5min, 600=10min
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(0) == 0);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(30) == 1);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(60) == 2);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(120) == 3);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(300) == 4);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(600) == 5);

    // Unknown value defaults to index 5 (10 minutes)
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(999) == 5);

    // index_to_dim_seconds round-trip
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(0) == 0);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(1) == 30);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(2) == 60);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(3) == 120);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(4) == 300);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(5) == 600);

    // Out of range defaults to 600 (10 minutes)
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(-1) == 600);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(99) == 600);
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager sleep seconds to index conversion",
                 "[display_settings]") {
    // sleep_seconds_to_index: 0=Never, 60=1min, 300=5min, 600=10min, 1200=20min, 1800=30min,
    // 3600=60min
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(0) == 0);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(60) == 1);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(300) == 2);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(600) == 3);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(1200) == 4);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(1800) == 5);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(3600) == 6);

    // Unknown value defaults to index 4 (20 minutes)
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(999) == 4);

    // index_to_sleep_seconds round-trip
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(0) == 0);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(1) == 60);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(2) == 300);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(3) == 600);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(4) == 1200);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(5) == 1800);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(6) == 3600);

    // Out of range defaults to 1200 (20 minutes)
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(-1) == 1200);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(99) == 1200);
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager subject values match getters",
                 "[display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("brightness subject reflects setter") {
        DisplaySettingsManager::instance().set_brightness(55);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_brightness()) == 55);
    }

    SECTION("animations_enabled subject reflects setter") {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_animations_enabled()) == 0);

        DisplaySettingsManager::instance().set_animations_enabled(true);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_animations_enabled()) == 1);
    }

    SECTION("keep_navbar_visible subject reflects setter") {
        DisplaySettingsManager::instance().set_keep_navbar_visible(true);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_keep_navbar_visible()) == 1);

        DisplaySettingsManager::instance().set_keep_navbar_visible(false);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_keep_navbar_visible()) == 0);
    }

    SECTION("time_format subject reflects setter") {
        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_24);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_time_format()) == 1);

        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_12);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_time_format()) == 0);
    }

    SECTION("display_dim subject reflects setter") {
        DisplaySettingsManager::instance().set_display_dim_sec(120);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_display_dim()) ==
                120);
    }

    SECTION("display_sleep subject reflects setter") {
        DisplaySettingsManager::instance().set_display_sleep_sec(600);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_display_sleep()) ==
                600);
    }

    SECTION("bed_mesh_render_mode subject reflects setter") {
        DisplaySettingsManager::instance().set_bed_mesh_render_mode(2);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_bed_mesh_render_mode()) == 2);
    }

    SECTION("gcode_render_mode subject reflects setter") {
        DisplaySettingsManager::instance().set_gcode_render_mode(1);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_gcode_render_mode()) == 1);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

// ============================================================================
// Slider preview vs commit (drag-tick persistence)
//
// set_brightness() writes settings.json — serialize, fsync the file, fsync the
// directory, then copy a rolling backup — and on some platforms the backlight
// backend additionally forks a shell. Running that per drag tick is what made
// the brightness slider stutter on flash-backed hardware. preview_brightness()
// is the per-tick half: it must apply everything the user can SEE and persist
// nothing. The slider's `released` handler calls set_brightness() once.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager preview_brightness does not persist",
                 "[display_settings]") {
    Config* config = Config::get_instance();
    config->set<int>("/brightness", 55);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("preview applies to the subject but leaves config untouched") {
        const int applied = DisplaySettingsManager::instance().preview_brightness(73);

        CHECK(applied == 73);
        // Visible to the UI immediately...
        CHECK(DisplaySettingsManager::instance().get_brightness() == 73);
        // ...but nothing was written.
        CHECK(config->get<int>("/brightness", -1) == 55);
    }

    SECTION("a whole drag persists exactly once, on commit") {
        // Simulate the per-tick handler firing repeatedly, then release.
        for (int v = 60; v <= 70; ++v) {
            DisplaySettingsManager::instance().preview_brightness(v);
        }
        CHECK(config->get<int>("/brightness", -1) == 55); // still the old value

        DisplaySettingsManager::instance().set_brightness(70);
        CHECK(config->get<int>("/brightness", -1) == 70);
    }

    SECTION("preview clamps the same way set_brightness does") {
        CHECK(DisplaySettingsManager::instance().preview_brightness(0) == 10);
        CHECK(DisplaySettingsManager::instance().preview_brightness(500) == 100);
        CHECK(config->get<int>("/brightness", -1) == 55);
    }
}

// ============================================================================
// Timezone
// ============================================================================

TEST_CASE("DisplaySettingsManager::match_timezone resolves a browser zone to an offered one",
          "[display_settings][timezone]") {
    using M = DisplaySettingsManager;
    SECTION("an offered IANA id matches itself, whatever the offsets say") {
        CHECK(M::match_timezone("America/New_York", -300, -240) == "America/New_York");
        CHECK(M::match_timezone("America/Phoenix", -420, -420) == "America/Phoenix");
        CHECK(M::match_timezone("Asia/Tokyo", 0, 0) == "Asia/Tokyo");
    }
    SECTION("an unlisted id falls back to the zone with the same January and July offsets") {
        CHECK(M::match_timezone("America/Toronto", -300, -240) == "America/New_York");
        CHECK(M::match_timezone("America/Edmonton", -420, -360) == "America/Denver");
        CHECK(M::match_timezone("Asia/Calcutta", 330, 330) == "Asia/Kolkata");
        CHECK(M::match_timezone("Australia/Melbourne", 660, 600) == "Australia/Sydney");
        CHECK(M::match_timezone("Europe/Paris", 60, 120) == "Europe/Berlin");
        CHECK(M::match_timezone("America/Punta_Arenas", -180, -180) == "America/Sao_Paulo");
        CHECK(M::match_timezone("", 0, 60) == "Europe/London");
    }
    SECTION("no offered zone has those offsets") {
        CHECK(M::match_timezone("Mars/Olympus_Mons", 17, 17).empty());
        CHECK(M::match_timezone("", -300, -300 + 30).empty());
    }
}

namespace {

// Restores /display/timezone, the process TZ and the subjects around a test.
struct TimezoneSettingScope {
    Config* config = Config::get_instance();
    std::optional<nlohmann::json> stored;
    std::optional<std::string> saved_tz;

    TimezoneSettingScope() {
        config->set<int>("/display/sleep_sec", 1200); // makes sure the /display group exists
        if (const nlohmann::json* node = config->try_get_json("/display/timezone")) {
            stored = *node;
        }
        if (const char* tz = std::getenv("TZ")) {
            saved_tz = tz;
        }
        helix::timezone_env::reset_probe_state();
    }
    void erase_setting() {
        config->get_json("/display").erase("timezone");
    }
    ~TimezoneSettingScope() {
        if (stored) {
            config->set<nlohmann::json>("/display/timezone", *stored);
        } else {
            erase_setting();
        }
        helix::timezone_env::reset_probe_state();
        if (saved_tz) {
            ::setenv("TZ", saved_tz->c_str(), 1);
        }
        ::tzset();
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();
    }
};

std::string env_tz() {
    const char* tz = std::getenv("TZ");
    return tz ? tz : "<unset>";
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "an unpicked timezone follows the system zone",
                 "[display_settings][timezone]") {
    REQUIRE(DisplaySettingsManager::kSystemTimezoneAvailable);
    TimezoneSettingScope scope;
    DisplaySettingsManager& mgr = DisplaySettingsManager::instance();
    ::setenv("TZ", "EST5EDT", 1); // the zone the host gave the process
    scope.erase_setting();
    mgr.deinit_subjects();
    mgr.init_subjects();

    SECTION("startup leaves the process zone alone and the row reads System") {
        CHECK(env_tz() == "EST5EDT");
        CHECK(mgr.get_timezone().empty());
        CHECK(mgr.get_timezone_index() == 0);
        CHECK(DisplaySettingsManager::get_timezone_options().rfind(lv_tr("System"), 0) == 0);
        CHECK_FALSE(scope.config->exists("/display/timezone"));
    }

    SECTION("an explicit UTC is a pick, not the system zone") {
        mgr.set_timezone("UTC");
        CHECK(env_tz() == "UTC");
        CHECK(mgr.get_timezone() == "UTC");
        CHECK(mgr.get_timezone_index() != 0);
        CHECK(scope.config->get<std::string>("/display/timezone", "") == "UTC");
    }

    SECTION("choosing System again restores the host zone and forgets the pick") {
        mgr.set_timezone("Asia/Tokyo");
        CHECK(env_tz() == "Asia/Tokyo");
        mgr.set_timezone_by_index(0);
        CHECK(env_tz() == "EST5EDT");
        CHECK(mgr.get_timezone().empty());
        CHECK_FALSE(scope.config->exists("/display/timezone"));
    }

    SECTION("the startup apply follows the same rule") {
        helix::timezone_env::reset_probe_state();
        ::setenv("TZ", "JST-9", 1);
        CHECK(DisplaySettingsManager::apply_configured_timezone() == 0);
        CHECK(env_tz() == "JST-9");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a browser-reported zone is adopted only while none is picked",
                 "[display_settings][timezone]") {
    TimezoneSettingScope scope;
    DisplaySettingsManager& mgr = DisplaySettingsManager::instance();

    SECTION("unpicked: the matched zone is applied and saved") {
        scope.erase_setting();
        mgr.deinit_subjects();
        mgr.init_subjects();
        mgr.adopt_timezone_if_unset("America/Toronto", -300, -240);
        CHECK(mgr.get_timezone() == "America/New_York");
        CHECK(env_tz() == "America/New_York");
        CHECK(scope.config->get<std::string>("/display/timezone", "") == "America/New_York");
    }
    SECTION("an existing pick wins") {
        mgr.set_timezone("Asia/Tokyo");
        mgr.adopt_timezone_if_unset("America/Toronto", -300, -240);
        CHECK(mgr.get_timezone() == "Asia/Tokyo");
        CHECK(scope.config->get<std::string>("/display/timezone", "") == "Asia/Tokyo");
    }
    SECTION("an unmatched zone changes nothing") {
        scope.erase_setting();
        mgr.deinit_subjects();
        mgr.init_subjects();
        mgr.adopt_timezone_if_unset("Mars/Olympus_Mons", 17, 17);
        CHECK(mgr.get_timezone().empty());
        CHECK_FALSE(scope.config->exists("/display/timezone"));
    }
}

#if HELIX_ENABLE_SCREENSAVER

// The default saver comes from the registry on every tier, not from a platform probe:
// the gate steps a too-heavy default down on the device instead of disabling it up
// front. The legacy screensaver_enabled key migrates onto the same value.
TEST_CASE_METHOD(LVGLTestFixture, "screensaver type defaults to the registry default on every tier",
                 "[display_settings][screensaver]") {
    Config* config = Config::get_instance();
    config->set<int>("/display/sleep_sec", 1200); // makes sure the /display group exists
    const nlohmann::json* type_node = config->try_get_json("/display/screensaver_type");
    const std::optional<nlohmann::json> stored_type =
        type_node ? std::optional<nlohmann::json>(*type_node) : std::nullopt;
    const bool had_legacy = config->exists("/display/screensaver_enabled");
    const bool stored_legacy = config->get<bool>("/display/screensaver_enabled", true);

    config->get_json("/display").erase("screensaver_type");
    config->get_json("/display").erase("screensaver_enabled");
    DisplaySettingsManager::instance().deinit_subjects();

    const int registry_default = static_cast<int>(helix::ui::kDefaultScreensaverType);
    DisplaySettingsManager& mgr = DisplaySettingsManager::instance();

    SECTION("a fresh install runs the registry default") {
        mgr.init_subjects();
        REQUIRE(mgr.get_screensaver_type() == registry_default);
    }

    SECTION("legacy screensaver_enabled=true migrates onto the registry default") {
        config->set<bool>("/display/screensaver_enabled", true);
        mgr.init_subjects();
        REQUIRE(mgr.get_screensaver_type() == registry_default);
        // The migration persists its answer, by name, so the legacy key is read once.
        REQUIRE(config->get<std::string>("/display/screensaver_type", "") ==
                helix::ui::screensaver_name(helix::ui::kDefaultScreensaverType));
    }

    SECTION("legacy screensaver_enabled=false migrates to Off") {
        config->set<bool>("/display/screensaver_enabled", false);
        mgr.init_subjects();
        REQUIRE(mgr.get_screensaver_type() == 0);
    }

    SECTION("a stored name selects its saver, in any case") {
        config->set<std::string>("/display/screensaver_type", "Fireworks");
        mgr.init_subjects();
        REQUIRE(mgr.get_screensaver_type() == static_cast<int>(ScreensaverType::FIREWORKS));
    }

    SECTION("a legacy type number still selects the saver it stood for") {
        config->set<int>("/display/screensaver_type", 7);
        mgr.init_subjects();
        REQUIRE(mgr.get_screensaver_type() == static_cast<int>(ScreensaverType::MAZE_3D));
    }

    SECTION("an unknown name or an out-of-range number reads as the registry default") {
        config->set<std::string>("/display/screensaver_type", "lava lamp");
        mgr.init_subjects();
        CHECK(mgr.get_screensaver_type() == registry_default);
        mgr.deinit_subjects();
        config->set<int>("/display/screensaver_type", 42);
        mgr.init_subjects();
        CHECK(mgr.get_screensaver_type() == registry_default);
    }

    SECTION("a choice persists as its stable name") {
        mgr.init_subjects();
        mgr.set_screensaver_type(static_cast<int>(ScreensaverType::HYPERSPACE));
        CHECK(config->get<std::string>("/display/screensaver_type", "") == "hyperspace");
        mgr.set_screensaver_type(static_cast<int>(ScreensaverType::OFF));
        CHECK(config->get<std::string>("/display/screensaver_type", "") == "off");
    }

    // Restore both keys and re-derive the live subject from them. A later test may
    // call init_subjects() without deinit_subjects() first, and that init is a no-op
    // while subjects are already up, so the subject must hold the restored value.
    if (stored_type) {
        config->set<nlohmann::json>("/display/screensaver_type", *stored_type);
    } else {
        config->get_json("/display").erase("screensaver_type");
    }
    if (had_legacy) {
        config->set<bool>("/display/screensaver_enabled", stored_legacy);
    } else {
        config->get_json("/display").erase("screensaver_enabled");
    }
    mgr.deinit_subjects();
    mgr.init_subjects();
}

// Without a dimmable backlight the dim timeout only starts a screensaver, so with the
// saver Off there is no dim stage: no row to offer and no sleep >= dim coupling.
TEST_CASE_METHOD(LVGLTestFixture, "dim row and sleep coupling follow the screensaver type",
                 "[display_settings][dim_row][screensaver]") {
    Config* config = Config::get_instance();
    config->set<int>("/display/sleep_sec", 1200); // makes sure the /display group exists
    const nlohmann::json* type_node = config->try_get_json("/display/screensaver_type");
    const std::optional<nlohmann::json> stored_type =
        type_node ? std::optional<nlohmann::json>(*type_node) : std::nullopt;

    using Kind = DisplaySettingsManager::DimRowKind;
    DisplaySettingsManager& mgr = DisplaySettingsManager::instance();
    mgr.deinit_subjects();
    config->set<std::string>("/display/screensaver_type", "off");
    mgr.init_subjects();
    REQUIRE_FALSE(mgr.has_dimming_control());
    lv_subject_t* kind = lv_xml_get_subject(nullptr, "settings_dim_row_kind");
    REQUIRE(kind != nullptr);

    SECTION("a saver stored as Off starts the row hidden") {
        CHECK(lv_subject_get_int(kind) == static_cast<int>(Kind::Hidden));
        CHECK_FALSE(mgr.should_couple_sleep_to_dim());
    }

    SECTION("choosing a saver shows Screensaver after, and Off hides it again") {
        mgr.set_screensaver_type(static_cast<int>(ScreensaverType::FIREWORKS));
        CHECK(lv_subject_get_int(kind) == static_cast<int>(Kind::ScreensaverAfter));
        CHECK(mgr.should_couple_sleep_to_dim());
        mgr.set_screensaver_type(static_cast<int>(ScreensaverType::OFF));
        CHECK(lv_subject_get_int(kind) == static_cast<int>(Kind::Hidden));
        CHECK_FALSE(mgr.should_couple_sleep_to_dim());
    }

    if (stored_type) {
        config->set<nlohmann::json>("/display/screensaver_type", *stored_type);
    } else {
        config->get_json("/display").erase("screensaver_type");
    }
    mgr.deinit_subjects();
    mgr.init_subjects();
}

#endif // HELIX_ENABLE_SCREENSAVER
