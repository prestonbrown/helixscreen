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

#include "ui_spinner.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// A slicer M117 banner long enough to overflow any of the print-status cards.
constexpr const char* LONG_MESSAGE =
    "Printing layer 42 of 900 - remaining 3h12m - EXTREMELY LONG STATUS BANNER";

/// A filename long enough to overflow the same cards. Overflow is what starts
/// the animation, so a short name would let every assertion below pass against
/// unfixed code.
constexpr const char* LONG_FILENAME =
    "CE3E3V2_articulated_crystal_dragon_supportless_remix_v7_bedslinger_edition_"
    "0.2mm_layer_PLA_matte_charcoal_4h13m_draft_quality_final.gcode";

/// The filament line on the detailed card, likewise overflowing.
constexpr const char* LONG_FILAMENT =
    "PLA Matte Charcoal Black - 24.81 m / 74.2 g remaining of 1.00 kg spool, slot 3";

/// Mirrors the production markup: a bound, width-constrained, scrolling label.
constexpr const char* SCROLL_PROBE_XML =
    "<component>"
    "  <view extends=\"lv_obj\" width=\"240\" height=\"80\" style_pad_all=\"0\">"
    "    <text_body name=\"scroller\" width=\"120\" bind_text=\"display_message\""
    "               long_mode=\"scroll_circular\"/>"
    "  </view>"
    "</component>";

/// True when LVGL is running any animation against @p obj — i.e. the label is
/// invalidating itself every frame.
bool is_animating(lv_obj_t* obj) {
    return lv_anim_get(obj, nullptr) != nullptr;
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
                               LONG_MESSAGE);
        lv_xml_register_subject(nullptr, "display_message", &message_);

        // The card keeps this label hidden until Klipper reports an M117, so a
        // print in progress is the condition under test.
        lv_subject_init_int(&message_visible_, 1);
        lv_xml_register_subject(nullptr, "display_message_visible", &message_visible_);

        lv_subject_init_string(&filename_, filename_buf_, nullptr, sizeof(filename_buf_),
                               LONG_FILENAME);
        lv_xml_register_subject(nullptr, "print_display_filename", &filename_);

        lv_subject_init_string(&filament_, filament_buf_, nullptr, sizeof(filament_buf_),
                               LONG_FILAMENT);
        lv_xml_register_subject(nullptr, "print_status_filament_text", &filament_);

        REQUIRE(lv_xml_register_component_from_data("scroll_motion_probe", SCROLL_PROBE_XML) ==
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

TEST_CASE_METHOD(ScrollMotionFixture, "animations off: scrolling label ellipsizes and stays still",
                 "[ui_text][label][scroll][1440]") {
    set_animations(false);

    lv_obj_t* label = build_probe();

    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);
    CHECK_FALSE(is_animating(label));
}

TEST_CASE_METHOD(ScrollMotionFixture, "toggling the animations preference reaches a live label",
                 "[ui_text][label][scroll][1440]") {
    lv_obj_t* label = build_probe();
    REQUIRE(is_animating(label));

    // Turning animations off must stop a label that is already scrolling, not
    // only affect labels built afterwards.
    set_animations(false);
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);
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

    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);
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

    CHECK(lv_label_get_long_mode(filename) == LV_LABEL_LONG_MODE_DOTS);
    CHECK(lv_label_get_long_mode(filament) == LV_LABEL_LONG_MODE_DOTS);
    CHECK_FALSE(is_animating(filename));
    CHECK_FALSE(is_animating(filament));
}
