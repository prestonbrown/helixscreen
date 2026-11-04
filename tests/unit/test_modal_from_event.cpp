// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_modal_from_event.cpp
 * @brief An XML callback finds its instance by the widget that fired:
 *        Modal::from_event() and OverlayBase::from_event() (prestonbrown/helixscreen#1957).
 *
 * Run with: ./build/bin/helix-tests "[from_event]"
 */

#include "ui_guided_load_modal.h"
#include "ui_job_queue_modal.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_plugin_install_modal.h"
#include "ui_settings_barcode_scanner.h"
#include "ui_spoolman_edit_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "lvgl/src/misc/lv_event_private.h"
#include "overlay_base.h"

#include "../catch_amalgamated.hpp"

namespace {

/// A real dialog with no subject bindings and no owning class of its own.
constexpr const char* kComponent = "print_cancel_confirm_modal";

class ProbeModal : public Modal {
  public:
    const char* get_name() const override {
        return "ProbeModal";
    }
    const char* component_name() const override {
        return kComponent;
    }
    HELIX_KIND_TAG(ProbeModal)
};

/// The shape of a subclass that declares no kind.
class UntaggedModal : public Modal {
  public:
    const char* get_name() const override {
        return "UntaggedModal";
    }
    const char* component_name() const override {
        return kComponent;
    }
};

class ProbeOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "ProbeOverlay";
    }
    lv_obj_t* create(lv_obj_t* parent) override {
        overlay_root_ = lv_obj_create(parent);
        lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
        child = lv_obj_create(overlay_root_);
        return overlay_root_;
    }
    lv_obj_t* child = nullptr;
    HELIX_KIND_TAG(ProbeOverlay)
};

/// The event an XML callback receives when @p target fires.
lv_event_t event_on(lv_obj_t* target) {
    lv_event_t e{};
    e.code = LV_EVENT_CLICKED;
    e.original_target = target;
    e.current_target = target;
    return e;
}

lv_obj_t* named(lv_obj_t* root, const char* name) {
    lv_obj_t* widget = lv_obj_find_by_name(root, name);
    REQUIRE(widget != nullptr);
    return widget;
}

class FromEventFixture : public LVGLUITestFixture {
  protected:
    FromEventFixture() {
        base_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(helix::PanelId::Home)] = base_;
        NavigationManager::instance().set_panels(panels);
    }
    ~FromEventFixture() override {
        lv_obj_delete(base_);
    }

    /// Runs the queued overlay pushes and the deferred deletes.
    static void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler();
        }
    }

  private:
    lv_obj_t* base_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(FromEventFixture,
                 "Modal::from_event answers the modal whose dialog holds the widget, not the top",
                 "[modal][from_event]") {
    ProbeModal a;
    ProbeModal b;
    REQUIRE(a.show(test_screen()));
    REQUIRE(b.show(test_screen()));
    lv_obj_t* inner = lv_obj_get_child(a.dialog(), 0);
    REQUIRE(inner != nullptr);

    lv_event_t e = event_on(inner);
    CHECK(Modal::from_event(&e) == &a);
    CHECK(Modal::from_event<ProbeModal>(&e) == &a);
    CHECK(Modal::from_event<helix::JobQueueModal>(&e) == nullptr);

    lv_event_t on_backdrop = event_on(a.backdrop());
    CHECK(Modal::from_event(&on_backdrop) == &a);

    b.hide();
    a.hide();
    CHECK(Modal::from_event(&e) == nullptr);
}

TEST_CASE_METHOD(FromEventFixture, "a widget outside every dialog belongs to the modal on top",
                 "[modal][from_event]") {
    lv_obj_t* loose = lv_obj_create(lv_layer_top());
    lv_event_t e = event_on(loose);
    CHECK(Modal::from_event(&e) == nullptr);

    ProbeModal a;
    ProbeModal b;
    REQUIRE(a.show(test_screen()));
    REQUIRE(b.show(test_screen()));
    CHECK(Modal::from_event(&e) == &b);
    b.hide();
    CHECK(Modal::from_event(&e) == &a);
    lv_obj_delete(loose);
}

TEST_CASE_METHOD(FromEventFixture,
                 "a static-factory dialog and an untagged subclass resolve no typed instance",
                 "[modal][from_event]") {
    lv_obj_t* bare = Modal::show(kComponent);
    REQUIRE(bare != nullptr);
    UntaggedModal untagged;
    REQUIRE(untagged.show(test_screen()));

    lv_event_t on_bare = event_on(bare);
    CHECK(Modal::from_event(&on_bare) == nullptr);
    lv_event_t on_untagged = event_on(untagged.dialog());
    CHECK(Modal::from_event(&on_untagged) == &untagged);
    CHECK(Modal::from_event<UntaggedModal>(&on_untagged) == nullptr);
    Modal::hide(bare);
}

