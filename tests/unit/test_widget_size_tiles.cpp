// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_tiles.cpp
 * @brief Centred-icon tiles resolve their icon face, value font, label
 *        visibility and direction from the per-instance subjects their sizing
 *        helper publishes.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][tile]"
 */

#include "ui_humidity_drop.h"
#include "ui_tile_rung.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/bypass_engaged_seed.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_widget_labels.h"
#include "../test_helpers/update_queue_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix/ui/shared_font_style.h"
#include "lvgl.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/led_widget.h"
#include "src/ui/panel_widgets/tile_layout.h"
#include "src/ui/panel_widgets/tile_sizing.h"
#include "system_settings_manager.h"
#include "theme_manager.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

namespace {

/// A widget that overrides nothing, to pin the base-class default. Widgets that
/// have not opted in must keep exactly their registry limits.
class PlainWidget : public helix::PanelWidget {
  public:
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return "plain";
    }
};

/// Every widget whose home tile is a centred icon sized by a TileSizing.
const std::vector<std::string> kCentredIconTiles = {"shutdown",
                                                    "lock",
                                                    "firmware_restart",
                                                    "led_controls",
                                                    "macros",
                                                    "motion",
                                                    "gcode_console",
                                                    "led",
                                                    "network",
                                                    "bypass",
                                                    "notifications",
                                                    "power_device",
                                                    "favorite_macro",
                                                    "humidity",
                                                    "width_sensor",
                                                    "fan",
                                                    "thermistor",
                                                    "filament",
                                                    "temperature",
                                                    "bed_temperature",
                                                    "chamber_temperature"};

} // namespace

TEST_CASE("PanelWidget::fits_at defaults to true", "[widget_size][tile][1559]") {
    PlainWidget plain;
    CHECK(plain.fits_at(1, 1));
    CHECK(plain.fits_at(0, 0));
    CHECK(plain.fits_at(4000, 4000));
}

TEST_CASE("every widget can draw in a generous box", "[widget_size][tile][1559]") {
    // A fits_at that refused a whole-screen box would make its widget
    // unplaceable, and the clamp would walk to the registry maximum and stop.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();

    int constructed = 0;
    for (const auto& def : get_all_widget_defs()) {
        if (!def.factory) {
            continue;
        }
        INFO("widget " << def.id);
        auto instance = def.factory(def.id);
        REQUIRE(instance != nullptr);
        CHECK(instance->fits_at(480, 480));
        ++constructed;
    }
    CHECK(constructed > 10);
}

namespace {

/// A tile component whose icon face is bound to whichever per-instance subject
/// it is handed. Two instances get different subject names, so a verdict
/// published to one must not move the other.
constexpr const char* kTileFixtureXml = R"(<component>
  <api>
    <prop name="tile_icon_subject" type="string" default=""/>
  </api>
  <styles>
    <style name="rung_xs" text_font="#icon_font_xs"/>
    <style name="rung_xl" text_font="#icon_font_xl"/>
  </styles>
  <view name="root" extends="lv_obj">
    <icon name="tile_glyph" src="power" size="md">
      <bind_style_if_eq name="rung_xs" subject="$tile_icon_subject" ref_value="0"/>
      <bind_style_if_eq name="rung_xl" subject="$tile_icon_subject" ref_value="4"/>
    </icon>
  </view>
</component>)";

lv_obj_t* create_tile(lv_obj_t* parent, const helix::TileSizing& sizing) {
    static bool registered = false;
    if (!registered) {
        REQUIRE(lv_xml_register_component_from_data("tile_size_fixture", kTileFixtureXml) ==
                LV_RESULT_OK);
        registered = true;
    }
    return static_cast<lv_obj_t*>(
        lv_xml_create(parent, "tile_size_fixture", sizing.subject_attrs()));
}

} // namespace

TEST_CASE("two tile instances resolve their rung independently", "[widget_size][tile][1559]") {
    // Per-instance subjects are the whole point: one shared name would make the
    // second instance overwrite the first's verdict and both would agree.
    LVGLUITestFixture fixture;

    helix::TileSizing wide("fan:0");
    helix::TileSizing narrow("fan:1");
    wide.set_content({"888 / 888", "888", "Fan", true});
    narrow.set_content({"888 / 888", "888", "Fan", true});

    lv_obj_t* wide_obj = create_tile(fixture.test_screen(), wide);
    lv_obj_t* narrow_obj = create_tile(fixture.test_screen(), narrow);
    REQUIRE(wide_obj != nullptr);
    REQUIRE(narrow_obj != nullptr);

    lv_obj_t* wide_glyph = lv_obj_find_by_name(wide_obj, "tile_glyph");
    lv_obj_t* narrow_glyph = lv_obj_find_by_name(narrow_obj, "tile_glyph");
    REQUIRE(wide_glyph != nullptr);
    REQUIRE(narrow_glyph != nullptr);

    const lv_font_t* xs = theme_manager_get_font("icon_font_xs");
    const lv_font_t* xl = theme_manager_get_font("icon_font_xl");
    REQUIRE(xs != nullptr);
    REQUIRE(xl != nullptr);
    REQUIRE(xs != xl);

    lv_subject_set_int(lv_xml_get_subject(nullptr, "fan:0_tile_icon"), 4);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "fan:1_tile_icon"), 0);

    // If the subjects had been registered too late to bind, both glyphs would
    // still read the size attribute's face and these two would be equal.
    CHECK(lv_obj_get_style_text_font(wide_glyph, LV_PART_MAIN) == xl);
    CHECK(lv_obj_get_style_text_font(narrow_glyph, LV_PART_MAIN) == xs);
}

TEST_CASE("a tile registers its subjects before any XML is parsed", "[widget_size][tile][1559]") {
    // Ruling 2: construction, not attach(). A subject that does not exist when
    // the component is parsed is dropped permanently, with nothing in the log.
    LVGLUITestFixture fixture;
    helix::TileSizing sizing("thermistor:2");
    for (const char* suffix : {"_tile_icon", "_tile_label", "_tile_dir", "_tile_target"}) {
        const std::string name = std::string("thermistor:2") + suffix;
        INFO("subject " << name);
        CHECK(lv_xml_get_subject(nullptr, name.c_str()) != nullptr);
    }
}

