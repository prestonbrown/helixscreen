// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_change_host_modal.h"

#include "ui_emergency_stop.h"
#include "ui_printer_list_overlay.h"
#include "ui_printer_switch_menu.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/lvgl.h"
#include "moonraker_manager.h"
#include "printer_retarget.h"
#include "printer_state.h"
#include "text_io.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"
#include "utils/network_validation.h"

#include <spdlog/spdlog.h>

#include <string>

using namespace helix;

// Static member initialization
bool ChangeHostModal::callbacks_registered_ = false;
ChangeHostModal* ChangeHostModal::active_instance_ = nullptr;

// ============================================================================
// Construction / Destruction
// ============================================================================

ChangeHostModal::ChangeHostModal() {
    spdlog::debug("[ChangeHostModal] Constructed");
}

ChangeHostModal::~ChangeHostModal() {
    deinit_subjects();
    spdlog::trace("[ChangeHostModal] Destroyed");
}

// ============================================================================
// Public API
// ============================================================================

void ChangeHostModal::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

bool ChangeHostModal::show_modal(lv_obj_t* parent, AddCallback on_add) {
    register_callbacks();
    init_subjects();

    add_callback_ = std::move(on_add);
    const bool adding = static_cast<bool>(add_callback_);
    lv_subject_set_int(&adding_subject_, adding ? 1 : 0);
    update_save_lock();

    // A new printer starts empty; otherwise the fields show the active printer's host.
    Config* config = Config::get_instance();
    std::string host =
        adding ? std::string{} : config->get<std::string>(config->df() + "moonraker_host", "");
    int port = adding ? 7125 : config->get<int>(config->df() + "moonraker_port", 7125);

    lv_subject_copy_string(&host_ip_subject_, host.c_str());
    lv_subject_copy_string(&host_port_subject_, std::to_string(port).c_str());

    bool result = show(parent);
    if (result && dialog()) {
        // Reset state
        client_borrowed_ = false;
        lv_subject_set_int(&testing_subject_, 0);
        lv_subject_set_int(&validated_subject_, 0);
        update_save_lock();

        // Set active instance for static callback dispatch
        active_instance_ = this;

        // Register keyboards for text inputs
        lv_obj_t* host_input = helix::ui::find_required(dialog(), "host_input", get_name());
        if (host_input) {
            helix::ui::modal_register_keyboard(dialog(), host_input);
        }

        lv_obj_t* port_input = helix::ui::find_required(dialog(), "port_input", get_name());
        if (port_input) {
            helix::ui::modal_register_keyboard(dialog(), port_input);
        }

        // Observe text input changes to invalidate validation when user edits.
        // Using ObserverGuard for RAII cleanup instead of lv_subject_add_observer_obj
        // which relies on widget deletion timing (unsafe during exit animation).
        host_ip_observer_ = ObserverGuard(&host_ip_subject_, on_input_changed_cb, nullptr);
        host_port_observer_ = ObserverGuard(&host_port_subject_, on_input_changed_cb, nullptr);
    }

    return result;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void ChangeHostModal::on_show() {
    spdlog::debug("[ChangeHostModal] on_show");
}

void ChangeHostModal::on_hide() {
    // Base class already called lifetime_.invalidate()

    active_instance_ = nullptr;

    // A test left the client on the typed host. Every way out but Save puts it back on the
    // saved one; deferred past the exit animation like Save's reconnect.
    if (client_borrowed_) {
        client_borrowed_ = false;
        helix::ui::queue_update("ChangeHostModal::restore_saved_host",
                                [] { reconnect_active_printer(); });
    }

    // Remove observers NOW rather than relying on auto-removal when dialog
    // widget is deleted after exit animation.
    host_ip_observer_.reset();
    host_port_observer_.reset();

    spdlog::debug("[ChangeHostModal] on_hide");
}

// ============================================================================
// Subject Management
// ============================================================================

void ChangeHostModal::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_string(&host_ip_subject_, host_ip_buf_, nullptr, sizeof(host_ip_buf_), "");
    lv_subject_init_string(&host_port_subject_, host_port_buf_, nullptr, sizeof(host_port_buf_),
                           "7125");
    lv_subject_init_int(&testing_subject_, 0);
    lv_subject_init_int(&validated_subject_, 0);
    lv_subject_init_int(&adding_subject_, 0);
    lv_subject_init_int(&save_locked_subject_, 1);

    // Register subjects for XML binding
    subjects_.publish("change_host_ip", &host_ip_subject_);
    subjects_.publish("change_host_port", &host_port_subject_);
    subjects_.publish("change_host_testing", &testing_subject_);
    subjects_.publish("change_host_validated", &validated_subject_);
    subjects_.publish("change_host_adding", &adding_subject_);
    subjects_.publish("change_host_save_locked", &save_locked_subject_);

    subjects_initialized_ = true;
    spdlog::trace("[ChangeHostModal] Subjects initialized");
}

