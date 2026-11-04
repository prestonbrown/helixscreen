// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_view_lifecycle_contract.cpp
 * @brief The deactivation contract PanelBase and OverlayBase share.
 *
 * ViewLifecycleBase owns both lifetime guards and the order they are touched in,
 * so these cases pin what a subclass is entitled to assume:
 *
 * - a view that overrides nothing still gets its screen guard invalidated;
 * - on_deactivating() runs BEFORE that invalidation, so work the hook cancels
 *   by hand is still live when it does so;
 * - object_lifetime_ outlives deactivation and dies at cleanup();
 * - the reason arrives as a parameter, and NavigationManager and the two
 *   rebuild() paths each send the right one.
 *
 * prestonbrown/helixscreen#1516.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_probe_overlay.h"

#include "../test_helpers/probe_overlay_test_access.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "overlay_base.h"
#include "test_fixtures.h"

#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Records what the base hands the hook, and takes a token from inside it so a
/// test can tell whether the invalidation happened before or after the call.
template <typename Base> class ReasonRecorder : public Base {
  public:
    using Base::Base;

    std::vector<DeactivateReason> reasons;
    std::optional<helix::LifetimeToken> token_taken_in_hook;

    helix::LifetimeToken screen_token() {
        return this->lifetime_.token();
    }
    helix::LifetimeToken object_token() {
        return this->object_lifetime_.token();
    }

  protected:
    void on_deactivating(DeactivateReason reason) override {
        reasons.push_back(reason);
        token_taken_in_hook = this->lifetime_.token();
    }
};

class BareOverlay : public OverlayBase {
  public:
    void init_subjects() override {
        subjects_initialized_ = true;
    }

    lv_obj_t* create(lv_obj_t* parent) override {
        parent_screen_ = parent;
        overlay_root_ = lv_obj_create(parent);
        return overlay_root_;
    }

    const char* get_name() const override {
        return "BareOverlay";
    }

    helix::LifetimeToken screen_token() {
        return lifetime_.token();
    }
    helix::LifetimeToken object_token() {
        return object_lifetime_.token();
    }
};

class BarePanel : public PanelBase {
  public:
    BarePanel(helix::PrinterState& state, MoonrakerAPI* api, const char* component)
        : PanelBase(state, api), component_(component) {
        subjects_initialized_ = true;
    }

    void init_subjects() override {}

    const char* get_name() const override {
        return "BarePanel";
    }
    const char* get_xml_component_name() const override {
        return component_;
    }

    helix::LifetimeToken screen_token() {
        return lifetime_.token();
    }
    helix::LifetimeToken object_token() {
        return object_lifetime_.token();
    }

  private:
    const char* component_;
};

using RecordingOverlay = ReasonRecorder<BareOverlay>;

/// A PanelBase stub whose XML component is a bare box, so rebuild() has
/// something real to re-create.
constexpr const char* kRebuildComponent = "lifetime_contract_panel";

void register_rebuild_component() {
    lv_xml_register_component_from_data(kRebuildComponent,
                                        "<component>"
                                        "<view extends=\"lv_obj\" width=\"100\" height=\"100\"/>"
                                        "</component>");
}

} // namespace

// ============================================================================
// The guard invalidation belongs to the base, not to the subclass
// ============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "An overlay that overrides nothing still loses its screen-scoped callbacks",
                 "[1516][lifecycle][navigation]") {
    BareOverlay overlay;
    auto token = overlay.screen_token();
    REQUIRE_FALSE(token.expired());

    overlay.on_deactivate(DeactivateReason::NavigateAway);

    CHECK(token.expired());
    CHECK_FALSE(overlay.is_visible());
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "A panel that overrides nothing still loses its screen-scoped callbacks",
                 "[1516][lifecycle][navigation]") {
    BarePanel panel(state(), &api(), kRebuildComponent);
    auto token = panel.screen_token();
    REQUIRE_FALSE(token.expired());

    panel.on_deactivate(DeactivateReason::NavigateAway);

    CHECK(token.expired());
}

