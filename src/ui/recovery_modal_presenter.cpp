// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recovery_modal_presenter.h"

#include "ui_afc_fault_path.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"

#include "action_prompt_manager.h"
#include "active_material_provider.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "error_event.h"
#include "error_modal_view.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "temperature_controller.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

/// Poll cadence while waiting for the nozzle. Fast enough that the recovery
/// fires within a frame of reaching temperature, slow enough to cost nothing.
constexpr uint32_t PREHEAT_POLL_MS = 250;

/// How long to wait for the nozzle before giving up. Same 300s budget the AFC
/// and AD5X backends give their own heating phases (HEATING_TIMEOUT_SECONDS),
/// so a recovery preheat abandons a dead heater on the same schedule the
/// backends do. IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS is numerically identical
/// but means "how long may an RPC take"; this is a physical heating budget.
constexpr uint32_t PREHEAT_TIMEOUT_MS = 300000;

/// Treat "within 5°C of target" as arrived — the same slack FilamentPanel and
/// AmsOperationSidebar use, so all three preheats release at the same point.
constexpr int TEMP_THRESHOLD_C = 5;

int nozzle_current_c() {
    auto* subj = get_printer_state().get_active_extruder_temp_subject();
    return subj ? helix::ui::temperature::deci_to_degrees(lv_subject_get_int(subj)) : 0;
}

} // namespace

namespace helix::ui {

RecoveryModalPresenter::RecoveryModalPresenter(IMoonrakerAPI* api)
    : api_(api), preheat_budget_ms_(PREHEAT_TIMEOUT_MS) {}

RecoveryModalPresenter::~RecoveryModalPresenter() {
    // The poll timer outlives the modal that started it, so nothing else can
    // stop it on the way down.
    cancel_preheat_timer();
}

bool RecoveryModalPresenter::is_visible() const {
    return modal_ && modal_->is_visible();
}

namespace {
/// Whether two recovery sets offer the user the same choices. Compares what the
/// modal actually renders and dispatches — label and gcode — so a set that
/// merely restyles a button is still "the same", while one that adds or changes
/// an affordance is not. log_tag is deliberately excluded: it is diagnostics.
bool same_actions(const std::vector<helix::RecoveryAction>& a,
                  const std::vector<helix::RecoveryAction>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].label != b[i].label || a[i].gcode != b[i].gcode) {
            return false;
        }
    }
    return true;
}

/// The prompt modal, plus a report of every hide that runs its on_hide() hook.
/// ActionPromptModal's dismiss affordance (an action with empty gcode) closes
/// the dialog without calling the gcode callback, so this hook is the presenter's
/// only sight of the user saying "I've read it" — and it covers the backdrop tap
/// and ESC the same way.
class HideReportingPromptModal : public helix::ui::ActionPromptModal {
  public:
    void set_hidden_callback(std::function<void()> cb) {
        on_hidden_ = std::move(cb);
    }

  protected:
    void on_hide() override {
        helix::ui::ActionPromptModal::on_hide();
        if (on_hidden_) {
            on_hidden_();
        }
    }

  private:
    std::function<void()> on_hidden_;
};
} // namespace

void RecoveryModalPresenter::mark_handled() {
    if (shown_detail_.empty()) {
        return;
    }
    handled_detail_ = shown_detail_;
    handled_actions_ = active_actions_;
}

void RecoveryModalPresenter::forget_handled_fault() {
    handled_detail_.clear();
    handled_actions_.clear();
}

void RecoveryModalPresenter::on_modal_hidden() {
    if (suppress_hide_notice_) {
        return;
    }
    // Nobody in this class asked for that hide, so the user closed it.
    mark_handled();
}

void RecoveryModalPresenter::dismiss() {
    if (modal_ && modal_->is_visible()) {
        suppress_hide_notice_ = true;
        modal_->hide();
        suppress_hide_notice_ = false;
    }
    shown_detail_.clear();
    // The episode is over (the AMS action left ERROR, or a caller explicitly
    // took the dialog down). Whatever the user answered applied to that episode
    // only; the same fault raised again later is news.
    forget_handled_fault();
}