TEST_CASE("a tile's verdict follows the box it is given", "[widget_size][tile][1559]") {
    LVGLUITestFixture fixture;
    helix::TileSizing sizing("fan:9");
    sizing.set_content({"888 / 888", "888", "Fan", true});

    sizing.measure_and_publish(240, 240);
    const int big = lv_subject_get_int(lv_xml_get_subject(nullptr, "fan:9_tile_icon"));
    sizing.measure_and_publish(60, 60);
    const int small = lv_subject_get_int(lv_xml_get_subject(nullptr, "fan:9_tile_icon"));

    INFO("rung at 240x240 = " << big << ", at 60x60 = " << small);
    CHECK(big > small);
    CHECK(sizing.fits(240, 240));
}

TEST_CASE("two real fan tiles at different sizes draw different glyphs",
          "[widget_size][tile][1559]") {
    // The end-to-end shape: a widget owns a TileSizing, the manager hands its
    // subject names to lv_xml_create, and the component's bind_style_if_eq
    // rungs resolve them. If any link in that chain is broken the two icons
    // come back identical, which is also what a subject registered too late
    // would produce.
    LVGLUITestFixture fixture;
    init_widget_registrations();

    const auto* def = find_widget_def("fan");
    REQUIRE(def != nullptr);
    REQUIRE(def->factory != nullptr);

    auto wide = def->factory("fan:0");
    auto narrow = def->factory("fan:1");
    REQUIRE(wide != nullptr);
    REQUIRE(narrow != nullptr);

    // The names must differ, or both tiles bind one subject and agree by
    // accident rather than by construction.
    REQUIRE(wide->xml_attrs() != nullptr);
    REQUIRE(narrow->xml_attrs() != nullptr);
    REQUIRE(std::string(wide->xml_attrs()[1]) != std::string(narrow->xml_attrs()[1]));

    lv_obj_t* wide_obj = static_cast<lv_obj_t*>(
        lv_xml_create(fixture.test_screen(), "panel_widget_fan", wide->xml_attrs()));
    lv_obj_t* narrow_obj = static_cast<lv_obj_t*>(
        lv_xml_create(fixture.test_screen(), "panel_widget_fan", narrow->xml_attrs()));
    REQUIRE(wide_obj != nullptr);
    REQUIRE(narrow_obj != nullptr);

    wide->notify_size_changed(8, 8, 420, 420);
    narrow->notify_size_changed(1, 1, 52, 52);

    lv_obj_t* wide_icon = lv_obj_find_by_name(wide_obj, "fan_widget_icon");
    lv_obj_t* narrow_icon = lv_obj_find_by_name(narrow_obj, "fan_widget_icon");
    REQUIRE(wide_icon != nullptr);
    REQUIRE(narrow_icon != nullptr);

    const lv_font_t* wide_face = lv_obj_get_style_text_font(wide_icon, LV_PART_MAIN);
    const lv_font_t* narrow_face = lv_obj_get_style_text_font(narrow_icon, LV_PART_MAIN);
    INFO("wide=" << static_cast<const void*>(wide_face)
                 << " narrow=" << static_cast<const void*>(narrow_face));
    CHECK(wide_face != narrow_face);

    wide->detach();
    narrow->detach();
}

TEST_CASE("a fan tile declines a box too small to draw in", "[widget_size][tile][1559]") {
    LVGLUITestFixture fixture;
    init_widget_registrations();
    const auto* def = find_widget_def("fan");
    REQUIRE(def != nullptr);
    auto fan = def->factory("fan:0");
    REQUIRE(fan != nullptr);

    CHECK(fan->fits_at(400, 400));
    CHECK_FALSE(fan->fits_at(6, 6));
}

TEST_CASE("half a cell at the small tiers depends on whether the tile shows a reading",
          "[widget_size][tile][1559]") {
    LVGLUITestFixture fixture;
    helix::TileSizing icon_only("led:half");
    icon_only.set_content({"", "", "Light", false});
    helix::TileSizing reading("thermistor:half");
    reading.set_content({"110.0°C", "110.0°C", "Sensor", true});

    SECTION("tiny: a 40px track holds a lone glyph but not a reading") {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Tiny);
        const CellMetrics m{40.0f, 36.0f, 2, 10, 8};
        icon_only.set_cell_metrics(m);
        reading.set_cell_metrics(m);
        CHECK(icon_only.fits(40, 76));
        CHECK_FALSE(reading.fits(40, 76));
    }

    SECTION("micro: a lone glyph takes half a cell, a reading floors at a whole one") {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
        const CellMetrics m{34.0f, 30.0f, 2, 12, 8};
        icon_only.set_cell_metrics(m);
        reading.set_cell_metrics(m);
        CHECK(icon_only.fits(34, 30));
        CHECK_FALSE(reading.fits(34, 64));
        CHECK_FALSE(reading.fits(70, 30));
        CHECK(reading.fits(70, 64));
    }
}

TEST_CASE("a glyph-only tile is not charged its stack's gap", "[widget_size][tile]") {
    // Two tiles identical but for the gap between glyph and label. With the
    // label off, the glyph is the only part drawn, so the gap must not shrink it.
    LVGLUITestFixture fixture;
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
    const CellMetrics m{34.0f, 30.0f, 2, 12, 8};

    auto make_stack = [&](int row_gap) {
        lv_obj_t* root = lv_obj_create(fixture.test_screen());
        lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
        lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(root, row_gap, LV_PART_MAIN);
        lv_obj_set_style_pad_column(root, row_gap, LV_PART_MAIN);
        lv_obj_create(root);
        lv_obj_t* label = lv_obj_create(root);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return root;
    };

    helix::TileSizing tight("lock:tight");
    helix::TileSizing loose("lock:loose");
    tight.set_content({"", "", "", false});
    loose.set_content({"", "", "", false});
    tight.set_cell_metrics(m);
    loose.set_cell_metrics(m);
    tight.set_content_root(make_stack(1));
    loose.set_content_root(make_stack(16));

    tight.measure_and_publish(50, 50);
    loose.measure_and_publish(50, 50);
    REQUIRE(tight.icon_rung() > 0);
    CHECK(loose.icon_rung() == tight.icon_rung());
}

