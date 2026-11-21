// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shutdown_widget.h"

#include "ui_event_safety.h"
#include "ui_shutdown_modal.h"
#include "ui_split_button.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "display_backend.h"
#include "display_manager.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "platform_info.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "system_power.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

namespace helix {

namespace {

// Split-button dropdown indices (must match XML option order:
// "Both\nPrinter\nScreen" in shutdown_modal.xml).
constexpr uint32_t SCOPE_BOTH = 0;
constexpr uint32_t SCOPE_PRINTER = 1;
constexpr uint32_t SCOPE_SCREEN = 2;

// After a successful machine.shutdown/reboot, Moonraker replies OK but the
// OS-level shutdown can silently no-op on some firmwares (observed on SonicPad
// Jpe230 — logind.PowerOff returns without initiating the shutdown). If the
// WebSocket has reconnected within this window, the host is clearly still up.
constexpr uint32_t VERIFICATION_WINDOW_MS = 20000;

struct VerifyCtx {
    IMoonrakerAPI* api;
    bool is_reboot;
};

void verify_host_down_timer_cb(lv_timer_t* timer) {
    std::unique_ptr<VerifyCtx> ctx(static_cast<VerifyCtx*>(lv_timer_get_user_data(timer)));
    lv_timer_delete(timer);

    if (!ctx || !ctx->api || !ctx->api->is_connected()) {
        return;
    }

    const char* action = ctx->is_reboot ? "reboot" : "shutdown";
    spdlog::warn("[ShutdownDialog] Host still reachable {}s after {} — {} silently failed",
                 VERIFICATION_WINDOW_MS / 1000, action, action);

    const char* msg = ctx->is_reboot ? lv_tr("Reboot failed — host is still reachable")
                                     : lv_tr("Shutdown failed — host is still reachable");
    ToastManager::instance().show(ToastSeverity::ERROR, msg, 6000);
}

// Invoked from Moonraker WebSocket (background) thread. Creating an lv_timer
// is not thread-safe, so hop to the main thread via queue_update.
void schedule_host_down_verification(IMoonrakerAPI* api, bool is_reboot) {
    if (!api) {
        return;
    }
    helix::ui::queue_update("ShutdownDialog::verify", [api, is_reboot]() {
        auto* ctx = new VerifyCtx{api, is_reboot};
        lv_timer_create(verify_host_down_timer_cb, VERIFICATION_WINDOW_MS, ctx);
    });
}

// Walk up from the clicked button to the view root stamped in
// ShutdownModal::on_show(), then read the user_data pointer set there.
// LVGL XML appends an instance suffix to view names (e.g. "shutdown_modal_#0"),
// so we match the prefix and require the next char be the suffix delimiter or end.
//
// `lv_event_get_current_target_obj(e)` returns the listener-attached object —
// for an XML <event_cb> registered on <ui_split_button>, that's the split
// button itself. We don't depend on which descendant emitted the event; the
// walk-up only needs to reach the named view-root ancestor (per L069).
ShutdownModal* find_shutdown_modal(lv_event_t* e) {
    constexpr const char* VIEW_NAME = "shutdown_modal";
    constexpr size_t VIEW_NAME_LEN = 14;
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && std::strncmp(name, VIEW_NAME, VIEW_NAME_LEN) == 0 &&
            (name[VIEW_NAME_LEN] == '\0' || name[VIEW_NAME_LEN] == '_')) {
            return static_cast<ShutdownModal*>(lv_obj_get_user_data(obj));
        }
        obj = lv_obj_get_parent(obj);
    }
    return nullptr;
}

// ---- Action helpers -------------------------------------------------------
//
// Free functions so any caller of show_shutdown_dialog() (home-panel widget,
// AdvancedPanel power rows) gets the same printer/screen/both behavior —
// including the dual-host async ordering for "both shutdown" / "both reboot",
// where the local SystemPower call is deferred until the printer-side ack.