void ChangeHostModal::deinit_subjects() {
    if (!subjects_initialized_)
        return;

    // release() because subjects are about to be destroyed —
    // calling reset() (which does lv_observer_remove) on a dead subject = crash.
    host_ip_observer_.release();
    host_port_observer_.release();

    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::trace("[ChangeHostModal] Subjects deinitialized");
}

// ============================================================================
// Event Handlers
// ============================================================================

void ChangeHostModal::handle_test_connection() {
    const char* ip = lv_subject_get_string(&host_ip_subject_);
    std::string port_clean = sanitize_port(lv_subject_get_string(&host_port_subject_));

    spdlog::debug("[ChangeHostModal] Test connection: {}:{}", ip ? ip : "", port_clean);

    lv_subject_set_int(&validated_subject_, 0);
    update_save_lock();

    if (!input_valid(ip, port_clean)) {
        return;
    }

    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        set_status("icon_close_circle", "danger", "Client not available");
        return;
    }

    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::NORMAL);
    client->disconnect();
    client_borrowed_ = true;

    // Cancel any in-flight test callbacks, get fresh token
    lifetime_.invalidate();
    auto token = lifetime_.token();

    lv_subject_set_int(&testing_subject_, 1);
    set_status("icon_question_circle", "text_muted", "Testing connection...");

    client->set_connection_timeout(5000);

    std::string ws_url = "ws://" + std::string(ip) + ":" + port_clean + "/websocket";
    std::string http_url = "http://" + std::string(ip) + ":" + port_clean;

    // Set HTTP base URL BEFORE connect: MoonrakerClient's on_connected handlers
    // (proc_stats initial fetch, performance source, etc.) fire REST calls as
    // soon as the WS opens. Without this, every test connection logs a burst of
    // "HTTP base URL not configured" errors until the subsequent Save triggers
    // manager->connect() (which sets it again). See bundle TV95LJGN.
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        api->set_http_base_url(http_url);
    }

    int result = client->connect(
        ws_url.c_str(),
        [this, token]() {
            token.defer("ChangeHostModal::dispatch_test_success", [this]() { on_test_success(); });
        },
        [this, token]() {
            token.defer("ChangeHostModal::dispatch_test_failure", [this]() { on_test_failure(); });
        });

    client->set_auto_reconnect(false);

    if (result != 0) {
        spdlog::error("[ChangeHostModal] Failed to initiate test connection: {}", result);
        set_status("icon_close_circle", "danger", "Error starting connection test");
        lv_subject_set_int(&testing_subject_, 0);
    }
}

void ChangeHostModal::on_test_success() {
    // Already on main thread — caller dispatches via tok.defer (see handle_test).
    spdlog::info("[ChangeHostModal] Test connection successful");

    if (!is_visible())
        return;

    set_status("icon_check_circle", "success", "Connection successful!");
    lv_subject_set_int(&testing_subject_, 0);
    lv_subject_set_int(&validated_subject_, 1);
    update_save_lock();

    spdlog::info("[ChangeHostModal] Test passed, Save button enabled");
}

void ChangeHostModal::on_test_failure() {
    // Already on main thread — caller dispatches via tok.defer (see handle_test).
    spdlog::warn("[ChangeHostModal] Test connection failed");

    if (!is_visible())
        return;

    set_status("icon_close_circle", "danger", "Connection failed");
    lv_subject_set_int(&testing_subject_, 0);

    spdlog::debug("[ChangeHostModal] Test failed, keeping Save disabled");
}

