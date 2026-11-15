// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_label_scroll_motion.cpp
 * @brief The "Animations" preference must reach scrolling labels.
 *
 * A label with long_mode="scroll_circular" whose text overflows runs an
 * LV_ANIM_REPEAT_INFINITE offset animation, and every step of that animation
 * calls lv_obj_invalidate(). One such label repaints its area at the display
 * refresh rate for as long as it is on screen, driving both the render and the
 * blend threads on hardware that has no cycles to spare.
 *
 * The print-status cards are where this bites: during a print the slicer's M117
 * text fills display_message, it overflows the card, and it scrolls for the
 * whole job. The animations_enabled preference is consulted at ~30 explicit
 * call sites, none of which touch label long_mode, so a user who turned
 * animations off still paid for this.
 */

#include "ui_fonts.h"
#include "ui_spinner.h"
#include "ui_text.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_text_private.h"
#include "utf8_validation.h"

#include <cstring>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// A slicer M117 banner long enough to overflow any of the print-status cards.
constexpr const char* kLongMessage =
    "Printing layer 42 of 900 - remaining 3h12m - EXTREMELY LONG STATUS BANNER";

/// A filename long enough to overflow the same cards. Overflow is what starts
/// the animation, so a short name would let every assertion below pass against
/// unfixed code.
constexpr const char* kLongFilename =
    "CE3E3V2_articulated_crystal_dragon_supportless_remix_v7_bedslinger_edition_"
    "0.2mm_layer_PLA_matte_charcoal_4h13m_draft_quality_final.gcode";

/// The filament line on the detailed card, likewise overflowing.
constexpr const char* kLongFilament =
    "PLA Matte Charcoal Black - 24.81 m / 74.2 g remaining of 1.00 kg spool, slot 3";

/// Mirrors the production markup: a bound, width-constrained, scrolling label.
constexpr const char* kScrollProbeXml =
    "<component>"
    "  <view extends=\"lv_obj\" width=\"240\" height=\"80\" style_pad_all=\"0\">"
    "    <text_body name=\"scroller\" width=\"120\" bind_text=\"display_message\""
    "               long_mode=\"scroll_circular\"/>"
    "  </view>"
    "</component>";

/// The two jobs from prestonbrown/helixscreen#1440, extension already stripped
/// the way get_display_filename() hands them to the card. Tail-cut to a card
/// they read "Delta filament barrel base_Hyp" and "Logo filament barrel base_Hype":
/// the material and the duration, the only parts that differ, are the parts lost.
constexpr const char* kDeltaJob = "Delta filament barrel base_Hyper PLA_49m";
constexpr const char* kLogoJob = "Logo filament barrel base_Hyper PLA_54m";

/// U+2026 HORIZONTAL kEllipsis, the glyph the shortener joins the two ends with.
constexpr const char* kEllipsis = "\xE2\x80\xA6";

/// True when LVGL is running any animation against @p obj — i.e. the label is
/// invalidating itself every frame.
bool is_animating(lv_obj_t* obj) {
    return lv_anim_get(obj, nullptr) != nullptr;
}

/// Drawn width of @p text in @p font, the same measure lv_draw_label lays it out with.
int32_t drawn_width(const std::string& text, const lv_font_t* font) {
    lv_text_attributes_t attributes = {};
    attributes.max_width = LV_COORD_MAX;
    return lv_text_get_width(text.c_str(), static_cast<uint32_t>(text.size()), font, &attributes);
}

using helix::text_io::is_valid_utf8;

/// Captures the text of the label draw task LVGL is about to hand a draw unit.
void capture_drawn_text(lv_event_t* e) {
    lv_draw_task_t* task = lv_event_get_draw_task(e);
    if (!task || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) {
        return;
    }
    auto* out = static_cast<std::optional<std::string>*>(lv_event_get_user_data(e));
    *out = lv_draw_task_get_label_dsc(task)->text;
}

