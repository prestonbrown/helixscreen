// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_rotation_probe.cpp
 * @brief RotationProbe: the rotation sequence, tap confirmation, saved result and screen.
 *
 * The probe reads the pointer's read callback directly. A scripted callback answers by call
 * index, counted from the moment the target rotation becomes current:
 *   1: leftover-contact check on the scan screen (released)
 *   2: first poll of the scan screen (pressed, latches the tap)
 *   3: draining that tap's release
 *   4: leftover-contact check on the confirm screen (released)
 *   5: first poll of the confirm screen (pressed, confirms)
 * Every other call reads released.
 */

#include "application_test_fixture.h"
#include "config.h"
#include "rotation_probe.h"
#include "test_helpers/config_test_access.h"

#include <string>
#include <vector>

#include "../../catch_amalgamated.hpp"

namespace {

struct ProbeScript {
    int tap_at_degrees = -1; // -1: never tap
    bool confirm = true;     // false: tap the scan prompt but let the confirm prompt time out
    int current_degrees = 0;
    int calls_since_rotation = 0;
    std::vector<int> settled; // degrees, in call order
    std::vector<bool> fanout;
    std::string scan_main_text;
    std::string scan_subtitle_text;
    /// Children on the active screen at each keep_backlight_on() call.
    std::vector<uint32_t> children_at_backlight;
};

ProbeScript* g_script = nullptr;

void scripted_read_cb(lv_indev_t*, lv_indev_data_t* data) {
    ProbeScript& s = *g_script;
    ++s.calls_since_rotation;
    if (s.calls_since_rotation == 2 && s.current_degrees == s.tap_at_degrees) {
        if (lv_obj_t* main = lv_obj_find_by_name(lv_screen_active(), "probe_main")) {
            s.scan_main_text = lv_label_get_text(main);
        }
        if (lv_obj_t* sub = lv_obj_find_by_name(lv_screen_active(), "probe_subtitle")) {
            s.scan_subtitle_text = lv_label_get_text(sub);
        }
    }
    const bool tap_rotation = s.current_degrees == s.tap_at_degrees;
    data->state = (tap_rotation &&
                   (s.calls_since_rotation == 2 || (s.confirm && s.calls_since_rotation == 5)))
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

int degrees_of(lv_display_rotation_t rot) {
    return static_cast<int>(rot) * 90;
}

class ProbeFixture : public ApplicationTestFixture {
  public:
    ProbeFixture() {
        g_script = &script;
        pointer = lv_indev_create();
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(pointer, scripted_read_cb);
        // Config::save() on the result must not write wherever another test left the
        // process-wide singleton pointing.
        helix::Config* cfg = helix::Config::get_instance();
        std::string& path = helix::ConfigTestAccess::path(*cfg);
        saved_path = path;
        path.clear();
        // Every case starts from a first boot nobody has answered.
        saved_data = helix::ConfigTestAccess::data(*cfg);
        json& display = cfg->get_json("/display");
        if (!display.is_object()) {
            display = json::object();
        }
        display.erase("rotate");
        display.erase("rotation_probed");
    }

    ~ProbeFixture() override {
        helix::ConfigTestAccess::data(*helix::Config::get_instance()) = saved_data;
        helix::ConfigTestAccess::path(*helix::Config::get_instance()) = saved_path;
        lv_indev_delete(pointer);
        g_script = nullptr;
    }

    int run(bool is_sdl, int tap_at_degrees, uint32_t backlight_reassert_ms = 0) {
        script.tap_at_degrees = tap_at_degrees;
        helix::RotationProbeHost host{
            pointer,
            is_sdl,
            [this](lv_display_rotation_t rot) {
                script.current_degrees = degrees_of(rot);
                script.calls_since_rotation = 0;
                script.settled.push_back(script.current_degrees);
            },
            [this](bool suspended) { script.fanout.push_back(suspended); },
            [this] {
                script.children_at_backlight.push_back(lv_obj_get_child_count(lv_screen_active()));
            },
            /*scan_timeout_ms=*/40,
            /*confirm_timeout_ms=*/400,
            backlight_reassert_ms};
        return helix::RotationProbe(std::move(host)).run();
    }

    ProbeScript script;
    lv_indev_t* pointer = nullptr;
    std::string saved_path;
    json saved_data;
};

} // namespace

TEST_CASE_METHOD(ProbeFixture, "rotation probe confirms the rotation that was tapped twice",
                 "[application][display][rotation][probe]") {
    CHECK(run(/*is_sdl=*/false, /*tap_at_degrees=*/180) == 180);

    // 0 and 90 time out untapped; 180 is tapped and confirmed; the result is then applied.
    CHECK(script.settled == std::vector<int>{0, 90, 180, 180});
    CHECK(script.fanout == std::vector<bool>{true, false});

    helix::Config* cfg = helix::Config::get_instance();
    CHECK(cfg->get<int>("/display/rotate", -1) == 180);
    CHECK(cfg->get<bool>("/display/rotation_probed", false));
    CHECK_FALSE(helix::rotation_probe_unanswered(*cfg));

    // The prompt text came through the XML screen's subject bindings.
    CHECK(script.scan_main_text == "Tap anywhere if this text is right-side up");
    CHECK(script.scan_subtitle_text.find("180") != std::string::npos);

    // The probe leaves the screen empty for the normal UI.
    CHECK(lv_obj_get_child_count(lv_screen_active()) == 0);
    CHECK(lv_indev_get_read_cb(pointer) == scripted_read_cb);
}

TEST_CASE_METHOD(ProbeFixture,
                 "an unanswered rotation probe uses 0 for the session and asks again next boot",
                 "[application][display][rotation][probe]") {
    helix::Config* cfg = helix::Config::get_instance();
    REQUIRE(helix::rotation_probe_unanswered(*cfg));

    CHECK(run(/*is_sdl=*/false, /*tap_at_degrees=*/-1) == 0);

    // Three full sweeps of four rotations, then 0 is applied.
    REQUIRE(script.settled.size() == 13);
    for (size_t i = 0; i < 12; ++i) {
        CHECK(script.settled[i] == static_cast<int>(i % 4) * 90);
    }
    CHECK(script.settled.back() == 0);

    // Nobody answered, so nothing is recorded and the next start probes again.
    CHECK_FALSE(cfg->exists("/display/rotate"));
    CHECK_FALSE(cfg->exists("/display/rotation_probed"));
    CHECK(helix::rotation_probe_unanswered(*cfg));
}

TEST_CASE_METHOD(ProbeFixture,
                 "every rotation probe prompt lights the backlight before it is built",
                 "[application][display][rotation][probe]") {
    SECTION("unanswered: twelve scan prompts") {
        run(/*is_sdl=*/false, /*tap_at_degrees=*/-1);
        CHECK(script.children_at_backlight.size() == 12);
    }
    SECTION("answered at 90: two scan prompts and one confirm prompt") {
        run(/*is_sdl=*/false, /*tap_at_degrees=*/90);
        CHECK(script.children_at_backlight.size() == 3);
    }
    // The first prompt is lit before the probe has put anything on the screen.
    REQUIRE_FALSE(script.children_at_backlight.empty());
    CHECK(script.children_at_backlight.front() == 0);
}

TEST_CASE_METHOD(ProbeFixture, "a prompt left up relights the backlight while it waits",
                 "[application][display][rotation][probe]") {
    // 0 is tapped every sweep and never confirmed: three sweeps of four scan prompts plus a
    // 400 ms confirm prompt each, so fifteen prompts in all.
    script.confirm = false;
    CHECK(run(/*is_sdl=*/false, /*tap_at_degrees=*/0, /*backlight_reassert_ms=*/100) == 0);

    // A relight lands inside each confirm wait on top of the one per prompt.
    CHECK(script.children_at_backlight.size() >= 18);
    CHECK(script.children_at_backlight.front() == 0);
}

TEST_CASE("a configured rotation answers the probe even without the probed flag",
          "[application][display][rotation][probe]") {
    helix::Config* cfg = helix::Config::get_instance();
    const json saved = helix::ConfigTestAccess::data(*cfg);
    json& display = cfg->get_json("/display");
    display = json::object();

    display["rotate"] = 0;
    CHECK_FALSE(helix::rotation_probe_unanswered(*cfg));

    display.erase("rotate");
    display["rotation_probed"] = true;
    CHECK_FALSE(helix::rotation_probe_unanswered(*cfg));

    display.erase("rotation_probed");
    CHECK(helix::rotation_probe_unanswered(*cfg));

    helix::ConfigTestAccess::data(*cfg) = saved;
}

TEST_CASE_METHOD(ProbeFixture, "rotation probe on SDL never rotates the display",
                 "[application][display][rotation][probe]") {
    CHECK(run(/*is_sdl=*/true, /*tap_at_degrees=*/0) == 0);
    CHECK(script.settled.empty());
}