bool ChangeHostModal::input_valid(const char* ip, const std::string& port_clean) {
    if (!ip || strlen(ip) == 0) {
        set_status(nullptr, nullptr, "Please enter a host address");
        return false;
    }
    if (!is_valid_ip_or_hostname(ip)) {
        set_status("icon_close_circle", "danger", "Invalid IP address or hostname");
        return false;
    }
    if (!is_valid_port(port_clean)) {
        set_status("icon_close_circle", "danger", "Invalid port (must be 1-65535)");
        return false;
    }
    return true;
}

void ChangeHostModal::handle_save() {
    spdlog::debug("[ChangeHostModal] Save clicked");

    const char* ip = lv_subject_get_string(&host_ip_subject_);
    std::string port_clean = sanitize_port(lv_subject_get_string(&host_port_subject_));

    // Save anyway skips the connection test, never the address check.
    if (!input_valid(ip, port_clean)) {
        return;
    }

    // Validate port before saving (defensive — should already be validated)
    const auto parsed_port = helix::text_io::parse_leading<int>(port_clean);
    if (!parsed_port) {
        spdlog::error("[ChangeHostModal] Invalid port '{}'", port_clean);
        return;
    }
    const int port = *parsed_port;

    const std::string host(helix::text_io::trim(ip));
    if (add_callback_) {
        if (lv_subject_get_int(&validated_subject_) != 0) {
            commit_add(host, port);
            return;
        }
        // Untested, failed or still testing: the printer may be off right now, so adding it
        // anyway is allowed, after asking.
        helix::ui::ConfirmOptions options;
        options.owner_token = lifetime_.token();
        helix::ui::modal_confirm(
            lv_tr("Add Printer"), lv_tr("Can't reach this printer. Save anyway?"),
            ModalSeverity::Warning, lv_tr("Save"), [this, host, port] { commit_add(host, port); },
            options);
        return;
    }

    // Save to config. The client reconnects to the new host, so there is nothing to restore.
    client_borrowed_ = false;
    Config* config = Config::get_instance();
    config->set(config->df() + "moonraker_host", host);
    config->set(config->df() + "moonraker_port", port);
    config->save();
    spdlog::info("[ChangeHostModal] Saved new host: {}:{}", host, port);
    // moonraker_host changed — flush the same-host detection cache so the
    // shutdown widget picks up the new value on next open.
    helix::invalidate_host_identity_cache();

    // Close modal first — on_hide() removes observers and clears state
    hide();

    // Defer the completion callback so the reconnection flood doesn't
    // overlap with the modal exit animation. Without this, manager->connect()
    // triggers a burst of subject notifications while the modal's LVGL
    // widgets are still alive (150ms exit animation) which can cause
    // heap corruption via stale observer dispatch.
    if (completion_callback_) {
        auto callback = completion_callback_;
        helix::ui::queue_update("ChangeHostModal::handle_save", [callback]() { callback(true); });
    }
}

void ChangeHostModal::commit_add(const std::string& host, int port) {
    // The borrow stays: on_hide puts the client back on the saved printer before the caller
    // runs, so a switch the caller declines or cannot save leaves it there.
    hide();
    auto on_add = add_callback_;
    helix::ui::queue_update("ChangeHostModal::handle_add",
                            [on_add, host, port]() { on_add(host, port); });
}

void ChangeHostModal::update_save_lock() {
    const bool locked =
        lv_subject_get_int(&adding_subject_) == 0 && lv_subject_get_int(&validated_subject_) == 0;
    lv_subject_set_int(&save_locked_subject_, locked ? 1 : 0);
}

void ChangeHostModal::handle_cancel() {
    spdlog::debug("[ChangeHostModal] Cancel clicked");

    hide();

    if (completion_callback_) {
        completion_callback_(false);
    }
}

// ============================================================================
// Status Display
// ============================================================================