// Powers this host down. Returns true if the action was initiated. Test mode
// reports success without touching the dev machine. Once the request is
// dispatched the panel holds opaque black until init stops the process; see
// power_then_hold_black().
bool invoke_local_power(bool is_reboot) {
    if (auto* rc = get_runtime_config(); rc && rc->test_mode) {
        spdlog::warn("[ShutdownDialog] TEST MODE: skipping SystemPower::{}_local() — "
                     "would have {} the dev host",
                     is_reboot ? "reboot" : "shutdown", is_reboot ? "rebooted" : "powered off");
        ToastManager::instance().show(
            ToastSeverity::INFO,
            is_reboot ? "TEST: would reboot screen" : "TEST: would shut down screen", 4000);
        return true;
    }

    auto* dm = DisplayManager::instance();
    return power_then_hold_black(
        [is_reboot]() {
            return is_reboot ? helix::SystemPower::reboot_local()
                             : helix::SystemPower::shutdown_local();
        },
        [dm]() {
            if (dm) {
                dm->hold_flush_for_power_off();
            }
        },
        [dm]() {
            if (dm && dm->backend()) {
                dm->backend()->clear_framebuffer(0xFF000000);
            }
        });
}

// Moonraker error callbacks arrive on the WebSocket thread; SystemPower's
// std::system() call and the fallback's logging belong on the main thread,
// matching the tok.defer() hop the "both" flows already use.
void queue_machine_power_failure(const MoonrakerError& err, bool is_reboot,
                                 bool allow_local_fallback) {
    const std::string message = err.message;
    helix::ui::queue_update("ShutdownDialog::power_failed",
                            [message, is_reboot, allow_local_fallback]() {
                                helix::handle_machine_power_failure(
                                    message, is_reboot, allow_local_fallback,
                                    [is_reboot]() { return invoke_local_power(is_reboot); });
                            });
}

// @p allow_local_fallback is set only when Moonraker runs on this same host —
// see handle_machine_power_failure().
void execute_printer_shutdown(IMoonrakerAPI* api, bool allow_local_fallback = false) {
    if (!api)
        return;
    spdlog::info("[ShutdownDialog] Executing machine shutdown");
    api->machine_shutdown(
        [api]() {
            spdlog::info("[ShutdownDialog] Machine shutdown command sent successfully");
            schedule_host_down_verification(api, /*is_reboot=*/false);
        },
        [allow_local_fallback](const MoonrakerError& err) {
            queue_machine_power_failure(err, /*is_reboot=*/false, allow_local_fallback);
        });
}

void execute_printer_reboot(IMoonrakerAPI* api, bool allow_local_fallback = false) {
    if (!api)
        return;
    spdlog::info("[ShutdownDialog] Executing machine reboot");
    api->machine_reboot(
        [api]() {
            spdlog::info("[ShutdownDialog] Machine reboot command sent successfully");
            schedule_host_down_verification(api, /*is_reboot=*/true);
        },
        [allow_local_fallback](const MoonrakerError& err) {
            queue_machine_power_failure(err, /*is_reboot=*/true, allow_local_fallback);
        });
}

void execute_screen_shutdown() {
    spdlog::info("[ShutdownDialog] Executing local screen shutdown");
    if (!invoke_local_power(/*is_reboot=*/false)) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Screen shutdown failed"), 6000);
    }
}

void execute_screen_reboot() {
    spdlog::info("[ShutdownDialog] Executing local screen reboot");
    if (!invoke_local_power(/*is_reboot=*/true)) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Screen reboot failed"), 6000);
    }
}

