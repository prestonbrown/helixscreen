// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_header_action_button.cpp
 * @brief The print-status header's Files button.
 *
 * The header carries one action: the round Files button (folder icon,
 * on_print_status_files) in its actions slot, which opens print select during
 * an active print, where starting a print stays blocked by
 * print_select_can_print (prestonbrown/helixscreen#1395). The tests drive the
 * real panel through every print state and require the button to stay visible
 * the whole way.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "core/update_queue.h"
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

    /// The Files entry: the round button in the header's actions slot.
    lv_obj_t* files_button() const {
        if (!root_) {
            return nullptr;
        }
        lv_obj_t* header = lv_obj_find_by_name(root_, "overlay_header");
        return header ? lv_obj_find_by_name(header, "btn_files") : nullptr;
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
                 "PrintStatusPanel: the Files button stays visible in every print state",
                 "[print_status][header_bar][1395]") {
    REQUIRE(root_ != nullptr);

    lv_obj_t* files = files_button();
    REQUIRE(files != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(files));

    // Every active state and the ones around them, entered more than once, so a
    // show/hide pair keyed on a transition cannot pass by restoring on the way
    // out.
    const PrintJobState sequence[] = {
        PrintJobState::STANDBY,  PrintJobState::PRINTING,  PrintJobState::PAUSED,
        PrintJobState::PRINTING, PrintJobState::COMPLETE,  PrintJobState::STANDBY,
        PrintJobState::PRINTING, PrintJobState::CANCELLED, PrintJobState::ERROR,
    };

    for (PrintJobState s : sequence) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        // The Files entry is the print-select affordance: it must stay reachable
        // for the whole print, not just the active states.
        CHECK_FALSE(lv_obj_is_hidden(files));
    }
}

TEST_CASE("print_status_panel.xml puts the Files button in the header's actions slot",
          "[print_status][header_bar][1395]") {
    // Structural companion to the behavioural test above: the Files entry is a
    // button in the actions slot naming the folder icon and the navigation
    // callback. One file serves every layout class: print_status_body carries
    // the arrangement.
    const std::string xml = read_xml("ui_xml/print_status_panel.xml");

    const auto actions = xml.find("<header_bar-actions>");
    REQUIRE(actions != std::string::npos);
    const auto actions_end = xml.find("</header_bar-actions>", actions);
    REQUIRE(actions_end != std::string::npos);
    const std::string slot = xml.substr(actions, actions_end - actions);
    CHECK(slot.find("name=\"btn_files\"") != std::string::npos);
    CHECK(slot.find("icon=\"folder\"") != std::string::npos);
    CHECK(slot.find("callback=\"on_print_status_files\"") != std::string::npos);
}

TEST_CASE_METHOD(PrintStatusHeaderFixture,
                 "PrintStatusPanel: tapping the Files button activates print select",
                 "[print_status][header_bar][1395]") {
    REQUIRE(root_ != nullptr);

    lv_obj_t* files = files_button();
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
