// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_error_router.h"

#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"

#include "ams_state.h"
#include "app_globals.h"
#include "error_classify.h"
#include "error_event.h"
#include "error_modal_view.h"
#include "fault_surface_correlation.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "print_control_buttons.h"
#include "printer_recovery_service.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "rpc_error_correlation.h"

#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
#endif

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <vector>

namespace helix {

namespace {

constexpr const char* NOTIFY_HANDLER_NAME = "gcode_error_notifier";
constexpr const char* REPLAY_OBSERVER_NAME = "gcode_store_replay";

/// The one recovery action that does NOT run through execute_gcode: key298's
/// rpi-MCU-bridge bounce goes via PrinterRecoveryService and so carries an
/// empty gcode. Matched on the tag error_classify.cpp:73 stamps, because an
/// empty gcode alone is not a reliable signal — see present_recover_toast().
constexpr const char* KEY298_RECOVER_TAG = "error_classify::key298_recover";

/// How long a recover toast stays tappable. Longer than a plain toast: the
/// user has to read the fault and decide, not just notice it.
constexpr uint32_t RECOVER_TOAST_MS = 15000;

/// Owns one recover toast's action for as long as the toast can be tapped.
/// See present_recover_toast() for why this is heap-allocated with a timer
/// reaper rather than parked on the router.
struct RecoverToastCtx {
    IMoonrakerAPI* api = nullptr;
    std::string gcode;
    std::string log_tag;
};

/// Replay age gate: a latched `!!` older than this in the gcode_store is
/// considered stale and is NOT re-surfaced on reconnect.
///
/// Sized to cover the legitimate case -- an error fired during a brief
/// WebSocket bounce or boot-autostart hiccup that the user genuinely
/// missed -- while killing the stale-after-restart case (#991): a UI
/// restart on a paused print replayed a 287s-old `[print_task_config]`
/// error as a blocking modal that sat over the print panel and blocked
/// Resume. 30s comfortably spans a reconnect blip but is far below the
/// multi-minute gap a manual restart leaves. (Was 600s, which let the
/// 287s error through.)
constexpr double REPLAY_MAX_AGE_SECONDS = 30.0;

/// gcode_store fetch depth on reconnect. The K2's box driver is chatty
/// (status polls every ~3s) so we need headroom to find a `!!` line.
constexpr int REPLAY_FETCH_COUNT = 50;

double now_unix_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

GcodeErrorRouter::GcodeErrorRouter(IMoonrakerAPI* api, IMoonrakerClient* client,
                                   helix::ui::RecoveryModalPresenter& presenter)
    : api_(api), client_(client), presenter_(presenter) {
    if (!client_) {
        spdlog::warn("[GcodeErrorRouter] Null client -- handlers not registered");
        return;
    }

    // [L072] Both registrations run on the WS thread. MoonrakerClient
    // copies the callback list under lock and invokes outside it, so
    // unregister_method_callback / remove_connected_observer in our dtor
    // do NOT block in-flight invocations. lifetime_.bg_cb wraps the
    // delivery: when the WS thread fires the wrapper, it queues `fn` to
    // the main thread with a generation snapshot; on main-thread dispatch
    // the gen is re-checked, so a callback that fires after the dtor
    // invalidates `lifetime_` is silently dropped.
    client_->register_method_callback(
        "notify_gcode_response", NOTIFY_HANDLER_NAME,
        lifetime_.bg_cb("GcodeErrorRouter::on_notify",
                        [this](const nlohmann::json& msg) { on_notify_gcode_response(msg); }));

    // Reconnect replay. Fires on WS open + Klippy ready transitions.
    // bg_cb takes a 0-arg callback fine -- the lambda below doesn't need
    // arguments; the wrapper just defers and gen-checks.
    client_->add_connected_observer(
        REPLAY_OBSERVER_NAME,
        lifetime_.bg_cb("GcodeErrorRouter::on_connected", [this]() { on_connected(); }));
}

GcodeErrorRouter::~GcodeErrorRouter() {
    // Erase the map entries so no NEW invocations start after this point.
    // In-flight invocations (already past the map lookup, queued for dispatch)
    // are handled by lifetime_'s generation guard -- see the bg_cb usage in
    // the ctor. lifetime_ destructs after this body returns and invalidates
    // all outstanding tokens, so any deferred body that lands on main after
    // the unregister is silently dropped.
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", NOTIFY_HANDLER_NAME);
        client_->remove_connected_observer(REPLAY_OBSERVER_NAME);
    }
}

bool GcodeErrorRouter::should_surface_replay(double entry_time, double now) {
    // Unavailable/zero timestamp: age cannot be positively determined.
    // Never suppress a possibly-fresh error on missing data -- preserve the
    // legacy surface-it behavior (the live `!!` path is unaffected by this
    // gate regardless).
    if (entry_time <= 0.0)
        return true;

    const double age = now - entry_time;
    // Clock skew or an entry stamped in the (apparent) future reads as a
    // negative age; treat as fresh rather than silently suppressing.
    if (age <= REPLAY_MAX_AGE_SECONDS)
        return true;

    spdlog::debug("[GcodeError replay] Skipping stale `!!` (age {:.0f}s)", age);
    return false;
}

// clean_error_text() lives in gcode_error_text.cpp — split out so the ESP32
// firmware slice links it without this TU's error-routing dependency web.

std::string GcodeErrorRouter::truncate_for_toast(std::string text) {
    // UTF-8 byte truncation is not strictly correct (could land mid-codepoint),
    // but matches prior behavior. The right long-term fix is wrapping text in
    // ToastManager; at that point this goes away.
    constexpr size_t MAX_LEN = 80;
    if (text.size() > MAX_LEN) {
        text = text.substr(0, MAX_LEN - 3) + "...";
    }
    return text;
}

// Cross-source dedup lookup, kept in ONE normalization on both sides.
//
// MoonrakerRequestTracker::route_response() records the RAW Klipper-supplied
// `error.message`, so the identity we look up is `raw_detail` — Klipper's
// wording before clean_error_text() rewrote it. Matching on `detail` alone
// misses every message the cleaner touches ("Must home axis first" ->
// "Must home axes first"), which double-toasts a single rejection.
//
// `detail` stays as a fallback for classifiers that leave `raw_detail` empty
// (the AMS backends, whose detail already IS the raw text). The match remains
// exact-string on both arms — see include/rpc_error_correlation.h for why
// substring matching is deliberately avoided.
static bool already_reported_via_rpc(const std::string& raw_detail, const std::string& detail) {
    if (!raw_detail.empty() && rpc_error_correlation::was_recently_handled(raw_detail))
        return true;
    return rpc_error_correlation::was_recently_handled(detail);
}

PresentAs decide_presentation(const ErrorEvent& e) {
    const bool has_recover = !e.recovery_actions.empty();
    if (e.severity == ErrorSeverity::CRITICAL)
        return has_recover ? PresentAs::MODAL_WITH_RECOVER : PresentAs::MODAL;
    if (e.severity == ErrorSeverity::WARNING)
        return has_recover ? PresentAs::TOAST_WITH_RECOVER : PresentAs::TOAST;
    return PresentAs::NONE; // INFO not surfaced in L0
}

PromptData build_recovery_prompt(const ErrorEvent& e) {
    PromptData p;
    p.title = e.title.empty() ? std::string(lv_tr("Printer Error")) : e.title;
    if (!e.detail.empty())
        p.text_lines.push_back(e.detail);
    for (const auto& a : e.recovery_actions) {
        PromptButton b;
        b.label = a.label;
        b.gcode = a.gcode;
        b.color = helix::ui::color_for_style(a.style);
        p.buttons.push_back(std::move(b));
    }
    return p;
}

void GcodeErrorRouter::present_recovery_modal(const ErrorEvent& e) {
    presenter_.present(e);
}

RecoverDispatch decide_recover_dispatch(const ErrorEvent& e) {
    if (e.recovery_actions.empty())
        return RecoverDispatch::PLAIN_TOAST;

    const RecoveryAction& action = e.recovery_actions.front();

    // A toast carries exactly ONE action button and has no preheat gate. Two
    // shapes therefore cannot be rendered faithfully here and go to the
    // recovery modal instead, which builds a button per action and owns the
    // preheat-then-send path (#1193):
    //   - several actions: a toast would silently drop all but the first
    //   - an action needing a hot nozzle: firing it cold fails exactly the way
    //     the operation that raised the error did
    if (e.recovery_actions.size() > 1 || action.needs_hot_nozzle)
        return RecoverDispatch::ESCALATE_TO_MODAL;

    // key298 -- rpi MCU bridge daemon shutdown. firmware_restart alone can't
    // recover; PrinterRecoveryService bounces klipper_mcu via the platform
    // recovery script. Recognised by its log_tag rather than by its empty
    // gcode, because an empty gcode now legitimately means "dismiss".
    if (action.log_tag == KEY298_RECOVER_TAG)
        return RecoverDispatch::RECOVERY_SERVICE;

    // No gcode and no service behind it: nothing to run, so no button.
    if (action.gcode.empty())
        return RecoverDispatch::PLAIN_TOAST;

    return RecoverDispatch::GCODE;
}

bool GcodeErrorRouter::present_recover_toast(const ErrorEvent& e) {
    const RecoverDispatch how = decide_recover_dispatch(e);

    if (how == RecoverDispatch::ESCALATE_TO_MODAL) {
        spdlog::debug("[GcodeError] Escalating WARNING+recover to modal ({} actions)",
                      e.recovery_actions.size());
        present_recovery_modal(e);
        return true;
    }

    // Nothing runnable, or no actions at all: surface the fault as a plain
    // toast rather than a button that silently does nothing. Deliberately NOT
    // returning false, which would leave the WARNING unshown entirely.
    if (how == RecoverDispatch::PLAIN_TOAST) {
        spdlog::warn("[GcodeError] Recovery action has nothing to run; plain toast for: {}",
                     e.detail);
        present_deferred_toast(e.detail, e.raw_detail);
        return true;
    }

    if (!api_)
        return false; // No API client -> recovery would be a no-op; nothing actionable to show.

    const RecoveryAction& action = e.recovery_actions.front();

    if (how == RecoverDispatch::RECOVERY_SERVICE) {
        IMoonrakerAPI* api = api_;
        ToastManager::instance().show_with_action(
            ToastSeverity::ERROR, truncate_for_toast(e.detail).c_str(), action.label.c_str(),
            [](void* ud) {
                auto* a = static_cast<IMoonrakerAPI*>(ud);
                if (!a)
                    return;
                spdlog::info("[GcodeError] User tapped Recover for key298");
                PrinterRecoveryService recovery(a);
                recovery.recover(
                    []() { spdlog::info("[Recovery] Auto-recovery initiated"); },
                    [](const MoonrakerError& err) {
                        spdlog::error("[Recovery] Auto-recovery failed: {}", err.message);
                        ToastManager::instance().show(
                            ToastSeverity::ERROR,
                            (std::string(lv_tr("Recovery failed: ")) + err.user_message()).c_str(),
                            6000);
                    });
            },
            api, /*duration_ms=*/RECOVER_TOAST_MS);
        return true;
    }

    // ToastManager's action callback takes a bare void* and never frees it,
    // and it only runs if the user taps -- so a per-toast heap context needs
    // its own reaper or every toast that simply expires leaks one. A one-shot
    // timer outliving the toast frees it exactly once. Both run on the main
    // thread, so the tap and the reaper cannot race.
    auto* ctx = new RecoverToastCtx{api_, action.gcode, action.log_tag};
    ToastManager::instance().show_with_action(
        ToastSeverity::ERROR, truncate_for_toast(e.detail).c_str(), action.label.c_str(),
        [](void* ud) {
            auto* c = static_cast<RecoverToastCtx*>(ud);
            if (!c || !c->api)
                return;
            spdlog::info("[GcodeError] User tapped recovery '{}' -> {}", c->log_tag, c->gcode);
            const std::string tag = c->log_tag;
            c->api->execute_gcode(
                c->gcode, [tag]() { spdlog::info("[Recovery] {} completed", tag); },
                [tag](const MoonrakerError& err) {
                    spdlog::error("[Recovery] {} failed: {}", tag, err.message);
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        (std::string(lv_tr("Recovery failed: ")) + err.user_message()).c_str(),
                        6000);
                },
                IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
        },
        ctx, /*duration_ms=*/RECOVER_TOAST_MS);

    auto* reaper = lv_timer_create(
        [](lv_timer_t* t) {
            delete static_cast<RecoverToastCtx*>(lv_timer_get_user_data(t));
            lv_timer_delete(t);
        },
        RECOVER_TOAST_MS + 5000, ctx);
    lv_timer_set_repeat_count(reaper, 1);
    return true;
}

void GcodeErrorRouter::present_deferred_toast(const std::string& text,
                                              const std::string& raw_text) {
    // Deferred toast for unclassified errors -- gives the late-arrival
    // RPC error response a chance to populate the correlation buffer
    // before we re-check at fire time. `raw` is carried alongside `clean`
    // so the re-check matches the same identity process_line() used.
    struct DeferredCtx {
        std::string clean;
        std::string raw;
        std::string short_form;
    };
    auto* dctx = new DeferredCtx{text, raw_text, truncate_for_toast(text)};
    auto* dt = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* c = static_cast<DeferredCtx*>(lv_timer_get_user_data(timer));
            if (c) {
                if (already_reported_via_rpc(c->raw, c->clean)) {
                    spdlog::info("[GcodeError] Suppressing deferred `!!` toast "
                                 "(caller-handled RPC error arrived after): {}",
                                 c->clean);
                } else {
                    ui_notification_error(lv_tr("Klipper Error"), c->short_form.c_str(),
                                          /*modal=*/false);
                }
                delete c;
            }
            lv_timer_delete(timer);
        },
        150, dctx);
    lv_timer_set_repeat_count(dt, 1);
}

