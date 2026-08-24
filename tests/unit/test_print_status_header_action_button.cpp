// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_header_action_button.cpp
 * @brief The print-status header must never reveal an unconfigured action button.
 *
 * print_status_panel.xml instantiates header_bar with NO action-button props, so
 * header_bar's defaults apply: hide_action_button="true", empty text, empty icon,
 * no callback. The e-stop that used to live there is now the estop_fab at the
 * panel root, bound to the estop_visible subject
 * (prestonbrown/helixscreen#1204).
 *
 * The regression this pins: on_print_state_changed() cleared the action button's
 * HIDDEN flag for Preparing/Printing/Paused, left over from when that button WAS
 * the e-stop. With the button no longer configured, that revealed an empty
 * primary-coloured pill in the top-right of the header for the whole print.
 * Nothing in XML could put it back, because clearing the flag from C++ reaches
 * past the binding. The header_bar helpers that did it are gone; the button's
 * visibility is a header_bar prop now and nothing else.
 *
 * So the assertion is behavioural, not structural: drive the real panel through
 * every print state and require the header's action_button to stay hidden the
 * whole way. Any C++ that un-hides it again fails here.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <fstream>
#include <lvgl.h>
#include <memory>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::ui::UpdateQueue;

namespace {

/// Owns a real PrintStatusPanel built from production XML.
///
/// LVGLUITestFixture registers every production XML component and the event
/// callbacks, which is what lets lv_xml_create() resolve the whole
/// print_status_panel tree (header_bar, ui_card, the gcode viewer widget, the
/// estop_fab). XMLTestFixture cannot: register_component() loads one file and
/// resolves no dependencies.
struct PrintStatusHeaderFixture : public LVGLUITestFixture {
    PrintStatusHeaderFixture() {
        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        // Subjects before lv_xml_create(), or the panel's own bindings resolve
        // to nothing and the tree we assert on is not the production one.
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
    }

    ~PrintStatusHeaderFixture() override {
        // Widgets first: the XML bindings observe subjects the panel owns, and
        // ~PrintStatusPanel calls deinit_subjects().
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
    }

    /// The header's action button, or nullptr if the tree did not build.
    lv_obj_t* action_button() const {
        if (!root_) {
            return nullptr;
        }
        lv_obj_t* header = lv_obj_find_by_name(root_, "overlay_header");
        return header ? lv_obj_find_by_name(header, "action_button") : nullptr;
    }

    void set_print_state(PrintJobState s) {
        lv_subject_set_int(state().get_print_state_enum_subject(), static_cast<int>(s));
        UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

/// Read a UI XML file whole. Mirrors the text-pinning case in
/// test_header_bar_estop_slot.cpp.
std::string read_xml(const std::string& path) {
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE_METHOD(PrintStatusHeaderFixture,
                 "PrintStatusPanel: header action button stays hidden through every print state",
                 "[print_status][header_bar][1204]") {
    REQUIRE(root_ != nullptr);

    lv_obj_t* action = action_button();
    REQUIRE(action != nullptr);

    // Baseline: header_bar's hide_action_button default, untouched.
    REQUIRE(lv_obj_has_flag(action, LV_OBJ_FLAG_HIDDEN));

    // The three states the removed code un-hid the button for, plus the ones
    // around them so a re-added show/hide pair cannot pass by hiding on the way
    // out. STANDBY twice on purpose: the transition into an active state is
    // what fired the old show call.
    const PrintJobState sequence[] = {
        PrintJobState::STANDBY,  PrintJobState::PRINTING,  PrintJobState::PAUSED,
        PrintJobState::PRINTING, PrintJobState::COMPLETE,  PrintJobState::STANDBY,
        PrintJobState::PRINTING, PrintJobState::CANCELLED, PrintJobState::ERROR,
    };

    for (PrintJobState s : sequence) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        // An unconfigured button has no text, no icon and no callback. Revealing
        // it paints an empty #primary pill over the header.
        CHECK(lv_obj_has_flag(action, LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE_METHOD(PrintStatusHeaderFixture,
                 "PrintStatusPanel: estop_fab at the panel root is what estop_visible drives",
                 "[print_status][header_bar][1204]") {
    REQUIRE(root_ != nullptr);

    // The FAB is a direct child of the panel root, not of the header - that is
    // what keeps it from clipping against header_height.
    lv_obj_t* fab = lv_obj_find_by_name(root_, "estop_fab");
    REQUIRE(fab != nullptr);
    REQUIRE(lv_obj_get_parent(fab) == root_);

    lv_subject_t* visible = lv_xml_get_subject(nullptr, "estop_visible");
    REQUIRE(visible != nullptr);

    lv_subject_set_int(visible, 0);
    UpdateQueue::instance().drain();
    REQUIRE(lv_obj_has_flag(fab, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(visible, 1);
    UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(fab, LV_OBJ_FLAG_HIDDEN));

    // The e-stop showing must not drag the header's action button along with it.
    lv_obj_t* action = action_button();
    REQUIRE(action != nullptr);
    CHECK(lv_obj_has_flag(action, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(visible, 0);
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_flag(fab, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE("print_status_panel.xml passes no action-button props to header_bar",
          "[print_status][header_bar][1204]") {
    // Structural companion to the behavioural test above. If someone gives this
    // panel a real header action button one day, they have to come here and
    // decide what the C++ show/hide contract should be, instead of silently
    // re-creating the empty pill. Both layout variants, since layout-class
    // resolution picks one at runtime and the unit test only exercises the base.
    const std::string files[] = {"ui_xml/print_status_panel.xml",
                                 "ui_xml/portrait/print_status_panel.xml"};

    for (const std::string& path : files) {
        const std::string xml = read_xml(path);
        CAPTURE(path);

        const auto tag_start = xml.find("<header_bar");
        REQUIRE(tag_start != std::string::npos);
        const auto tag_end = xml.find('>', tag_start);
        REQUIRE(tag_end != std::string::npos);
        const std::string tag = xml.substr(tag_start, tag_end - tag_start);

        // No action-button configuration at all: header_bar's defaults
        // (hide_action_button="true", empty text/icon, no callback) must stand.
        CHECK(tag.find("action_button") == std::string::npos);
        CHECK(tag.find("hide_action_button") == std::string::npos);

        // And the panel keeps its own root-level FAB rather than a header button.
        CHECK(xml.find("name=\"estop_fab\"") != std::string::npos);
    }
}
