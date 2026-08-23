// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_about_update_channel_gate.cpp
 * @brief The Update Channel dropdown must vanish wherever update checking does.
 *
 * about_settings_overlay.xml gates every update surface on show_update_settings
 * (= !update_checks_suppressed()), EXCEPT that the channel dropdown used to be
 * gated on show_beta_features alone. On a firmware-managed install
 * (HELIX_DISABLE_AUTO_UPDATES) that left a live-looking Stable/Beta/Dev picker
 * sitting directly above the "Managed by your firmware" notice, with
 * "Check for Updates" and "Install Update" both correctly gone. Picking a
 * channel there persists a setting nothing reads: on_channel_changed() calls
 * check_for_updates(), which short-circuits on the same predicate.
 *
 * The subjects are driven directly rather than through the environment because
 * updates_externally_managed() caches getenv() for the life of the process. What
 * needs pinning is the XML binding's reaction to the two subject values, and that
 * is exactly what these cases exercise.
 *
 * Reverting the binding to beta-only fails "firmware-managed hides the channel
 * dropdown" while every other case still passes — which is precisely how the
 * original escaped review.
 */

#include "ui_panel_settings.h"
#include "ui_settings_about.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

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

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(AboutUpdateGateFixture, "About overlay update-surface visibility gates",
                 "[settings][about][update][channel]") {
    REQUIRE(root_ != nullptr);

    SECTION("normal install with beta unlocked shows the channel dropdown") {
        set_gates(/*beta=*/1, /*update_settings=*/1);
        CHECK_FALSE(hidden("container_update_channel"));
        CHECK_FALSE(hidden("container_check_updates"));
    }

    SECTION("firmware-managed hides the channel dropdown") {
        // The regression. Checking is suppressed, so the picker has nothing to
        // pick between — it must go with the rest of the update surface.
        set_gates(/*beta=*/1, /*update_settings=*/0);
        CHECK(hidden("container_update_channel"));
        CHECK(hidden("container_check_updates"));
    }

    SECTION("beta locked still hides the channel dropdown on a normal install") {
        // Pre-existing behaviour, unchanged: the dropdown is beta-only, and
        // UpdateChecker::get_channel() clamps to Stable to match.
        set_gates(/*beta=*/0, /*update_settings=*/1);
        CHECK(hidden("container_update_channel"));
        CHECK_FALSE(hidden("container_check_updates"));
    }

    SECTION("beta locked AND firmware-managed hides both") {
        set_gates(/*beta=*/0, /*update_settings=*/0);
        CHECK(hidden("container_update_channel"));
        CHECK(hidden("container_check_updates"));
    }
}