// Single-scope local power: on a same-host screen this host IS the printer,
// so a failure reads as the machine's, not the screen's. A connected Moonraker
// gets the request next, since the host verdict can be wrong (a container
// without /run/systemd, a localhost tunnel to another machine).
void execute_host_power(IMoonrakerAPI* api, bool is_reboot) {
    spdlog::info("[ShutdownDialog] Executing local {} of this host",
                 is_reboot ? "reboot" : "shutdown");
    const bool dispatched = invoke_local_power(is_reboot);
    switch (host_power_follow_up(dispatched, api && api->is_connected())) {
    case HostPowerFollowUp::Done:
        break;
    case HostPowerFollowUp::RetryViaMoonraker:
        spdlog::info("[ShutdownDialog] Local {} failed — asking Moonraker",
                     is_reboot ? "reboot" : "shutdown");
        if (is_reboot) {
            execute_printer_reboot(api);
        } else {
            execute_printer_shutdown(api);
        }
        break;
    case HostPowerFollowUp::Toast:
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      is_reboot ? lv_tr("Reboot failed") : lv_tr("Shutdown failed"),
                                      6000);
        break;
    }
}

// "Both" flows wait for the Moonraker ack before invoking SystemPower locally
// — otherwise the local kernel can SIGTERM us before libhv flushes the queued
// WS frame, leaving the printer up. Fire screen on both success AND error:
// the user said "Both", so we power down the screen even if the printer side
// reported a failure.
void execute_both_shutdown(IMoonrakerAPI* api, AsyncLifetimeGuard& lifetime) {
    if (!api)
        return;
    spdlog::info("[ShutdownDialog] Executing both shutdown — printer first, screen on ack");
    auto tok = lifetime.token();
    api->machine_shutdown(
        [tok, api]() {
            spdlog::info("[ShutdownDialog] Printer shutdown ack'd — now shutting down screen");
            schedule_host_down_verification(api, /*is_reboot=*/false);
            if (tok.expired())
                return;
            tok.defer([]() { execute_screen_shutdown(); });
        },
        [tok](const MoonrakerError& err) {
            spdlog::error(
                "[ShutdownDialog] Printer shutdown failed: {} — proceeding with screen anyway",
                err.message);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Shutdown failed"), 6000);
            if (tok.expired())
                return;
            tok.defer([]() { execute_screen_shutdown(); });
        });
}

void execute_both_reboot(IMoonrakerAPI* api, AsyncLifetimeGuard& lifetime) {
    if (!api)
        return;
    spdlog::info("[ShutdownDialog] Executing both reboot — printer first, screen on ack");
    auto tok = lifetime.token();
    api->machine_reboot(
        [tok, api]() {
            spdlog::info("[ShutdownDialog] Printer reboot ack'd — now rebooting screen");
            schedule_host_down_verification(api, /*is_reboot=*/true);
            if (tok.expired())
                return;
            tok.defer([]() { execute_screen_reboot(); });
        },
        [tok](const MoonrakerError& err) {
            spdlog::error(
                "[ShutdownDialog] Printer reboot failed: {} — proceeding with screen anyway",
                err.message);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Reboot failed"), 6000);
            if (tok.expired())
                return;
            tok.defer([]() { execute_screen_reboot(); });
        });
}

// Single-scope mode uses these directly (XML modal_button_row callbacks
// in the ref_value=0 button row). Dual-scope dispatches via the split-button
// callbacks below.
void on_shutdown_printer_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ShutdownModal] shutdown_printer");
    if (auto* m = find_shutdown_modal(e))
        m->fire_printer_shutdown();
    LVGL_SAFE_EVENT_CB_END();
}
void on_reboot_printer_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ShutdownModal] reboot_printer");
    if (auto* m = find_shutdown_modal(e))
        m->fire_printer_reboot();
    LVGL_SAFE_EVENT_CB_END();
}

