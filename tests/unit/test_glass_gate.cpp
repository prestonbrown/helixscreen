// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_appearance.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "board_fingerprint.h"
#include "config.h"
#include "display_settings_manager.h"
#include "glass_gate.h"
#include "glass_painter.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_capabilities.h"
#include "theme_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::decide_glass_gate;
using helix::ui::GlassGateFacts;
using helix::ui::GlassGateVerdict;
using helix::ui::GlassProbeRecord;

namespace {

constexpr const char* kVersion = "1.1.0 (abc)";
constexpr const char* kBoard = "egl/4c/100bm/800x480/32bpp";

/// A board that clears every hard fail and has nothing stored.
GlassGateFacts capable() {
    GlassGateFacts f;
    f.color_depth = 32;
    f.style_effects_allowed = true;
    f.software_rotated = false;
    f.version = kVersion;
    f.board = kBoard;
    return f;
}

GlassProbeRecord record(bool passed, const char* version = kVersion, const char* board = kBoard) {
    return GlassProbeRecord{passed, 3.5, version, board};
}

} // namespace

TEST_CASE("glass gate: a capable board with nothing stored needs a probe", "[glass]") {
    CHECK(decide_glass_gate(capable()) == GlassGateVerdict::NeedsProbe);
}

TEST_CASE("glass gate: the env override beats every other fact", "[glass]") {
    GlassGateFacts f = capable();
    f.color_depth = 16;
    f.env_force = true;
    CHECK(decide_glass_gate(f) == GlassGateVerdict::Pass);

    GlassGateFacts g = capable();
    g.stored = record(true);
    g.env_force = false;
    CHECK(decide_glass_gate(g) == GlassGateVerdict::Fail);
}

TEST_CASE("glass gate: hard fails need no probe", "[glass]") {
    SECTION("16 bpp") {
        GlassGateFacts f = capable();
        f.color_depth = 16;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
    SECTION("24 bpp is enough") {
        GlassGateFacts f = capable();
        f.color_depth = 24;
        CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);
    }
    SECTION("style effects not allowed") {
        GlassGateFacts f = capable();
        f.style_effects_allowed = false;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
    SECTION("a draw backend that renders two gradient stops") {
        GlassGateFacts f = capable();
        f.multi_stop_gradients = false;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
    SECTION("software-rotated display") {
        GlassGateFacts f = capable();
        f.software_rotated = true;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
}

TEST_CASE("glass gate: a stored record counts only for the same version and board", "[glass]") {
    GlassGateFacts f = capable();

    f.stored = record(true);
    CHECK(decide_glass_gate(f) == GlassGateVerdict::Pass);

    f.stored = record(false);
    CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);

    f.stored = record(true, "1.0.9 (old)");
    CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);

    f.stored = record(true, kVersion, "fbdev/4c/100bm/800x480/32bpp");
    CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);

    f.stored = record(false, "1.0.9 (old)");
    CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);
}

TEST_CASE("glass gate: HELIX_GLASS parses 1 and 0 only", "[glass]") {
    helix::ScopedEnv env{"HELIX_GLASS"};
    unsetenv("HELIX_GLASS");
    CHECK_FALSE(helix::ui::glass_env_force().has_value());
    setenv("HELIX_GLASS", "1", 1);
    CHECK(helix::ui::glass_env_force() == true);
    setenv("HELIX_GLASS", "0", 1);
    CHECK(helix::ui::glass_env_force() == false);
    for (const char* bad : {"", "yes", "true", "2", " 1", "01"}) {
        CAPTURE(bad);
        setenv("HELIX_GLASS", bad, 1);
        CHECK_FALSE(helix::ui::glass_env_force().has_value());
    }
}

TEST_CASE("glass probe record: malformed nodes are ignored and a written one parses back",
          "[glass]") {
    using json = nlohmann::json;
    CHECK_FALSE(helix::ui::parse_glass_probe(nullptr).has_value());
    for (const char* bad : {
             R"(null)",
             R"([])",
             R"({"passed": true, "frame_ms": 1.0, "version": "v"})",
             R"({"passed": 1, "frame_ms": 1.0, "version": "v", "board": "b"})",
             R"({"passed": true, "frame_ms": "1", "version": "v", "board": "b"})",
             R"({"passed": true, "frame_ms": 1.0, "version": 3, "board": "b"})",
             R"({"passed": true, "frame_ms": 1.0, "version": "v", "board": false})",
         }) {
        CAPTURE(bad);
        const json node = json::parse(bad);
        CHECK_FALSE(helix::ui::parse_glass_probe(&node).has_value());
    }
    const GlassProbeRecord r = record(true);
    const json node = helix::ui::glass_probe_json(r);
    CHECK(helix::ui::parse_glass_probe(&node) == r);
}