TEST_CASE_METHOD(MoonrakerTestFixture, "The hook runs before the screen guard is invalidated",
                 "[1516][lifecycle][navigation]") {
    // A hook that cancels work by hand has to see that work still live. The
    // token the hook takes is expired afterwards only if the invalidation
    // follows the call; invalidating first would hand the hook a fresh
    // generation that nothing ever expires.
    RecordingOverlay overlay;
    overlay.on_deactivate(DeactivateReason::NavigateAway);

    REQUIRE(overlay.token_taken_in_hook.has_value());
    CHECK(overlay.token_taken_in_hook->expired());
}

TEST_CASE_METHOD(MoonrakerTestFixture, "Deactivation is idempotent across repeated calls",
                 "[1516][lifecycle][navigation]") {
    RecordingOverlay overlay;
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    auto token_after_first = overlay.screen_token();

    overlay.on_deactivate(DeactivateReason::NavigateAway);

    CHECK(overlay.reasons.size() == 2);
    // The second pass expires anything armed since the first one, which is what
    // makes a re-entered deactivation safe rather than merely tolerated.
    CHECK(token_after_first.expired());
}

// ============================================================================
// The two guards differ, and the difference is the whole point of having both
// ============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture, "The object-scoped guard survives deactivation",
                 "[1516][lifecycle][navigation]") {
    BareOverlay overlay;
    auto screen = overlay.screen_token();
    auto object = overlay.object_token();

    overlay.on_deactivate(DeactivateReason::NavigateAway);

    CHECK(screen.expired());
    CHECK_FALSE(object.expired());
}

TEST_CASE_METHOD(MoonrakerTestFixture, "cleanup() invalidates the object-scoped guard too",
                 "[1516][lifecycle][navigation]") {
    BareOverlay overlay;
    auto object = overlay.object_token();
    overlay.on_deactivate(DeactivateReason::Shutdown);
    REQUIRE_FALSE(object.expired());

    overlay.cleanup();

    CHECK(object.expired());
    CHECK(overlay.cleanup_called());
}

TEST_CASE_METHOD(MoonrakerTestFixture, "Destruction expires both guards",
                 "[1516][lifecycle][navigation]") {
    std::optional<helix::LifetimeToken> screen;
    std::optional<helix::LifetimeToken> object;
    {
        BareOverlay overlay;
        screen = overlay.screen_token();
        object = overlay.object_token();
    }
    CHECK(screen->expired());
    CHECK(object->expired());
}

// ============================================================================
// The reason is a parameter, not a singleton query
// ============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture, "Each deactivation reason reaches the hook unchanged",
                 "[1516][lifecycle][navigation]") {
    RecordingOverlay overlay;

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_deactivate(DeactivateReason::Shutdown);
    overlay.on_deactivate(DeactivateReason::Rebuild);

    REQUIRE(overlay.reasons.size() == 3);
    CHECK(overlay.reasons[0] == DeactivateReason::NavigateAway);
    CHECK(overlay.reasons[1] == DeactivateReason::Shutdown);
    CHECK(overlay.reasons[2] == DeactivateReason::Rebuild);
}

TEST_CASE("Every reason has its own log name", "[1516][lifecycle]") {
    CHECK(std::string(deactivate_reason_name(DeactivateReason::NavigateAway)) == "navigate-away");
    CHECK(std::string(deactivate_reason_name(DeactivateReason::Shutdown)) == "shutdown");
    CHECK(std::string(deactivate_reason_name(DeactivateReason::Rebuild)) == "rebuild");
}

// ============================================================================
// The call sites send the right reason
// ============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture, "An overlay rebuild reports Rebuild, not NavigateAway",
                 "[1516][lifecycle][hot-reload]") {
    RecordingOverlay overlay;
    overlay.init_subjects();
    REQUIRE(overlay.create(test_screen()) != nullptr);

    NavigationManager::instance().register_overlay_instance(overlay.get_root(), &overlay);
    const bool rebuilt = overlay.rebuild();
    NavigationManager::instance().unregister_overlay_instance(overlay.get_root());

    REQUIRE(rebuilt);
    REQUIRE(overlay.reasons.size() == 1);
    CHECK(overlay.reasons[0] == DeactivateReason::Rebuild);

    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
}

