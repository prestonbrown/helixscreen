// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_side_band.cpp
 * @brief The ultrawide side band: Motion beside Controls, AMS beside Filament
 *
 * Run with: ./build/bin/helix-tests "[uw_side_band]"
 *
 * The gate decides when a host's band shows its content. SideBand hosts one
 * singleton view's tree in a host panel's band and shares it with the overlay
 * path, so there is never a second tree.
 */

#include "ui_nav_manager.h"
#include "ui_panel_ams_overview.h"
#include "ui_panel_motion.h"
#include "ui_side_band.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/side_band_test_host.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::test::ScopedUltrawide;
using helix::ui::side_band_shows;
using helix::ui::SideBand;
using helix::ui::SideBandContent;
using helix::ui::SideBandGuest;

TEST_CASE("side band gate: Motion shows on ultrawide only", "[uw_side_band]") {
    CHECK(side_band_shows(SideBandContent::Motion, true, false));
    CHECK(side_band_shows(SideBandContent::Motion, true, true));
    CHECK_FALSE(side_band_shows(SideBandContent::Motion, false, false));
    CHECK_FALSE(side_band_shows(SideBandContent::Motion, false, true));
}

TEST_CASE("side band gate: AMS needs ultrawide and a lane tray", "[uw_side_band]") {
    CHECK(side_band_shows(SideBandContent::Ams, true, true));
    CHECK_FALSE(side_band_shows(SideBandContent::Ams, true, false));
    CHECK_FALSE(side_band_shows(SideBandContent::Ams, false, true));
}

namespace {

/// A guest that records what the band asks of it.
struct FakeGuestLog {
    int attaches = 0;
    int detaches = 0;
    bool refuse = false; ///< attach() answers "open elsewhere"
    bool stale = false;  ///< stale() answers "rebuild me"
    int host_left = 0;   ///< on_host_left() calls
    lv_obj_t* root = nullptr;
};

class FakeLifecycle : public IPanelLifecycle {
  public:
    int activations = 0;
    int deactivations = 0;
    void on_activate() override {
        ++activations;
    }
    void on_deactivate(DeactivateReason) override {
        ++deactivations;
    }
    const char* get_name() const override {
        return "FakeGuest";
    }
};

class FakeGuest : public SideBandGuest {
  public:
    FakeGuest(FakeGuestLog& log, FakeLifecycle& view) : log_(log), view_(view) {}
    lv_obj_t* attach(lv_obj_t* band) override {
        if (log_.refuse) {
            return nullptr;
        }
        ++log_.attaches;
        log_.root = lv_obj_create(band);
        return log_.root;
    }
    void detach() override {
        ++log_.detaches;
        if (log_.root) {
            // Moved out first, as the real guests' teardown does: the band may
            // be mid-deletion.
            lv_obj_set_parent(log_.root, lv_layer_top());
            lv_obj_delete_async(log_.root);
            log_.root = nullptr;
        }
    }
    IPanelLifecycle* lifecycle() override {
        return &view_;
    }
    bool stale() override {
        return log_.stale;
    }
    void on_host_left() override {
        ++log_.host_left;
    }

