// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_about_update_channel_gate.cpp
 * @brief Which Update Channel picker the About overlay shows, and what it reads.
 *
 * A dropdown's options are a static string, so about_settings_overlay.xml carries
 * two channel rows with complementary conditions: Stable/Beta for every install,
 * Stable/Beta/Dev only with beta features unlocked. Index 1 is Beta in both lists,
 * so the shared callback and the stored /update/channel mean the same thing
 * whichever row is visible.
 *
 * Both rows are also gated on show_update_settings, because a firmware-managed
 * install (HELIX_DISABLE_AUTO_UPDATES) has nothing to pick between: picking a
 * channel there persists a setting nothing reads, since on_channel_changed()
 * calls check_for_updates(), which short-circuits on the same predicate.
 *
 * The subjects are driven directly rather than through the environment because
 * updates_externally_managed() caches getenv() for the life of the process. What
 * needs pinning is the XML binding's reaction to the two subject values, and that
 * is exactly what these cases exercise.
 */

#include "ui_panel_settings.h"
#include "ui_settings_about.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>

#include <string>

#include "../catch_amalgamated.hpp"

using helix::settings::AboutSettingsOverlay;
using helix::ui::UpdateQueue;

namespace {

/// Builds the real About overlay from production XML.
///
/// SettingsPanel::init_subjects() is what registers show_update_settings and
/// updates_firmware_managed; AboutSettingsOverlay::init_subjects() registers the
/// version/channel subjects the same tree binds. Both must run before
/// lv_xml_create() or the bindings resolve to nothing and the tree under
/// assertion is not the production one.
struct AboutUpdateGateFixture : public LVGLUITestFixture {
    AboutUpdateGateFixture() {
        // The test binary links a STUB app_globals_init_subjects()
        // (tests/ui_test_utils.cpp) that registers only the notification / edit-mode
        // / wizard / host-power subjects — show_beta_features is absent, so half
        // this tree's condition would resolve to nothing and the container would
        // never hide. Supply it the way the stub supplies platform_host_power_supported:
        // register if absent, function-local static so it outlives the fixture.
        static lv_subject_t beta_subject;
        if (!lv_xml_get_subject(nullptr, "show_beta_features")) {
            lv_subject_init_int(&beta_subject, 1);
            lv_xml_register_subject(nullptr, "show_beta_features", &beta_subject);
        }
        get_global_settings_panel().init_subjects();
        helix::settings::get_about_settings_overlay().init_subjects();
        root_ =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "about_settings_overlay", nullptr));
    }

    ~AboutUpdateGateFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        UpdateQueue::instance().drain();
    }

    /// Drive both gate subjects, then let the observers land.
    void set_gates(int beta, int update_settings) {
        lv_subject_t* b = lv_xml_get_subject(nullptr, "show_beta_features");
        lv_subject_t* u = lv_xml_get_subject(nullptr, "show_update_settings");
        REQUIRE(b != nullptr);
        REQUIRE(u != nullptr);
        lv_subject_set_int(b, beta);
        lv_subject_set_int(u, update_settings);
        process_lvgl(10);
    }

    bool hidden(const char* name) const {
        REQUIRE(root_ != nullptr);
        lv_obj_t* obj = lv_obj_find_by_name(root_, name);
        REQUIRE(obj != nullptr);
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* dropdown(const char* row_name) const {
        REQUIRE(root_ != nullptr);
        lv_obj_t* row = lv_obj_find_by_name(root_, row_name);
        REQUIRE(row != nullptr);
        lv_obj_t* dd = lv_obj_find_by_name(row, "dropdown");
        REQUIRE(dd != nullptr);
        return dd;
    }

    /// The option label at `index`, read back off the widget rather than the XML.
    static std::string option_at(lv_obj_t* dd, uint32_t index) {
        lv_dropdown_set_selected(dd, index);
        char buf[32] = {};
        lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
        return std::string(buf);
    }

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(AboutUpdateGateFixture, "About overlay update-surface visibility gates",
                 "[settings][about][update][channel]") {
    REQUIRE(root_ != nullptr);

    SECTION("a stock install gets the Stable/Beta picker") {
        // The whole point of the split: with beta locked there is still a channel
        // control, so a stable-line user can move to beta without the 7-tap egg.
        set_gates(/*beta=*/0, /*update_settings=*/1);
        CHECK_FALSE(hidden("container_update_channel"));
        CHECK(hidden("container_update_channel_dev"));
        CHECK_FALSE(hidden("container_check_updates"));
    }

    SECTION("beta unlocked swaps in the picker that carries Dev") {
        set_gates(/*beta=*/1, /*update_settings=*/1);
        CHECK(hidden("container_update_channel"));
        CHECK_FALSE(hidden("container_update_channel_dev"));
        CHECK_FALSE(hidden("container_check_updates"));
    }

    SECTION("firmware-managed hides both channel pickers") {
        // Checking is suppressed, so neither picker has anything to pick between —
        // they must go with the rest of the update surface.
        set_gates(/*beta=*/1, /*update_settings=*/0);
        CHECK(hidden("container_update_channel"));
        CHECK(hidden("container_update_channel_dev"));
        CHECK(hidden("container_check_updates"));
    }

    SECTION("firmware-managed with beta locked hides both channel pickers") {
        set_gates(/*beta=*/0, /*update_settings=*/0);
        CHECK(hidden("container_update_channel"));
        CHECK(hidden("container_update_channel_dev"));
        CHECK(hidden("container_check_updates"));
    }

    SECTION("exactly one picker is ever on screen") {
        for (int beta = 0; beta <= 1; ++beta) {
            set_gates(beta, /*update_settings=*/1);
            CHECK(hidden("container_update_channel") != hidden("container_update_channel_dev"));
        }
    }
}

TEST_CASE_METHOD(AboutUpdateGateFixture, "Update channel pickers agree on what an index means",
                 "[settings][about][update][channel]") {
    REQUIRE(root_ != nullptr);

    lv_obj_t* basic = dropdown("row_update_channel");
    lv_obj_t* with_dev = dropdown("row_update_channel_dev");

    SECTION("the stock picker offers Stable and Beta only") {
        CHECK(lv_dropdown_get_option_count(basic) == 2);
    }

    SECTION("the beta picker adds Dev as a third entry") {
        CHECK(lv_dropdown_get_option_count(with_dev) == 3);
    }

    SECTION("index 0 and index 1 mean the same thing in both") {
        // /update/channel is an integer index shared by both rows and by the one
        // callback behind them, so a disagreement here silently moves a user's
        // channel when the pickers swap.
        CHECK(option_at(basic, 0) == option_at(with_dev, 0));
        CHECK(option_at(basic, 1) == option_at(with_dev, 1));
        CHECK(option_at(with_dev, 1) == "Beta");
    }
}

TEST_CASE_METHOD(AboutUpdateGateFixture, "Update channel picker shows the channel it is given",
                 "[settings][about][update][channel]") {
    REQUIRE(root_ != nullptr);

    // The channel arrives as a parameter, so these pin the widget rule on its own.
    // That the value is UpdateChecker's effective channel rather than the stored
    // /update/channel is pinned separately, in test_update_channel_beta_gate.cpp.

    SECTION("Beta shows as Beta on the stock picker") {
        set_gates(/*beta=*/0, /*update_settings=*/1);
        AboutSettingsOverlay::sync_update_channel_rows(root_, 1);
        CHECK(lv_dropdown_get_selected(dropdown("row_update_channel")) == 1);
    }

    SECTION("Stable shows as Stable on the stock picker") {
        set_gates(/*beta=*/0, /*update_settings=*/1);
        AboutSettingsOverlay::sync_update_channel_rows(root_, 0);
        CHECK(lv_dropdown_get_selected(dropdown("row_update_channel")) == 0);
    }

    SECTION("Dev never renders as Beta on the two-entry picker") {
        // LVGL clamps an out-of-range selection to the last option, so handing
        // Dev's index 2 to the stock picker would read Beta — a channel the user
        // did not choose. A row too short for the value is left alone instead.
        set_gates(/*beta=*/1, /*update_settings=*/1);
        lv_obj_t* basic = dropdown("row_update_channel");
        lv_dropdown_set_selected(basic, 0);
        AboutSettingsOverlay::sync_update_channel_rows(root_, 2);
        CHECK(lv_dropdown_get_selected(basic) == 0);
    }

    SECTION("Dev shows as Dev on the beta picker") {
        set_gates(/*beta=*/1, /*update_settings=*/1);
        AboutSettingsOverlay::sync_update_channel_rows(root_, 2);
        CHECK(lv_dropdown_get_selected(dropdown("row_update_channel_dev")) == 2);
    }

    SECTION("both pickers move together so a swap shows no stale value") {
        set_gates(/*beta=*/0, /*update_settings=*/1);
        AboutSettingsOverlay::sync_update_channel_rows(root_, 1);
        CHECK(lv_dropdown_get_selected(dropdown("row_update_channel")) == 1);
        CHECK(lv_dropdown_get_selected(dropdown("row_update_channel_dev")) == 1);
    }
}
