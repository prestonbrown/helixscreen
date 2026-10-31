// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "config.h"
#include "display_settings_manager.h"
#include "screensaver.h"
#include "screensaver_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Screensaver Settings Tests
// ============================================================================

#if HELIX_ENABLE_SCREENSAVER

TEST_CASE_METHOD(LVGLTestFixture,
                 "a fresh install defaults the screensaver to the flip clock on every tier",
                 "[screensaver][display_settings]") {
    Config* config = Config::get_instance();
    const bool had_type = config->exists("/display/screensaver_type");
    const int stored_type = config->get<int>("/display/screensaver_type", 0);
    const bool had_legacy = config->exists("/display/screensaver_enabled");
    const bool stored_legacy = config->get<bool>("/display/screensaver_enabled", true);
    if (had_type) {
        config->get_json("/display").erase("screensaver_type");
    }
    if (had_legacy) {
        config->get_json("/display").erase("screensaver_enabled");
    }

    DisplaySettingsManager::instance().init_subjects();
    CHECK(DisplaySettingsManager::instance().get_screensaver_type() ==
          static_cast<int>(ScreensaverType::FLIP_CLOCK));
    DisplaySettingsManager::instance().deinit_subjects();

    if (had_type) {
        config->set<int>("/display/screensaver_type", stored_type);
    }
    if (had_legacy) {
        config->set<bool>("/display/screensaver_enabled", stored_legacy);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Screensaver type set/get round trip",
                 "[screensaver][display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("set to Off") {
        DisplaySettingsManager::instance().set_screensaver_type(0);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 0);
    }

    SECTION("set to Starfield") {
        DisplaySettingsManager::instance().set_screensaver_type(2);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 2);
    }

    SECTION("set to 3D Pipes") {
        DisplaySettingsManager::instance().set_screensaver_type(3);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 3);
    }

    SECTION("set to Bouncing Printer") {
        DisplaySettingsManager::instance().set_screensaver_type(4);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 4);
    }

    SECTION("set to Bouncing Logo") {
        DisplaySettingsManager::instance().set_screensaver_type(6);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 6);
    }

    SECTION("set back to Flying Toasters") {
        DisplaySettingsManager::instance().set_screensaver_type(0);
        DisplaySettingsManager::instance().set_screensaver_type(1);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 1);
    }

    SECTION("out of range clamped") {
        DisplaySettingsManager::instance().set_screensaver_type(99);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() ==
                helix::ui::screensaver_last_type());

        DisplaySettingsManager::instance().set_screensaver_type(-1);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 0);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "Screensaver type survives a restart",
                 "[screensaver][display_settings]") {
    // The load path reads the stored name independently of the setter, so a choice the
    // setter accepts must read back as the same saver on the next boot.
    Config* config = Config::get_instance();

    // The fixture leaves the subjects initialized, and init_subjects() returns
    // early in that state — so the config has to be seeded while they are down
    // for the boot-time read to be the thing under test.
    auto boot_with = [&](const nlohmann::json& persisted) {
        DisplaySettingsManager::instance().deinit_subjects();
        config->set<nlohmann::json>("/display/screensaver_type", persisted);
        DisplaySettingsManager::instance().init_subjects();
        return DisplaySettingsManager::instance().get_screensaver_type();
    };
    const int bounce = static_cast<int>(ScreensaverType::BOUNCING_PRINTER);
    const int fallback = static_cast<int>(helix::ui::kDefaultScreensaverType);

    SECTION("Bouncing Printer is read back as itself") {
        DisplaySettingsManager::instance().set_screensaver_type(bounce);
        const nlohmann::json* written = config->try_get_json("/display/screensaver_type");
        REQUIRE(written != nullptr);
        CHECK(*written == "bounce");
        REQUIRE(boot_with(*written) == bounce);
        REQUIRE(boot_with("bounce") == bounce);
    }

    SECTION("a legacy type number past the last one reads as the default, not wrapped") {
        REQUIRE(boot_with(99) == fallback);
    }

    SECTION("a negative legacy number reads as the default") {
        REQUIRE(boot_with(-7) == fallback);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Screensaver type subject reflects setter",
                 "[screensaver][display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    DisplaySettingsManager::instance().set_screensaver_type(0);
    REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_screensaver_type()) == 0);

    DisplaySettingsManager::instance().set_screensaver_type(2);
    REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_screensaver_type()) == 2);

    DisplaySettingsManager::instance().set_screensaver_type(1);
    REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_screensaver_type()) == 1);

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a stored number past the last type reads as the default; a subject clamps to the last",
    "[screensaver][display_settings]") {
    Config* config = Config::get_instance();
    const nlohmann::json* stored_node = config->try_get_json("/display/screensaver_type");
    const nlohmann::json stored_before = stored_node ? *stored_node : nlohmann::json("toasters");
    config->set<int>("/display/screensaver_type", 99);

    // init_subjects() returns early while subjects are up, and the fixture leaves
    // them up, so the stored read is only exercised with them down.
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();
    CHECK(DisplaySettingsManager::instance().get_screensaver_type() ==
          static_cast<int>(helix::ui::kDefaultScreensaverType));

    // The manager clamps what the subject holds, whoever wrote it.
    lv_subject_set_int(DisplaySettingsManager::instance().subject_screensaver_type(), 99);
    CHECK(ScreensaverManager::configured_type() ==
          static_cast<ScreensaverType>(helix::ui::screensaver_last_type()));

    DisplaySettingsManager::instance().deinit_subjects();
    config->set<nlohmann::json>("/display/screensaver_type", stored_before);
}

// ============================================================================
// FlyingToasterScreensaver Lifecycle Tests
// ============================================================================