TEST_CASE("glass probe record: saved to config loads back from its path", "[glass]") {
    helix::Config* config = helix::Config::get_instance();
    CHECK(std::string(helix::ui::kGlassProbeStorePath) == "/display/glass_probe");

    const GlassProbeRecord r{false, 9.25, "9.9.9 (probe)", "fbdev/2c/1000bm/480x272/32bpp"};
    helix::ui::save_glass_probe(*config, r);
    CHECK(helix::ui::load_glass_probe(*config) == r);

    config->get_json("/display").erase("glass_probe");
    CHECK_FALSE(helix::ui::load_glass_probe(*config).has_value());
}

TEST_CASE("board facts come from the capabilities and the build", "[glass]") {
    helix::PlatformCapabilities caps;
    caps.cpu_cores = 4;
    caps.bogomips = 1234.0f;
    const helix::ui::BoardFacts facts = helix::ui::current_board_facts(caps, "");
    CHECK(facts.display_backend == "unknown");
    CHECK(facts.cores == 4);
    CHECK(facts.bogomips == 1234.0f);
    CHECK(facts.color_depth == LV_COLOR_DEPTH);
    CHECK(helix::ui::current_board_facts(caps, "egl").display_backend == "egl");
}

TEST_CASE_METHOD(LVGLTestFixture, "glass probe draws offscreen and reports a time", "[glass]") {
    const uint32_t children_before = lv_obj_get_child_count(lv_layer_sys());
    const double ms = helix::ui::measure_glass_frame_ms(320, 240);
    CHECK(ms > 0.0);
    CHECK(lv_obj_get_child_count(lv_layer_sys()) == children_before);
    CHECK(helix::ui::measure_glass_frame_ms(0, 240) == 0.0);
}

// ============================================================================
// The running gate
// ============================================================================

namespace {

class GlassGateFixture : public LVGLTestFixture {
  public:
    GlassGateFixture() {
        helix::DisplaySettingsManager::instance().init_subjects();
        helix::DisplaySettingsManager::instance().set_glass_effects(false);
    }
    ~GlassGateFixture() override {
        helix::ui::glass_gate_stop();
        helix::DisplaySettingsManager::instance().set_glass_effects(false);
        helix::DisplaySettingsManager::instance().set_glass_available(false);
        helix::Config::get_instance()->get_json("/display").erase("glass_probe");
    }

    static helix::ui::GlassMeasureFn measure_returning(double ms, int* calls) {
        return [ms, calls](int32_t, int32_t) {
            ++*calls;
            return ms;
        };
    }
};

int glass_available() {
    return lv_subject_get_int(helix::DisplaySettingsManager::instance().subject_glass_available());
}

} // namespace

TEST_CASE_METHOD(GlassGateFixture, "glass: a saved choice on a board that fails stays flat",
                 "[glass]") {
    helix::DisplaySettingsManager::instance().set_glass_effects(true);
    GlassGateFacts f = capable();
    f.color_depth = 16;
    int calls = 0;
    helix::ui::glass_gate_start(f, measure_returning(1.0, &calls));
    CHECK(glass_available() == 0);
    CHECK_FALSE(helix::ui::glass_effective());
    CHECK_FALSE(ThemeManager::instance().is_glass());
    CHECK(helix::DisplaySettingsManager::instance().get_glass_effects());
    process_lvgl(static_cast<int>(helix::ui::kGlassProbeDelayMs) + 500);
    CHECK(calls == 0);
}

TEST_CASE_METHOD(GlassGateFixture,
                 "glass: available and on attaches every layer, off detaches and goes flat",
                 "[glass]") {
    lv_obj_t* top_card = lv_obj_create(lv_layer_top());
    lv_obj_add_style(top_card, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);
    lv_obj_t* screen_child = lv_obj_create(test_screen());

    GlassGateFacts f = capable();
    f.stored = record(true);
    int calls = 0;
    helix::ui::glass_gate_start(f, measure_returning(1.0, &calls));
    REQUIRE(glass_available() == 1);
    CHECK_FALSE(helix::ui::glass_effective());

    helix::DisplaySettingsManager::instance().set_glass_effects(true);
    CHECK(helix::ui::glass_effective());
    CHECK(ThemeManager::instance().is_glass());
    CHECK(lv_obj_is_send_draw_task_events(top_card));
    CHECK(helix::ui::glass_attached(top_card));
    CHECK(helix::ui::glass_attached(screen_child));

    lv_obj_t* created_later = lv_obj_create(test_screen());
    helix::ui::glass_on_object_created(created_later);
    CHECK(helix::ui::glass_attached(created_later));

    helix::DisplaySettingsManager::instance().set_glass_effects(false);
    CHECK_FALSE(helix::ui::glass_effective());
    CHECK_FALSE(ThemeManager::instance().is_glass());
    CHECK_FALSE(lv_obj_is_send_draw_task_events(top_card));
    CHECK_FALSE(helix::ui::glass_attached(screen_child));

    lv_obj_t* created_flat = lv_obj_create(test_screen());
    helix::ui::glass_on_object_created(created_flat);
    CHECK_FALSE(helix::ui::glass_attached(created_flat));
    CHECK(calls == 0);
    lv_obj_delete(top_card);
}

