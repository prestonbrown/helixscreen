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
 * The status row below has the same class of hazard: a long elapsed/remaining
 * pair once ran under the percent label on its right; the eta now grows and
 * dots.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

using helix::ui::UpdateQueue;

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>
#include <utility>

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

    void heal_global_print_status_panel_subjects() {
        auto& global = get_global_print_status_panel();
        if (!global.are_subjects_initialized()) {
            global.init_subjects();
        }
    }

    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
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
    lv_label_set_text(layer, "474 / 13340 · 554.4mm plus a pathological tail");
    lv_obj_update_layout(root_);
    REQUIRE(lv_label_get_long_mode(layer) == LV_LABEL_LONG_MODE_DOTS);
    lv_point_t full;
    lv_text_get_size(&full, lv_label_get_text(layer),
                     lv_obj_get_style_text_font(layer, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(layer, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    REQUIRE(lv_obj_get_width(layer) < full.x);
    REQUIRE(left_edge_screen(filament) >= right_edge_screen(objects));
}

TEST_CASE_METHOD(MetadataStripFixture,
                 "Metadata strip: elapsed/remaining row never runs under the percent",
                 "[print_status][metadata_strip]") {
    lv_obj_t* elapsed = label("print_elapsed");
    lv_obj_t* remaining = label("print_remaining");
    lv_obj_t* eta = label("print_eta");
    lv_obj_t* percent = label("print_percent");

    // Longer than the live values that already fit with zero headroom
    // ("16h 03m ... 8h 28m (~10:06 PM)" measured exactly 322px of a 322px
    // slot): a multi-day print pushes the eta into the percent label.
    lv_label_set_text(elapsed, "2d 16h 03m");
    lv_label_set_text(remaining, "2d 08h 26m");
    lv_label_set_text(eta, "(~10:06 PM +1d)");

    lv_obj_update_layout(root_);

    CAPTURE(right_edge_screen(eta), left_edge_screen(percent));

    // The inner time row is a grow slot left of the percent label; whatever
    // it holds, it must end before the percent begins. Screen coordinates,
    // because the two labels sit under different parents.
    REQUIRE(right_edge_screen(eta) <= left_edge_screen(percent));
}