TEST_CASE("every centred-icon tile resolves a different glyph at two sizes",
          "[widget_size][tile][1559]") {
    // The acceptance check for the whole change: each of the eighteen must
    // actually move its glyph between a small box and a large one. A tile whose
    // binding was dropped, whose subject name never reached its XML, or whose
    // icon was missed in a state-swapped set comes back identical at both sizes.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();

    int scaled = 0;
    std::vector<std::string> unscaled;

    for (const auto& id : kCentredIconTiles) {
        const auto* def = find_widget_def(id);
        INFO("tile " << id);
        REQUIRE(def != nullptr);
        REQUIRE(def->factory != nullptr);

        auto instance = def->factory(id);
        REQUIRE(instance != nullptr);

        instance->notify_size_changed(1, 1, 48, 48);
        const int small =
            lv_subject_get_int(lv_xml_get_subject(nullptr, (id + "_tile_icon").c_str()));
        instance->notify_size_changed(8, 8, 420, 420);
        const int large =
            lv_subject_get_int(lv_xml_get_subject(nullptr, (id + "_tile_icon").c_str()));

        if (large > small) {
            ++scaled;
        } else {
            unscaled.push_back(id + " (" + std::to_string(small) + "->" + std::to_string(large) +
                               ")");
        }
    }

    std::string joined;
    for (const auto& u : unscaled) {
        joined += "\n  " + u;
    }
    INFO("these tiles do not grow their glyph between 48px and 420px:" << joined);
    CHECK(unscaled.empty());
    CHECK(scaled == static_cast<int>(kCentredIconTiles.size()));
}

namespace {

/// Every descendant drawing in an MDI icon face, in tree order.
///
/// All of them, not just the first: a state-swapped tile keeps one icon per
/// state and only one is visible, so a set where a single icon was missed
/// resizes until the state changes and then stops.
void collect_icon_fonts(lv_obj_t* root, std::vector<const lv_font_t*>& out) {
    if (!root) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        const lv_font_t* f = lv_obj_get_style_text_font(child, LV_PART_MAIN);
        const bool glyph =
            lv_obj_check_type(child, &lv_label_class) || helix::ui::is_humidity_drop(child);
        if (glyph && helix::ui::is_icon_font(f)) {
            out.push_back(f);
        }
        collect_icon_fonts(child, out);
    }
}

} // namespace

TEST_CASE("every centred-icon tile RENDERS a different glyph at two sizes",
          "[widget_size][tile][1559]") {
    // The subject changing proves measurement ran; it does not prove the XML
    // binds it. This asserts the resolved FONT on the tile's own tree, which is
    // what a dropped binding, a missed icon in a swapped set, or a subject name
    // that never reached the component all fail.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();

    std::vector<std::string> unscaled;
    for (const auto& id : kCentredIconTiles) {
        const auto* def = find_widget_def(id);
        INFO("tile " << id);
        REQUIRE(def != nullptr);
        REQUIRE(def->factory != nullptr);
        auto instance = def->factory(id);
        REQUIRE(instance != nullptr);

        lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
            fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
        if (!root) {
            unscaled.push_back(id + " (component would not build)");
            continue;
        }

        std::vector<const lv_font_t*> small;
        std::vector<const lv_font_t*> large;
        instance->notify_size_changed(1, 1, 48, 48);
        collect_icon_fonts(root, small);
        instance->notify_size_changed(8, 8, 420, 420);
        collect_icon_fonts(root, large);

        if (small.empty() || small.size() != large.size()) {
            unscaled.push_back(id + " (no icon glyph found)");
        } else {
            for (size_t i = 0; i < small.size(); ++i) {
                if (small[i] == large[i]) {
                    unscaled.push_back(id + " (icon " + std::to_string(i) + " of " +
                                       std::to_string(small.size()) + " kept one face)");
                }
            }
        }
        lv_obj_delete(root);
    }

    std::string joined;
    for (const auto& u : unscaled) {
        joined += "\n  " + u;
    }
    INFO("these tiles do not RENDER a different glyph between 48px and 420px:" << joined);
    CHECK(unscaled.empty());
}

TEST_CASE("a badged tile scales its disc with its glyph", "[widget_size][tile][badge]") {
    // A glyph that grows inside a disc that does not spills out of it, so the
    // disc must move with the rung and TileSizing must measure its edge.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    for (const char* id : {"power_device", "favorite_macro"}) {
        INFO("tile " << id);
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        auto instance = def->factory(id);
        REQUIRE(instance != nullptr);
        lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
            fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
        REQUIRE(root != nullptr);
        lv_obj_t* disc = lv_obj_find_by_name(
            root, std::string(id) == "power_device" ? "power_badge" : "fav_macro_badge");
        REQUIRE(disc != nullptr);

        instance->notify_size_changed(2, 2, 60, 60);
        lv_obj_update_layout(root);
        const int small = lv_obj_get_width(disc);
        instance->notify_size_changed(8, 8, 420, 420);
        lv_obj_update_layout(root);
        const int large = lv_obj_get_width(disc);
        INFO("disc " << small << "px at 60x60, " << large << "px at 420x420");
        CHECK(large > small);
        CHECK(lv_obj_get_height(disc) == large);
        lv_obj_delete(root);
    }
}