TEST_CASE_METHOD(GlassGateFixture, "glass: HELIX_GLASS=0 hides it over a stored pass", "[glass]") {
    helix::DisplaySettingsManager::instance().set_glass_effects(true);
    GlassGateFacts f = capable();
    f.stored = record(true);
    f.env_force = false;
    int calls = 0;
    helix::ui::glass_gate_start(f, measure_returning(1.0, &calls));
    CHECK(glass_available() == 0);
    CHECK_FALSE(helix::ui::glass_effective());
}

TEST_CASE_METHOD(GlassGateFixture, "glass: an unmeasured board is probed later and the result kept",
                 "[glass]") {
    const double ms = GENERATE(2.0, 40.0);
    CAPTURE(ms);
    const bool pass = ms <= helix::ui::kGlassFrameBudgetMs;
    int calls = 0;
    helix::ui::glass_gate_start(capable(), measure_returning(ms, &calls));
    CHECK(glass_available() == 0);
    CHECK(calls == 0);

    process_lvgl(static_cast<int>(helix::ui::kGlassProbeDelayMs) + 500);
    CHECK(calls == 1);
    CHECK(glass_available() == (pass ? 1 : 0));
    const auto stored = helix::ui::load_glass_probe(*helix::Config::get_instance());
    REQUIRE(stored.has_value());
    CHECK(stored->passed == pass);
    CHECK(stored->frame_ms == ms);
    CHECK(stored->version == kVersion);
    CHECK(stored->board == kBoard);

    process_lvgl(static_cast<int>(helix::ui::kGlassProbeDelayMs) * 2);
    CHECK(calls == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "glass gate: the depth is what the display renders at",
                 "[glass]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    const lv_color_format_t before = lv_display_get_color_format(disp);
    // A 32 bpp build on a 16 bpp framebuffer: the fbdev driver sets the display to RGB565.
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    const int depth_565 = helix::ui::glass_gate_facts_now().color_depth;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    const int depth_8888 = helix::ui::glass_gate_facts_now().color_depth;
    lv_display_set_color_format(disp, before);
    CHECK(depth_565 == 16);
    CHECK(depth_8888 == 32);
    CHECK(helix::ui::glass_gate_facts_now().multi_stop_gradients == !LV_USE_DRAW_NANOVG);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "glass: a widget made under the app theme while glass is on is attached",
                 "[glass][theme]") {
    lv_obj_t* before = lv_obj_create(test_screen());
    REQUIRE_FALSE(helix::ui::glass_attached(before));

    helix::ui::glass_set_effective(true);
    lv_obj_t* made_on = lv_obj_create(test_screen());
    lv_obj_t* button_on = lv_button_create(test_screen());
    const bool obj_attached = helix::ui::glass_attached(made_on);
    const bool button_attached = helix::ui::glass_attached(button_on);
    helix::ui::glass_set_effective(false);

    CHECK(obj_attached);
    CHECK(button_attached);
    lv_obj_t* made_off = lv_obj_create(test_screen());
    CHECK_FALSE(helix::ui::glass_attached(made_off));
}

TEST_CASE_METHOD(LVGLUITestFixture, "glass: the Glass Effects row's switch saves the setting",
                 "[glass][display_settings]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    dsm.set_glass_effects(false);
    helix::settings::get_appearance_settings_overlay().register_callbacks();
    lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, "on_glass_effects_changed");
    REQUIRE(cb != nullptr);

    lv_obj_t* toggle = lv_switch_create(test_screen());
    lv_obj_add_event_cb(toggle, cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(dsm.get_glass_effects());
    CHECK(lv_subject_get_int(dsm.subject_glass_effects()) == 1);
    CHECK(helix::Config::get_instance()->get<bool>("/display/glass_effects", false));

    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK_FALSE(dsm.get_glass_effects());
    CHECK_FALSE(helix::Config::get_instance()->get<bool>("/display/glass_effects", true));
}
