// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_header_action_button.cpp
 * @brief The print-status header's action-button contract.
 *
 * The header carries exactly one configured action: the icon-only action
 * (folder icon, on_print_status_files) that opens print select during an active
 * print, where starting a print stays blocked by print_select_can_print
 * (prestonbrown/helixscreen#1395). The primary action button stays unconfigured.
 *
 * An unconfigured primary shown during a print is an empty primary-coloured
 * pill, so the tests drive the real panel through every print state and
 * require the primary to stay hidden while the Files button stays visible the
 * whole way.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
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
/// print_status_panel tree (header_bar, ui_card, the gcode viewer widget).
/// XMLTestFixture cannot: register_component() loads one file and
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
        if (root_ && helix::test::widget_in_tree(root_)) {
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

    /// The header's icon-only action (the Files entry).
    lv_obj_t* action_icon_button() const {
        if (!root_) {
            return nullptr;
        }
        lv_obj_t* header = lv_obj_find_by_name(root_, "overlay_header");
        return header ? lv_obj_find_by_name(header, "action_icon_button") : nullptr;
    }

    void set_print_state(PrintJobState s) {
        lv_subject_set_int(state().print_state().get_print_state_enum_subject(),
                           static_cast<int>(s));
        UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

/// Read a UI XML file whole.
std::string read_xml(const std::string& path) {
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE_METHOD(PrintStatusHeaderFixture,
                 "PrintStatusPanel: primary action button stays hidden, Files button visible",
                 "[print_status][header_bar][1204][1395]") {
    REQUIRE(root_ != nullptr);

    lv_obj_t* action = action_button();
    REQUIRE(action != nullptr);
    lv_obj_t* files = action_icon_button();
    REQUIRE(files != nullptr);

    // Baseline: header_bar's hide_action_button default, untouched; the Files
    // entry opts in via hide_action_icon="false".
    REQUIRE(lv_obj_has_flag(action, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(files, LV_OBJ_FLAG_HIDDEN));

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
        // The Files entry is the print-select affordance: it must stay reachable
        // for the whole print, not just the active states.
        CHECK_FALSE(lv_obj_has_flag(files, LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE("print_status_panel.xml passes no action-button props to header_bar",
          "[print_status][header_bar][1204][1395]") {
    // Structural companion to the behavioural test above. The PRIMARY slot stays
    // unconfigured (header_bar defaults: hidden, empty text/icon, no callback) so
    // the empty-pill regression cannot return; the icon-only action is the Files
    // entry and must name the folder icon and the navigation callback. Both
    // layout variants, since layout-class resolution picks one at runtime and the
    // unit test only exercises the base.
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

        // Primary slot bare: none of its props appear (the `_2` spellings do not
        // match these searches because a `=` or `_2` follows the prefix).
        CHECK(tag.find("action_button_icon=") == std::string::npos);
        CHECK(tag.find("action_button_text=") == std::string::npos);
        CHECK(tag.find("action_button_callback=") == std::string::npos);
        CHECK(tag.find("hide_action_button=") == std::string::npos);

        // The icon-only action fully configured as the Files entry.
        CHECK(tag.find("hide_action_icon=\"false\"") != std::string::npos);
        CHECK(tag.find("action_icon=\"folder\"") != std::string::npos);
        CHECK(tag.find("action_icon_callback=\"on_print_status_files\"") != std::string::npos);
    }
}

TEST_CASE_METHOD(PrintStatusHeaderFixture,
                 "PrintStatusPanel: tapping the Files button activates print select",
                 "[print_status][header_bar][1395]") {
    REQUIRE(root_ != nullptr);

    lv_obj_t* files = action_icon_button();
    REQUIRE(files != nullptr);

    // Pin the callback wiring: the XML name resolves, and the handler queues
    // the navbar-tap switch (LVGL event callbacks must not mutate the tree
    // inline). The drain runs it; the full overlay lifecycle is asserted by
    // the ctl walkthrough instead - here it would land on whatever stack
    // state earlier tests left.
    NavigationManager::instance().set_active(PanelId::Home);
    lv_obj_send_event(files, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(NavigationManager::instance().get_active() == PanelId::PrintSelect);
}