class ScrollMotionFixture : public XMLTestFixture {
  public:
    ScrollMotionFixture() {
        // print_status_preview_card draws a <spinner>; an unregistered widget
        // name makes that element vanish from the built tree.
        ui_spinner_init();

        lv_subject_init_int(&animations_, 1);
        lv_xml_register_subject(nullptr, "settings_animations_enabled", &animations_);

        lv_subject_init_string(&message_, message_buf_, nullptr, sizeof(message_buf_),
                               kLongMessage);
        lv_xml_register_subject(nullptr, "display_message", &message_);

        // The card keeps this label hidden until Klipper reports an M117, so a
        // print in progress is the condition under test.
        lv_subject_init_int(&message_visible_, 1);
        lv_xml_register_subject(nullptr, "display_message_visible", &message_visible_);

        lv_subject_init_string(&filename_, filename_buf_, nullptr, sizeof(filename_buf_),
                               kLongFilename);
        lv_xml_register_subject(nullptr, "print_display_filename", &filename_);

        lv_subject_init_string(&filament_, filament_buf_, nullptr, sizeof(filament_buf_),
                               kLongFilament);
        lv_xml_register_subject(nullptr, "print_status_filament_text", &filament_);

        REQUIRE(lv_xml_register_component_from_data("scroll_motion_probe", kScrollProbeXml) ==
                LV_RESULT_OK);
    }

    ~ScrollMotionFixture() override {
        lv_xml_component_unregister("scroll_motion_probe");
        lv_xml_unregister_subject(nullptr, "display_message");
        lv_xml_unregister_subject(nullptr, "display_message_visible");
        lv_xml_unregister_subject(nullptr, "print_display_filename");
        lv_xml_unregister_subject(nullptr, "print_status_filament_text");
        lv_xml_unregister_subject(nullptr, "settings_animations_enabled");
        lv_subject_deinit(&filament_);
        lv_subject_deinit(&filename_);
        lv_subject_deinit(&message_visible_);
        lv_subject_deinit(&message_);
        lv_subject_deinit(&animations_);
    }

    /// Flip the preference, then let LVGL settle.
    ///
    /// lv_label_set_long_mode() stops any running animation at once but only
    /// MARKS the text for refresh; LVGL re-evaluates overflow (and restarts a
    /// scroll) on LV_EVENT_UPDATE_LAYOUT_COMPLETED. In the app that is the next
    /// frame — here it has to be asked for.
    void set_animations(bool on) {
        lv_subject_set_int(&animations_, on ? 1 : 0);
        lv_obj_update_layout(test_screen());
    }

    /// What the label puts on screen: the text of the draw task it emits when the
    /// display is refreshed. That descriptor is what the draw unit renders, so a
    /// string captured here is the string a user sees, whatever
    /// lv_label_get_text() says. Empty when the label emitted no draw-task event,
    /// which is how a label that is not hooked at all reads.
    std::optional<std::string> rendered_text(lv_obj_t* label) {
        std::optional<std::string> drawn;
        lv_obj_add_event_cb(label, capture_drawn_text, LV_EVENT_DRAW_TASK_ADDED, &drawn);
        // The suite's displays render into a buffer nobody reads, and some are
        // created without a flush callback, so a refresh of one never completes
        // its flush. Give this one a callback that reports the flush done.
        lv_display_t* display = lv_obj_get_display(label);
        lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
            lv_display_flush_ready(d);
        });
        lv_obj_invalidate(label);
        lv_refr_now(display);
        lv_obj_remove_event_cb_with_user_data(label, capture_drawn_text, &drawn);
        return drawn;
    }

    /// Replace the bound filename before a card is built.
    void set_filename(const char* text) {
        lv_subject_set_string(&filename_, text);
    }

    /// Replace the bound message and let the binding reach the label.
    void set_message(const char* text) {
        lv_subject_set_string(&message_, text);
        lv_obj_update_layout(test_screen());
    }

    /// Build the probe and hand back its scrolling label, laid out.
    lv_obj_t* build_probe() {
        auto* view =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "scroll_motion_probe", nullptr));
        REQUIRE(view != nullptr);
        lv_obj_update_layout(view);
        lv_obj_t* label = lv_obj_find_by_name(view, "scroller");
        REQUIRE(label != nullptr);
        return label;
    }

  private:
    lv_subject_t animations_{};
    lv_subject_t message_{};
    lv_subject_t message_visible_{};
    lv_subject_t filename_{};
    lv_subject_t filament_{};
    char message_buf_[256]{};
    char filename_buf_[256]{};
    char filament_buf_[256]{};
};

} // namespace

