// SPDX-License-Identifier: GPL-3.0-or-later
#include "ams_error_bridge.h"

#include "ui_error_reporting.h"
#include "ui_panel_ams.h"

#include "ams_state.h"
#include "fault_surface_correlation.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "post_op_cooldown_manager.h"

#include <spdlog/spdlog.h>

namespace helix {

AmsErrorBridge::AmsErrorBridge(helix::ui::RecoveryModalPresenter& presenter)
    : presenter_(presenter) {}

void AmsErrorBridge::start() {
    // One-shot: Application calls this once. Re-calling would reinstall the
    // observer while leaving prev_action_/presented_ stale — don't.
    action_observer_ = helix::ui::observe_int_sync<AmsErrorBridge>(
        AmsState::instance().get_ams_action_subject(), this,
        [](AmsErrorBridge* self, int action) { self->on_action_changed(action); },
        AmsState::instance().get_subjects_lifetime());

    // Re-consult current_error() when the operation detail moves while the
    // action is still ERROR. Without this, a fault whose text changes while
    // AmsAction stays ERROR never re-presents and the user keeps reading the
    // first message. present() dedups on (detail, action-set), so a detail
    // change that does not move current_error() is absorbed as a no-op.
    detail_observer_ = helix::ui::observe_string<AmsErrorBridge>(
        AmsState::instance().get_ams_action_detail_subject(), this,
        [](AmsErrorBridge* self, const char* detail) { self->on_detail_changed(detail); },
        AmsState::instance().get_subjects_lifetime());
}

void AmsErrorBridge::on_action_changed(int action) {
    const bool now_error = (action == static_cast<int>(AmsAction::ERROR));
    const bool was_error = (prev_action_ == static_cast<int>(AmsAction::ERROR));
    prev_action_ = action;
    if (now_error && !was_error) {
        // Disarm the post-op cooldown an EARLIER operation left running. Nothing
        // else cancels it on a fault, so ~120s after the error it zeroes the
        // extruder and whichever recovery the user taps next runs into a cold
        // nozzle — failing exactly the way the operation that faulted did.
        //
        // Before the early returns below, deliberately: AmsBackendAfc's
        // current_error() answers nullopt whenever AFC itself has not set
        // error_state — the stuck-action latch drives the action to ERROR on
        // its own — so anything placed after that check would not run on the
        // path this cancel exists for.
        PostOpCooldownManager::instance().cancel();

        // A new fault episode opens here. Anything the user dismissed or acted
        // on during an earlier one stops suppressing, so a fault identical to a
        // previously answered one is shown again instead of being swallowed.
        presenter_.forget_handled_fault();

        auto* backend = AmsState::instance().get_backend();

        // Snapshot what the backend was doing. AFC's local action timeout
        // preserves it as "<detail> (timed out)", which is the only description
        // of the fault that exists on that path.
        error_detail_ = backend ? backend->get_system_info().operation_detail : std::string();

        // Last-resort fallback, armed on every ERROR edge and armed BEFORE the
        // early returns for the same reason the cooldown cancel is. It only
        // toasts if nothing else surfaced the fault; see
        // surface_unhandled_error(). Deferred by one queue tick so the other
        // observers on this subject (AmsPanel's loading-error dialog) and
        // GcodeErrorRouter get their chance first — observe_int_sync queues
        // every handler, so at this instant none of them have run yet and an
        // inline check would toast and then get a modal stacked on top.
        // process_pending() swaps the queue out before draining, so this lands
        // in the following tick, after the whole current batch.
        lifetime_.defer("AmsErrorBridge::surface_unhandled_error",
                        [this]() { surface_unhandled_error(); });

        if (!backend)
            return;
        auto ev = backend->current_error();
        if (ev) {
            spdlog::debug("[AmsErrorBridge] presenting error: {}", ev->detail);
            presenter_.present(*ev);
            presented_ = true;
        }
    } else if (!now_error && was_error && presented_) {
        spdlog::debug("[AmsErrorBridge] dismissing error modal (action exited ERROR)");
        presenter_.dismiss();
        presented_ = false;
    }
}

void AmsErrorBridge::on_detail_changed(const char* /*detail*/) {
    // Only mid-episode: the rising edge is on_action_changed's job, and a detail
    // change outside ERROR is normal operation chatter (Loading/Unloading/etc.).
    // present_ gates the edge-had-no-ErrorEvent case: if the edge produced no
    // modal (AFC's stuck-action latch), surface_unhandled_error() toasted the
    // detail and re-presenting an ErrorEvent that arrived later would stack a
    // modal on top of that toast.
    if (lv_subject_get_int(AmsState::instance().get_ams_action_subject()) !=
            static_cast<int>(AmsAction::ERROR) ||
        !presented_) {
        return;
    }
    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    auto ev = backend->current_error();
    if (!ev) {
        // The fault cleared under our feet before the action subject fell; leave
        // the existing modal and let the falling-edge dismiss handle it.
        return;
    }
    spdlog::debug("[AmsErrorBridge] re-consulting error on detail change mid-episode: {}",
                  ev->detail);
    // present() decides whether this is actually new. It suppresses both a fault
    // already on screen and one the user has answered this episode — the latter
    // matters here because a dismiss-only event's single {"OK",""} action closes
    // the modal without any callback, so every subsequent detail change would
    // otherwise pop the identical dialog straight back up.
    presenter_.present(*ev);
    // presented_ stays true: this is the same ERROR episode, and the falling-edge
    // dismiss in on_action_changed still fires when action exits ERROR.
}

void AmsErrorBridge::surface_unhandled_error() {
    // The fault can clear inside the tick we waited out (a macro recovered it,
    // another client cleared it). Say nothing about an error that is over.
    if (lv_subject_get_int(AmsState::instance().get_ams_action_subject()) !=
        static_cast<int>(AmsAction::ERROR)) {
        return;
    }
    if (fault_already_on_screen()) {
        return;
    }

    // ERROR, not WARNING: the operation the user asked for did not happen and
    // the machine is sitting in a fault state. A warning-coloured toast reads
    // as "heads up" next to the spinner that just stopped.
    const std::string message = error_detail_.empty()
                                    ? std::string(lv_tr("The filament system reported an error"))
                                    : error_detail_;
    spdlog::warn("[AmsErrorBridge] fault surfaced by nothing else; toasting: {}", message);
    // Claim it, so a `!!` for the same fault arriving after us (a backend that
    // raises ERROR ahead of Klipper's broadcast) does not stack a second toast.
    // `message` and not error_detail_: whichever string we are about to put on
    // screen is the one a later `!!` has to match. When it is the generic
    // fallback nothing will ever match it, which is correct — that string names
    // no fault.
    fault_surface_correlation::record_surfaced(message);
    NOTIFY_ERROR_T(lv_tr("Filament System Error"), "{}", message);
}

bool AmsErrorBridge::fault_already_on_screen() const {
    // Our own present() above, and equally the recovery modal GcodeErrorRouter
    // raises for a classified `!!` line — both end up in this one presenter.
    if (presented_ || presenter_.is_visible()) {
        return true;
    }

    // AmsPanel's loading-error dialog. get_existing_ams_panel(), never
    // get_global_ams_panel(): the latter lazily builds the entire AMS UI, and
    // the user never having opened that panel is precisely the case this
    // fallback exists for.
    //
    // Deliberately NOT ModalStack::empty(): any unrelated modal being open
    // (settings, a confirmation) would suppress a genuine fault notification,
    // and the stack carries no way to tell "a dialog about this fault" from
    // "a dialog". The two specific checks above cover every dialog that
    // actually describes an AMS fault.
    const AmsPanel* panel = get_existing_ams_panel();
    if (panel && panel->is_error_modal_visible()) {
        return true;
    }

    // GcodeErrorRouter classified the same `!!` line to a toast, or to the
    // plain non-recovery modal — neither of which touches presenter_, so
    // nothing above sees them and the user gets two notifications for one
    // fault (#1197). The registry is keyed on the fault text precisely so this
    // stays "a toast about THIS fault": ToastManager::is_visible() is blanket,
    // and consulting it would let an unrelated toast silence a real error,
    // which is the same objection that ruled out ModalStack::empty().
    //
    // AFC makes the match exact rather than approximate: it emits `!! <msg>`
    // and queues the byte-identical string into AFC.message, which is where
    // error_detail_ comes from.
    return fault_surface_correlation::was_recently_surfaced(error_detail_);
}

} // namespace helix
