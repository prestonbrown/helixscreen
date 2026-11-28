// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_metadata_strip_fit.cpp
 * @brief The metadata strip's rows must degrade when their labels do not fit.
 *
 * The layer/objects/filament row (print_status_preview_card.xml) was a flex
 * row of content-sized prose labels. LVGL computes a space_between row's
 * inter-item gap as (row_width - content_width) / (n-1) with no clamp at zero
 * (lv_flex.c place_content), so when the labels were together wider than the
 * row the gap went negative and adjacent labels rendered on top of each
 * other. Observed on a live K2 Plus (800x480) mid-print: the row is 363px
 * there and "Layer 474 / 1334 (55.4mm)" + "12 of 12 objects" + "92.6m used"
 * measured 393px, so each pair overlapped by 15px.
 *
 * The strip is now icon clusters over bare values (language-neutral widths),
 * with the layer cluster growing and dotting as the overflow floor. The
 * contract these tests pin: no cluster ever starts before its left neighbour
 * ends, whatever the data, and an overflowing layer value yields instead of
 * interpenetrating. The strings below mirror that print at this fixture's
 * 800x480.
 *
 * The strip's time line (print_status_strip.xml) has the same class of
 * hazard: the times sit at the line's right edge, and the finish time takes
 * only what the other times leave and dots, so a long elapsed/remaining pair
 * shrinks it instead of pushing the elapsed time past the progress block.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

using helix::ui::UpdateQueue;

#include "ui_breakpoint.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "filament_sensor_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <cstdlib>
#include <lvgl.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Owns a real PrintStatusPanel built from production XML (same shape as
/// test_print_status_header_action_button.cpp's fixture).
///
/// A local PrintStatusPanel whose destructor runs leaves its helix-xml
/// subject registrations dangling for the rest of the process (see the header
/// comment in test_widget_size_print_status.cpp for the full anatomy). Poking
/// production's process-lifetime singleton re-registers every name against
/// stable storage, healing the entries this fixture's teardown dangles.
struct MetadataStripFixture : public LVGLUITestFixture {
    MetadataStripFixture() {
        heal_global_print_status_panel_subjects();
        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
    }

    ~MetadataStripFixture() override {
        if (eta_saved_) {
            if (lv_subject_t* subject = lv_xml_get_subject(nullptr, "print_eta")) {
                lv_subject_copy_string(subject, saved_eta_.c_str());
            }
        }
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
        heal_global_print_status_panel_subjects();
    }

    lv_obj_t* label(const char* name) const {
        lv_obj_t* found = lv_obj_find_by_name(root_, name);
        REQUIRE(found != nullptr);
        return found;
    }

    /// Right edge of a widget in its parent's content coordinates, where all
    /// row siblings are directly comparable.
    static int right_edge(const lv_obj_t* w) {
        return lv_obj_get_x(w) + lv_obj_get_width(w);
    }

    /// Screen-absolute edges, comparable across different parents.
    static int right_edge_screen(const lv_obj_t* w) {
        lv_area_t a;
        lv_obj_get_coords(w, &a);
        return a.x2;
    }

    static int left_edge_screen(const lv_obj_t* w) {
        lv_area_t a;
        lv_obj_get_coords(w, &a);
        return a.x1;
    }

    static lv_area_t area_of(const lv_obj_t* w) {
        lv_area_t a;
        lv_obj_get_coords(w, &a);
        return a;
    }

    /// The box a widget's background draws in: its coords grown by its
    /// transform width and height.
    static lv_area_t drawn_area(lv_obj_t* w) {
        lv_area_t a = area_of(w);
        lv_area_increase(&a, lv_obj_get_style_transform_width(w, LV_PART_MAIN),
                         lv_obj_get_style_transform_height(w, LV_PART_MAIN));
        return a;
    }

