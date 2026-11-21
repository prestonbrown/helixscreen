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
#include "theme_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::ui::side_band_shows;
using helix::ui::SideBand;
using helix::ui::SideBandContent;
using helix::ui::SideBandGuest;

TEST_CASE("side band gate: Motion shows on ultrawide only", "[uw_side_band]") {
    CHECK(side_band_shows(SideBandContent::Motion, true, 0, false));
    CHECK(side_band_shows(SideBandContent::Motion, true, 4, true));
    CHECK_FALSE(side_band_shows(SideBandContent::Motion, false, 0, false));
    CHECK_FALSE(side_band_shows(SideBandContent::Motion, false, 4, true));
}

TEST_CASE("side band gate: AMS needs ultrawide and slots or several heads", "[uw_side_band]") {
    CHECK(side_band_shows(SideBandContent::Ams, true, 4, false));
    CHECK(side_band_shows(SideBandContent::Ams, true, 1, false));
    CHECK(side_band_shows(SideBandContent::Ams, true, 0, true));
    CHECK(side_band_shows(SideBandContent::Ams, true, 4, true));
    CHECK_FALSE(side_band_shows(SideBandContent::Ams, true, 0, false));
    CHECK_FALSE(side_band_shows(SideBandContent::Ams, false, 4, false));
    CHECK_FALSE(side_band_shows(SideBandContent::Ams, false, 0, true));
}

namespace {

class ScopedUltrawide {
  public:
    explicit ScopedUltrawide(int value) : subject_(lv_xml_get_subject(nullptr, "ui_is_ultrawide")) {
        REQUIRE(subject_ != nullptr);
        original_ = lv_subject_get_int(subject_);
        lv_subject_set_int(subject_, value);
    }
    ~ScopedUltrawide() {
        lv_subject_set_int(subject_, original_);
    }
    void set(int value) {
        lv_subject_set_int(subject_, value);
    }

  private:
    lv_subject_t* subject_;
    int original_ = 0;
};

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

/// A host tree shaped like controls_panel.xml: a row holding side_band_main
/// and side_band.
lv_obj_t* make_host(lv_obj_t* parent) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_t* main = lv_obj_create(root);
    lv_obj_set_name(main, "side_band_main");
    lv_obj_set_size(main, lv_pct(100), lv_pct(100));
    lv_obj_t* band = lv_obj_create(root);
    lv_obj_set_name(band, "side_band");
    lv_obj_set_height(band, lv_pct(100));
    lv_obj_set_flex_grow(band, 1);
    return root;
}

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
        host = make_host(lv_screen_active());
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

// The real Motion panel: its one tree lives in the band, and the overlay path
// never builds a second while the host shows it.
TEST_CASE_METHOD(LVGLUITestFixture, "side band: Controls hosts the Motion panel's own tree",
                 "[uw_side_band][motion]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    ScopedUltrawide ultrawide(1);
    SideBand band{SideBandContent::Motion, helix::ui::make_motion_side_band_guest(),
                  "test_controls_side_band", "motion_in_side_band"};
    band.init_subjects();
    lv_obj_t* host = make_host(lv_screen_active());
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