#include "ui_screensaver.h"

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver starts inactive", "[screensaver]") {
    FlyingToasterScreensaver ss;
    REQUIRE(ss.is_active() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver start/stop lifecycle",
                 "[screensaver]") {
    FlyingToasterScreensaver ss;

    SECTION("start activates screensaver") {
        ss.start();
        REQUIRE(ss.is_active() == true);
        ss.stop();
    }

    SECTION("stop deactivates screensaver") {
        ss.start();
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }

    SECTION("double start is safe") {
        ss.start();
        ss.start();
        REQUIRE(ss.is_active() == true);
        ss.stop();
    }

    SECTION("double stop is safe") {
        ss.start();
        ss.stop();
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }

    SECTION("stop without start is safe") {
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver creates overlay on lv_layer_top",
                 "[screensaver]") {
    FlyingToasterScreensaver ss;

    int children_before = lv_obj_get_child_count(lv_layer_top());
    ss.start();
    int children_after = lv_obj_get_child_count(lv_layer_top());
    REQUIRE(children_after > children_before);

    ss.stop();
    REQUIRE_FALSE(ss.is_active());
    // Overlay is hidden and queued for async deletion (can't flush without
    // running all timers, which disrupts other fixture state)
    int children_final = lv_obj_get_child_count(lv_layer_top());
    REQUIRE(children_final <= children_after);
}

// ============================================================================
// FlyingToasterScreensaver motion follows elapsed time
// ============================================================================

#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_motion.h"

namespace {

using ToasterAccess = FlyingToasterScreensaverTestAccess;
using helix::ui::screensaver::flap_frame_at;
using helix::ui::screensaver::flight_pos_at;
using helix::ui::screensaver::FlightPos;

constexpr int32_t kToasterFlightPx = 1600;

void run_toaster_tick(const FlyingToasterScreensaver& ss) {
    lv_timer_t* timer = SaverTestAccess::timer(ss);
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

struct SpriteView {
    bool hidden;
    int32_t x;
    int32_t y;
    int frame;

    bool operator==(const SpriteView& o) const {
        return hidden == o.hidden && x == o.x && y == o.y && frame == o.frame;
    }
};

std::vector<SpriteView> view_sprites(const FlyingToasterScreensaver& ss) {
    std::vector<SpriteView> out;
    for (const auto& s : ToasterAccess::sprites(ss)) {
        out.push_back(
            {lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN), lv_obj_get_style_x(s.img, LV_PART_MAIN),
             lv_obj_get_style_y(s.img, LV_PART_MAIN), ToasterAccess::frame_of(ss, s.img)});
    }
    return out;
}

std::vector<int> shown_frames(const FlyingToasterScreensaver& ss) {
    std::vector<int> frames;
    for (const auto& s : ToasterAccess::sprites(ss)) {
        frames.push_back(ToasterAccess::frame_of(ss, s.img));
    }
    return frames;
}

/// A toaster on a 10 s or 16 s flight changes wing frame every 50 ms; one on a 24 s flight,
/// every 100 ms.
uint32_t flap_step_ms_for(int32_t fly_ms) {
    return fly_ms >= 20000 ? 100 : 50;
}

struct SpriteTally {
    int visible_toasters = 0;
    int visible_reversed_toasters = 0;
    int visible_only_at_full_size = 0;
};

/// Checks every flying sprite against where `elapsed_ms` of flight puts it, and counts what
/// the check covered. `initial_frames` are the frames the sprites showed at start.
SpriteTally check_sprites_at(const FlyingToasterScreensaver& ss, uint32_t elapsed_ms,
                             const std::vector<int>& initial_frames) {
    lv_display_t* disp = lv_display_get_default();
    const int32_t w = lv_display_get_horizontal_resolution(disp);
    const int32_t h = lv_display_get_vertical_resolution(disp);
    const int32_t size = w > 800 ? 128 : 64;
    const auto sprites = ToasterAccess::sprites(ss);
    REQUIRE(sprites.size() == initial_frames.size());

    SpriteTally tally;
    for (size_t i = 0; i < sprites.size(); i++) {
        const auto& s = sprites[i];
        CAPTURE(i, elapsed_ms);
        const FlightPos pos =
            flight_pos_at(elapsed_ms, s.start_x, s.start_y, s.fly_ms, s.delay_ms, kToasterFlightPx);
        if (!pos.started) {
            continue;
        }
        const bool on_screen = pos.x + size > 0 && pos.x < w && pos.y + size > 0 && pos.y < h;
        CHECK(lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN) == !on_screen);
        if (!on_screen) {
            continue;
        }
        CHECK(lv_obj_get_style_x(s.img, LV_PART_MAIN) == pos.x);
        CHECK(lv_obj_get_style_y(s.img, LV_PART_MAIN) == pos.y);
        if (size > 64 && (pos.x + 64 <= 0 || pos.y + 64 <= 0)) {
            tally.visible_only_at_full_size++;
        }
        if (s.is_toaster) {
            tally.visible_toasters++;
            // Reversed toasters start on wing frame 2.
            if (initial_frames[i] == 2) {
                tally.visible_reversed_toasters++;
            }
            CHECK(ToasterAccess::frame_of(ss, s.img) ==
                  flap_frame_at(elapsed_ms, s.delay_ms, flap_step_ms_for(s.fly_ms),
                                static_cast<uint8_t>(initial_frames[i])));
        }
    }
    return tally;
}

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "FlyingToasterScreensaver lands sprites by elapsed time however callbacks split it",
    "[screensaver][screensaver_motion]") {
    FlyingToasterScreensaver one_tick;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_one{one_tick};
    FlyingToasterScreensaver eleven_ticks;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_eleven{eleven_ticks};

    one_tick.start();
    eleven_ticks.start();
    REQUIRE(one_tick.is_active());
    REQUIRE(eleven_ticks.is_active());
    REQUIRE(ToasterAccess::frames_decoded(one_tick));
    REQUIRE(ToasterAccess::frames_decoded(eleven_ticks));

    // Every sprite starts off-screen. Four seconds in, the 16 s and 24 s flights are
    // crossing it; both savers get there by the same calls.
    for (int i = 0; i < 10; i++) {
        lv_tick_inc(400);
        run_toaster_tick(one_tick);
        run_toaster_tick(eleven_ticks);
    }
    const auto before = view_sprites(one_tick);

    for (int i = 0; i < 11; i++) {
        lv_tick_inc(10);
        run_toaster_tick(eleven_ticks);
    }
    run_toaster_tick(one_tick);

    const auto one = view_sprites(one_tick);
    const auto eleven = view_sprites(eleven_ticks);
    const auto sprites = ToasterAccess::sprites(one_tick);
    REQUIRE(one.size() == eleven.size());
    REQUIRE(one.size() == before.size());
    int moved = 0;
    int flapped = 0;
    for (size_t i = 0; i < one.size(); i++) {
        CAPTURE(i);
        CHECK(one[i].hidden == eleven[i].hidden);
        if (one[i].hidden || eleven[i].hidden) {
            continue;
        }
        CHECK(one[i].x == eleven[i].x);
        CHECK(one[i].y == eleven[i].y);
        CHECK(one[i].frame == eleven[i].frame);
        if (!before[i].hidden && one[i].x != before[i].x) {
            moved++;
        }
        if (sprites[i].is_toaster && !before[i].hidden && one[i].frame != before[i].frame) {
            flapped++;
        }
    }
    // The 110 ms window moved visible sprites and changed toasters' wing frames, so the
    // comparison covered both.
    REQUIRE(moved > 0);
    REQUIRE(flapped > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver flies 128 px sprites on displays wider than 800 px",
                 "[screensaver][screensaver_motion]") {
    ScopedResolution res(lv_display_get_default(), 1024, 600);
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    for (const auto& s : ToasterAccess::sprites(ss)) {
        CHECK(lv_image_get_scale(s.img) == 512);
    }
    const auto initial = shown_frames(ss);

    lv_tick_inc(100);
    run_toaster_tick(ss);

    const SpriteTally tally = check_sprites_at(ss, 100, initial);
    REQUIRE(tally.visible_toasters > 0);
    // Some visible sprite overlaps the screen only because it is 128 px, so the sprite size
    // decided its visibility.
    REQUIRE(tally.visible_only_at_full_size > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver flaps each toaster from its own initial frame",
                 "[screensaver][screensaver_motion]") {
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    const auto initial = shown_frames(ss);

    // 4.1 s in, the reversed 16 s toasters are on screen at a point in the wing cycle where
    // one started from frame 2 shows a different frame than one started from frame 0.
    for (int i = 0; i < 10; i++) {
        lv_tick_inc(400);
        run_toaster_tick(ss);
    }
    lv_tick_inc(100);
    run_toaster_tick(ss);

    REQUIRE(check_sprites_at(ss, 4100, initial).visible_reversed_toasters > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver restarts its flight on start after stop",
                 "[screensaver][screensaver_motion]") {
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    const auto initial = shown_frames(ss);
    // 370 ms puts both flap rates on a frame other than the initial one.
    lv_tick_inc(370);
    run_toaster_tick(ss);
    REQUIRE(check_sprites_at(ss, 370, initial).visible_toasters > 0);
    const auto first_run = view_sprites(ss);

    ss.stop();
    lv_tick_inc(5000);
    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    CHECK(shown_frames(ss) == initial);

    lv_tick_inc(370);
    run_toaster_tick(ss);
    CHECK(check_sprites_at(ss, 370, initial).visible_toasters > 0);
    CHECK(view_sprites(ss) == first_run);
}

// ============================================================================
// ScreensaverManager Tests
// ============================================================================

#include "screensaver.h"

TEST_CASE("HELIX_SCREENSAVER_NOW names a screensaver", "[screensaver]") {
    SECTION("each name selects its own screensaver") {
        REQUIRE(screensaver_type_from_env("toasters", ScreensaverType::OFF) ==
                ScreensaverType::FLYING_TOASTERS);
        REQUIRE(screensaver_type_from_env("starfield", ScreensaverType::OFF) ==
                ScreensaverType::STARFIELD);
        REQUIRE(screensaver_type_from_env("pipes", ScreensaverType::OFF) ==
                ScreensaverType::PIPES_3D);
        REQUIRE(screensaver_type_from_env("bounce", ScreensaverType::OFF) ==
                ScreensaverType::BOUNCING_PRINTER);
        REQUIRE(screensaver_type_from_env("fireworks", ScreensaverType::OFF) ==
                ScreensaverType::FIREWORKS);
        REQUIRE(screensaver_type_from_env("logo", ScreensaverType::OFF) ==
                ScreensaverType::BOUNCING_LOGO);
    }

    SECTION("a name wins over what is configured") {
        REQUIRE(screensaver_type_from_env("bounce", ScreensaverType::STARFIELD) ==
                ScreensaverType::BOUNCING_PRINTER);
        REQUIRE(screensaver_type_from_env("starfield", ScreensaverType::BOUNCING_PRINTER) ==
                ScreensaverType::STARFIELD);
    }

    SECTION("anything else means the configured one") {
        REQUIRE(screensaver_type_from_env("1", ScreensaverType::BOUNCING_PRINTER) ==
                ScreensaverType::BOUNCING_PRINTER);
        REQUIRE(screensaver_type_from_env("yes", ScreensaverType::PIPES_3D) ==
                ScreensaverType::PIPES_3D);
    }

    SECTION("with nothing configured it falls back to toasters") {
        REQUIRE(screensaver_type_from_env("1", ScreensaverType::OFF) ==
                ScreensaverType::FLYING_TOASTERS);
        REQUIRE(screensaver_type_from_env("", ScreensaverType::OFF) ==
                ScreensaverType::FLYING_TOASTERS);
    }

    SECTION("a name is matched exactly, not by prefix") {
        REQUIRE(screensaver_type_from_env("bouncing", ScreensaverType::OFF) ==
                ScreensaverType::FLYING_TOASTERS);
        REQUIRE(screensaver_type_from_env("Bounce", ScreensaverType::OFF) ==
                ScreensaverType::FLYING_TOASTERS);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager starts inactive", "[screensaver]") {
    REQUIRE(ScreensaverManager::instance().is_active() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager routes the configured setting to a type",
                 "[screensaver]") {
    // configured_type() bounds the persisted int independently of the setter.
    // A type the settings screen can select but this rejects reads as OFF, and
    // the screensaver the user chose simply never starts.
    auto configured_for = [](int setting) {
        DisplaySettingsManager::instance().set_screensaver_type(setting);
        return ScreensaverManager::configured_type();
    };

    REQUIRE(configured_for(0) == ScreensaverType::OFF);
    REQUIRE(configured_for(1) == ScreensaverType::FLYING_TOASTERS);
    REQUIRE(configured_for(2) == ScreensaverType::STARFIELD);
    REQUIRE(configured_for(3) == ScreensaverType::PIPES_3D);
    REQUIRE(configured_for(4) == ScreensaverType::BOUNCING_PRINTER);
    REQUIRE(configured_for(6) == ScreensaverType::BOUNCING_LOGO);

    DisplaySettingsManager::instance().set_screensaver_type(0);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager start/stop lifecycle", "[screensaver]") {
    auto& mgr = ScreensaverManager::instance();

    SECTION("start OFF does nothing") {
        mgr.start(ScreensaverType::OFF);
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop Flying Toasters") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop Starfield") {
        mgr.start(ScreensaverType::STARFIELD);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop 3D Pipes") {
        mgr.start(ScreensaverType::PIPES_3D);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop Bouncing Printer") {
        mgr.start(ScreensaverType::BOUNCING_PRINTER);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop Bouncing Logo") {
        mgr.start(ScreensaverType::BOUNCING_LOGO);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("switching types stops previous") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(mgr.is_active() == true);
        mgr.start(ScreensaverType::STARFIELD);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("double stop is safe") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        mgr.stop();
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }
}

// ============================================================================
// Active screen hidden while a saver runs
// ============================================================================

#include "screen_hide_hold.h"

namespace {

bool is_hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Stops the shared manager however the test exits, so a failed assertion cannot
/// leave a saver and its screen hold running into the next test.
struct StopSaverOnExit {
    ~StopSaverOnExit() {
        ScreensaverManager::instance().stop();
    }
};

void count_event(lv_event_t* e) {
    ++*static_cast<int*>(lv_event_get_user_data(e));
}

/// lv_obj_remove_flag(HIDDEN) invalidates the screen after clearing the flag, so an
/// unhide at any moment reaches the display as an invalidation with the flag clear.
struct UnhideProbe {
    lv_obj_t* screen;
    bool saw_visible;
};

void record_invalidate_while_visible(lv_event_t* e) {
    auto* probe = static_cast<UnhideProbe*>(lv_event_get_user_data(e));
    if (!lv_obj_has_flag(probe->screen, LV_OBJ_FLAG_HIDDEN)) {
        probe->saw_visible = true;
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager hides the active screen while a saver runs",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    REQUIRE(lv_screen_active() == test_screen());

    for (ScreensaverType type : {ScreensaverType::FLYING_TOASTERS, ScreensaverType::STARFIELD,
                                 ScreensaverType::PIPES_3D}) {
        CAPTURE(static_cast<int>(type));
        REQUIRE_FALSE(is_hidden(test_screen()));

        mgr.start(type);
        REQUIRE(mgr.is_active());
        CHECK(is_hidden(test_screen()));

        mgr.stop();
        CHECK_FALSE(is_hidden(test_screen()));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager leaves an already hidden screen hidden",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    lv_obj_add_flag(test_screen(), LV_OBJ_FLAG_HIDDEN);

    mgr.start(ScreensaverType::STARFIELD);
    REQUIRE(mgr.is_active());
    REQUIRE(helix::active_screen_hide_hold().is_held());

    mgr.stop();
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    CHECK(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture, "switching screensaver type keeps the screen hidden throughout",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    lv_display_t* disp = lv_obj_get_display(test_screen());

    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());
    REQUIRE(is_hidden(test_screen()));

    UnhideProbe probe{test_screen(), false};
    lv_display_add_event_cb(disp, record_invalidate_while_visible, LV_EVENT_INVALIDATE_AREA,
                            &probe);

    mgr.start(ScreensaverType::STARFIELD);
    const bool starfield_active = mgr.is_active();
    mgr.start(ScreensaverType::PIPES_3D);
    const bool pipes_active = mgr.is_active();
    const bool unhidden_during_switch = probe.saw_visible;
    const bool hidden_after_switch = is_hidden(test_screen());

    mgr.stop();
    const bool probe_saw_final_unhide = probe.saw_visible;
    lv_display_remove_event_cb_with_user_data(disp, record_invalidate_while_visible, &probe);

    CHECK(starfield_active);
    CHECK(pipes_active);
    CHECK_FALSE(unhidden_during_switch);
    CHECK(hidden_after_switch);
    // The probe does register an unhide when one happens.
    CHECK(probe_saw_final_unhide);
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a screensaver that fails to start leaves the screen visible and the manager idle",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    const ScreensaverType type = GENERATE(ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
    CAPTURE(static_cast<int>(type));

    // Starfield and pipes refuse to start without a default display.
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    lv_display_set_default(nullptr);
    mgr.start(type);
    lv_display_set_default(disp);

    CHECK_FALSE(mgr.is_active());
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    CHECK_FALSE(is_hidden(test_screen()));

    // Nothing was left behind: the next saver that does start still hides the screen.
    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());
    CHECK(is_hidden(test_screen()));
    mgr.stop();
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a widget created on the screen during a saver is visible after it stops",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());

    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());

    // Stands in for a modal or notification opened while the saver is up: both are
    // children of the active screen.
    lv_obj_t* dialog = lv_obj_create(lv_screen_active());
    lv_obj_set_size(dialog, 200, 120);
    lv_obj_center(dialog);
    lv_obj_update_layout(dialog);
    CHECK_FALSE(lv_obj_is_visible(dialog));

    mgr.stop();
    lv_obj_update_layout(dialog);
    CHECK(lv_obj_is_visible(dialog));
}

TEST_CASE_METHOD(LVGLTestFixture, "the panel under a running screensaver is not drawn",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    const ScreensaverType type = GENERATE(ScreensaverType::FLYING_TOASTERS,
                                          ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
    CAPTURE(static_cast<int>(type));

    lv_obj_t* child = lv_obj_create(test_screen());
    lv_obj_set_size(child, 100, 100);
    lv_obj_update_layout(child);
    int draws = 0;
    lv_obj_add_event_cb(child, count_event, LV_EVENT_DRAW_MAIN, &draws);
    lv_display_t* disp = lv_obj_get_display(child);

    mgr.start(type);
    REQUIRE(mgr.is_active());
    lv_obj_invalidate(child);
    lv_refr_now(disp);
    const int draws_under_saver = draws;

    mgr.stop();
    lv_obj_invalidate(child);
    lv_refr_now(disp);

    CHECK(draws_under_saver == 0);
    // The same invalidate and refresh draw the child once the saver is gone.
    CHECK(draws > draws_under_saver);
}

// ============================================================================
// Starfield and pipes redraw only what their canvas changes
// ============================================================================

#include <algorithm>
#include <cstring>

namespace {

using StarAccess = StarfieldScreensaverTestAccess;
using PipesAccess = PipesScreensaverTestAccess;

// A pipe grows one grid step per 100 ms of frame time.
constexpr uint32_t kPipesStepMs = 100;

/// Counts the draw tasks `obj` itself adds to refreshes and snapshots, for as long as it lives.
class DrawTaskCounter {
  public:
    explicit DrawTaskCounter(lv_obj_t* obj) : obj_(obj) {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(obj_, on_task_added, LV_EVENT_DRAW_TASK_ADDED, this);
    }
    ~DrawTaskCounter() {
        if (helix::test::widget_in_tree(obj_)) {
            lv_obj_remove_event_cb_with_user_data(obj_, on_task_added, this);
            lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        }
    }
    DrawTaskCounter(const DrawTaskCounter&) = delete;
    DrawTaskCounter& operator=(const DrawTaskCounter&) = delete;

    int total = 0;
    int fills = 0;
    int images = 0;

  private:
    static void on_task_added(lv_event_t* e) {
        auto* self = static_cast<DrawTaskCounter*>(lv_event_get_user_data(e));
        const lv_draw_task_type_t type = lv_draw_task_get_type(lv_event_get_draw_task(e));
        self->total++;
        self->fills += type == LV_DRAW_TASK_TYPE_FILL;
        self->images += type == LV_DRAW_TASK_TYPE_IMAGE;
    }

    lv_obj_t* obj_;
};

using helix::test::InvalidatedAreas;

lv_area_t coords_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

/// Redraws everything on the top layer now.
void refresh_top_layer() {
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(lv_display_get_default());
}

void check_overlay_draws_only_its_canvas(lv_obj_t* overlay, lv_obj_t* canvas) {
    REQUIRE(overlay != nullptr);
    REQUIRE(canvas != nullptr);
    DrawTaskCounter overlay_tasks(overlay);
    DrawTaskCounter canvas_tasks(canvas);

    refresh_top_layer();

    CHECK(overlay_tasks.fills == 0);
    // The refresh did reach the overlay: it drew the canvas on top of it.
    CHECK(canvas_tasks.images > 0);
}

/// The overlay is deleted on a later timer pass. Until then a refresh (a wake's
/// lv_refr_now) or a top-layer snapshot (a screenshot) must not draw the canvas, whose
/// buffer is already freed.
void check_stopped_canvas_is_not_drawn(lv_obj_t* canvas) {
    REQUIRE(helix::test::widget_in_tree(canvas));
    CHECK(lv_obj_has_flag(canvas, LV_OBJ_FLAG_HIDDEN));

    DrawTaskCounter canvas_tasks(canvas);
    refresh_top_layer();
    lv_draw_buf_t* snapshot = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snapshot != nullptr);
    lv_draw_buf_destroy(snapshot);

    CHECK(canvas_tasks.total == 0);
}

struct PixelChanges {
    size_t changed = 0;
    size_t outside_areas = 0;
};

/// Compares the canvas buffer with `before`, and counts the changed pixels no area in
/// `areas` (display coordinates) covers.
PixelChanges diff_canvas(const std::vector<uint8_t>& before, const lv_draw_buf_t* buf,
                         const lv_area_t& canvas_coords, const std::vector<lv_area_t>& areas) {
    PixelChanges out;
    for (uint32_t y = 0; y < buf->header.h; y++) {
        for (uint32_t x = 0; x < buf->header.w; x++) {
            const size_t i = y * buf->header.stride + x * 4;
            if (std::memcmp(&before[i], &buf->data[i], 4) == 0) {
                continue;
            }
            out.changed++;
            const int32_t px = canvas_coords.x1 + static_cast<int32_t>(x);
            const int32_t py = canvas_coords.y1 + static_cast<int32_t>(y);
            const bool covered =
                std::any_of(areas.begin(), areas.end(), [px, py](const lv_area_t& a) {
                    return px >= a.x1 && px <= a.x2 && py >= a.y1 && py <= a.y2;
                });
            out.outside_areas += covered ? 0 : 1;
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "the starfield and pipes overlays draw nothing under their canvas",
                 "[screensaver][screensaver_canvas]") {
    SECTION("starfield") {
        StarfieldScreensaver ss;
        ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        check_overlay_draws_only_its_canvas(SaverTestAccess::overlay(ss),
                                            SaverTestAccess::canvas(ss));
    }

    SECTION("pipes") {
        PipesScreensaver ss;
        ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        check_overlay_draws_only_its_canvas(SaverTestAccess::overlay(ss),
                                            SaverTestAccess::canvas(ss));
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "each pipes grow step invalidates less than the canvas and every pixel it changes",
                 "[screensaver][screensaver_canvas]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 42);
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* canvas = SaverTestAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    REQUIRE(lv_color_format_get_size(static_cast<lv_color_format_t>(buf->header.cf)) == 4);
    const lv_area_t canvas_coords = coords_of(canvas);
    const int64_t canvas_px = lv_area_get_size(&canvas_coords);

    InvalidatedAreas invalidated(lv_obj_get_display(canvas));
    // The first step also starts the third pipe; later ones turn corners and draw joints.
    for (int step = 0; step < 40; step++) {
        CAPTURE(step);
        const std::vector<uint8_t> before(buf->data, buf->data + buf->data_size);
        invalidated.areas.clear();

        advance_saver(ss, kPipesStepMs);

        const PixelChanges changes = diff_canvas(before, buf, canvas_coords, invalidated.areas);
        REQUIRE(changes.changed > 0);
        CHECK(changes.outside_areas == 0);
        REQUIRE_FALSE(invalidated.areas.empty());
        for (const lv_area_t& area : invalidated.areas) {
            CHECK(lv_area_get_size(&area) < canvas_px);
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "the pipes dissolve invalidates every pixel it clears",
                 "[screensaver][screensaver_canvas]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 11);
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* canvas = SaverTestAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    REQUIRE(lv_color_format_get_size(static_cast<lv_color_format_t>(buf->header.cf)) == 4);
    const lv_area_t canvas_coords = coords_of(canvas);

    for (int i = 0; i < 20000 && PipesAccess::sim(ss).phase() == helix::ui::PipesPhase::GROWING;
         i++) {
        advance_saver(ss, 3 * kPipesStepMs);
    }
    REQUIRE(PipesAccess::sim(ss).phase() == helix::ui::PipesPhase::DISSOLVING);

    size_t cleared = 0;
    while (PipesAccess::sim(ss).phase() == helix::ui::PipesPhase::DISSOLVING) {
        const std::vector<uint8_t> before(buf->data, buf->data + buf->data_size);
        InvalidatedAreas invalidated(lv_obj_get_display(canvas));
        advance_saver(ss, kPipesStepMs);
        const PixelChanges changes = diff_canvas(before, buf, canvas_coords, invalidated.areas);
        cleared += changes.changed;
        CHECK(changes.outside_areas == 0);
    }
    CHECK(cleared > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a stopped starfield or pipes canvas is hidden until its overlay is deleted",
                 "[screensaver][screensaver_canvas]") {
    SECTION("starfield") {
        StarfieldScreensaver ss;
        ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        lv_obj_t* canvas = SaverTestAccess::canvas(ss);
        REQUIRE(canvas != nullptr);
        ss.stop();
        check_stopped_canvas_is_not_drawn(canvas);
    }

    SECTION("pipes") {
        PipesScreensaver ss;
        ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        lv_obj_t* canvas = SaverTestAccess::canvas(ss);
        REQUIRE(canvas != nullptr);
        ss.stop();
        check_stopped_canvas_is_not_drawn(canvas);
    }
}

// ============================================================================
// Refresh period while a saver runs
// ============================================================================

#include "../test_helpers/refresh_period_hold_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "refresh_period_hold.h"
#include "refresh_timing_env.h"
#include "screensaver_base.h"

namespace {

using helix::anim_timer_period;
using helix::default_refr_timer_period;

constexpr uint32_t kGlobalPeriodMs = 40;
// Unlike the 16 ms default, so the checks see the configured period reach each saver.
constexpr uint32_t kSaverPeriodMs = 20;

/// Period of the timer the manager's running saver ticks on.
uint32_t running_saver_timer_period(ScreensaverType type) {
    Screensaver* active =
        helix::ScreensaverManagerTestAccess::active(ScreensaverManager::instance());
    REQUIRE(active != nullptr);
    REQUIRE(active->type() == type);
    // Every registered saver runs on SaverBase.
    const lv_timer_t* timer =
        SaverTestAccess::timer(static_cast<const helix::ui::SaverBase&>(*active));
    REQUIRE(timer != nullptr);
    return timer->period;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a running screensaver ticks at HELIX_SCREENSAVER_REFR_PERIOD_MS until it stops",
                 "[screensaver][refresh_period]") {
    const ScreensaverType type = GENERATE(ScreensaverType::FLYING_TOASTERS,
                                          ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
    CAPTURE(static_cast<int>(type));
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env;
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == kGlobalPeriodMs);

    mgr.start(type);
    REQUIRE(mgr.is_active());
    // A saver reads the configured period as it starts, as its level 0 frame period.
    CHECK(running_saver_timer_period(type) == kSaverPeriodMs);
    CHECK(default_refr_timer_period() == kSaverPeriodMs);
    CHECK(anim_timer_period() == kSaverPeriodMs);

    mgr.stop();
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == kGlobalPeriodMs);
    CHECK(anim_timer_period() == kGlobalPeriodMs);
}

TEST_CASE_METHOD(LVGLTestFixture, "switching screensaver type keeps the saver refresh period",
                 "[screensaver][refresh_period]") {
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env;
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());

    for (ScreensaverType type : {ScreensaverType::FLYING_TOASTERS, ScreensaverType::STARFIELD,
                                 ScreensaverType::PIPES_3D}) {
        CAPTURE(static_cast<int>(type));
        mgr.start(type);
        REQUIRE(mgr.is_active());
        CHECK(running_saver_timer_period(type) == kSaverPeriodMs);
        CHECK(default_refr_timer_period() == kSaverPeriodMs);
    }

    // One stop gives back everything the three starts took.
    mgr.stop();
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == kGlobalPeriodMs);
    CHECK(anim_timer_period() == kGlobalPeriodMs);
}

TEST_CASE_METHOD(LVGLTestFixture, "a screensaver that fails to start gives the refresh period back",
                 "[screensaver][refresh_period]") {
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env;
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());

    SECTION("no saver is registered for the requested type") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(mgr.is_active());
        REQUIRE(default_refr_timer_period() == kSaverPeriodMs);

        mgr.start(static_cast<ScreensaverType>(helix::ui::screensaver_last_type() + 1));

        CHECK_FALSE(mgr.is_active());
        CHECK_FALSE(helix::active_refresh_period_hold().is_held());
        CHECK(default_refr_timer_period() == kGlobalPeriodMs);
        CHECK(anim_timer_period() == kGlobalPeriodMs);
    }

    SECTION("the saver refuses to start") {
        const ScreensaverType type =
            GENERATE(ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
        CAPTURE(static_cast<int>(type));
        // Starfield and pipes refuse to start without a default display.
        lv_display_t* disp = lv_display_get_default();
        REQUIRE(disp != nullptr);
        lv_display_set_default(nullptr);
        mgr.start(type);
        lv_display_set_default(disp);

        CHECK_FALSE(mgr.is_active());
        CHECK_FALSE(helix::active_refresh_period_hold().is_held());
        CHECK(default_refr_timer_period() == kGlobalPeriodMs);
    }

    // Nothing was left behind: the next saver that does start still gets the fast period.
    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());
    CHECK(running_saver_timer_period(ScreensaverType::FLYING_TOASTERS) == kSaverPeriodMs);
    mgr.stop();
    CHECK(default_refr_timer_period() == kGlobalPeriodMs);
}

// ============================================================================
// BouncingPrinterScreensaver Tests
// ============================================================================

#include "../test_helpers/screensaver_bounce_test_access.h"
#include "screensaver_bounce.h"

TEST_CASE_METHOD(LVGLTestFixture, "BouncingPrinterScreensaver starts inactive", "[screensaver]") {
    BouncingPrinterScreensaver ss;
    REQUIRE(ss.is_active() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "BouncingPrinterScreensaver start/stop lifecycle",
                 "[screensaver]") {
    BouncingPrinterScreensaver ss;

    SECTION("start activates screensaver") {
        ss.start();
        REQUIRE(ss.is_active() == true);
        ss.stop();
    }

    SECTION("stop deactivates screensaver") {
        ss.start();
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }

    SECTION("double start is safe") {
        ss.start();
        ss.start();
        REQUIRE(ss.is_active() == true);
        ss.stop();
    }

    SECTION("double stop is safe") {
        ss.start();
        ss.stop();
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }

    SECTION("stop without start is safe") {
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "BouncingLogoScreensaver bounces the HelixScreen logo",
                 "[screensaver]") {
    lv_image_header_t logo{};
    REQUIRE(lv_image_decoder_get_info("A:assets/images/helixscreen-logo-bounce.png", &logo) ==
            LV_RESULT_OK);

    BouncingLogoScreensaver ss;
    CHECK(ss.type() == ScreensaverType::BOUNCING_LOGO);
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(BounceTestAccess::source_w(ss) == static_cast<int32_t>(logo.w));
    CHECK(BounceTestAccess::source_h(ss) == static_cast<int32_t>(logo.h));
    ss.stop();
    CHECK_FALSE(ss.is_active());
}

TEST_CASE_METHOD(LVGLTestFixture, "Bouncing logo keeps its own colors; the printer is tinted",
                 "[screensaver]") {
    BouncingPrinterScreensaver printer;
    printer.start();
    REQUIRE(printer.is_active());
    REQUIRE(BounceTestAccess::sprite(printer) != nullptr);
    CHECK(lv_obj_get_style_image_recolor_opa(BounceTestAccess::sprite(printer), LV_PART_MAIN) >
          LV_OPA_TRANSP);
    printer.stop();

    BouncingLogoScreensaver logo;
    logo.start();
    REQUIRE(logo.is_active());
    REQUIRE(BounceTestAccess::sprite(logo) != nullptr);
    CHECK(lv_obj_get_style_image_recolor_opa(BounceTestAccess::sprite(logo), LV_PART_MAIN) ==
          LV_OPA_TRANSP);
    logo.stop();
}

TEST_CASE_METHOD(LVGLTestFixture, "BouncingPrinterScreensaver creates overlay on lv_layer_top",
                 "[screensaver]") {
    BouncingPrinterScreensaver ss;

    int children_before = lv_obj_get_child_count(lv_layer_top());
    ss.start();
    int children_after = lv_obj_get_child_count(lv_layer_top());
    REQUIRE(children_after > children_before);

    ss.stop();
    REQUIRE_FALSE(ss.is_active());
    int children_final = lv_obj_get_child_count(lv_layer_top());
    REQUIRE(children_final <= children_after);
}

// The motion is a pure function of elapsed time, so the parts that decide where
// the sprite is and whether it just hit something are testable without a display.

TEST_CASE("Bouncing printer: fold keeps travel inside the wall", "[screensaver][bounce_math]") {
    using helix::ui::screensaver::fold;

    SECTION("every point of a long run stays in range") {
        for (float u = -5000.0f; u <= 5000.0f; u += 7.3f) {
            const float x = fold(u, 400.0f);
            REQUIRE(x >= 0.0f);
            REQUIRE(x <= 400.0f);
        }
    }

    SECTION("reflects at the wall and returns to the origin") {
        REQUIRE(fold(0.0f, 400.0f) == Catch::Approx(0.0f));
        REQUIRE(fold(400.0f, 400.0f) == Catch::Approx(400.0f));
        REQUIRE(fold(600.0f, 400.0f) == Catch::Approx(200.0f));
        REQUIRE(fold(800.0f, 400.0f) == Catch::Approx(0.0f).margin(0.001f));
    }

    SECTION("travelling backwards mirrors travelling forwards") {
        REQUIRE(fold(-100.0f, 400.0f) == Catch::Approx(100.0f));
        REQUIRE(fold(-500.0f, 400.0f) == Catch::Approx(300.0f));
    }

    SECTION("a zero range cannot divide by zero") {
        REQUIRE(fold(123.0f, 0.0f) == Catch::Approx(0.0f));
        REQUIRE(fold(123.0f, -5.0f) == Catch::Approx(0.0f));
    }
}

TEST_CASE("Bouncing printer: fold_index counts one per wall", "[screensaver][bounce_math]") {
    using helix::ui::screensaver::fold_index;

    REQUIRE(fold_index(0.0f, 400.0f) == 0);
    REQUIRE(fold_index(399.0f, 400.0f) == 0);
    REQUIRE(fold_index(401.0f, 400.0f) == 1);
    REQUIRE(fold_index(801.0f, 400.0f) == 2);
    REQUIRE(fold_index(-1.0f, 400.0f) == -1);
    REQUIRE(fold_index(123.0f, 0.0f) == 0);
}

TEST_CASE("Bouncing printer: near_rational rejects short repeating paths",
          "[screensaver][bounce_math]") {
    using helix::ui::screensaver::near_rational;

    // Simple ratios close a loop the sprite would retrace forever.
    REQUIRE(near_rational(1.0f, 6, 0.02f));
    REQUIRE(near_rational(0.5f, 6, 0.02f));
    REQUIRE(near_rational(2.0f / 3.0f, 6, 0.02f));
    REQUIRE(near_rational(1.25f, 6, 0.02f));

    // A ratio far from every p/q is what fills the screen instead of tracing a
    // handful of lines.
    REQUIRE_FALSE(near_rational(0.55f, 6, 0.02f));
    REQUIRE_FALSE(near_rational(1.1f, 6, 0.02f));

    // The band is wide enough to swallow irrationals that sit near a simple
    // fraction: the golden ratio is 0.018 from 3/5, so it is rejected too. The
    // seed loop redraws rather than insisting on any particular angle.
    REQUIRE(near_rational(0.61803f, 6, 0.02f));
}

TEST_CASE("Bouncing printer: a corner needs both walls and both edges",
          "[screensaver][bounce_math]") {
    using helix::ui::screensaver::is_corner_hit;

    constexpr float kRx = 400.0f;
    constexpr float kRy = 300.0f;
    constexpr float kTol = 6.0f;

    SECTION("both walls, sprite in the corner") {
        REQUIRE(is_corner_hit(true, true, 0.0f, 0.0f, kRx, kRy, kTol));
        REQUIRE(is_corner_hit(true, true, kRx, kRy, kRx, kRy, kTol));
        REQUIRE(is_corner_hit(true, true, kRx, 0.0f, kRx, kRy, kTol));
        REQUIRE(is_corner_hit(true, true, 3.0f, 2.0f, kRx, kRy, kTol));
    }

    SECTION("one wall is just a wall") {
        REQUIRE_FALSE(is_corner_hit(true, false, 0.0f, 0.0f, kRx, kRy, kTol));
        REQUIRE_FALSE(is_corner_hit(false, true, 0.0f, 0.0f, kRx, kRy, kTol));
        REQUIRE_FALSE(is_corner_hit(false, false, 0.0f, 0.0f, kRx, kRy, kTol));
    }

    SECTION("both axes turning far apart is not a corner") {
        // The near miss a coarse tick would otherwise report: both fold indices
        // moved, but the sprite is nowhere near a corner on one axis.
        REQUIRE_FALSE(is_corner_hit(true, true, 0.0f, kRy / 2.0f, kRx, kRy, kTol));
        REQUIRE_FALSE(is_corner_hit(true, true, kRx / 2.0f, 0.0f, kRx, kRy, kTol));
        REQUIRE_FALSE(is_corner_hit(true, true, kTol + 1.0f, kTol + 1.0f, kRx, kRy, kTol));
    }
}

TEST_CASE("Bouncing printer: sprite leaves room to bounce at every breakpoint",
          "[screensaver][bounce_math]") {
    using helix::screensaver_bounce::kMinRangePx;
    using helix::screensaver_bounce::sprite_size_for;

    struct Panel {
        int w;
        int h;
    };
    // The supported breakpoints, plus the micro layout.
    const Panel panels[] = {{1024, 600}, {800, 480}, {480, 320}, {480, 272}, {600, 1024}};

    for (const auto& panel : panels) {
        const int sprite = sprite_size_for(panel.w, panel.h);
        INFO("panel " << panel.w << "x" << panel.h << " sprite " << sprite);
        REQUIRE(sprite > 0);
        REQUIRE(panel.w - sprite >= kMinRangePx);
        REQUIRE(panel.h - sprite >= kMinRangePx);
    }

    // A panel with no room to bounce declines rather than pinning the sprite.
    REQUIRE(sprite_size_for(64, 64) == 0);
}

TEST_CASE("Bouncing printer: a resize rescales the speed to the screen's narrow axis",
          "[screensaver][bounce_math]") {
    BouncingPrinterScreensaver ss;

    BounceTestAccess::rebase(ss, 480, 320);
    const float small = BounceTestAccess::speed(ss);
    REQUIRE(small > 0.0f);

    // The narrow axis sets the speed whichever side of the screen it is on.
    BounceTestAccess::rebase(ss, 320, 480);
    CHECK(BounceTestAccess::speed(ss) == Catch::Approx(small).epsilon(0.0001f));

    // A taller narrow axis speeds the path proportionally: 480 over 320.
    BounceTestAccess::rebase(ss, 800, 480);
    const float wide = BounceTestAccess::speed(ss);
    CHECK(wide == Catch::Approx(small * (480.0f / 320.0f)).epsilon(0.0001f));

    // Growing the long axis alone leaves the speed where it was.
    BounceTestAccess::rebase(ss, 1024, 480);
    CHECK(BounceTestAccess::speed(ss) == Catch::Approx(wide).epsilon(0.0001f));
}

TEST_CASE("Bouncing printer: the sprite box is the shape of the artwork",
          "[screensaver][bounce_math]") {
    using helix::screensaver_bounce::fit_sprite;

    int w = 0;
    int h = 0;

    SECTION("a tall render keeps its proportions inside the box") {
        // voron-trident.png is 750x930.
        fit_sprite(168, 750, 930, w, h);
        REQUIRE(h == 168);
        REQUIRE(w == 135);
    }

    SECTION("a wide render is bounded by its width") {
        fit_sprite(100, 400, 200, w, h);
        REQUIRE(w == 100);
        REQUIRE(h == 50);
    }

    SECTION("a square render fills the box") {
        fit_sprite(90, 1080, 1080, w, h);
        REQUIRE(w == 90);
        REQUIRE(h == 90);
    }

    SECTION("neither edge ever exceeds the box") {
        const int box = 120;
        const int dims[][2] = {{750, 930}, {1080, 1080}, {1920, 200}, {7, 4000}};
        for (const auto& d : dims) {
            fit_sprite(box, d[0], d[1], w, h);
            INFO("src " << d[0] << "x" << d[1] << " -> " << w << "x" << h);
            REQUIRE(w <= box);
            REQUIRE(h <= box);
            REQUIRE(w >= 1);
            REQUIRE(h >= 1);
        }
    }

    SECTION("an undecodable size falls back to the square box") {
        fit_sprite(80, 0, 0, w, h);
        REQUIRE(w == 80);
        REQUIRE(h == 80);
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "without HELIX_SCREENSAVER_REFR_PERIOD_MS the display refreshes at the saver's level period",
    "[screensaver][refresh_period]") {
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env(std::nullopt);
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());

    mgr.start(ScreensaverType::PIPES_3D);
    REQUIRE(mgr.is_active());
    CHECK(default_refr_timer_period() == helix::ui::kSaverFastPeriod);
    CHECK(anim_timer_period() == helix::ui::kSaverFastPeriod);

    auto* pipes =
        static_cast<helix::ui::SaverBase*>(helix::ScreensaverManagerTestAccess::active(mgr));
    pipes->request_level(1);
    CHECK(running_saver_timer_period(ScreensaverType::PIPES_3D) == 33);
    CHECK(default_refr_timer_period() == 33);
    CHECK(anim_timer_period() == 33);

    mgr.stop();
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == kGlobalPeriodMs);
    CHECK(anim_timer_period() == kGlobalPeriodMs);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "each existing saver runs 16 ms at level 0, then 33 ms, without a motion jump",
                 "[screensaver][screensaver_motion]") {
    // Matches neither LVGL's default refresh period nor the level 0 period.
    ScopedRefreshPeriod refresh(20);
    ScopedGlobalRefreshHold clean_hold; // no configured period, so level 0 is 16 ms

    SECTION("flying toasters") {
        FlyingToasterScreensaver ss;
        ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        REQUIRE(ToasterAccess::frames_decoded(ss));
        const auto initial = shown_frames(ss);
        const auto all_sprites = ToasterAccess::sprites(ss);
        REQUIRE(all_sprites.size() > 10);
        CHECK(ss.level_count() == 4);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::kSaverFastPeriod);
        for (int i = 0; i < 10; i++) {
            lv_tick_inc(400);
            run_toaster_tick(ss);
        }

        ss.request_level(1);
        CHECK(ss.level() == 1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
        CHECK(ToasterAccess::sprites(ss).size() == all_sprites.size());

        lv_tick_inc(100);
        run_toaster_tick(ss);
        // Sprites sit exactly where 4.1 s of flight puts them, so the switch cost no time.
        CHECK(check_sprites_at(ss, 4100, initial).visible_toasters > 0);

        // Rungs past half rate thin the flock instead of slowing it again, and each one keeps
        // strictly fewer sprites than the rung above it.
        size_t previous = ToasterAccess::sprites(ss).size();
        for (size_t level = 2; level < ss.level_count(); level++) {
            ss.request_level(level);
            CAPTURE(level);
            CHECK(ss.level() == level);
            CHECK(SaverTestAccess::timer(ss)->period == 33);
            const size_t kept = ToasterAccess::sprites(ss).size();
            CHECK(kept < previous);
            CHECK(lv_obj_has_flag(all_sprites[kept].img, LV_OBJ_FLAG_HIDDEN));
            previous = kept;
        }
        // The bottom rung is the sparsest thing the gate can fall back to.
        CHECK(previous == 10);
    }

    SECTION("flying toasters started at the lowest rung") {
        FlyingToasterScreensaver ss;
        ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
        FlyingToasterScreensaver probe;
        ss.set_start_level(probe.level_count() - 1);
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ToasterAccess::sprites(ss).size() == 10);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
    }

    SECTION("starfield") {
        StarfieldScreensaver ss;
        ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ss.level_count() == 4);
        CHECK(StarAccess::active_star_count(ss) == 150);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::kSaverFastPeriod);
        ss.request_level(1);
        CHECK(ss.level() == 1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
        CHECK(StarAccess::active_star_count(ss) == 96);
        // Every rung past half rate holds 33 ms and thins the population strictly.
        int previous_stars = 96;
        for (size_t level = 2; level < ss.level_count(); level++) {
            ss.request_level(level);
            CAPTURE(level);
            CHECK(SaverTestAccess::timer(ss)->period == 33);
            const int stars = StarAccess::active_star_count(ss);
            CHECK(stars < previous_stars);
            previous_stars = stars;
        }
        // The bottom rung is the sparsest sky the gate can fall back to.
        CHECK(previous_stars == 32);
    }

    SECTION("pipes") {
        PipesScreensaver ss;
        ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ss.level_count() == 3);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::kSaverFastPeriod);
        ss.request_level(1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
        ss.request_level(2);
        CHECK(SaverTestAccess::timer(ss)->period == 100);
    }

    SECTION("bouncing printer") {
        BouncingPrinterScreensaver ss;
        ScreensaverStopOnExit<BouncingPrinterScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        CHECK(ss.level_count() == 3);
        CHECK(SaverTestAccess::timer(ss)->period == helix::ui::kSaverFastPeriod);
        ss.request_level(1);
        CHECK(ss.level() == 1);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
        ss.request_level(2);
        CHECK(ss.level() == 2);
        CHECK(SaverTestAccess::timer(ss)->period == 33);
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
