// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_home.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "panel_widget_manager.h"

#include <vector>

// Friend access to HomePanel internals. `ui_panel_home.h` declares
// `friend struct ::HomePanelTestAccess;` in the GLOBAL namespace, so the
// definition must live there too — and in ONE place, or two test translation
// units defining their own copy would be an ODR violation.
//
// Two ways to give the panel pages. A test of the long-press entry alone seeds
// one page container directly (set_single_page_container): the handler does
// nothing until the panel owns one. A test of the carousel installs a root
// holding a carousel_host and runs the panel's own build_carousel() on it
// (set_panel_root, build_carousel), then releases what the build created
// (release_carousel).
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
struct HomePanelTestAccess {
    /// Install a single page container and make it the active page, standing in
    /// for the carousel build.
    static void set_single_page_container(HomePanel& panel, lv_obj_t* container) {
        panel.pages_.clear();
        panel.pages_.emplace_back();
        panel.pages_.back().container = container;
        panel.active_page_index_ = 0;
    }

    /// Install one container per config page, standing in for the carousel
    /// build when a test needs a multi-page layout.
    static void set_page_containers(HomePanel& panel, const std::vector<lv_obj_t*>& containers) {
        panel.pages_.clear();
        for (lv_obj_t* c : containers) {
            panel.pages_.emplace_back();
            panel.pages_.back().container = c;
        }
        panel.active_page_index_ = 0;
    }

    /// Populate every page, as a config rebuild does.
    static void populate(HomePanel& panel) {
        panel.populate_widgets(/*force=*/true);
    }

    static void clear_page_containers(HomePanel& panel) {
        panel.pages_.clear();
        panel.active_page_index_ = 0;
    }

    /// Register the panel's real gate-observer rebuild slot, as finalize_setup
    /// does, so a test can deliver notify_widget_defs_changed() down the
    /// production async path.
    static void setup_gate_observers(HomePanel& panel) {
        panel.setup_widget_gate_observers();
    }

    static bool edit_mode_active(const HomePanel& panel) {
        return panel.grid_edit_mode_.is_active();
    }

    /// The panel's edit-mode instance, for driving it the way the XML handlers do.
    static helix::GridEditMode& grid_edit(HomePanel& panel) {
        return panel.grid_edit_mode_;
    }

    /// Install @p root as the panel's XML root, where build_carousel() finds
    /// carousel_host, and its screen as the one the panel opens overlays on and
    /// a hot-reload rebuild requires; nullptr detaches both.
    static void set_panel_root(HomePanel& panel, lv_obj_t* root) {
        panel.panel_ = root;
        panel.parent_screen_ = root ? lv_obj_get_screen(root) : nullptr;
    }

    /// The panel's XML root, which a hot-reload rebuild replaces.
    static lv_obj_t* panel_root(const HomePanel& panel) {
        return panel.panel_;
    }

    /// Release what finalize_setup() registers beside the carousel, as the
    /// panel's destructor does, and clear its finalized mark: the gate
    /// observers and the settings rebuild callback for the "home" config.
    static void release_finalize(HomePanel& panel) {
        helix::PanelWidgetManager::instance().clear_gate_observers("home");
        helix::PanelWidgetManager::instance().unregister_rebuild_callback("home");
        panel.finalized_ = false;
    }

    /// The panel's carousel build as finalize_setup() runs it, on the main
    /// page: a page container per config page, the next-page slot below the
    /// page cap, the page subject and its observer.
    static void build_carousel(HomePanel& panel) {
        panel.build_carousel(static_cast<int>(
            helix::PanelWidgetManager::instance().get_widget_config("home").main_page_index()));
    }

    /// The edit-mode page callbacks finalize_setup() wires: the rebuild that
    /// repopulates every page, and the page-deletion confirmation.
    static void wire_grid_edit_page_callbacks(HomePanel& panel) {
        panel.wire_grid_edit_page_callbacks();
    }