TEST_CASE("every tile exposes its live instance to edit mode", "[widget_size][tile][1559]") {
    // The resize clamp reaches the selected tile through its root's user data
    // (grid_edit_mode.cpp#handle_resize_move). A tile that does not set it
    // reads as nullptr, the clamp is skipped entirely, and any span the drag
    // produced is accepted and saved: the tile renders surrendered and the next
    // load quietly grows it somewhere else. Nothing about that is visible at
    // the call site, which is why it is pinned here.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();

    std::vector<std::string> unreachable;
    for (const auto& id : kCentredIconTiles) {
        const auto* def = find_widget_def(id);
        INFO("tile " << id);
        REQUIRE(def != nullptr);
        REQUIRE(def->factory != nullptr);

        // The harness owns attach/detach and the teardown some of these
        // widgets need: attaching them by hand leaves a poll thread running
        // into later tests.
        RegistryWidgetHarness harness(fixture.test_screen(), *def);
        REQUIRE(harness.created());
        if (lv_obj_get_user_data(harness.root()) == nullptr) {
            unreachable.push_back(id);
        }
    }

    std::string joined;
    for (const auto& u : unreachable) {
        joined += "\n  " + u;
    }
    INFO("these tiles are invisible to the resize clamp:" << joined);
    CHECK(unreachable.empty());
}

TEST_CASE("a hidden label costs a tile no glyph, and toggling it re-measures",
          "[widget_size][tile][labels]") {
    // The label is drawn only while show_widget_labels is on, so a tile that
    // reserved room for it with the setting off would draw a smaller glyph
    // than its box holds, and one that measured without it would spill once
    // the setting came back on.
    LVGLUITestFixture fixture;
    helix::SettingsManager::instance().init_subjects();
    lv_subject_t* shown = helix::SettingsManager::instance().subject_show_widget_labels();
    const int original = lv_subject_get_int(shown);
    auto drain = [] {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    };
    auto rung = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "labeltest_tile_icon"));
    };

    helix::TileSizing sizing("labeltest", helix::TileSizing::Content{"", "", "Motion", false});

    // Find a height where the label costs a rung. Its existence is the claim;
    // the exact height is this tier's arithmetic.
    int costly_h = -1;
    int off_rung = -1;
    int on_rung = -1;
    for (int h = 30; h <= 200 && costly_h < 0; h += 2) {
        lv_subject_set_int(shown, 0);
        drain();
        sizing.measure_and_publish(200, h);
        const int off = rung();
        lv_subject_set_int(shown, 1);
        drain();
        sizing.measure_and_publish(200, h);
        const int on = rung();
        if (off > on) {
            costly_h = h;
            off_rung = off;
            on_rung = on;
        }
    }
    INFO("no height between 30 and 200px where the label costs the glyph a rung");
    REQUIRE(costly_h > 0);

    // Measured once, then only the setting moves: the tile re-measures itself.
    lv_subject_set_int(shown, 0);
    drain();
    sizing.measure_and_publish(200, costly_h);
    CHECK(rung() == off_rung);
    lv_subject_set_int(shown, 1);
    drain();
    CHECK(rung() == on_rung);
    lv_subject_set_int(shown, 0);
    drain();
    CHECK(rung() == off_rung);

    lv_subject_set_int(shown, original);
    drain();
}

TEST_CASE("a sensor tile's name stays one line however narrow the tile",
          "[widget_size][tile][labels]") {
    // The name is long_mode="dots", which ellipsizes only at a fixed height; at
    // content height a narrow tile wraps it onto a second line instead.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    for (const char* id : {"fan", "thermistor"}) {
        INFO("tile " << id);
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        auto instance = def->factory(id);
        REQUIRE(instance != nullptr);
        lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
            fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
        REQUIRE(root != nullptr);
        lv_obj_t* name =
            lv_obj_find_by_name(root, std::string(id) == "fan" ? "fan_name" : "thermistor_name");
        REQUIRE(name != nullptr);
        lv_label_set_text(name, "A very long sensor name indeed");
        lv_obj_set_size(root, 40, 160);
        instance->notify_size_changed(1, 3, 40, 160);
        lv_obj_update_layout(root);

        const lv_font_t* face = lv_obj_get_style_text_font(name, LV_PART_MAIN);
        CHECK(lv_obj_get_height(name) == lv_font_get_line_height(face));
        lv_obj_delete(root);
    }
}

namespace {

/// Point #tile_icon_xxl_size at @p px for one scope, restoring it after.
class ScopedXxlSize {
  public:
    explicit ScopedXxlSize(int px) {
        const char* v = lv_xml_get_const_silent(nullptr, "tile_icon_xxl_size");
        REQUIRE(v != nullptr);
        saved_ = v;
        lv_xml_set_const(nullptr, "tile_icon_xxl_size", std::to_string(px).c_str());
    }
    ~ScopedXxlSize() {
        lv_xml_set_const(nullptr, "tile_icon_xxl_size", saved_.c_str());
    }

  private:
    std::string saved_;
};

} // namespace

TEST_CASE("the xxl rung scales the largest linked face up to its size, at most 2x",
          "[widget_size][tile][xxl]") {
    // A build whose largest linked face is below the tier's xxl size draws that
    // face scaled up, so a tile grows on every platform, but never past 2x.
    LVGLUITestFixture fixture;
    const lv_font_t* largest = lv_xml_get_font_silent(nullptr, "mdi_icons_128");
    REQUIRE(largest != nullptr);

    {
        ScopedXxlSize size(160);
        const auto face = helix::ui::tile_rung_face(helix::ui::TileLadder::Icon, 5);
        CHECK(face.font == largest);
        CHECK(face.scale == 160 * LV_SCALE_NONE / 128);
    }
    {
        ScopedXxlSize size(400);
        CHECK(helix::ui::tile_rung_face(helix::ui::TileLadder::Icon, 5).scale ==
              helix::ui::kTileMaxScale);
    }

    // The binding draws it: the transform carries the scale, and the layout box
    // the parent spaces is the scaled size.
    ScopedXxlSize size(160);
    lv_subject_t rung;
    lv_subject_init_int(&rung, 0);
    lv_obj_t* icon = lv_label_create(fixture.test_screen());
    lv_label_set_text(icon, "\xF3\xB0\x90\xA5");
    helix::ui::bind_tile_rung(icon, &rung, helix::ui::TileLadder::Icon);
    lv_obj_update_layout(fixture.test_screen());
    CHECK(lv_obj_get_style_transform_scale_x(icon, LV_PART_MAIN) == LV_SCALE_NONE);

    lv_subject_set_int(&rung, 5);
    lv_obj_update_layout(fixture.test_screen());
    CHECK(lv_obj_get_style_transform_scale_x(icon, LV_PART_MAIN) == 160 * LV_SCALE_NONE / 128);
    const int face_h = lv_font_get_line_height(largest);
    CHECK(lv_obj_get_height(icon) >= face_h * 160 / 128 - 1);

    lv_obj_delete(icon);
    lv_subject_deinit(&rung);
}