void ChangeHostModal::set_status(const char* icon_name, const char* color_token, const char* text) {
    if (!dialog())
        return;

    lv_obj_t* icon_label = helix::ui::find_required(dialog(), "status_icon", get_name());
    if (icon_label) {
        if (icon_name) {
            const char* icon_text = lv_xml_get_const(nullptr, icon_name);
            lv_label_set_text(icon_label, icon_text ? icon_text : "");
        } else {
            lv_label_set_text(icon_label, "");
        }
        if (color_token) {
            lv_obj_set_style_text_color(icon_label, theme_manager_get_color(color_token),
                                        LV_PART_MAIN);
        }
    }

    lv_obj_t* text_label = helix::ui::find_required(dialog(), "status_text", get_name());
    if (text_label) {
        lv_label_set_text(text_label, text ? text : "");
    }
}

// ============================================================================
// Input Change Observer
// ============================================================================

void ChangeHostModal::on_input_changed_cb(lv_observer_t* /*observer*/, lv_subject_t* /*subject*/) {
    // A status line describes the address it was shown for, so an edit clears it.
    if (active_instance_) {
        active_instance_->set_status(nullptr, nullptr, "");
    }
    // Reset validation when user edits host or port after a successful test
    lv_subject_t* validated = lv_xml_get_subject(nullptr, "change_host_validated");
    if (validated && lv_subject_get_int(validated) != 0) {
        lv_subject_set_int(validated, 0);
        if (active_instance_) {
            active_instance_->update_save_lock();
        }
        spdlog::debug("[ChangeHostModal] Input changed, validation reset");
    }
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void ChangeHostModal::register_callbacks() {
    if (callbacks_registered_)
        return;

    lv_xml_register_event_cb(nullptr, "on_change_host_test", on_test_connection_cb);
    lv_xml_register_event_cb(nullptr, "on_change_host_save", on_save_cb);
    lv_xml_register_event_cb(nullptr, "on_change_host_cancel", on_cancel_cb);

    callbacks_registered_ = true;
    spdlog::trace("[ChangeHostModal] Callbacks registered");
}

void ChangeHostModal::on_test_connection_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_test_connection();
    }
}

void ChangeHostModal::on_save_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_save();
    }
}

void ChangeHostModal::on_cancel_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_cancel();
    }
}

// ============================================================================
// Free entry points
// ============================================================================