void RecoveryModalPresenter::present(const helix::ErrorEvent& e) {
    // Dedup: if the same detail AND the same action set are still on screen, do
    // not re-show. A dismissed-but-ongoing fault clears shown_detail_ so it can
    // re-show.
    //
    // The action set is part of the identity, not decoration. One fault legally
    // reaches here twice from two sources carrying the SAME text but different
    // affordances: AFC emits `!! <msg>` and queues that identical string
    // (AFC_logger.error sends "!! {msg}" then appends msg to message_queue), so
    // the line-arrival event and the later status-driven current_error() event
    // are byte-identical in detail while only the second carries AFC's
    // Unload/Eject/Recover set. Comparing detail alone would pin whichever
    // arrived first — always the poorer one, since the `!!` lands before AFC
    // pauses (prestonbrown/helixscreen#1171).
    if (modal_ && modal_->is_visible() && e.detail == shown_detail_ &&
        same_actions(active_actions_, e.recovery_actions)) {
        spdlog::debug("[RecoveryModalPresenter] Skipping duplicate (still visible): {}", e.detail);
        return;
    }

    // The user already answered this exact fault — closed the dialog, or tapped
    // one of these very buttons — and the backend is merely re-notifying it.
    // AmsState::recompute_action_detail() fires on any strcmp difference in
    // operation_detail, so a fault that is still latched re-reaches us on every
    // cosmetic wording change; without this the dismissed dialog pops straight
    // back up, and after a recovery tap it would put live buttons over the
    // preheat that tap started (a second tap re-arms and re-dispatches it).
    //
    // The action set is part of the identity here for the same reason it is
    // above: a genuinely richer set of affordances for the same text is a new
    // offer, not a re-notification.
    if (!handled_detail_.empty() && e.detail == handled_detail_ &&
        same_actions(handled_actions_, e.recovery_actions)) {
        spdlog::debug("[RecoveryModalPresenter] Skipping fault the user already answered: {}",
                      e.detail);
        return;
    }

    if (!modal_) {
        auto modal = std::make_unique<HideReportingPromptModal>();
        modal->set_gcode_callback([this](const std::string& gcode) { on_recovery_tapped(gcode); });
        modal->set_hidden_callback([this]() { on_modal_hidden(); });
        modal_ = std::move(modal);
    }

    // Anything we are about to put on screen is new to the user, so the previous
    // answer stops applying.
    forget_handled_fault();

    active_actions_ = e.recovery_actions;
    shown_detail_ = e.detail;

    // Build the prompt. modal_title_for encodes the CFS "Filament System Error"
    // rule; build_recovery_prompt only knows e.title, which the classifier
    // leaves empty for CFS events.
    helix::PromptData prompt;
    prompt.title = modal_title_for(e);
    // AFC welds a monospace position diagram onto its lane faults, which our
    // proportional font renders as noise (#1184). Publish the stop point to the
    // modal's <afc_fault_path> graphic and drop the art rows from the text. Every
    // event goes through here, not just AFC's: an unrecognised detail sets the
    // subject to 0 (graphic hidden) and comes back unchanged, which is also what
    // clears a previous fault's marker off a reused modal instance.
    const std::string detail = afc_fault_path_apply(e.detail);
    if (!detail.empty())
        prompt.text_lines.push_back(detail);
    for (const auto& a : e.recovery_actions) {
        helix::PromptButton b;
        b.label = a.label;
        b.gcode = a.gcode;
        b.color = color_for_style(a.style);
        prompt.buttons.push_back(std::move(b));
    }
    // MODAL_WITH_RECOVER is always an error severity -- restore the red error affordance.
    prompt.severity = "error";

    lv_obj_t* screen = lv_screen_active();
    // Replacing visible content makes Modal::show() hide the old dialog first.
    // That hide is ours, not the user's, so keep it out of on_modal_hidden().
    suppress_hide_notice_ = true;
    const bool shown = screen && modal_->show_prompt(screen, prompt);
    suppress_hide_notice_ = false;
    if (!shown) {
        spdlog::warn("[RecoveryModal] show_prompt failed; falling back to alert");
        shown_detail_.clear();
        ui_notification_printer_fault(modal_title_for(e), e.detail.c_str());
    }
}