// The control: with animations on, an overflowing label really does run an
// infinite animation. Without this the "no animation" assertions below could
// pass for the wrong reason — a label that never overflowed in the first place.
TEST_CASE_METHOD(ScrollMotionFixture, "scrolling label animates while animations are enabled",
                 "[ui_text][label][scroll][1440]") {
    lv_obj_t* label = build_probe();

    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    CHECK(is_animating(label));
}

TEST_CASE_METHOD(ScrollMotionFixture,
                 "animations off: scrolling label holds still and stays one line",
                 "[ui_text][label][scroll][1440]") {
    set_animations(false);

    lv_obj_t* label = build_probe();

    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    CHECK_FALSE(is_animating(label));
}

TEST_CASE_METHOD(ScrollMotionFixture, "toggling the animations preference reaches a live label",
                 "[ui_text][label][scroll][1440]") {
    lv_obj_t* label = build_probe();
    REQUIRE(is_animating(label));

    // Turning animations off must stop a label that is already scrolling, not
    // only affect labels built afterwards.
    set_animations(false);
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    CHECK_FALSE(is_animating(label));

    // And turning it back on restores the mode the XML declared.
    set_animations(true);
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    CHECK(is_animating(label));
}

// The shipping markup, not just a probe: print_status_preview_card is what is on
// screen for the length of a print, and its display_message label is the one the
// slicer's M117 banner overflows.
TEST_CASE_METHOD(ScrollMotionFixture, "print status card stops scrolling when animations go off",
                 "[ui_text][label][scroll][1440]") {
    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_preview_card.xml") == LV_RESULT_OK);

    auto* card =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_status_preview_card", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);

    lv_obj_t* label = lv_obj_find_by_name(card, "display_message");
    REQUIRE(label != nullptr);

    // Premise: mid-print, the M117 banner overflows and the card scrolls it.
    REQUIRE(is_animating(label));

    set_animations(false);

    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    CHECK_FALSE(is_animating(label));
}

// The component the device's XML parse log shows on screen during a print. It
// carries TWO scrolling labels — the filename and the filament line — so a print
// with a long filename and a long filament string runs two infinite invalidating
// animations on one card at once. Both must stop.
TEST_CASE_METHOD(ScrollMotionFixture, "detailed print card stops both scrolling labels",
                 "[ui_text][label][scroll][1440]") {
    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);

    auto* card = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);

    lv_obj_t* filename = lv_obj_find_by_name(card, "detailed_filename");
    lv_obj_t* filament = lv_obj_find_by_name(card, "detailed_filament_text");
    REQUIRE(filename != nullptr);
    REQUIRE(filament != nullptr);

    // Premise: both overflow mid-print, so both are animating.
    REQUIRE(is_animating(filename));
    REQUIRE(is_animating(filament));

    set_animations(false);

    CHECK(lv_label_get_long_mode(filename) == LV_LABEL_LONG_MODE_CLIP);
    CHECK(lv_label_get_long_mode(filament) == LV_LABEL_LONG_MODE_CLIP);
    CHECK_FALSE(is_animating(filename));
    CHECK_FALSE(is_animating(filament));
}

