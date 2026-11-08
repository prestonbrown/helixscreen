// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "first_run_tour.h"

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_next_tick.h"
#include "ui_panel_ams_overview.h"

#include "app_globals.h"
#include "config.h"
#include "observer_factory.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "tour_overlay.h"
#include "tour_steps.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::tour {

namespace {
// The overlay slide-in takes 200ms; the margin covers a panel's first layout.
constexpr uint32_t kPanelSettleMs = 280;

PanelId base_panel_for(TourPanel panel) {
    return panel == TourPanel::Home ? PanelId::Home : PanelId::Filament;
}

// By name rather than go_back(): something else can have opened on top of it.
bool close_ams_overlay() {
    lv_obj_t* screen = lv_screen_active();
    for (const char* name : {"ams_panel", "ams_overview_panel"}) {
        lv_obj_t* overlay = lv_obj_find_by_name(screen, name);
        if (overlay && helix::nav::is_in_stack(overlay)) {
            NavigationManager::instance().close_overlay(overlay);
            return true;
        }
    }
    return false;
}
} // namespace

FirstRunTour& FirstRunTour::instance() {
    static FirstRunTour s;
    static bool registered = []() {
        // Tear the overlay down before LVGL deinits — otherwise the unique_ptr's
        // destructor runs during C++ static teardown, after lv_deinit(), and
        // ~TourOverlay's lv_obj_delete() touches freed LVGL state.
        StaticPanelRegistry::instance().register_destroy("FirstRunTour", []() {
            auto& tour = FirstRunTour::instance();
            tour.panel_settle_timer_.reset();
            tour.overlay_.reset();
        });
        return true;
    }();
    (void)registered;
    return s;
}

bool FirstRunTour::should_auto_start() {
    auto* cfg = ::helix::Config::get_instance();

    if (cfg->is_wizard_required()) {
        spdlog::debug("[FirstRunTour] gate: wizard_required");
        return false;
    }

    // Finishing the tour is final, so it waits until it knows whether the
    // printer has a filament system; on_printer_ready() offers it again.
    if (!instance().printer_ready_) {
        spdlog::debug("[FirstRunTour] gate: printer_not_discovered");
        return false;
    }

    // Every tour step highlights a home widget, and edit mode is where those
    // widgets move, resize and get deleted. The gate stays open, so the tour is
    // offered again at the next home activation.
    if (lv_subject_get_int(&get_home_edit_mode_subject()) != 0) {
        spdlog::debug("[FirstRunTour] gate: home_edit_mode_active");
        return false;
    }

    // The tour's card would sit over a modal or an overlay covering Home (a
    // power-loss resume offer, its wizard), which outranks it. The gate stays
    // open, so the tour is offered again at the next home activation.
    if (!ModalStack::instance().empty() || NavigationManager::instance().has_open_overlays()) {
        spdlog::debug("[FirstRunTour] gate: home_covered");
        return false;
    }

    const bool completed = cfg->get<bool>("/tour/completed", false);
    const int last_seen = cfg->get<int>("/tour/last_seen_version", 0);

    if (!completed) {
        spdlog::debug("[FirstRunTour] gate: fresh_auto_start");
        return true;
    }
    if (last_seen < kTourVersion) {
        spdlog::debug("[FirstRunTour] gate: version_bumped_since_last_seen "
                      "(last_seen={}, current={})",
                      last_seen, kTourVersion);
        return true;
    }
    spdlog::debug("[FirstRunTour] gate: already_completed_current_version");
    return false;
}

void FirstRunTour::mark_completed() {
    auto* cfg = ::helix::Config::get_instance();
    cfg->set<bool>("/tour/completed", true);
    cfg->set<int>("/tour/last_seen_version", kTourVersion);
    cfg->save();
    spdlog::debug("[FirstRunTour] Marked completed (version={})", kTourVersion);
}

void FirstRunTour::maybe_start() {
    if (running_ || start_queued_)
        return;
    if (!should_auto_start())
        return;
    start_queued_ = true;
    // Defer one tick so the caller (e.g., HomePanel::on_activate) completes first.
    // Unguarded `this` is safe here because FirstRunTour is a function-local
    // static (immortal lifetime) — see instance().
    helix::ui::run_next_tick([this]() {
        start_queued_ = false;
        // start() may have run the tour synchronously while this was queued,
        // and a modal or overlay may have come up over Home since the check.
        if (running_ || !should_auto_start())
            return;
        auto* cfg = ::helix::Config::get_instance();
        start_impl(auto_start_seen_version(cfg->get<bool>("/tour/completed", false),
                                           cfg->get<int>("/tour/last_seen_version", 0)));
    });
}

void FirstRunTour::on_printer_ready() {
    printer_ready_ = true;
    maybe_start();
}

void FirstRunTour::start() {
    if (running_)
        return;
    start_impl(/*seen_version=*/0);
}