TEST_CASE("an unconfigured power tile reserves no state line", "[widget_size][tile][badge]") {
    // With no device there is no ON/OFF/LOCKED to draw, so the line is hidden
    // and not measured: the badge and name centre instead of sitting above an
    // empty gap, and the badge may take the room.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    const auto* def = find_widget_def("power_device");
    REQUIRE(def != nullptr);
    auto instance = def->factory("power_device:7");
    REQUIRE(instance != nullptr);
    instance->set_config(nlohmann::json::object());
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
        fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
    REQUIRE(root != nullptr);
    lv_obj_t* status = lv_obj_find_by_name(root, "power_device_status");
    REQUIRE(status != nullptr);
    lv_subject_t* rung = lv_xml_get_subject(nullptr, "power_device:7_tile_icon");
    REQUIRE(rung != nullptr);

    instance->notify_size_changed(2, 2, 110, 90);
    CHECK(lv_obj_has_flag(status, LV_OBJ_FLAG_HIDDEN));
    const int unconfigured = lv_subject_get_int(rung);

    instance->set_config(nlohmann::json{{"device", "printer_psu"}});
    CHECK_FALSE(lv_obj_has_flag(status, LV_OBJ_FLAG_HIDDEN));
    const int configured = lv_subject_get_int(rung);
    INFO("rung " << unconfigured << " unconfigured, " << configured << " with a state line");
    CHECK(unconfigured > configured);
    lv_obj_delete(root);
}

TEST_CASE("the alerts badge scales with the bell and hangs from its shoulder",
          "[widget_size][tile][badge]") {
    // A fixed badge floats off a large bell and swamps a small one.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    const auto* def = find_widget_def("notifications");
    REQUIRE(def != nullptr);
    auto instance = def->factory("notifications");
    REQUIRE(instance != nullptr);
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
        fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
    REQUIRE(root != nullptr);
    lv_obj_t* badge = lv_obj_find_by_name(root, "notification_badge");
    lv_obj_t* bell = lv_obj_find_by_name(root, "status_notification_icon");
    REQUIRE(badge != nullptr);
    REQUIRE(bell != nullptr);
    auto check_at = [&](int px) {
        lv_obj_set_size(root, px, px);
        instance->notify_size_changed(2, 2, px, px);
        lv_obj_update_layout(root);
        // Hung from the glyph box's top-right corner, not the tile's.
        lv_area_t b, g;
        lv_obj_get_coords(badge, &b);
        lv_obj_get_coords(bell, &g);
        CHECK(b.x2 <= g.x2 + 1);
        CHECK(b.y1 >= g.y1 - 1);
        CHECK(b.x1 > g.x1);
        return lv_obj_get_width(badge);
    };
    const int small = check_at(60);
    const int large = check_at(420);
    INFO("badge " << small << "px at 60, " << large << "px at 420");
    CHECK(large > small);
    lv_obj_delete(root);
}

TEST_CASE("an xxl glyph no larger than the xl face draws at the xl rung",
          "[widget_size][tile][xxl]") {
    using helix::ui::tile_drawn_rung;
    const int xxl = helix::kTileRungs - 1;
    // The largest face is the xl face and the cap holds it at 1x.
    CHECK(tile_drawn_rung(xxl, 64, 64) == xxl - 1);
    CHECK(tile_drawn_rung(xxl, 64, 48) == xxl - 1);
    // A bigger face, or a scaled one, is a real xxl glyph.
    CHECK(tile_drawn_rung(xxl, 64, 96) == xxl);
    CHECK(tile_drawn_rung(xxl, 64, 128) == xxl);
    // Every other rung draws at its own size.
    for (int r = 0; r < xxl; ++r) {
        CHECK(tile_drawn_rung(r, 64, 64) == r);
    }
}

TEST_CASE("the xxl scale reaches the target size up to the platform's cap",
          "[widget_size][tile][xxl]") {
    using helix::ui::tile_xxl_scale;
    // A face as large as the target draws unscaled.
    CHECK(tile_xxl_scale(128, 128, 2 * LV_SCALE_NONE) == LV_SCALE_NONE);
    // A smaller face scales up to reach it...
    CHECK(tile_xxl_scale(160, 128, 2 * LV_SCALE_NONE) == 160 * LV_SCALE_NONE / 128);
    // ...never past the cap...
    CHECK(tile_xxl_scale(400, 128, 2 * LV_SCALE_NONE) == 2 * LV_SCALE_NONE);
    // ...and a cap of 1x (the ESP32 image) draws the largest real face as it is.
    CHECK(tile_xxl_scale(160, 128, LV_SCALE_NONE) == LV_SCALE_NONE);
    CHECK(tile_xxl_scale(128, 64, LV_SCALE_NONE) == LV_SCALE_NONE);
}

TEST_CASE("an animated glyph never takes the scaled xxl path", "[widget_size][tile][xxl]") {
    // A scaled glyph re-renders its whole layer on every frame an animation
    // touches it, so a glyph that pulses draws the largest real face at 1x.
    LVGLUITestFixture fixture;
    ScopedXxlSize size(160);
    const auto still = helix::ui::tile_rung_face(helix::ui::TileLadder::Icon, 5);
    const auto animated =
        helix::ui::tile_rung_face(helix::ui::TileLadder::Icon, 5, helix::ui::kTileAnimatedMaxScale);
    CHECK(still.scale > LV_SCALE_NONE);
    CHECK(animated.scale == LV_SCALE_NONE);
    CHECK(animated.font == still.font);

    // The heater tile measures and draws that way.
    helix::init_widget_registrations();
    const auto* def = find_widget_def("bed_temperature");
    REQUIRE(def != nullptr);
    auto instance = def->factory("bed_temperature");
    REQUIRE(instance != nullptr);
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
        fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
    REQUIRE(root != nullptr);
    lv_obj_t* glyph = lv_obj_find_by_name(root, "bed_icon_glyph");
    REQUIRE(glyph != nullptr);
    instance->notify_size_changed(8, 8, 420, 420);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "bed_temperature_tile_icon")) == 5);
    CHECK(lv_obj_get_style_transform_scale_x(glyph, LV_PART_MAIN) == LV_SCALE_NONE);
    lv_obj_delete(root);
}