  private:
    FakeGuestLog& log_;
    FakeLifecycle& view_;
};

int subject_value(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

struct FakeBand {
    FakeGuestLog log;
    FakeLifecycle view;
    SideBand band{SideBandContent::Motion, std::make_unique<FakeGuest>(log, view), "test_side_band",
                  "test_in_side_band"};
    lv_obj_t* host = nullptr;

    FakeBand() {
        band.init_subjects();
        host = helix::test::make_side_band_host(lv_screen_active());
        band.bind(host);
    }
    ~FakeBand() {
        band.deinit_subjects();
    }
    lv_obj_t* band_obj() const {
        return lv_obj_find_by_name(host, "side_band");
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "side band: off ultrawide the host is untouched",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(0);
    FakeBand fx;
    fx.band.on_host_activate();

    CHECK(fx.log.attaches == 0);
    CHECK(fx.band.hosted() == nullptr);
    CHECK(subject_value("test_side_band") == 0);
    CHECK_FALSE(fx.band.uses_full_width());
    lv_obj_t* main = lv_obj_find_by_name(fx.host, "side_band_main");
    CHECK(lv_obj_get_style_width(main, LV_PART_MAIN) == lv_pct(100));
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: ultrawide hosts one tree in the band",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();

    REQUIRE(fx.log.attaches == 1);
    CHECK(fx.band.hosted() == fx.log.root);
    CHECK(lv_obj_get_parent(fx.log.root) == fx.band_obj());
    CHECK(fx.view.activations == 1);
    CHECK(subject_value("test_side_band") == 1);
    CHECK(subject_value("test_in_side_band") == 1);
    CHECK(fx.band.uses_full_width());
    // The main column stops where the panel cap would have stopped the panel.
    lv_obj_t* main = lv_obj_find_by_name(fx.host, "side_band_main");
    CHECK(lv_obj_get_style_width(main, LV_PART_MAIN) == live_content_max_width(fx.host));

    // Leaving and coming back reuses the tree: no second one is built.
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    CHECK(fx.view.deactivations == 1);
    fx.band.on_host_activate();
    CHECK(fx.log.attaches == 1);
    CHECK(fx.view.activations == 2);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: an open request while the host shows it is a no-op",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() != nullptr);

    CHECK(SideBand::take_open_request(SideBandContent::Motion));
    CHECK(fx.log.detaches == 0);
    CHECK(fx.band.hosted() != nullptr);
    // Another content's request is not this band's business.
    CHECK_FALSE(SideBand::take_open_request(SideBandContent::Ams));
    CHECK(fx.log.detaches == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: an open request elsewhere takes the tree back",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);

    CHECK_FALSE(SideBand::take_open_request(SideBandContent::Motion));
    CHECK(fx.log.detaches == 1);
    CHECK(fx.band.hosted() == nullptr);
    CHECK(subject_value("test_side_band") == 0);
    CHECK(subject_value("test_in_side_band") == 0);

    // The overlay holds the tree now: the band waits instead of building a second.
    fx.log.refuse = true;
    fx.band.on_host_activate();
    CHECK(fx.log.attaches == 1);
    CHECK(subject_value("test_side_band") == 0);

    // Released: the band builds it again.
    fx.log.refuse = false;
    fx.band.refresh();
    CHECK(fx.log.attaches == 2);
    CHECK(subject_value("test_side_band") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: leaving ultrawide hands the tree back",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() != nullptr);

    ultrawide.set(0);
    fx.band.refresh();
    CHECK(fx.log.detaches == 1);
    CHECK(fx.view.deactivations == 1);
    CHECK(fx.band.hosted() == nullptr);
    CHECK(subject_value("test_side_band") == 0);
    lv_obj_t* main = lv_obj_find_by_name(fx.host, "side_band_main");
    CHECK(lv_obj_get_style_width(main, LV_PART_MAIN) == lv_pct(100));
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: a host rebuild hands the tree back first",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() != nullptr);

    fx.band.on_host_deactivate(DeactivateReason::Rebuild);
    CHECK(fx.log.detaches == 1);
    CHECK(fx.band.hosted() == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: deleting the host hands the tree back",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() != nullptr);

    lv_obj_delete(fx.host);
    fx.host = nullptr;
    CHECK(fx.log.detaches == 1);
}

TEST_CASE("side band AMS view: the overlay path's choice, by unit count", "[uw_side_band][ams]") {
    using helix::ui::ams_view_for;
    using helix::ui::AmsView;
    AmsSystemInfo info;
    info.units.resize(1);
    CHECK(ams_view_for(info) == AmsView::Detail);
    info.units.resize(2);
    CHECK(ams_view_for(info) == AmsView::Overview);
    info.units.resize(4);
    CHECK(ams_view_for(info) == AmsView::Overview);
    info.units.clear();
    CHECK(ams_view_for(info) == AmsView::Detail);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: a theme change rebuilds the hosted tree",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.log.attaches == 1);
    lv_obj_t* first = fx.log.root;

    lv_subject_t* theme = theme_manager_get_changed_subject();
    REQUIRE(theme != nullptr);
    // Unchanged generation: nothing to rebuild.
    fx.band.refresh();
    CHECK(fx.log.attaches == 1);

    lv_subject_set_int(theme, lv_subject_get_int(theme) + 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.detaches == 1);
    CHECK(fx.log.attaches == 2);
    CHECK(fx.band.hosted() == fx.log.root);
    CHECK(fx.log.root != first);
    CHECK(fx.view.activations == 2);
    CHECK(subject_value("test_side_band") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: a stale guest is rebuilt, now or on return",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.log.attaches == 1);

    fx.log.stale = true;
    fx.band.refresh();
    fx.log.stale = false;
    CHECK(fx.log.detaches == 1);
    CHECK(fx.log.attaches == 2);

    // Off screen: handed back now, built again when the host returns.
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    fx.log.stale = true;
    fx.band.refresh();
    fx.log.stale = false;
    CHECK(fx.log.detaches == 2);
    CHECK(fx.band.hosted() == nullptr);
    fx.band.on_host_activate();
    CHECK(fx.log.attaches == 3);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "side band: leaving the host resets the guest, a suspend does not",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() != nullptr);

    fx.band.on_host_deactivate(DeactivateReason::Suspended);
    CHECK(fx.log.host_left == 0);
    fx.band.on_host_activate();
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    CHECK(fx.log.host_left == 1);
    // Nothing hosted (the tree is open as an overlay): nothing to reset.
    CHECK_FALSE(SideBand::take_open_request(SideBandContent::Motion));
    fx.log.refuse = true;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() == nullptr);
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    CHECK(fx.log.host_left == 1);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "side band: an overlay over the host keeps the guest's place, a panel switch does not",
    "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    auto& nav = NavigationManager::instance();
    lv_subject_t* active = nav.get_active_panel_subject();
    const int active_before = lv_subject_get_int(active);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.band.hosted() != nullptr);

    // An overlay covers the host and closes again: the place is kept.
    NavigationManagerTestAccess::set_main_panel_covered(nav, true);
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.host_left == 0);
    NavigationManagerTestAccess::set_main_panel_covered(nav, false);
    fx.band.on_host_activate();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.host_left == 0);

    // Covered, then the user switches main panels from under the overlay.
    NavigationManagerTestAccess::set_main_panel_covered(nav, true);
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    lv_subject_set_int(active, active_before + 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.host_left == 1);
    NavigationManagerTestAccess::set_main_panel_covered(nav, false);

    // A plain switch away resets at once.
    fx.band.on_host_activate();
    fx.band.on_host_deactivate(DeactivateReason::NavigateAway);
    CHECK(fx.log.host_left == 2);
    lv_subject_set_int(active, active_before);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.host_left == 2);
}

namespace {
int g_hosted_raises = 0;
void count_hosted_raise(lv_observer_t*, lv_subject_t* s) {
    if (lv_subject_get_int(s) == 1) {
        ++g_hosted_raises;
    }
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "side band: a failed attach never raises the hosted subject",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.log.refuse = true;
    g_hosted_raises = 0;
    lv_subject_t* hosted = lv_xml_get_subject(nullptr, "test_in_side_band");
    REQUIRE(hosted != nullptr);
    lv_observer_t* obs = lv_subject_add_observer(hosted, count_hosted_raise, nullptr);
    fx.band.on_host_activate();
    fx.band.refresh();
    fx.band.refresh();
    REQUIRE(fx.band.hosted() == nullptr);
    CHECK(g_hosted_raises == 0);
    fx.log.refuse = false;
    fx.band.refresh();
    CHECK(g_hosted_raises == 1);
    lv_observer_remove(obs);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: after deinit a theme change touches nothing",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    REQUIRE(fx.log.attaches == 1);
    fx.band.deinit_subjects();
    lv_subject_t* theme = theme_manager_get_changed_subject();
    lv_subject_set_int(theme, lv_subject_get_int(theme) + 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.attaches == 1);
    CHECK(fx.log.detaches == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "side band: a tree a cache pulls out is rebuilt, not kept",
                 "[uw_side_band]") {
    ScopedUltrawide ultrawide(1);
    FakeBand fx;
    fx.band.on_host_activate();
    lv_obj_t* first = fx.band.hosted();
    REQUIRE(first != nullptr);

    // What a cache's DetachSubtree does: out of the band now, freed later.
    lv_obj_set_parent(first, lv_layer_top());
    fx.log.root = nullptr;
    SideBand::notify_released(SideBandContent::Motion);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.log.attaches == 2);
    CHECK(fx.band.hosted() != first);
    CHECK(lv_obj_get_parent(fx.band.hosted()) == fx.band_obj());
    CHECK(subject_value("test_side_band") == 1);
    lv_obj_delete(first);
}

// The real Motion panel: its one tree lives in the band, and the overlay path
// never builds a second while the host shows it.
TEST_CASE_METHOD(LVGLUITestFixture, "side band: Controls hosts the Motion panel's own tree",
                 "[uw_side_band][motion]") {
    // MotionPanel is a process-wide singleton whose observers outlive this case;
    // its tree dies with the fixture's screen. Destroyed last (declared first),
    // after the band and the layout subject restore, as the other Motion
    // fixtures do, so no later case drives it.
    struct DestroyPanelsOnExit {
        ~DestroyPanelsOnExit() {
            helix::ui::UpdateQueue::instance().drain();
            StaticPanelRegistry::instance().destroy_all();
            helix::ui::UpdateQueue::instance().drain();
        }
    } destroy_panels_on_exit;

    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    ScopedUltrawide ultrawide(1);
    SideBand band{SideBandContent::Motion, helix::ui::make_motion_side_band_guest(),
                  "test_controls_side_band", "motion_in_side_band"};
    band.init_subjects();
    lv_obj_t* host = helix::test::make_side_band_host(lv_screen_active());
    band.bind(host);
    band.on_host_activate();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* root = get_global_motion_panel().get_root();
    REQUIRE(root != nullptr);
    CHECK(band.hosted() == root);
    CHECK(lv_obj_get_parent(root) == lv_obj_find_by_name(host, "side_band"));
    CHECK_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    CHECK(get_global_motion_panel().is_visible());

    // Opening Motion while Controls shows it: nothing new, nothing pushed.
    open_motion_panel(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(get_global_motion_panel().get_root() == root);
    CHECK_FALSE(helix::nav::is_in_stack(root));

    // From elsewhere: the band gives the tree up, and the overlay's is the only one.
    band.on_host_deactivate(DeactivateReason::NavigateAway);
    CHECK_FALSE(get_global_motion_panel().is_visible());
    open_motion_panel(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* overlay_root = get_global_motion_panel().get_root();
    REQUIRE(overlay_root != nullptr);
    CHECK(overlay_root != root);
    CHECK(band.hosted() == nullptr);
    CHECK(lv_obj_get_parent(overlay_root) != lv_obj_find_by_name(host, "side_band"));

    get_global_motion_panel().close();
    helix::ui::UpdateQueue::instance().drain();
    band.deinit_subjects();
}