void FirstRunTour::start_impl(int seen_version) {
    steps_ = build_tour_steps(hardware_has_ams(), seen_version);
    if (steps_.empty()) {
        mark_completed();
        return;
    }
    running_ = true;
    current_index_ = 0;
    shown_panel_ = TourPanel::Home;

    // Overlay requires an active LVGL screen; unit tests run headless.
    if (lv_screen_active()) {
        overlay_ = std::make_unique<TourOverlay>(
            steps_, [this] { this->advance(); }, [this] { this->skip(); });
        render_current_step();

        // Cancel the tour if something else moves off the panel the current step
        // is on, which would orphan the tour over a stale target.
        auto* nav_subject = NavigationManager::instance().get_active_panel_subject();
        nav_observer_ = helix::ui::observe<int>(
            nav_subject, this,
            [](FirstRunTour* self, int panel_id) {
                if (!self->running_)
                    return;
                if (panel_id != static_cast<int>(base_panel_for(self->shown_panel_))) {
                    spdlog::debug("[FirstRunTour] Cancelled: navigated away from the step's panel");
                    // Whoever navigated decided where the user is going.
                    self->shown_panel_ = TourPanel::Home;
                    self->skip();
                }
            },
            NavigationManager::instance().get_subjects_lifetime());

        // Re-resolve the current step's target widget when the responsive
        // breakpoint changes. The breakpoint swap rebuilds panel widgets, so
        // the highlight_'s cached target pointer becomes stale. Defer one
        // tick via run_next_tick so the new widget tree is built before we
        // look up the target by name.
        if (auto* bp_subj = theme_manager_get_breakpoint_subject()) {
            breakpoint_observer_ = helix::ui::observe<int>(
                bp_subj, this,
                [](FirstRunTour* self, int /*bp*/) {
                    if (!self->running_ || !self->overlay_)
                        return;
                    // Defer: panel rebuild on breakpoint change is async; the
                    // new widget tree isn't ready synchronously. Unguarded
                    // `self` is safe — FirstRunTour is a function-local static
                    // (immortal lifetime).
                    helix::ui::run_next_tick([self]() {
                        if (!self->running_ || !self->overlay_)
                            return;
                        self->render_current_step();
                    });
                },
                subject_never_freed());
        }
    }

    spdlog::debug("[FirstRunTour] Started ({} steps)", steps_.size());
}

void FirstRunTour::advance() {
    if (!running_)
        return;
    current_index_++;
    if (current_index_ >= steps_.size()) {
        finish();
        return;
    }
    render_current_step();
}

void FirstRunTour::skip() {
    if (!running_)
        return;
    spdlog::debug("[FirstRunTour] Skipped at step {}/{}", current_index_ + 1, steps_.size());
    end_tour();
}

void FirstRunTour::finish() {
    spdlog::debug("[FirstRunTour] Finished");
    end_tour();
}

void FirstRunTour::end_tour() {
    mark_completed();
    running_ = false;
    nav_observer_.reset();
    breakpoint_observer_.reset();
    panel_settle_timer_.reset();
    // This can run from the card's own button or inside an update-queue batch,
    // and neither may delete the widgets or callback currently executing.
    if (TourOverlay* overlay = overlay_.release()) {
        overlay->hide_card();
        helix::ui::run_next_tick([overlay]() { delete overlay; });
    }
    show_panel(TourPanel::Home);
}

void FirstRunTour::arm_panel_timer(lv_timer_cb_t callback) {
    lv_timer_t* timer = lv_timer_create(callback, kPanelSettleMs, this);
    lv_timer_set_repeat_count(timer, 1);
    panel_settle_timer_.reset(timer);
}

void FirstRunTour::show_panel(TourPanel panel) {
    if (panel == shown_panel_)
        return;
    const bool leaving_ams = shown_panel_ == TourPanel::AmsOverlay;
    // Set first: the active-panel observer compares against it.
    shown_panel_ = panel;
    // Switching panels before the close lands would activate the destination
    // twice: once for the switch and once as the panel the overlay restores.
    if (leaving_ams && close_ams_overlay()) {
        arm_panel_timer(on_ams_closed);
        return;
    }
    helix::nav::set_active(base_panel_for(panel));
    if (panel == TourPanel::AmsOverlay)
        navigate_to_ams_panel();
}

void FirstRunTour::on_ams_closed(lv_timer_t* timer) {
    auto* self = static_cast<FirstRunTour*>(lv_timer_get_user_data(timer));
    // A one-shot timer frees itself when this returns.
    self->panel_settle_timer_.release();
    helix::nav::set_active(base_panel_for(self->shown_panel_));
    if (self->running_ && self->overlay_)
        self->arm_panel_timer(on_panel_settled);
}

void FirstRunTour::on_panel_settled(lv_timer_t* timer) {
    auto* self = static_cast<FirstRunTour*>(lv_timer_get_user_data(timer));
    // A one-shot timer frees itself when this returns.
    self->panel_settle_timer_.release();
    if (self->running_ && self->overlay_)
        self->overlay_->show_step(self->current_index_);
}

void FirstRunTour::render_current_step() {
    if (!overlay_)
        return;
    const TourPanel wanted = steps_[current_index_].panel;
    if (wanted == shown_panel_) {
        overlay_->show_step(current_index_);
        return;
    }
    overlay_->hide_card();
    show_panel(wanted);
    if (!panel_settle_timer_)
        arm_panel_timer(on_panel_settled);
}

} // namespace helix::tour
