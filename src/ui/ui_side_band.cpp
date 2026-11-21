// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_side_band.h"

#include "ui_nav.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_panel_motion.h"

#include "ams_state.h"
#include "observer_factory.h"
#include "static_panel_registry.h"
#include "target_tool.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace helix::ui {

bool side_band_shows(SideBandContent content, bool ultrawide, int ams_slot_count, bool multi_head) {
    if (!ultrawide) {
        return false;
    }
    switch (content) {
    case SideBandContent::Motion:
        return true;
    case SideBandContent::Ams:
        return has_filament_system_or_multi_tool(ams_slot_count, multi_head);
    }
    return false;
}

namespace {

std::vector<SideBand*>& live_bands() {
    static std::vector<SideBand*> bands;
    return bands;
}

int theme_generation() {
    lv_subject_t* changed = theme_manager_get_changed_subject();
    return changed ? lv_subject_get_int(changed) : 0;
}

bool ultrawide_now() {
    lv_subject_t* s = lv_xml_get_subject(nullptr, "ui_is_ultrawide");
    return s && lv_subject_get_int(s) == 1;
}

/// The tree fills the band. Overlay trees are built hidden and sized for the
/// overlay stack; in the band they take the band's box instead.
void fill_band(lv_obj_t* root, lv_obj_t* band) {
    if (lv_obj_get_parent(root) != band) {
        lv_obj_set_parent(root, band);
    }
    // DECLARATIVE_OK: the band places a tree built for the overlay stack; its
    // size and visibility are what NavigationManager would otherwise set on push.
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_align(root, LV_ALIGN_TOP_LEFT);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    if (const lv_xml_style_t* xs = lv_xml_get_style_by_name(nullptr, "styles.side_band_guest")) {
        lv_obj_add_style(root, const_cast<lv_style_t*>(&xs->style), LV_PART_MAIN);
    }
}

/// A tree on the navigation stack belongs to the overlay path until it leaves.
bool open_as_overlay(lv_obj_t* root) {
    return root && (nav::is_in_stack(root) || nav::is_push_pending(root));
}

class MotionGuest final : public SideBandGuest {
  public:
    lv_obj_t* attach(lv_obj_t* band) override {
        MotionPanel& motion = get_global_motion_panel();
        if (lv_obj_t* existing = motion.get_root()) {
            // Built for the overlay path; the band waits for it to be freed.
            return lv_obj_get_parent(existing) == band ? existing : nullptr;
        }
        if (!motion.are_subjects_initialized()) {
            motion.init_subjects();
        }
        motion.register_callbacks(); // XML callback slots are last-write-wins
        lv_obj_t* root = motion.create(band);
        if (root) {
            fill_band(root, band);
        }
        return root;
    }

    void detach() override {
        if (auto* motion = lazy_global_if_exists<MotionPanel>()) {
            motion->destroy_overlay_ui();
        }
    }

    IPanelLifecycle* lifecycle() override {
        return lazy_global_if_exists<MotionPanel>();
    }
};

/// The AMS view navigate_to_ams_panel() would open, built fresh in the band:
/// a cached tree laid out its slots for the overlay's width, and slots lay out
/// on activation, after the band is shown.
class AmsGuest final : public SideBandGuest {
  public:
    lv_obj_t* attach(lv_obj_t* band) override {
        auto* backend = AmsState::instance().get_backend();
        if (!backend) {
            return nullptr;
        }
        const AmsPanel* detail = get_existing_ams_panel();
        const auto* overview = lazy_global_if_exists<AmsOverviewPanel>();
        lv_obj_t* detail_root = detail ? detail->get_panel() : nullptr;
        lv_obj_t* overview_root = overview ? overview->get_panel() : nullptr;
        if (open_as_overlay(detail_root) || open_as_overlay(overview_root)) {
            return nullptr;
        }
        // One of the two serves a given unit count; neither cached tree is reused.
        if (detail_root) {
            destroy_ams_panel_ui();
        }
        if (overview_root) {
            destroy_ams_overview_panel_ui();
        }
        view_ = ams_view_for(backend->get_system_info());
        lv_obj_t* root = view_ == AmsView::Overview ? get_global_ams_overview_panel().get_panel()
                                                    : get_global_ams_panel().get_panel();
        if (root) {
            fill_band(root, band);
        }
        return root;
    }

    void detach() override {
        if (view_ == AmsView::Overview) {
            destroy_ams_overview_panel_ui();
        } else {
            destroy_ams_panel_ui();
        }
    }

    IPanelLifecycle* lifecycle() override {
        if (view_ == AmsView::Overview) {
            return lazy_global_if_exists<AmsOverviewPanel>();
        }
        return get_existing_ams_panel();
    }

    bool stale() override {
        auto* backend = AmsState::instance().get_backend();
        return backend && ams_view_for(backend->get_system_info()) != view_;
    }