TEST_CASE("a tile measures its label in the language it draws", "[widget_size][tile][labels]") {
    // The component draws the translated caption, so an English measurement
    // under a wider translation keeps a label the tile has no room for.
    LVGLUITestFixture fixture;
    // Always drawn, so the measurement does not hinge on show_widget_labels.
    helix::TileSizing sizing("trtest", helix::TileSizing::Content{"", "", "Shutdown", false, "",
                                                                  /*label_always_drawn=*/true});
    auto label_at = [&](int w) {
        sizing.measure_and_publish(w, 120);
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "trtest_tile_label"));
    };

    helix::SystemSettingsManager::instance().set_language("de");
    REQUIRE(std::string(lv_tr("Shutdown")) == "Herunterfahren");
    int de_first = -1;
    for (int w = 30; w <= 300 && de_first < 0; w += 2) {
        if (label_at(w)) {
            de_first = w;
        }
    }
    helix::SystemSettingsManager::instance().set_language("en");
    int en_first = -1;
    for (int w = 30; w <= 300 && en_first < 0; w += 2) {
        if (label_at(w)) {
            en_first = w;
        }
    }
    INFO("label first fits at " << en_first << "px in English, " << de_first << "px in German");
    REQUIRE(en_first > 0);
    REQUIRE(de_first > 0);
    CHECK(de_first > en_first);
}

TEST_CASE("fan and sensor tiles keep their name while the glyph shrinks",
          "[widget_size][tile][identity]") {
    // Two fan tiles differ only by name, so where a tile would drop its label
    // to keep its glyph, these shrink the glyph and keep the name. The same
    // content without that flag is the control.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    for (const char* id : {"fan", "thermistor"}) {
        INFO(id);
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        auto instance = def->factory(id);
        REQUIRE(instance != nullptr);
        lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
            fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
        REQUIRE(root != nullptr);
        instance->attach(root, fixture.test_screen());
        helix::TileSizing* sizing = instance->tile_sizing();
        REQUIRE(sizing != nullptr);
        REQUIRE(sizing->content().label_is_identity);

        helix::TileSizing::Content plain = sizing->content();
        plain.label_is_identity = false;
        helix::TileSizing twin(std::string(id) + "_twin", plain);
        twin.set_content_root(sizing->content_root());

        auto get = [](const std::string& name) {
            return lv_subject_get_int(lv_xml_get_subject(nullptr, name.c_str()));
        };
        const std::string named = std::string(id) + "_tile_";
        const std::string control = std::string(id) + "_twin_tile_";
        int kept_where_control_dropped = 0;
        for (int px = 40; px <= 200; px += 2) {
            INFO(px << "px square");
            instance->notify_size_changed(2, 2, px, px);
            twin.measure_and_publish(px, px);
            // Never fewer labels than the control, and only ever by giving up glyph.
            CHECK(get(named + "label") >= get(control + "label"));
            if (get(named + "label") > get(control + "label")) {
                CHECK(get(named + "icon") < get(control + "icon"));
                ++kept_where_control_dropped;
            }
        }
        CHECK(kept_where_control_dropped > 0);
        twin.set_content_root(nullptr);
        instance->detach();
        lv_obj_delete(root);
    }
}

TEST_CASE("a wide light tile reserves the chevron zone it draws", "[widget_size][tile][led]") {
    // The › zone is at least #button_height wide, and wider when its glyph is,
    // so the bulb must be measured in what that zone actually leaves.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    const auto* def = find_widget_def("led");
    REQUIRE(def != nullptr);
    auto instance = def->factory("led");
    REQUIRE(instance != nullptr);
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_xml_create(
        fixture.test_screen(), instance->get_component_name().c_str(), instance->xml_attrs()));
    REQUIRE(root != nullptr);
    lv_obj_t* button = lv_obj_find_by_name(root, "light_button");
    lv_obj_t* bulb = lv_obj_find_by_name(root, "light_icon");
    REQUIRE(button != nullptr);
    REQUIRE(bulb != nullptr);

    int checked = 0;
    for (int w = 120; w <= 320; w += 4) {
        for (int h : {110, 150, 200}) {
            lv_obj_set_size(root, w, h);
            instance->notify_size_changed(4, 4, w, h);
            lv_obj_update_layout(root);
            lv_area_t b, g;
            lv_obj_get_coords(button, &b);
            lv_obj_get_coords(bulb, &g);
            INFO(w << "x" << h << ": bulb " << g.x1 << ".." << g.x2 << " in button " << b.x1 << ".."
                   << b.x2);
            CHECK(g.x1 >= b.x1);
            CHECK(g.x2 <= b.x2);
            ++checked;
        }
    }
    CHECK(checked > 0);

    // At the bulb's top rung the chevron draws one rung under it, the widest it
    // gets; the width reserved for the zone must cover what it draws there.
    lv_obj_t* zone = lv_obj_find_by_name(root, "light_more_button");
    REQUIRE(zone != nullptr);
    lv_obj_set_size(root, 420, 420);
    instance->notify_size_changed(8, 8, 420, 420);
    lv_obj_update_layout(root);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "led_tile_icon")) ==
            helix::kTileRungs - 1);
    INFO("zone draws " << lv_obj_get_width(zone) << "px, reserve "
                       << helix::light_chevron_reserve_px());
    CHECK(lv_obj_get_width(zone) <= helix::light_chevron_reserve_px());
    lv_obj_delete(root);
}