// A scrolling mode measures its text unwrapped, so the label is exactly one line
// tall and the layout around it is built for that. CLIP sets the same
// LV_TEXT_FLAG_EXPAND and is the only still mode that does; DOTS clears it, so
// the label wraps and pushes whatever sits below it out of its container.
// panel_widget_active_spool is one such stack: three labels in a card that does
// not scroll, sized to the three single lines (#1286).
TEST_CASE_METHOD(ScrollMotionFixture, "animations off leaves an overflowing label one line tall",
                 "[ui_text][label][scroll][1440]") {
    lv_obj_t* label = build_probe();
    REQUIRE(is_animating(label));

    const int32_t scrolling_height = lv_obj_get_height(label);
    const int32_t line_height =
        lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
    REQUIRE(scrolling_height == line_height);

    set_animations(false);
    lv_obj_update_layout(label);

    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    CHECK(lv_obj_get_height(label) == scrolling_height);
}

// LVGL's DOTS mode rewrites the label's own text buffer with the ellipsized
// string, so lv_label_get_text() stops returning what was set. The test fixture
// holds animations OFF for every test in the suite, so a still mode that did
// that would make every label in the tree report truncated text under test, and
// would show a user with animations off "No..." where the card says "No Spool".
TEST_CASE_METHOD(ScrollMotionFixture, "animations off does not rewrite the label's text",
                 "[ui_text][label][scroll][1440]") {
    lv_obj_t* label = build_probe();
    REQUIRE(is_animating(label));
    REQUIRE(std::string(lv_label_get_text(label)) == kLongMessage);

    set_animations(false);
    lv_obj_update_layout(label);

    CHECK(std::string(lv_label_get_text(label)) == kLongMessage);
    CHECK(std::string(lv_label_get_text(label)).find("...") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Middle ellipsis (prestonbrown/helixscreen#1441)
//
// With animations off a still label clips, so a filename that overflows is cut
// mid-glyph at the label's edge. The shortener keeps both ends instead: the
// prefix that names the job and the suffix that tells it from its neighbours.
// ---------------------------------------------------------------------------

// The pure rule first: a function over (text, width, font), no widget involved.
TEST_CASE_METHOD(ScrollMotionFixture, "middle ellipsis leaves text that fits alone",
                 "[ui_text][label][ellipsis][1441]") {
    const lv_font_t* font = LV_FONT_DEFAULT;
    const std::string text = "benchy";
    const int32_t width = drawn_width(text, font);

    CHECK(helix::ui::middle_ellipsize(text.c_str(), width, font) == text);
    CHECK(helix::ui::middle_ellipsize(text.c_str(), width * 4, font) == text);
    CHECK(helix::ui::middle_ellipsize("", width, font).empty());
}

TEST_CASE_METHOD(ScrollMotionFixture, "middle ellipsis keeps both ends within the width",
                 "[ui_text][label][ellipsis][1441]") {
    const lv_font_t* font = LV_FONT_DEFAULT;
    const std::string text = kDeltaJob;
    const int32_t width = drawn_width(text, font) / 2;
    REQUIRE(drawn_width(text, font) > width);

    const std::string fitted = helix::ui::middle_ellipsize(text.c_str(), width, font);

    INFO("fitted: " << fitted);
    CHECK(drawn_width(fitted, font) <= width);
    const size_t cut = fitted.find(kEllipsis);
    REQUIRE(cut != std::string::npos);
    const std::string head = fitted.substr(0, cut);
    const std::string tail = fitted.substr(cut + std::strlen(kEllipsis));
    CHECK_FALSE(head.empty());
    CHECK_FALSE(tail.empty());
    CHECK(text.compare(0, head.size(), head) == 0);
    CHECK(text.compare(text.size() - tail.size(), tail.size(), tail) == 0);
    // Split roughly evenly: neither end is starved to feed the other.
    CHECK(drawn_width(head, font) >= width / 3);
    CHECK(drawn_width(tail, font) >= width / 3);
}

// The point of the change: two jobs that differ only in their suffix stay
// distinguishable once shortened to the same card.
TEST_CASE_METHOD(ScrollMotionFixture, "middle ellipsis keeps the suffix that tells two jobs apart",
                 "[ui_text][label][ellipsis][1441]") {
    const lv_font_t* font = LV_FONT_DEFAULT;
    const int32_t width = drawn_width(kDeltaJob, font) * 2 / 3;

    const std::string delta = helix::ui::middle_ellipsize(kDeltaJob, width, font);
    const std::string logo = helix::ui::middle_ellipsize(kLogoJob, width, font);

    INFO("delta: " << delta << "  logo: " << logo);
    CHECK(delta != logo);
    CHECK(delta.rfind("PLA_49m") == delta.size() - std::strlen("PLA_49m"));
    CHECK(logo.rfind("PLA_54m") == logo.size() - std::strlen("PLA_54m"));
    CHECK(delta.rfind("Delta", 0) == 0);
    CHECK(logo.rfind("Logo", 0) == 0);
}

// Width is measured, never counted: a Cyrillic name is two bytes per glyph and
// draws wider than a Latin one of the same length, and the cut must land on a
// glyph boundary on both sides of the ellipsis.
TEST_CASE_METHOD(ScrollMotionFixture, "middle ellipsis cuts a Cyrillic name on glyph boundaries",
                 "[ui_text][label][ellipsis][1441]") {
    const lv_font_t* font = LV_FONT_DEFAULT;
    const std::string cyrillic = "Кораблик для проверки принтера_PLA_49m";
    const std::string latin = "Benchy for checking the printer_PLA_49m";
    REQUIRE(cyrillic.size() > latin.size());
    const int32_t width = drawn_width(latin, font) / 2;

    const std::string fitted = helix::ui::middle_ellipsize(cyrillic.c_str(), width, font);

    INFO("fitted: " << fitted);
    REQUIRE(fitted.find(kEllipsis) != std::string::npos);
    CHECK(is_valid_utf8(fitted));
    CHECK(drawn_width(fitted, font) <= width);
    CHECK(fitted.rfind("_49m") == fitted.size() - std::strlen("_49m"));
    CHECK(fitted.rfind("Кора", 0) == 0);
}

// Now the wiring: what the still label actually draws, read off the draw task
// LVGL hands the draw unit, while the label's own text and height stay put.
TEST_CASE_METHOD(ScrollMotionFixture,
                 "animations off: overflowing text is drawn with its middle elided",
                 "[ui_text][label][ellipsis][1441]") {
    set_animations(false);
    lv_obj_t* label = build_probe();
    REQUIRE(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    const int32_t line_height =
        lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));

    const auto drawn = rendered_text(label);

    REQUIRE(drawn.has_value());
    INFO("drawn: " << *drawn);
    const size_t cut = drawn->find(kEllipsis);
    REQUIRE(cut != std::string::npos);
    const std::string head = drawn->substr(0, cut);
    const std::string tail = drawn->substr(cut + std::strlen(kEllipsis));
    const std::string source = kLongMessage;
    CHECK_FALSE(head.empty());
    CHECK_FALSE(tail.empty());
    CHECK(source.compare(0, head.size(), head) == 0);
    CHECK(source.compare(source.size() - tail.size(), tail.size(), tail) == 0);
    CHECK(drawn_width(*drawn, lv_obj_get_style_text_font(label, LV_PART_MAIN)) <=
          lv_obj_get_content_width(label));

    // Neither constraint from the clip fallback is given up for the affordance.
    CHECK(std::string(lv_label_get_text(label)) == kLongMessage);
    CHECK(lv_obj_get_height(label) == line_height);
}

TEST_CASE_METHOD(ScrollMotionFixture, "animations off: text that fits is drawn whole",
                 "[ui_text][label][ellipsis][1441]") {
    set_animations(false);
    lv_obj_t* label = build_probe();
    const auto before = rendered_text(label);
    REQUIRE(before.has_value());
    REQUIRE(before->find(kEllipsis) != std::string::npos);

    // The bound subject changes mid-print; the hook must follow the new text,
    // not keep drawing the string it fitted for the old one.
    set_message("Layer 42");

    const auto drawn = rendered_text(label);
    REQUIRE(drawn.has_value());
    CHECK(*drawn == "Layer 42");
}

TEST_CASE_METHOD(ScrollMotionFixture, "animations on: the scrolling label draws its text whole",
                 "[ui_text][label][ellipsis][1441]") {
    lv_obj_t* label = build_probe();
    REQUIRE(is_animating(label));

    // Ask for the draw-task event ourselves: while scrolling, the hook must
    // leave the descriptor alone even when it hears about the task.
    lv_obj_set_send_draw_task_events(label, true);
    const auto drawn = rendered_text(label);

    REQUIRE(drawn.has_value());
    CHECK(*drawn == kLongMessage);
}

// The issue names both: the card is re-laid out on rotation and breakpoint
// changes, and the font tier changes with the display size.
TEST_CASE_METHOD(ScrollMotionFixture, "middle ellipsis is recomputed when the label is resized",
                 "[ui_text][label][ellipsis][1441]") {
    set_animations(false);
    lv_obj_t* label = build_probe();
    const auto narrow = rendered_text(label);
    REQUIRE(narrow.has_value());
    REQUIRE(narrow->find(kEllipsis) != std::string::npos);

    lv_obj_set_width(lv_obj_get_parent(label), kTestDisplayWidth);
    lv_obj_set_width(label, kTestDisplayWidth);
    lv_obj_update_layout(label);
    const auto wide = rendered_text(label);

    REQUIRE(wide.has_value());
    INFO("narrow: " << *narrow << "  wide: " << *wide);
    CHECK(wide->size() > narrow->size());
    CHECK(std::string(lv_label_get_text(label)) == kLongMessage);
}

TEST_CASE_METHOD(ScrollMotionFixture, "middle ellipsis is recomputed when the font changes",
                 "[ui_text][label][ellipsis][1441]") {
    set_animations(false);
    lv_obj_t* label = build_probe();
    const lv_font_t* small = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const lv_font_t* large = &noto_sans_28;
    REQUIRE(lv_font_get_line_height(large) > lv_font_get_line_height(small));
    const auto in_small = rendered_text(label);
    REQUIRE(in_small.has_value());
    REQUIRE(in_small->find(kEllipsis) != std::string::npos);

    lv_obj_set_style_text_font(label, large, LV_PART_MAIN);
    lv_obj_update_layout(label);
    const auto in_large = rendered_text(label);

    REQUIRE(in_large.has_value());
    INFO("small: " << *in_small << "  large: " << *in_large);
    CHECK(in_large->find(kEllipsis) != std::string::npos);
    CHECK(in_large->size() < in_small->size());
    CHECK(drawn_width(*in_large, large) <= lv_obj_get_content_width(label));
}

// The shipping card, with the filename the issue was opened on.
TEST_CASE_METHOD(ScrollMotionFixture, "detailed print card elides the filename's middle when still",
                 "[ui_text][label][ellipsis][1441]") {
    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);
    set_filename(kDeltaJob);
    set_animations(false);

    auto* card = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_set_width(card, 200);
    lv_obj_update_layout(card);
    lv_obj_t* filename = lv_obj_find_by_name(card, "detailed_filename");
    REQUIRE(filename != nullptr);
    REQUIRE(drawn_width(kDeltaJob, lv_obj_get_style_text_font(filename, LV_PART_MAIN)) >
            lv_obj_get_content_width(filename));

    const auto drawn = rendered_text(filename);

    REQUIRE(drawn.has_value());
    INFO("drawn: " << *drawn);
    CHECK(drawn->rfind("Delta", 0) == 0);
    CHECK(drawn->find(kEllipsis) != std::string::npos);
    CHECK(drawn->rfind("_49m") == drawn->size() - std::strlen("_49m"));
    CHECK(std::string(lv_label_get_text(filename)) == kDeltaJob);
}