TEST_CASE_METHOD(MoonrakerTestFixture, "A panel rebuild reports Rebuild, not NavigateAway",
                 "[1516][lifecycle][hot-reload]") {
    register_rebuild_component();
    auto& nav = NavigationManager::instance();
    nav.init();

    lv_obj_t* widget =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), kRebuildComponent, nullptr));
    REQUIRE(widget != nullptr);

    lv_obj_t* panels[kUiPanelCount] = {nullptr};
    panels[static_cast<int>(helix::PanelId::Home)] = widget;
    nav.set_panels(panels);

    ReasonRecorder<BarePanel> panel(state(), &api(), kRebuildComponent);
    panel.setup(widget, test_screen());
    nav.register_panel_instance(helix::PanelId::Home, &panel);

    const bool rebuilt = panel.rebuild();
    nav.register_panel_instance(helix::PanelId::Home, nullptr);

    REQUIRE(rebuilt);
    REQUIRE(panel.reasons.size() == 1);
    CHECK(panel.reasons[0] == DeactivateReason::Rebuild);

    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
}

TEST_CASE_METHOD(MoonrakerTestFixture, "App shutdown reports Shutdown, not NavigateAway",
                 "[1516][lifecycle][navigation]") {
    // ZOffsetCalibrationPanel keeps a running calibration alive on anything but
    // NavigateAway, so the reason this path sends decides whether closing the
    // app aborts a probe the printer is still executing.
    auto& nav = NavigationManager::instance();
    nav.init();

    RecordingOverlay overlay;
    overlay.init_subjects();
    REQUIRE(overlay.create(test_screen()) != nullptr);

    nav.register_overlay_instance(overlay.get_root(), &overlay);
    nav.push_overlay(overlay.get_root());
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    overlay.reasons.clear(); // the push deactivated whatever was underneath

    nav.shutdown();

    REQUIRE(overlay.reasons.size() == 1);
    CHECK(overlay.reasons[0] == DeactivateReason::Shutdown);

    // shutdown() latches shutting_down_; deinit_subjects() is what clears it,
    // and leaving it set would change how every later case in this process
    // behaves.
    nav.deinit_subjects();
    REQUIRE_FALSE(nav.is_shutting_down());
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
}

// ============================================================================
// A concrete overlay inherits the sequence rather than restating it
// ============================================================================

// The cases above construct their own stubs, so they pin what the base does and
// nothing about whether a real subclass still lets it happen. ProbeOverlay parks
// its accuracy run and its config reads on these guards, so an override of
// either half of the sequence that skips the invalidation leaves those callbacks
// armed against a view that is gone.

TEST_CASE_METHOD(MoonrakerTestFixture, "The probe overlay's deactivation drops screen-scoped work",
                 "[lifecycle][probe][navigation]") {
    ProbeOverlay overlay;
    auto screen = ProbeOverlayTestAccess::screen_token(overlay);
    auto object = ProbeOverlayTestAccess::object_token(overlay);
    REQUIRE_FALSE(screen.expired());
    REQUIRE_FALSE(object.expired());

    overlay.on_deactivate(DeactivateReason::NavigateAway);

    CHECK(screen.expired());
    CHECK_FALSE(object.expired());
}

TEST_CASE_METHOD(MoonrakerTestFixture, "The probe overlay's cleanup() drops both guards",
                 "[lifecycle][probe][navigation]") {
    ProbeOverlay overlay;
    auto screen = ProbeOverlayTestAccess::screen_token(overlay);
    auto object = ProbeOverlayTestAccess::object_token(overlay);
    REQUIRE_FALSE(screen.expired());
    REQUIRE_FALSE(object.expired());

    overlay.cleanup();

    CHECK(screen.expired());
    CHECK(object.expired());
    CHECK(overlay.cleanup_called());
}