  private:
    AmsView view_ = AmsView::Detail;
};

} // namespace

std::unique_ptr<SideBandGuest> make_motion_side_band_guest() {
    return std::make_unique<MotionGuest>();
}

std::unique_ptr<SideBandGuest> make_ams_side_band_guest() {
    return std::make_unique<AmsGuest>();
}

SideBand::SideBand(SideBandContent content, std::unique_ptr<SideBandGuest> guest,
                   const char* subject_name, const char* hosted_subject_name)
    : content_(content), guest_(std::move(guest)), subject_name_(subject_name),
      hosted_subject_name_(hosted_subject_name) {
    live_bands().push_back(this);
}

SideBand::~SideBand() {
    auto& bands = live_bands();
    bands.erase(std::remove(bands.begin(), bands.end(), this), bands.end());
    lifetime_.invalidate();
    if (lv_is_initialized()) {
        if (lv_obj_t* band = band_.get()) {
            lv_obj_remove_event_cb_with_user_data(band, on_band_deleted, this);
        }
        if (lv_obj_t* root = host_root_.get()) {
            lv_obj_remove_event_cb_with_user_data(root, on_host_size_changed, this);
        }
    }
}

void SideBand::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(shows_subject_, 0, subject_name_, subjects_);
    UI_MANAGED_SUBJECT_INT(hosted_subject_, 0, hosted_subject_name_, subjects_);
    subjects_initialized_ = true;
}

void SideBand::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    ultrawide_observer_.reset();
    slot_count_observer_.reset();
    multi_head_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void SideBand::bind(lv_obj_t* host_root) {
    if (lv_obj_t* old = host_root_.get(); old && old != host_root) {
        lv_obj_remove_event_cb_with_user_data(old, on_host_size_changed, this);
    }
    if (lv_obj_t* old = band_.get()) {
        lv_obj_remove_event_cb_with_user_data(old, on_band_deleted, this);
    }
    host_root_ = host_root;
    main_ = host_root ? lv_obj_find_by_name(host_root, "side_band_main") : nullptr;
    band_ = host_root ? lv_obj_find_by_name(host_root, "side_band") : nullptr;
    if (!main_.get() || !band_.get()) {
        spdlog::error("[SideBand] host tree lacks side_band_main/side_band");
        return;
    }
    // LV_EVENT_DELETE and SIZE_CHANGED have no declarative form.
    lv_obj_add_event_cb(band_.get(), on_band_deleted, LV_EVENT_DELETE, this);
    lv_obj_add_event_cb(host_root, on_host_size_changed, LV_EVENT_SIZE_CHANGED, this);
    observe_inputs();
    refresh();
}

void SideBand::observe_inputs() {
    auto on_change = [](SideBand* self, int) { self->refresh(); };
    // ui_is_ultrawide is a theme global, deinited only after LVGL is gone.
    if (lv_subject_t* ultrawide = lv_xml_get_subject(nullptr, "ui_is_ultrawide")) {
        ultrawide_observer_ = observe<int>(ultrawide, this, on_change, subject_never_freed());
    }
    // A theme or mode change leaves the hosted tree in the old colors.
    if (lv_subject_t* theme = theme_manager_get_changed_subject()) {
        theme_observer_ = observe<int>(theme, this, on_change, subject_never_freed());
    }
    if (content_ != SideBandContent::Ams) {
        return;
    }
    auto& ams = AmsState::instance();
    if (lv_subject_t* slots = ams.get_slot_count_subject()) {
        slot_count_observer_ = observe<int>(slots, this, on_change, ams.get_subjects_lifetime());
    }
    if (lv_subject_t* multi = lv_xml_get_subject(nullptr, "target_tool_multi_head")) {
        multi_head_observer_ =
            observe<int>(multi, this, on_change, TargetTool::instance().get_subjects_lifetime());
    }
}

bool SideBand::uses_full_width() const {
    return ultrawide_now();
}

void SideBand::refresh() {
    int slots = 0;
    bool multi = false;
    if (content_ == SideBandContent::Ams) {
        if (lv_subject_t* s = AmsState::instance().get_slot_count_subject()) {
            slots = lv_subject_get_int(s);
        }
        if (lv_subject_t* m = lv_xml_get_subject(nullptr, "target_tool_multi_head")) {
            multi = lv_subject_get_int(m) == 1;
        }
    }
    shows_ = side_band_shows(content_, ultrawide_now(), slots, multi);
    fit_main_column();

    // The guest's tree went away without the band (a theme change, memory
    // pressure, a printer switch): its lifecycle already wound itself down.
    if (guest_active_ && !hosted_.get()) {
        guest_active_ = false;
    }

    if (hosted_.get() && (theme_generation() != theme_gen_ || guest_->stale())) {
        spdlog::debug("[SideBand] hosted tree is stale; rebuilding");
        detach_guest();
    }

    if (!shows_) {
        detach_guest();
    } else if (host_active_) {
        attach_guest();
        // Shown before the guest activates: a guest lays out on activation
        // (the AMS slots) and a hidden band has no size.
        publish();
        if (hosted_.get()) {
            lv_obj_update_layout(hosted_.get());
        }
        activate_guest();
    }
    publish();
}

