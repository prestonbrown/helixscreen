// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_home.h"

#include <vector>

// Friend access to HomePanel internals. `ui_panel_home.h` declares
// `friend struct ::HomePanelTestAccess;` in the GLOBAL namespace, so the
// definition must live there too — and in ONE place, or two test translation
// units defining their own copy would be an ODR violation.
//
// Edit mode is only reachable through the long-press handler, and that handler
// only does anything when the panel already owns a page container (the real one
// is built by build_carousel() from the home_panel XML tree). Seeding the
// container list directly is what lets a test drive the real
// on_home_grid_long_press() path without standing up the whole carousel.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
struct HomePanelTestAccess {
    /// Install a single page container and make it the active page, standing in
    /// for the carousel build.
    static void set_single_page_container(HomePanel& panel, lv_obj_t* container) {
        panel.page_containers_.assign(1, container);
        panel.active_page_index_ = 0;
    }

    static void clear_page_containers(HomePanel& panel) {
        panel.page_containers_.clear();
        panel.active_page_index_ = 0;
    }

    static bool edit_mode_active(const HomePanel& panel) {
        return panel.grid_edit_mode_.is_active();
    }

    /// The XML-registered LV_EVENT_LONG_PRESSED handler, as wired onto
    /// carousel_host_ in production.
    static lv_event_cb_t long_press_cb() {
        return &HomePanel::on_home_grid_long_press;
    }
};