    /// The widget-config rebuild callback finalize_setup() registers.
    static void register_config_rebuild_callback(HomePanel& panel) {
        panel.register_config_rebuild_callback();
    }

    /// Tear the carousel down and build it again showing @p shown, as a
    /// page-set change does before it shows the edit session a page.
    static void rebuild_carousel(HomePanel& panel, int shown) {
        panel.rebuild_carousel(shown);
    }

    /// Release what build_carousel() created through the panel's own teardown,
    /// then forget the host and the page and press tracking, leaving the panel
    /// as before a build.
    static void release_carousel(HomePanel& panel) {
        panel.teardown_carousel();
        panel.carousel_host_ = nullptr;
        panel.active_page_index_ = 0;
        panel.press_point_valid_ = false;
    }

    /// The live carousel. A rebuild replaces it.
    static lv_obj_t* carousel(HomePanel& panel) {
        return panel.carousel_;
    }

    /// One container per config page, in page order.
    static std::vector<lv_obj_t*> page_containers(const HomePanel& panel) {
        std::vector<lv_obj_t*> containers;
        for (const auto& page : panel.pages_) {
            containers.push_back(page.container);
        }
        return containers;
    }

    /// The next-page slot's page container past the last page, or nullptr at
    /// the page cap.
    static lv_obj_t* next_page_container(const HomePanel& panel) {
        return panel.next_page_container_;
    }

    /// True once a PRESSED the grid handlers received has recorded where its
    /// press landed: the press handler records every press that reaches
    /// carousel_host.
    static bool press_tracked(const HomePanel& panel) {
        return panel.press_point_valid_;
    }

    /// The page the panel treats as active, as on_page_changed() last set it.
    static int active_page(const HomePanel& panel) {
        return panel.active_page_index_;
    }

    /// Confirm the delete-page dialog: the action its Delete button runs.
    static void confirm_delete_page(HomePanel& panel) {
        panel.delete_edit_page();
    }

    /// The observer-fired page change, as carousel_scroll_end_cb delivers it.
    static void fire_page_changed(HomePanel& panel, int new_page) {
        panel.on_page_changed(new_page);
    }

    /// Publish the panel-lifetime subjects (the page badge and the populated
    /// page count) again, as the constructor does.
    static void init_panel_subjects(HomePanel& panel) {
        panel.init_panel_subjects();
    }

    /// Recount the badge's populated pages from the config, as the panel does
    /// after populating, and return the count written to its subject.
    static int recount_populated_pages(HomePanel& panel) {
        panel.update_populated_pages_subject();
        return lv_subject_get_int(&panel.populated_pages_subject_);
    }

    /// The XML-registered LV_EVENT_LONG_PRESSED handler, as wired onto
    /// carousel_host_ in production.
    static lv_event_cb_t long_press_cb() {
        return &HomePanel::on_home_grid_long_press;
    }

    /// One grid handler home_panel.xml attaches to carousel_host.
    struct GridHandler {
        lv_event_code_t code;
        lv_event_cb_t cb;
    };

    /// Every grid handler home_panel.xml attaches to carousel_host, one entry
    /// per event_cb element.
    static std::vector<GridHandler> grid_handlers() {
        return {
            {LV_EVENT_PRESSED, &HomePanel::on_home_grid_pressed},
            {LV_EVENT_LONG_PRESSED, &HomePanel::on_home_grid_long_press},
            {LV_EVENT_CLICKED, &HomePanel::on_home_grid_clicked},
            {LV_EVENT_PRESSING, &HomePanel::on_home_grid_pressing},
            {LV_EVENT_RELEASED, &HomePanel::on_home_grid_released},
            {LV_EVENT_PRESS_LOST, &HomePanel::on_home_grid_press_cancelled},
            {LV_EVENT_INDEV_RESET, &HomePanel::on_home_grid_press_cancelled},
        };
    }
};