void SideBand::publish() {
    // The band shows only with content in it: a guest that cannot be built (an
    // AMS gate with no backend yet) leaves the host exactly as it was.
    const int value = shows_ && hosted_.get() ? 1 : 0;
    if (subjects_initialized_ && lv_subject_get_int(&shows_subject_) != value) {
        lv_subject_set_int(&shows_subject_, value);
    }
}

void SideBand::fit_main_column() {
    lv_obj_t* main = main_.get();
    lv_obj_t* root = host_root_.get();
    if (!main || !root) {
        return;
    }
    // Measured layout: the main column stops where the panel cap would have
    // stopped the whole panel, and the band takes the rest.
    // DECLARATIVE_OK: the cap is a live per-screen number (live_content_max_width).
    const int32_t width = ultrawide_now() ? live_content_max_width(root) : lv_pct(100);
    if (lv_obj_get_style_width(main, LV_PART_MAIN) != width) {
        lv_obj_set_width(main, width);
    }
}

void SideBand::attach_guest() {
    lv_obj_t* band = band_.get();
    if (hosted_.get() || !band) {
        return;
    }
    // The tree may hide its back button as it is built.
    if (subjects_initialized_) {
        lv_subject_set_int(&hosted_subject_, 1);
    }
    const int theme_gen = theme_generation();
    lv_obj_t* root = guest_->attach(band);
    hosted_ = root;
    theme_gen_ = theme_gen;
    if (!root) {
        if (subjects_initialized_) {
            lv_subject_set_int(&hosted_subject_, 0);
        }
        spdlog::debug("[SideBand] content open elsewhere; the band waits for it");
        return;
    }
    spdlog::debug("[SideBand] hosting {}", content_ == SideBandContent::Motion ? "Motion" : "AMS");
}

void SideBand::detach_guest() {
    if (!hosted_.get()) {
        guest_active_ = false;
        if (subjects_initialized_) {
            lv_subject_set_int(&hosted_subject_, 0);
        }
        return;
    }
    deactivate_guest(DeactivateReason::NavigateAway);
    hosted_.reset();
    guest_->detach();
    if (subjects_initialized_) {
        lv_subject_set_int(&hosted_subject_, 0);
    }
}

void SideBand::activate_guest() {
    if (guest_active_ || !hosted_.get()) {
        return;
    }
    if (IPanelLifecycle* view = guest_->lifecycle()) {
        guest_active_ = true;
        view->on_activate();
    }
}

void SideBand::deactivate_guest(DeactivateReason reason) {
    if (!guest_active_) {
        return;
    }
    guest_active_ = false;
    if (IPanelLifecycle* view = guest_->lifecycle()) {
        view->on_deactivate(reason);
    }
}

void SideBand::on_host_activate() {
    host_active_ = true;
    refresh();
}

void SideBand::on_host_deactivate(DeactivateReason reason) {
    host_active_ = false;
    deactivate_guest(reason);
    // A rebuild deletes the host tree, band and all: hand the guest's tree back first.
    if (reason == DeactivateReason::Rebuild) {
        detach_guest();
        publish();
    }
}

bool SideBand::take_open_request(SideBandContent content) {
    for (SideBand* band : live_bands()) {
        if (band->content_ != content || !band->hosted_.get()) {
            continue;
        }
        if (band->host_active_ && band->shows_) {
            return true;
        }
        band->detach_guest();
        band->publish();
    }
    return false;
}

void SideBand::notify_released(SideBandContent content) {
    for (SideBand* band : live_bands()) {
        if (band->content_ != content) {
            continue;
        }
        band->lifetime_.defer("SideBand::notify_released", [band]() {
            if (band->host_active_ && band->shows_ && !band->hosted_.get()) {
                band->refresh();
            }
        });
    }
}

void SideBand::on_band_deleted(lv_event_t* e) {
    auto* self = static_cast<SideBand*>(lv_event_get_user_data(e));
    // The band's children go next. A tree still in it moves out first so the
    // guest frees it on its own path and never keeps a pointer to a freed tree.
    if (self->hosted_.get() && !StaticPanelRegistry::is_destroying_all()) {
        self->detach_guest();
        self->publish();
    }
}

void SideBand::on_host_size_changed(lv_event_t* e) {
    static_cast<SideBand*>(lv_event_get_user_data(e))->fit_main_column();
}

} // namespace helix::ui