void GcodeErrorRouter::process_line(const std::string& line) {
    if (line.empty())
        return;

    // Build classify context from current printer state. process_line runs
    // on the MAIN thread (the ctor's lifetime_.bg_cb wrapper defers the
    // notify body to main), so these synchronous getters are safe.
    ClassifyContext ctx;
    ctx.is_paused = get_printer_state().is_paused();
    // RAW_PRINT_STATE_OK: classifiers offer resume/retry actions off this flag.
    // A failure inside a host-side pre-start block is PrintPreparationManager's
    // to report, and claiming a print is running would offer a Resume that has
    // nothing to resume.
    ctx.is_printing = get_printer_state().get_print_job_state() == PrintJobState::PRINTING;

    // Ask the active AMS backend first (domain-aware); else the generic
    // classifier. get_backend() may return nullptr -- guarded.
    std::optional<ErrorEvent> ev;
    if (auto* backend = AmsState::instance().get_backend())
        ev = backend->classify_error(line, ctx);
    if (!ev)
        ev = error_classify::classify(line, ctx);
    if (!ev)
        return;

    spdlog::error("[GcodeError] sev={} src={} code={}: {}", static_cast<int>(ev->severity),
                  static_cast<int>(ev->source), ev->code.empty() ? "-" : ev->code, ev->detail);

    // Release an optimistic Pause/Resume spinner. Klipper aborts a rejected
    // RESUME by broadcasting `!!` and returning normally, so the gcode.script
    // RPC still answers `ok` and the dispatch error path never runs; the button
    // would otherwise sit disabled on "Resuming..." for the full 150s backstop.
    // Deliberately BEFORE the RPC-dedup return below — that suppression is about
    // avoiding a duplicate *toast*, not about whether the command failed.
    ui::PrintControlButtons::instance().notify_printer_error(ev->detail);

    // Cross-source dedup: when an RPC caller triggered the gcode that
    // emitted this error, the caller's error_cb already surfaced a
    // contextual toast. Skipping our generic surfacing avoids double-
    // notification for the same root cause. (The deferred-toast path
    // re-checks at fire time for late-arriving RPC responses.)
    if (already_reported_via_rpc(ev->raw_detail, ev->detail)) {
        spdlog::info("[GcodeError] Suppressing duplicate (RPC-handled): {}", ev->detail);
        return;
    }

    const PresentAs how = decide_presentation(*ev);
    if (how == PresentAs::NONE) {
        // INFO is not surfaced in L0. Returning here rather than falling into
        // the switch keeps record_surfaced() below meaning "the user saw this"
        // -- recording an event we never showed would make the bridge stand
        // down on a fault that is genuinely on nobody's screen.
        return;
    }

    // AmsErrorBridge's last-resort fallback may already have toasted this exact
    // fault off the AmsAction::ERROR edge -- the reverse of the ordering the
    // bridge itself guards against. Only the toast arms defer to it: a modal
    // carries the recovery actions, so standing it down because a transient
    // toast got there first would drop the only actionable thing on screen.
    // Checked BEFORE record_surfaced() below, so our own record can never
    // suppress the presentation that wrote it.
    if ((how == PresentAs::TOAST || how == PresentAs::TOAST_WITH_RECOVER) &&
        (fault_surface_correlation::was_recently_surfaced(ev->detail) ||
         fault_surface_correlation::was_recently_surfaced(ev->raw_detail))) {
        spdlog::info("[GcodeError] Suppressing duplicate toast (already surfaced): {}", ev->detail);
        return;
    }

    bool surfaced = true;
    switch (how) {
    case PresentAs::MODAL:
        // CRITICAL without a recovery action -- see helix::ui::modal_title_for().
        ui_notification_printer_fault(helix::ui::modal_title_for(*ev), ev->detail.c_str());
        break;
    case PresentAs::MODAL_WITH_RECOVER:
        present_recovery_modal(*ev);
        break;
    case PresentAs::TOAST_WITH_RECOVER:
        surfaced = present_recover_toast(*ev);
        break;
    case PresentAs::TOAST:
        // Claimed here, not when the 150ms timer fires: AmsErrorBridge's
        // deferred re-check runs one UpdateQueue tick after the ERROR edge and
        // would otherwise find nothing recorded and toast on top of a toast
        // that is already scheduled.
        present_deferred_toast(ev->detail, ev->raw_detail);
        break;
    case PresentAs::NONE:
        return; // unreachable -- handled above; kept for switch exhaustiveness
    }

    if (!surfaced)
        return;

    // Claim this fault. AmsErrorBridge's fallback cannot see a toast, nor the
    // plain modal above -- neither goes through RecoveryModalPresenter -- so
    // this record is the only signal that stops it adding a second
    // notification for the fault just shown. Both spellings are recorded: the
    // bridge holds the backend's operation_detail, which matches Klipper's raw
    // wording, while clean_error_text() may have rewritten `detail`.
    fault_surface_correlation::record_surfaced(ev->detail);
    if (ev->raw_detail != ev->detail) {
        fault_surface_correlation::record_surfaced(ev->raw_detail);
    }
}