TEST_CASE("a scaled glyph's layout box is the box TileSizing measures",
          "[widget_size][tile][xxl]") {
    // The padding that grows a scaled glyph's box and the width TileSizing
    // measures come from one rule, so a scaled glyph takes exactly the room
    // the verdict budgeted for it.
    LVGLUITestFixture fixture;
    ScopedXxlSize size(256); // twice the 128 face: scaled 2x
    const auto face = helix::ui::tile_rung_face(helix::ui::TileLadder::Icon, 5);
    REQUIRE(face.scale == 2 * LV_SCALE_NONE);
    const auto box = helix::ui::tile_glyph_box(face);

    lv_subject_t rung;
    lv_subject_init_int(&rung, 5);
    lv_obj_t* icon = lv_label_create(fixture.test_screen());
    lv_label_set_text(icon, "\xF3\xB0\x90\xA5");
    helix::ui::bind_tile_rung(icon, &rung, helix::ui::TileLadder::Icon);
    lv_obj_update_layout(fixture.test_screen());
    CHECK(lv_obj_get_width(icon) == box.w);
    CHECK(lv_obj_get_height(icon) == box.h);
    lv_obj_delete(icon);
    lv_subject_deinit(&rung);
}

TEST_CASE("a tile forgets its content root when the root is deleted", "[widget_size][tile]") {
    // The deferred label-setting re-measure walks the root long after the tree
    // it named may be gone; a deleted root must leave nothing to walk.
    LVGLUITestFixture fixture;
    helix::TileSizing sizing("roottest", helix::TileSizing::Content{"", "", "Motion", false});
    lv_obj_t* root = lv_obj_create(fixture.test_screen());
    sizing.set_content_root(root);
    CHECK(sizing.content_root() == root);
    lv_obj_delete(root);
    CHECK(sizing.content_root() == nullptr);
    sizing.measure_and_publish(120, 120); // walks nothing
}

namespace {

/// Each of @p tiles, with the boxes it draws differently on a second decision.
std::vector<std::string> unsettled_tiles(lv_obj_t* screen, const std::vector<std::string>& tiles,
                                         const std::vector<std::pair<int, int>>& boxes) {
    std::vector<std::string> unsettled;
    for (const auto& id : tiles) {
        const auto* def = find_widget_def(id);
        INFO("tile " << id);
        REQUIRE(def != nullptr);
        RegistryWidgetHarness h(screen, *def);
        REQUIRE(h.created());
        std::string joined;
        for (const auto& box : h.unsettled_boxes(boxes)) {
            joined += " " + box;
        }
        if (!joined.empty()) {
            unsettled.push_back(id + ":" + joined);
        }
    }
    return unsettled;
}

} // namespace

TEST_CASE("every centred-icon tile draws a box the same whatever it drew before",
          "[widget_size][tile][1805]") {
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    helix::test::ScopedWidgetLabelsShown labels;

    std::vector<std::pair<int, int>> boxes;
    for (int w : {44, 114, 173}) {
        const auto column = box_sweep(w, w, 20, 160);
        boxes.insert(boxes.end(), column.begin(), column.end());
    }
    for (int h : {30, 60}) {
        const auto row = box_sweep(20, 240, h, h);
        boxes.insert(boxes.end(), row.begin(), row.end());
    }

    {
        helix::test::BypassEngagedSeed engaged;
        CHECK(unsettled_tiles(fixture.test_screen(), {"bypass"}, boxes) ==
              std::vector<std::string>{});
    }
    CHECK(unsettled_tiles(fixture.test_screen(), kCentredIconTiles, boxes) ==
          std::vector<std::string>{});
}

TEST_CASE("an engaged bypass tile's spool row fits the box it is given",
          "[widget_size][tile][1805]") {
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    helix::test::BypassEngagedSeed engaged;
    // The row is drawn only under a drawn caption.
    helix::test::ScopedWidgetLabelsShown labels;

    const auto* def = find_widget_def("bypass");
    REQUIRE(def != nullptr);
    RegistryWidgetHarness h(fixture.test_screen(), *def);
    REQUIRE(h.created());
    lv_obj_t* row = h.child("bypass_active_row");
    REQUIRE(row != nullptr);

    int row_drawn = 0;
    for (int edge = 40; edge <= 240; edge += 2) {
        h.resize(1, 1, edge, edge);
        if (!lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
            ++row_drawn;
        }
        require_no_overflow(h.root(), {}, "bypass at " + std::to_string(edge) + "px");
    }
    // The row is drawn somewhere in the sweep, so the fit check measured it.
    CHECK(row_drawn > 0);
}

TEST_CASE("a bypass tile re-decides its box when bypass engages and disengages",
          "[widget_size][tile][1805]") {
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    helix::test::BypassEngagedSeed seed;
    helix::test::ScopedWidgetLabelsShown labels;
    lv_subject_t* engaged = helix::AmsState::instance().get_bypass_active_subject();
    lv_subject_set_int(engaged, 0);

    const auto* def = find_widget_def("bypass");
    REQUIRE(def != nullptr);
    RegistryWidgetHarness h(fixture.test_screen(), *def);
    REQUIRE(h.created());
    lv_obj_t* row = h.child("bypass_active_row");
    REQUIRE(row != nullptr);
    auto verdict = [&] {
        return "rung " +
               std::to_string(lv_subject_get_int(lv_xml_get_subject(nullptr, "bypass_tile_icon"))) +
               " label " +
               std::to_string(
                   lv_subject_get_int(lv_xml_get_subject(nullptr, "bypass_tile_label"))) +
               " row " + (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) ? "hidden" : "drawn");
    };

    int differing = 0;
    for (int edge = 40; edge <= 240; edge += 2) {
        INFO("bypass at " << edge << "px");
        lv_subject_set_int(engaged, 0);
        h.resize(1, 1, edge, edge);
        const std::string off = verdict();

        // Engaged with no resize: the tile decides as a fresh resize engaged would.
        lv_subject_set_int(engaged, 1);
        lv_obj_update_layout(h.root());
        const std::string flipped_on = verdict();
        h.resize(1, 1, edge, edge);
        const std::string on = verdict();
        CHECK(flipped_on == on);
        if (on != off) {
            ++differing;
        }

        // Disengaged again with no resize: back to the disengaged verdict.
        lv_subject_set_int(engaged, 0);
        lv_obj_update_layout(h.root());
        CHECK(verdict() == off);
    }
    // Some box decides differently with the row than without it, so the flips are
    // proved to re-decide rather than agreeing by coincidence.
    CHECK(differing > 0);
}