void RecoveryModalPresenter::on_recovery_tapped(const std::string& gcode) {
    // Runs on the main thread (button tap). Find the action for its metadata.
    const helix::RecoveryAction* action = nullptr;
    for (const auto& a : active_actions_) {
        if (a.gcode == gcode) {
            action = &a;
            break;
        }
    }
    const std::string tag = action ? action->log_tag : "RecoveryModalPresenter::recovery";
    const std::string label = action ? action->label : gcode;

    // The user answered this fault, so it must not come back on its own. A
    // re-present here is worse than a stale dialog: the tap may have started a
    // preheat (the modal closes on the tap, so the toast is the only sign of
    // it), and tapping the re-presented buttons would clear_preheat() and
    // re-arm the whole recovery on top of the one already in flight.
    mark_handled();
    shown_detail_.clear(); // allow re-show on a genuinely different fault
    spdlog::info("[RecoveryModal] User tapped recovery: {} ({})", tag, gcode);

    if (!api_) {
        spdlog::warn("[RecoveryModal] No API client; cannot execute gcode: {}", gcode);
        return;
    }

    // Filament-moving recovery on a cold hotend: heat first, send on arrival.
    // The heater is routinely off by now — the post-op cooldown armed by the
    // operation that failed, TURN_OFF_HEATERS on a print ERROR, or idle_timeout.
    if (action && action->needs_hot_nozzle && !nozzle_ready_for_extrusion()) {
        begin_preheat(gcode, tag, label);
        return;
    }

    dispatch_recovery(gcode, tag);
}