// Split-button dispatchers (dual-scope mode). The split button's selected
// dropdown index encodes the scope (SCOPE_BOTH/SCOPE_PRINTER/SCOPE_SCREEN).
void on_restart_split_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ShutdownModal] restart_split");
    auto* m = find_shutdown_modal(e);
    lv_obj_t* sb = lv_event_get_current_target_obj(e);
    if (!m || !sb)
        return;
    switch (ui_split_button_get_selected(sb)) {
    case SCOPE_BOTH:
        m->fire_both_reboot();
        break;
    case SCOPE_PRINTER:
        m->fire_printer_reboot();
        break;
    case SCOPE_SCREEN:
        m->fire_screen_reboot();
        break;
    }
    LVGL_SAFE_EVENT_CB_END();
}
void on_shutdown_split_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ShutdownModal] shutdown_split");
    auto* m = find_shutdown_modal(e);
    lv_obj_t* sb = lv_event_get_current_target_obj(e);
    if (!m || !sb)
        return;
    switch (ui_split_button_get_selected(sb)) {
    case SCOPE_BOTH:
        m->fire_both_shutdown();
        break;
    case SCOPE_PRINTER:
        m->fire_printer_shutdown();
        break;
    case SCOPE_SCREEN:
        m->fire_screen_shutdown();
        break;
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

void register_shutdown_widget() {
    register_widget_factory("shutdown", [](const std::string&) {
        auto* api = PanelWidgetManager::instance().shared_resource<IMoonrakerAPI>();
        return std::make_unique<ShutdownWidget>(api);
    });

    // Register XML event callback at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "shutdown_clicked_cb", ShutdownWidget::shutdown_clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_shutdown_printer_clicked", on_shutdown_printer_clicked);
    lv_xml_register_event_cb(nullptr, "on_reboot_printer_clicked", on_reboot_printer_clicked);
    lv_xml_register_event_cb(nullptr, "on_restart_split_clicked", on_restart_split_clicked);
    lv_xml_register_event_cb(nullptr, "on_shutdown_split_clicked", on_shutdown_split_clicked);
}

ShutdownWidget::ShutdownWidget(IMoonrakerAPI* api) : api_(api) {}

ShutdownWidget::~ShutdownWidget() {
    detach();
}

void ShutdownWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Set user_data on the root lv_obj, NOT on the ui_button child.
    // ui_button allocates its own UiButtonData in user_data — overwriting it
    // leaks memory and breaks button style/contrast auto-updates.
    lv_obj_set_user_data(widget_obj_, this);

    shutdown_btn_ = lv_obj_find_by_name(widget_obj_, "shutdown_button");
    if (shutdown_btn_) {
        lv_obj_add_event_cb(shutdown_btn_, shutdown_clicked_cb, LV_EVENT_CLICKED, this);
    }
}