// ============================================================================
// The notifications tile's count badge follows the rung its bell glyph draws at
// ============================================================================

namespace {

/// One notifications tile, built the way the manager builds it: a factory
/// instance owns the TileSizing (so the per-instance subjects exist before the
/// component is parsed) and the component is created with its attrs.
struct NotificationsTile {
    std::unique_ptr<helix::PanelWidget> widget;
    lv_obj_t* root = nullptr;
    lv_obj_t* bell = nullptr;
    lv_obj_t* badge = nullptr;

    explicit NotificationsTile(LVGLUITestFixture& fixture) {
        const helix::PanelWidgetDef* def = helix::find_widget_def("notifications");
        REQUIRE(def != nullptr);
        REQUIRE(def->factory != nullptr);
        widget = def->factory("notifications:0");
        REQUIRE(widget != nullptr);

        root = static_cast<lv_obj_t*>(lv_xml_create(
            fixture.test_screen(), "panel_widget_notifications", widget->xml_attrs()));
        REQUIRE(root != nullptr);
        bell = lv_obj_find_by_name(root, "status_notification_icon");
        badge = lv_obj_find_by_name(root, "notification_badge");
        REQUIRE(bell != nullptr);
        REQUIRE(badge != nullptr);
    }

    ~NotificationsTile() {
        if (widget)
            widget->detach();
    }

    /// Publish @p rung as TileSizing does: the requested rung, and the rung
    /// the glyph draws at (@p drawn, or this build's own answer).
    void set_rung(int rung, int drawn = -1) const {
        lv_subject_set_int(lv_xml_get_subject(nullptr, "notifications:0_tile_icon"), rung);
        lv_subject_set_int(lv_xml_get_subject(nullptr, "notifications:0_tile_drawn"),
                           drawn >= 0 ? drawn : helix::ui::tile_drawn_rung(rung));
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE("the notification badge follows the bell's rung", "[widget_size][tile]") {
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    NotificationsTile tile(fixture);

    for (int rung = 0; rung < helix::kTileRungs; ++rung) {
        INFO("rung " << rung);
        tile.set_rung(rung);

        const lv_font_t* bell_face = lv_obj_get_style_text_font(tile.bell, LV_PART_MAIN);
        REQUIRE(bell_face != nullptr);

        const int32_t badge_w = lv_obj_get_style_width(tile.badge, LV_PART_MAIN);

        // The contract is the proportion, not the ladder value: 2/5 of the
        // glyph the bell is actually drawing at this rung.
        // The xxl glyph may draw a smaller face scaled up to its size.
        const int bell_h = bell_face->line_height *
                           lv_obj_get_style_transform_scale_x(tile.bell, LV_PART_MAIN) /
                           LV_SCALE_NONE;
        INFO("badge " << badge_w << " on bell line_height " << bell_h);
        CHECK(std::abs(badge_w * 5 - bell_h * 2) <= 6);
    }
}

TEST_CASE("the notification badge's label face fits the circle", "[widget_size][tile]") {
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    NotificationsTile tile(fixture);

    const lv_obj_t* label = lv_obj_get_child(tile.badge, 0);
    REQUIRE(label != nullptr);

    const lv_font_t* font_xs = theme_manager_get_font("font_xs");
    const lv_font_t* font_small = theme_manager_get_font("font_small");
    REQUIRE(font_xs != nullptr);
    REQUIRE(font_small != nullptr);
    REQUIRE(font_xs != font_small);

    // The cramped end: a badge too small for any face falls back to the
    // smallest one rather than keeping a face that overflows.
    tile.set_rung(0);
    const int32_t dot = lv_obj_get_style_width(tile.badge, LV_PART_MAIN);
    if (font_xs->line_height > dot) {
        CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) == font_xs);
    }

    // The roomy end: the face grows past font_xs and stays inside the circle.
    tile.set_rung(4);
    const int32_t big = lv_obj_get_style_width(tile.badge, LV_PART_MAIN);
    const lv_font_t* face = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    REQUIRE(face != nullptr);
    CHECK(face != font_xs);
    CHECK(face->line_height <= big);
}

TEST_CASE("an xxl bell held to its xl face keeps the xl badge", "[widget_size][tile][xxl]") {
    // xxl requested where the cap leaves the largest face at the xl size (the
    // ESP32 image, or an animated glyph): the glyph draws at xl, so the badge
    // must too, not at the xxl size the rung asked for.
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    NotificationsTile tile(fixture);

    const int xxl = helix::kTileRungs - 1;
    const int drawn = helix::ui::tile_drawn_rung(xxl, 64, 64);
    REQUIRE(drawn == xxl - 1);
    tile.set_rung(xxl, drawn);
    CHECK(lv_obj_get_style_width(tile.badge, LV_PART_MAIN) ==
          theme_manager_get_spacing("tile_badge_xl"));
    CHECK(theme_manager_get_spacing("tile_badge_xl") !=
          theme_manager_get_spacing("tile_badge_xxl"));
}

TEST_CASE("the notifications tile publishes the rung its bell draws at",
          "[widget_size][tile][xxl]") {
    LVGLUITestFixture fixture;
    helix::init_widget_registrations();
    NotificationsTile tile(fixture);
    lv_subject_t* icon = lv_xml_get_subject(nullptr, "notifications:0_tile_icon");
    lv_subject_t* drawn = lv_xml_get_subject(nullptr, "notifications:0_tile_drawn");
    REQUIRE(icon != nullptr);
    REQUIRE(drawn != nullptr);

    for (int px : {60, 420, 900}) {
        INFO("box " << px);
        tile.widget->notify_size_changed(2, 2, px, px);
        CHECK(lv_subject_get_int(drawn) == helix::ui::tile_drawn_rung(lv_subject_get_int(icon)));
    }
    // The biggest box asks for the top rung, which the default seed never holds.
    CHECK(lv_subject_get_int(icon) != 2);
}