    /// The finish time is shown by a label and measured by an invisible copy,
    /// both bound to print_eta, so a case sets the subject, not one label.
    /// The fixture puts the original back.
    void set_eta(const char* text) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "print_eta");
        REQUIRE(subject != nullptr);
        if (!eta_saved_) {
            const char* now = lv_subject_get_string(subject);
            saved_eta_ = now ? now : "";
            eta_saved_ = true;
        }
        lv_subject_copy_string(subject, text);
    }

    void heal_global_print_status_panel_subjects() {
        auto& global = get_global_print_status_panel();
        if (!global.are_subjects_initialized()) {
            global.init_subjects();
        }
    }

    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
    std::string saved_eta_;
    bool eta_saved_ = false;
};

} // namespace

TEST_CASE_METHOD(MetadataStripFixture,
                 "Metadata strip: layer/objects/filament row never overlaps its clusters",
                 "[print_status][metadata_strip]") {
    // The icon-cluster strings for the print that broke the prose row on a
    // live K2 Plus (393px of prose in this 800x480 fixture's 363px row).
    lv_obj_t* layer = label("layer_progress_label");
    lv_obj_t* objects = label("objects_count_label");
    lv_obj_t* filament = label("filament_used_label");

    // Simulate the multi-object print state: the whole objects cluster is
    // bound hidden while fewer than two objects are defined, and its text
    // arrives from a subject. Both are set directly here - the row's flex
    // behaviour is under test, not the visibility binding.
    lv_obj_t* objects_cluster = lv_obj_find_by_name(root_, "objects_cluster");
    REQUIRE(objects_cluster != nullptr);
    lv_obj_clear_flag(objects_cluster, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(layer, "474 / 1334 · 55.4mm");
    lv_label_set_text(objects, "12/12");
    lv_label_set_text(filament, "92.6m");

    lv_obj_update_layout(root_);

    const int objects_left = left_edge_screen(objects_cluster);
    const int layer_right = right_edge_screen(layer);
    const int objects_right = right_edge_screen(objects);
    const int filament_left = left_edge_screen(filament);
    CAPTURE(layer_right, objects_left, objects_right, filament_left);

    // No cluster may start before its left neighbour ends. The unfixed row
    // distributed its deficit as negative gaps, which lands here.
    REQUIRE(objects_left >= layer_right);
    REQUIRE(filament_left >= objects_right);

    // Overflow must still degrade, not interpenetrate: with long_mode="dots"
    // the layer value's box is narrower than the full text it holds.
    // A dotted label rewrites its own text with the ellipsis, so the full
    // width is measured from the string that was set.
    const char* overflowing = "474 / 13340 · 554.4mm plus a pathological tail";
    lv_label_set_text(layer, overflowing);
    lv_obj_update_layout(root_);
    REQUIRE(lv_label_get_long_mode(layer) == LV_LABEL_LONG_MODE_DOTS);
    lv_point_t full;
    lv_text_get_size(&full, overflowing, lv_obj_get_style_text_font(layer, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(layer, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    REQUIRE(lv_obj_get_width(layer) < full.x);
    REQUIRE(left_edge_screen(filament) >= right_edge_screen(objects));
}

TEST_CASE_METHOD(MetadataStripFixture,
                 "Strip: a multi-day print's times stay inside the progress block",
                 "[print_status][metadata_strip]") {
    lv_obj_t* elapsed = label("print_elapsed");
    lv_obj_t* remaining = label("print_remaining");
    lv_obj_t* eta = label("print_eta");
    lv_obj_t* line = label("ps_strip_print_line");

    lv_label_set_text(elapsed, "2d 16h 03m");
    lv_label_set_text(remaining, "2d 08h 26m");
    set_eta("(~10:06 PM +1d)");

    lv_obj_update_layout(root_);

    CAPTURE(right_edge_screen(eta), left_edge_screen(elapsed), right_edge_screen(elapsed),
            right_edge_screen(line));
    REQUIRE(lv_obj_get_width(line) > 0);
    // The eta yields: it ends before the elapsed time begins, and the elapsed
    // time ends inside the line.
    REQUIRE(right_edge_screen(eta) <= left_edge_screen(elapsed));
    REQUIRE(right_edge_screen(elapsed) <= right_edge_screen(line));
    CHECK(lv_obj_get_width(eta) < lv_obj_get_width(label("print_eta_measure")));
}

TEST_CASE_METHOD(MetadataStripFixture,
                 "Strip: micro and tiny give the layer count the filament's room and drop elapsed",
                 "[print_status][metadata_strip][micro_declutter]") {
    lv_subject_t* breakpoint = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(breakpoint != nullptr);
    const int32_t original = lv_subject_get_int(breakpoint);
    REQUIRE(original == to_int(UiBreakpoint::Medium));

    lv_obj_t* filament = label("filament_cluster");
    lv_obj_t* layer = label("layer_progress_label");
    lv_obj_t* elapsed_icon = label("elapsed_icon");
    lv_obj_t* elapsed = label("print_elapsed");
    lv_obj_t* percent = label("print_percent");
    lv_obj_t* eta = label("print_eta");
    lv_obj_t* line = label("ps_strip_print_line");
    lv_label_set_text(label("filament_used_label"), "92.6m");
    // The corner toggle and the pause glyph are shown directly: their
    // placement is under test, not the bindings that show them.
    lv_obj_t* toggle = label("btn_view_toggle");
    lv_obj_t* hint = label("layer_pause_hint");
    lv_obj_t* count = label("layer_count");
    lv_obj_remove_flag(label("preview_corner_buttons"), LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(toggle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(layer, "9 / 240");
    const char* touch_min_text = lv_xml_get_const(nullptr, "touch_target_min");
    REQUIRE(touch_min_text != nullptr);
    const int32_t touch_min = std::atoi(touch_min_text);

    lv_obj_update_layout(root_);
    CHECK_FALSE(lv_obj_has_flag(filament, LV_OBJ_FLAG_HIDDEN));
    const lv_area_t toggle_medium = drawn_area(toggle);
    const lv_area_t art = area_of(label("preview_art"));
    // Small and up: the glyph ends the row.
    CHECK(right_edge_screen(hint) == right_edge_screen(count));
    CHECK_FALSE(lv_obj_has_flag(elapsed_icon, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(elapsed, LV_OBJ_FLAG_HIDDEN));
    CHECK(right_edge_screen(elapsed) == right_edge_screen(line));
    // The times hug the right edge: a shorter finish time moves the time-left
    // clock rightward instead of leaving a gap before elapsed.
    {
        lv_obj_t* clock = label("remaining_icon");
        set_eta("(~10:06 PM)");
        lv_obj_update_layout(root_);
        const int32_t clock_long = left_edge_screen(clock);
        set_eta("(~1 PM)");
        lv_obj_update_layout(root_);
        CAPTURE(clock_long, left_edge_screen(clock));
        CHECK(left_edge_screen(clock) > clock_long);
        CHECK(right_edge_screen(elapsed) == right_edge_screen(line));
        // A finish time that fits shows whole, after it shrinks and after it
        // grows again: no dots while the line has room.
        lv_obj_t* measure = label("print_eta_measure");
        CHECK(lv_obj_get_width(eta) == lv_obj_get_width(measure));
        set_eta("(~10:06 PM)");
        lv_obj_update_layout(root_);
        CAPTURE(lv_obj_get_width(eta), lv_obj_get_width(measure));
        CHECK(lv_obj_get_width(eta) == lv_obj_get_width(measure));
        CHECK(left_edge_screen(clock) == clock_long);
    }
    const int32_t count_width_medium = lv_obj_get_width(count);

    for (const UiBreakpoint cramped : {UiBreakpoint::Micro, UiBreakpoint::Tiny}) {
        lv_subject_set_int(breakpoint, to_int(cramped));
        lv_obj_update_layout(root_);
        CAPTURE(to_int(cramped), count_width_medium, lv_obj_get_width(count),
                lv_obj_get_width(percent));
        CHECK(lv_obj_has_flag(filament, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(elapsed_icon, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(elapsed, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_width(count) > count_width_medium);
        // With elapsed hidden the finish time ends the line.
        CHECK(right_edge_screen(eta) == right_edge_screen(line));

        // The corner toggle draws smaller and nearer the corner, and still
        // takes taps across a touch_target_min box.
        const lv_area_t drawn = drawn_area(toggle);
        lv_area_t hit;
        lv_obj_get_click_area(toggle, &hit);
        CAPTURE(lv_area_get_width(&toggle_medium), lv_area_get_width(&drawn), drawn.x1, drawn.y1,
                toggle_medium.x1, toggle_medium.y1, lv_area_get_width(&hit),
                lv_area_get_height(&hit));
        CHECK(lv_area_get_width(&drawn) < lv_area_get_width(&toggle_medium));
        CHECK(lv_area_get_height(&drawn) < lv_area_get_height(&toggle_medium));
        CHECK(drawn.x1 - art.x1 < toggle_medium.x1 - art.x1);
        CHECK(drawn.y1 - art.y1 < toggle_medium.y1 - art.y1);
        CHECK(lv_area_get_width(&hit) >= touch_min);
        CHECK(lv_area_get_height(&hit) >= touch_min);

        // The pause glyph follows the count's text instead of ending the row.
        const int32_t gap = lv_obj_get_style_pad_column(count, LV_PART_MAIN);
        lv_point_t text;
        lv_text_get_size(&text, "9 / 240", lv_obj_get_style_text_font(layer, LV_PART_MAIN),
                         lv_obj_get_style_text_letter_space(layer, LV_PART_MAIN), 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        CAPTURE(right_edge_screen(layer), left_edge_screen(hint), gap, text.x);
        CHECK(lv_obj_get_width(layer) == text.x);
        CHECK(left_edge_screen(hint) == right_edge_screen(layer) + 1 + gap);
        CHECK(right_edge_screen(hint) < right_edge_screen(count));
    }

    // A count too long for its box ends in dots, and the glyph after it
    // stays inside the box.
    lv_subject_set_int(breakpoint, to_int(UiBreakpoint::Micro));
    const char* overflowing = "474 / 13340 · 554.4mm plus a pathological tail";
    lv_label_set_text(layer, overflowing);
    lv_obj_update_layout(root_);
    lv_point_t full;
    lv_text_get_size(&full, overflowing, lv_obj_get_style_text_font(layer, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(layer, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    CAPTURE(lv_obj_get_width(layer), full.x, right_edge_screen(hint), right_edge_screen(count));
    CHECK(lv_obj_get_width(layer) < full.x);
    CHECK(left_edge_screen(hint) > right_edge_screen(layer));
    CHECK(right_edge_screen(hint) <= right_edge_screen(count));

    lv_subject_set_int(breakpoint, to_int(UiBreakpoint::Small));
    lv_obj_update_layout(root_);
    CHECK_FALSE(lv_obj_has_flag(filament, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(elapsed, LV_OBJ_FLAG_HIDDEN));
    const lv_area_t toggle_small = drawn_area(toggle);
    CHECK(lv_area_get_width(&toggle_small) == lv_area_get_width(&toggle_medium));
    CHECK(toggle_small.x1 == toggle_medium.x1);

    lv_subject_set_int(breakpoint, original);
}

namespace {

/// Sets the named int subjects for one case and restores them on scope exit.
class SubjectValues {
  public:
    void set(const char* name, int32_t value) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        CAPTURE(name);
        REQUIRE(subject != nullptr);
        saved_.emplace_back(subject, lv_subject_get_int(subject));
        lv_subject_set_int(subject, value);
    }
    ~SubjectValues() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            lv_subject_set_int(it->first, it->second);
        }
    }

  private:
    std::vector<std::pair<lv_subject_t*, int32_t>> saved_;
};

} // namespace

TEST_CASE_METHOD(MetadataStripFixture,
                 "Strip: the filament line hides a lone loaded glyph and an unnamed lane",
                 "[print_status][metadata_strip][micro_declutter]") {
    // The filament line binds AmsState and FilamentSensorManager subjects at
    // create time, so they are brought up first and the panel rebuilt on them.
    helix::AmsState::instance().init_subjects(true);
    helix::FilamentSensorManager::instance().init_subjects();
    lv_obj_delete(root_);
    UpdateQueue::instance().drain();
    panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
    panel_->init_subjects();
    root_ = panel_->create(test_screen());

    lv_obj_t* line = label("filament_ams_status_row");
    lv_obj_t* glyph_row = label("filament_status_row");
    lv_obj_t* lane_row = label("ams_status_row");
    SubjectValues subjects;
    subjects.set("toolchange_visible", 0);

    SECTION("no AMS: a sensor reading loaded hides the line") {
        subjects.set("ams_slot_count", 0);
        subjects.set("filament_sensor_count", 1);
        subjects.set("filament_runout_scoped", 1);
        CHECK(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
    }
    SECTION("no AMS: runout and disabled stay on screen") {
        subjects.set("ams_slot_count", 0);
        subjects.set("filament_sensor_count", 1);
        for (const int32_t actionable : {0, 2}) {
            CAPTURE(actionable);
            subjects.set("filament_runout_scoped", actionable);
            CHECK_FALSE(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
            CHECK_FALSE(lv_obj_has_flag(glyph_row, LV_OBJ_FLAG_HIDDEN));
        }
    }
    SECTION("AMS: an active lane with no filament name hides the line") {
        subjects.set("ams_slot_count", 4);
        subjects.set("ams_current_tool", 0);
        subjects.set("filament_sensor_count", 1);
        subjects.set("filament_runout_scoped", 1);
        subjects.set("ams_current_has_material", 0);
        CHECK(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));

        subjects.set("ams_current_has_material", 1);
        CHECK_FALSE(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(lane_row, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(glyph_row, LV_OBJ_FLAG_HIDDEN));
    }
    SECTION("a tool change count keeps the line") {
        subjects.set("ams_slot_count", 0);
        subjects.set("filament_sensor_count", 0);
        subjects.set("toolchange_visible", 1);
        CHECK_FALSE(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
    }
    SECTION("micro and tiny hide the whole line when only the tool change count would fill it") {
        subjects.set("ams_slot_count", 0);
        subjects.set("filament_sensor_count", 1);
        subjects.set("filament_runout_scoped", 1);
        subjects.set("toolchange_visible", 1);
        lv_subject_t* breakpoint = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(breakpoint != nullptr);
        const int32_t original = lv_subject_get_int(breakpoint);

        CHECK_FALSE(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
        for (const UiBreakpoint cramped : {UiBreakpoint::Micro, UiBreakpoint::Tiny}) {
            lv_subject_set_int(breakpoint, to_int(cramped));
            CAPTURE(to_int(cramped));
            CHECK(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
        }
        lv_subject_set_int(breakpoint, original);
    }
    SECTION("micro and tiny leave the tool change count off, so the lane's name keeps the line") {
        subjects.set("ams_slot_count", 4);
        subjects.set("ams_current_tool", 0);
        subjects.set("ams_current_has_material", 1);
        subjects.set("toolchange_visible", 1);
        lv_obj_t* changes = label("toolchange_status");
        lv_subject_t* breakpoint = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(breakpoint != nullptr);
        const int32_t original = lv_subject_get_int(breakpoint);

        CHECK_FALSE(lv_obj_has_flag(changes, LV_OBJ_FLAG_HIDDEN));
        for (const UiBreakpoint cramped : {UiBreakpoint::Micro, UiBreakpoint::Tiny}) {
            lv_subject_set_int(breakpoint, to_int(cramped));
            CAPTURE(to_int(cramped));
            CHECK(lv_obj_has_flag(changes, LV_OBJ_FLAG_HIDDEN));
            CHECK_FALSE(lv_obj_has_flag(lane_row, LV_OBJ_FLAG_HIDDEN));
        }
        lv_subject_set_int(breakpoint, to_int(UiBreakpoint::Small));
        CHECK_FALSE(lv_obj_has_flag(changes, LV_OBJ_FLAG_HIDDEN));
        lv_subject_set_int(breakpoint, original);
    }
}