namespace helix::ui {

namespace {

// ChangeHostModal's active_instance_ is a static singleton, so both entry points share one
// owner. The instance must also outlive the dialog it shows.
ChangeHostModal& shared_change_host_modal() {
    static std::unique_ptr<ChangeHostModal> modal;
    if (!modal) {
        modal = std::make_unique<ChangeHostModal>();
    }
    return *modal;
}

} // namespace

void show_add_printer_modal(std::function<void(const std::string& host, int port)> on_add) {
    ChangeHostModal& modal = shared_change_host_modal();
    modal.set_completion_callback(nullptr);
    modal.show_modal(lv_screen_active(), std::move(on_add));
}

void show_change_host_modal(std::function<void(bool changed)> extra_on_complete) {
    ChangeHostModal* modal = &shared_change_host_modal();

    modal->set_completion_callback([extra = std::move(extra_on_complete)](bool changed) {
        if (!changed) {
            return;
        }

        if (extra) {
            extra(true);
        }
        // A new host is a different printer.
        retarget_printer_connection();
    });

    modal->show_modal(lv_screen_active());
}

namespace {

void present_connection_failed(const std::string& title, const std::string& message) {
    // Reconnect first: a wedged transport (reported on Android, where the
    // process outlives its sockets) cannot be revived from outside the app,
    // and a full teardown/rebuild re-resolves the host — the one thing the
    // auto-retry loop cannot do for a changed IP. The prompt's job is to
    // offer that action; address surgery stays one tap away but secondary.
    //
    // The declarative helpers close their own dialog once the callback
    // returns, so this only has to do the work. The Modal::get_top() guess
    // it replaces named whatever happened to be on top rather than this
    // prompt, which is only ever correct by luck of ordering.
    auto reconnect = [] {
        if (auto* client = get_moonraker_client()) {
            client->force_reconnect();
        } else {
            spdlog::warn("[ChangeHost] Reconnect requested but no client is registered");
        }
    };

    // On a printer that runs HelixScreen itself, the address is not the
    // fault and "Change Address" is a trap: it walks the user into editing
    // a correct 127.0.0.1 while the real problem is a Moonraker service
    // that did not start. Retrying those services is the meaningful action.
    //
    // Only when we POSITIVELY know the printer is this machine. The default
    // is deliberately "" rather than "localhost": an unconfigured host is
    // the one case where changing the address is exactly the right action,
    // and defaulting to a loopback literal would take that action away from
    // every user who has not set a host yet.
    // Locality here must read the ATTEMPTED host, not the live endpoint:
    // this dialog fires exactly when the connection failed, so the
    // moonraker_is_remote subject is still at its default (local) and
    // would suppress "Change Address" for every remote host.
    std::string host;
    Config* cfg = Config::get_instance();
    host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");

    if (!host.empty() && helix::is_moonraker_on_same_host(host)) {
        helix::ui::modal_alert(title.c_str(), message.c_str(), ModalSeverity::Error,
                               lv_tr("Reconnect"), reconnect);
        return;
    }

    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [] {
        // This prompt closes itself the moment this returns, so the host
        // form is never left stacked over a live error modal whose
        // buttons stay pressable behind it.
        show_change_host_modal();
    };
    opts.cancel_text = lv_tr("Change Address");

    helix::ui::modal_confirm(title.c_str(), message.c_str(), ModalSeverity::Error,
                             lv_tr("Reconnect"), reconnect, opts);
}

/// A printer list the user is choosing from. A prompt popping over it takes the tap meant
/// for a row, and the list already shows each printer's connection.
bool printer_chooser_open() {
    return ContextMenu::active_as<PrinterSwitchMenu>() != nullptr ||
           get_printer_list_overlay().is_visible();
}

/// The newest failure reported while a chooser was open, shown once it closes if the user
/// stayed on the same printer and it is still not connected.
struct DeferredFailure {
    std::string title;
    std::string message;
    std::string printer_id;
    bool held = false;
    lv_timer_t* timer = nullptr;
};

DeferredFailure& deferred_failure() {
    static DeferredFailure d;
    return d;
}

constexpr uint32_t CHOOSER_POLL_MS = 300;

void defer_connection_failed(const std::string& title, const std::string& message) {
    DeferredFailure& d = deferred_failure();
    d.title = title;
    d.message = message;
    d.printer_id = Config::get_instance()->get_active_printer_id();
    d.held = true;
    if (d.timer) {
        return;
    }
    // TIMER_DTOR_OK: process-lifetime state with no owner object; the one-shot re-arms itself
    // while the chooser is open and is deleted by LVGL after its last run.
    d.timer = lv_timer_create(
        [](lv_timer_t* t) {
            if (printer_chooser_open()) {
                lv_timer_set_repeat_count(t, 1); // look again next period
                return;
            }
            // A one-shot, so LVGL deletes it once this returns.
            DeferredFailure& pending = deferred_failure();
            pending.timer = nullptr;
            if (!pending.held) {
                return; // dropped when the chooser closed on a selection
            }
            pending.held = false;
            IMoonrakerClient* client = get_moonraker_client();
            const bool same_printer =
                Config::get_instance()->get_active_printer_id() == pending.printer_id;
            if (!same_printer ||
                (client && client->get_connection_state() == ConnectionState::CONNECTED)) {
                spdlog::debug(
                    "[ChangeHost] Dropping a connection-failed prompt the user moved past");
                return;
            }
            present_connection_failed(pending.title, pending.message);
        },
        CHOOSER_POLL_MS, nullptr);
    lv_timer_set_repeat_count(d.timer, 1);
}

} // namespace

void drop_held_connection_failed() {
    DeferredFailure& d = deferred_failure();
    if (d.held) {
        spdlog::debug("[ChangeHost] Dropping the held connection-failed prompt: the user chose "
                      "a printer");
    }
    d.held = false;
}

void show_connection_failed_modal(const std::string& title, const std::string& message) {
    // Callers include MoonrakerClient::on_ws_close on the libhv event-loop
    // thread. Everything below touches LVGL, so hop to the main thread first.
    helix::ui::queue_update("ui_change_host_modal::show_connection_failed_modal",
                            [title, message]() {
                                if (printer_chooser_open()) {
                                    defer_connection_failed(title, message);
                                    return;
                                }
                                present_connection_failed(title, message);
                            });
}

} // namespace helix::ui