void GcodeErrorRouter::on_notify_gcode_response(const nlohmann::json& msg) {
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const auto& params = msg["params"];
    if (params[0].is_array()) {
        for (const auto& line : params[0]) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    } else if (params[0].is_string()) {
        for (const auto& line : params) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    }
}

void GcodeErrorRouter::on_connected() {
    if (!client_)
        return;
    // [L072] get_gcode_store's success callback fires on the WS thread when
    // Moonraker responds. The request tracker holds the callback for the
    // duration of the RPC, so a late response delivered after our dtor would
    // otherwise re-enter `this` on freed memory. bg_cb defers to main with
    // a generation guard.
    client_->get_gcode_store(
        REPLAY_FETCH_COUNT,
        lifetime_.bg_cb(
            "GcodeErrorRouter::replay_response",
            [this](const std::vector<GcodeStoreEntry>& entries) {
                // gcode_store is oldest-first; walk reverse for newest.
                for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                    if (it->type != "response")
                        continue;
                    const std::string& raw = it->message;
                    if (raw.size() < 3 || raw[0] != '!' || raw[1] != '!')
                        continue;

                    const double now = now_unix_seconds();
                    if (!should_surface_replay(it->time, now)) {
                        // should_surface_replay logs the stale skip at debug.
                        return;
                    }
                    const double age = now - it->time;

                    {
                        std::lock_guard<std::mutex> lock(replay_mutex_);
                        if (it->time == last_replayed_time_) {
                            spdlog::debug("[GcodeError replay] Already replayed t={}", it->time);
                            return;
                        }
                        last_replayed_time_ = it->time;
                    }

                    std::string clean =
                        (raw.size() > 3 && raw[2] == ' ') ? raw.substr(3) : raw.substr(2);
                    std::string code;
                    clean_error_text(clean, code);

                    spdlog::info(
                        "[GcodeError replay] Surfacing prior `!!` (age {:.0f}s, code={}): {}", age,
                        code.empty() ? "-" : code, clean);

                    // Replay is always modal -- the user was disconnected; a
                    // transient toast they can miss isn't enough on first
                    // reconnect. Modal dedup-by-title prevents the live
                    // notify_gcode_response (if Klippy re-emits) from
                    // duplicating.
                    const char* title = (code.size() >= 4 && code.compare(0, 4, "key8") == 0)
                                            ? lv_tr("Filament System Error")
                                            : lv_tr("Printer Error");
                    ui_notification_printer_fault(title, clean.c_str());
                    return;
                }
            }),
        [](const MoonrakerError& err) {
            spdlog::debug("[GcodeError replay] gcode_store query failed: {}", err.message);
        });
}

} // namespace helix