void RecoveryModalPresenter::dispatch_recovery(const std::string& gcode, const std::string& tag) {
    if (!api_)
        return;
    api_->execute_gcode(
        gcode, [tag]() { spdlog::info("[Recovery] {} completed", tag); },
        [tag](const MoonrakerError& err) {
            spdlog::error("[Recovery] {} failed: {}", tag, err.message);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          ("Recovery failed: " + err.user_message()).c_str(), 6000);
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
}

bool RecoveryModalPresenter::nozzle_ready_for_extrusion() const {
    // #978 opt-out: users whose macros heat the nozzle themselves, or who are
    // deliberately cold-pulling, already bypass this gate on the filament panel.
    // The recovery modal must not re-impose it.
    if (helix::SafetySettingsManager::instance().get_allow_cold_extrude()) {
        return true;
    }
    const int min_extrude = static_cast<int>(api_->get_safety_limits().min_extrude_temp_celsius);
    return helix::ui::temperature::is_extrusion_safe(nozzle_current_c(), min_extrude);
}

int RecoveryModalPresenter::resolve_preheat_target() const {
    const int floor_c = static_cast<int>(api_->get_safety_limits().min_extrude_temp_celsius);

    // 1. What this printer was last told to hold. A filament fault interrupts an
    //    operation that had already chosen a material temperature, and the latch
    //    survives the heater being zeroed afterwards — which is the whole window
    //    this gate covers. Same value TemperatureController floors against for
    //    keep_previous_hot, so a deferred recovery reheats to where the failed
    //    operation was running rather than to a guess.
    int target = static_cast<int>(
        std::lround(get_printer_state().get_active_extruder_last_nonzero_target()));

    // 2. Nothing latched (the operation failed before it ever heated, or this is
    //    a fresh session): ask the loaded filament. get_active_material() is the
    //    shared active-slot -> external-spool chain the sidebar's preheat uses;
    //    the presenter has no slot context of its own to key off.
    if (target <= 0) {
        if (auto material = helix::get_active_material();
            material && material->material_info.nozzle_min > 0) {
            target = material->material_info.nozzle_min;
        }
    }

    // 3. Still nothing known: the shared load-preheat default rather than a new
    //    number invented here.
    if (target <= 0) {
        target = AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP;
    }

    // A latch or material temp below min_extrude_temp would heat to a nozzle
    // Klipper still refuses to extrude from, and the poll would then dispatch
    // into a rejection.
    return std::max(target, floor_c);
}

void RecoveryModalPresenter::begin_preheat(const std::string& gcode, const std::string& tag,
                                           const std::string& label) {
    const int target = resolve_preheat_target();

    // A second tap replaces the first: only one recovery may be in flight, and
    // the newest choice is the one the user meant.
    clear_preheat();

    pending_gcode_ = gcode;
    pending_tag_ = tag;
    pending_label_ = label;
    pending_target_c_ = target;
    polls_remaining_ =
        std::max<int32_t>(1, static_cast<int32_t>(preheat_budget_ms_ / PREHEAT_POLL_MS));

    if (auto* c = get_temperature_controller()) {
        c->set_target(helix::HeaterType::Nozzle, static_cast<double>(target),
                      {.toast = false, .keep_previous_hot = true});
    }

    preheat_timer_ = lv_timer_create(preheat_timer_cb, PREHEAT_POLL_MS, this);
    // The wait is bounded by counting polls rather than by wall clock, so the
    // budget cannot be stretched by a stalled main loop. One repeat MORE than
    // polls_remaining_ so poll_preheat() always ends the wait itself — were LVGL
    // to exhaust the count first it would delete the timer under preheat_timer_.
    lv_timer_set_repeat_count(preheat_timer_, polls_remaining_ + 1);

    // The modal closed on the tap, so this is the only acknowledgement the user
    // gets that their choice was accepted rather than ignored.
    NOTIFY_INFO(lv_tr("Heating to {}°C before {}..."), target, label);
    spdlog::info("[RecoveryModal] Nozzle at {}C is below min_extrude; preheating to {}C "
                 "before {} ({})",
                 nozzle_current_c(), target, tag, gcode);
}

void RecoveryModalPresenter::poll_preheat() {
    if (nozzle_current_c() >= (pending_target_c_ - TEMP_THRESHOLD_C)) {
        const std::string gcode = pending_gcode_;
        const std::string tag = pending_tag_;
        const int reached = nozzle_current_c();
        clear_preheat();
        spdlog::info("[RecoveryModal] Nozzle reached {}C; running deferred recovery {} ({})",
                     reached, tag, gcode);
        dispatch_recovery(gcode, tag);
        return;
    }

    if (--polls_remaining_ > 0) {
        return;
    }

    // Bounded: a heater that never arrives (thermistor fault, MCU shutdown, the
    // user cancelling the heat elsewhere) must not leave a recovery armed
    // forever, and must not fire it cold either.
    const std::string tag = pending_tag_;
    const std::string label = pending_label_;
    const int target = pending_target_c_;
    const int current = nozzle_current_c();
    clear_preheat();

    spdlog::error("[RecoveryModal] Preheat to {}C timed out at {}C after {}s; abandoning {}",
                  target, current, preheat_budget_ms_ / 1000, tag);
    NOTIFY_ERROR(lv_tr("Nozzle did not reach {}°C — {} cancelled"), target, label);
}

void RecoveryModalPresenter::cancel_preheat_timer() {
    if (preheat_timer_ && lv_is_initialized()) {
        helix::ui::lv_timer_cancel_safe(preheat_timer_);
    }
    preheat_timer_ = nullptr;
}

void RecoveryModalPresenter::clear_preheat() {
    cancel_preheat_timer();
    pending_gcode_.clear();
    pending_tag_.clear();
    pending_label_.clear();
    pending_target_c_ = 0;
    polls_remaining_ = 0;
}

void RecoveryModalPresenter::preheat_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<RecoveryModalPresenter*>(lv_timer_get_user_data(timer));
    if (self) {
        self->poll_preheat();
    }
}

} // namespace helix::ui