void ShutdownWidget::detach() {
    lifetime_.invalidate();

    if (shutdown_modal_.is_visible()) {
        shutdown_modal_.hide();
    }

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    shutdown_btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ShutdownWidget::handle_click() {
    spdlog::info("[ShutdownWidget] Shutdown button clicked");
    show_shutdown_dialog(api_, shutdown_modal_, lifetime_, lv_screen_active());
}

bool handle_machine_power_failure(const std::string& err_message, bool is_reboot,
                                  bool allow_local_fallback,
                                  const std::function<bool()>& local_action) {
    const char* action = is_reboot ? "reboot" : "shutdown";
    spdlog::error("[ShutdownDialog] Machine {} failed: {}", action, err_message);

    if (allow_local_fallback && local_action) {
        // Moonraker is on this host, so its machine.reboot and our local power
        // action mean the same thing. Moonraker's `provider: none` path shells
        // out to sudo/systemctl, which non-systemd firmwares don't have;
        // SystemPower falls through to busybox /sbin/reboot, which they do.
        spdlog::info("[ShutdownDialog] Moonraker cannot {} this host — falling back to SystemPower",
                     action);
        if (local_action()) {
            return true;
        }
        spdlog::error("[ShutdownDialog] Local {} fallback also failed", action);
    }

    ToastManager::instance().show(
        ToastSeverity::ERROR, is_reboot ? lv_tr("Reboot failed") : lv_tr("Shutdown failed"), 6000);
    return false;
}

bool power_then_hold_black(const std::function<bool()>& power,
                           const std::function<void()>& stop_drawing,
                           const std::function<void()>& clear) {
    const bool dispatched = power();
    if (dispatched) {
        stop_drawing();
        clear();
    }
    return dispatched;
}

HostPowerFollowUp host_power_follow_up(bool local_dispatched, bool api_connected) {
    if (local_dispatched) {
        return HostPowerFollowUp::Done;
    }
    return api_connected ? HostPowerFollowUp::RetryViaMoonraker : HostPowerFollowUp::Toast;
}

ShutdownDialogMode plan_shutdown_dialog(bool configured_host_same, bool api_connected,
                                        bool this_host_runs_systemd) {
    if (!configured_host_same) {
        return ShutdownDialogMode::Dual;
    }
    return api_connected && this_host_runs_systemd ? ShutdownDialogMode::SingleViaMoonraker
                                                   : ShutdownDialogMode::SingleViaLocalPower;
}

bool power_job_active() {
    return lv_subject_get_int(get_printer_state().print_state().get_job_holds_machine_subject()) !=
           0;
}

bool power_job_state_known() {
    auto& ps = get_printer_state();
    return lv_subject_get_int(ps.get_printer_connection_state_subject()) ==
               static_cast<int>(ConnectionState::CONNECTED) &&
           lv_subject_get_int(ps.get_klippy_state_subject()) ==
               static_cast<int>(KlippyState::READY) &&
           ps.print_state().print_stats_received();
}

void show_shutdown_dialog(IMoonrakerAPI* api, ShutdownModal& modal, AsyncLifetimeGuard& lifetime,
                          lv_obj_t* parent_screen) {
    // Backstop behind the UI gating (home-grid widget + Advanced POWER rows):
    // platforms without host power controls (Android) must never reach the
    // machine.reboot/shutdown RPCs, whatever entry point wires this up next.
    if (!helix::platform_host_power_supported()) {
        spdlog::info("[ShutdownDialog] Platform without host power support — ignoring request");
        return;
    }

    if (!api) {
        spdlog::warn("[ShutdownDialog] No API available");
        return;
    }

    std::string host;
    if (Config* cfg = Config::get_instance()) {
        host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    }

    const bool connected = api->is_connected();
    switch (plan_shutdown_dialog(helix::is_moonraker_on_same_host(host), connected,
                                 helix::host_runs_systemd())) {
    case ShutdownDialogMode::SingleViaMoonraker:
        // Moonraker's machine.shutdown brings the whole device down. Moonraker
        // with `provider: none` answers it by shelling out to sudo, which a
        // host without passwordless sudo refuses. Pass allow_local_fallback so
        // a rejected RPC retries through SystemPower instead of dead-ending in
        // a toast.
        modal.set_single_callbacks(
            [api]() { execute_printer_shutdown(api, /*allow_local_fallback=*/true); },
            [api]() { execute_printer_reboot(api, /*allow_local_fallback=*/true); });
        break;
    case ShutdownDialogMode::SingleViaLocalPower:
        // Moonraker is down (Klipper failed to boot, or a screen-only setup with
        // the local Moonraker disabled), or this host has no systemd, where
        // Moonraker's machine.reboot/shutdown always fails. Either way this host
        // is the printer, and SystemPower powers it directly.
        if (connected) {
            spdlog::info("[ShutdownDialog] Host does not run systemd — Moonraker cannot power "
                         "it; using local SystemPower");
        } else {
            spdlog::info(
                "[ShutdownDialog] Moonraker not connected — using local SystemPower fallback");
        }
        modal.set_single_callbacks([api]() { execute_host_power(api, /*is_reboot=*/false); },
                                   [api]() { execute_host_power(api, /*is_reboot=*/true); });
        break;
    case ShutdownDialogMode::Dual:
        modal.set_dual_callbacks(
            [api, &lifetime]() { execute_both_shutdown(api, lifetime); },
            [api, &lifetime]() { execute_both_reboot(api, lifetime); },
            [api]() { execute_printer_shutdown(api); }, [api]() { execute_printer_reboot(api); },
            []() { execute_screen_shutdown(); }, []() { execute_screen_reboot(); });
        break;
    }

    modal.show(parent_screen);
}

void ShutdownWidget::shutdown_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ShutdownWidget] shutdown_clicked_cb");
    auto* self = static_cast<ShutdownWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