TEST_CASE_METHOD(FromEventFixture,
                 "OverlayBase::from_event answers the open overlay whose tree holds the widget",
                 "[overlay][overlay_base][from_event]") {
    ProbeOverlay a;
    ProbeOverlay b;
    REQUIRE(a.show(test_screen()));
    settle();
    REQUIRE(b.show(test_screen()));
    settle();

    lv_event_t on_b = event_on(b.child);
    CHECK(OverlayBase::from_event(&on_b) == &b);
    CHECK(OverlayBase::from_event<ProbeOverlay>(&on_b) == &b);
    CHECK(OverlayBase::from_event<helix::ui::BarcodeScannerSettingsOverlay>(&on_b) == nullptr);

    b.destroy_overlay_ui();
    CHECK(OverlayBase::from_event(&on_b) == nullptr);

    // cleanup() ends it too, on a tree the stack can still bring back to the top.
    lv_event_t on_a = event_on(a.child);
    a.cleanup();
    a.on_activate();
    CHECK(OverlayBase::from_event(&on_a) == nullptr);
    a.destroy_overlay_ui();
    settle();
}

TEST_CASE_METHOD(FromEventFixture, "JobQueueModal: the close button closes the modal it belongs to",
                 "[modal][job_queue][from_event]") {
    helix::JobQueueModal a;
    helix::JobQueueModal b;
    REQUIRE(a.show(test_screen()));
    REQUIRE(b.show(test_screen()));
    lv_obj_send_event(named(a.dialog(), "btn_close"), LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(a.is_visible());
    CHECK(b.is_visible());
}

TEST_CASE_METHOD(FromEventFixture, "PluginInstallModal: Done closes the modal it belongs to",
                 "[modal][plugin_install][from_event]") {
    PluginInstallModal a;
    PluginInstallModal b;
    REQUIRE(a.show(test_screen()));
    REQUIRE(b.show(test_screen()));
    lv_obj_send_event(named(a.dialog(), "btn_done"), LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(a.is_visible());
    CHECK(b.is_visible());
}

TEST_CASE_METHOD(FromEventFixture,
                 "GuidedLoadModal: Cancel on the buried wizard closes it and leaves the top one",
                 "[modal][guided_load][from_event]") {
    using helix::ui::GuidedLoadModal;
    helix::filament_macros::GuidedLoad steps;
    REQUIRE(GuidedLoadModal::show_owned(steps, true));
    lv_obj_t* first = Modal::get_top();
    REQUIRE(GuidedLoadModal::show_owned(steps, true));
    lv_obj_t* second = Modal::get_top();
    REQUIRE(first != second);
    auto& stack = ModalStack::instance();
    REQUIRE(stack.owner_for(first) != nullptr);

    lv_obj_send_event(named(first, "btn_cancel_push"), LV_EVENT_CLICKED, nullptr);
    CHECK(stack.owner_for(first) == nullptr);
    CHECK(stack.owner_for(second) != nullptr);
}

TEST_CASE_METHOD(FromEventFixture,
                 "SpoolEditModal: the header's close reaches the modal it belongs to",
                 "[modal][spoolman][from_event]") {
    helix::ui::SpoolEditModal a;
    helix::ui::SpoolEditModal b;
    bool a_done = false;
    bool b_done = false;
    a.set_completion_callback([&](bool) { a_done = true; });
    b.set_completion_callback([&](bool) { b_done = true; });
    REQUIRE(a.show_for_spool(test_screen(), SpoolInfo{}, nullptr));
    REQUIRE(b.show_for_spool(test_screen(), SpoolInfo{}, nullptr));

    lv_obj_send_event(named(a.dialog(), "btn_close"), LV_EVENT_CLICKED, nullptr);
    CHECK(a_done);
    CHECK_FALSE(b_done);
    CHECK_FALSE(a.is_visible());
    CHECK(b.is_visible());
}

TEST_CASE_METHOD(FromEventFixture,
                 "BarcodeScannerSettingsOverlay: only the overlay on show answers its widgets",
                 "[overlay][barcode_scanner][from_event]") {
    using helix::ui::BarcodeScannerSettingsOverlay;
    BarcodeScannerSettingsOverlay a;
    BarcodeScannerSettingsOverlay b;
    REQUIRE(a.show(test_screen()));
    settle();
    REQUIRE(b.show(test_screen()));
    settle();

    lv_event_t on_a = event_on(named(a.get_root(), "keymap_dropdown"));
    CHECK(OverlayBase::from_event<BarcodeScannerSettingsOverlay>(&on_a) == nullptr);
    lv_event_t on_b = event_on(named(b.get_root(), "keymap_dropdown"));
    CHECK(OverlayBase::from_event<BarcodeScannerSettingsOverlay>(&on_b) == &b);

    // Sliding out: the widgets still take presses, the hooks have already run.
    b.on_deactivate(DeactivateReason::NavigateAway);
    CHECK(OverlayBase::from_event<BarcodeScannerSettingsOverlay>(&on_b) == nullptr);

    a.destroy_overlay_ui();
    b.destroy_overlay_ui();
    settle();
}
