// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_change_host_modal.h"

#include "ui_emergency_stop.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/lvgl.h"
#include "moonraker_manager.h"
#include "theme_manager.h"
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

bool ChangeHostModal::show_modal(lv_obj_t* parent) {
    register_callbacks();
    init_subjects();

    // Populate with current config values
    Config* config = Config::get_instance();
    if (config) {
        std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
        int port = config->get<int>(config->df() + "moonraker_port", 7125);

        lv_subject_copy_string(&host_ip_subject_, host.c_str());
        lv_subject_copy_string(&host_port_subject_, std::to_string(port).c_str());
    }

    bool result = show(parent);
    if (result && dialog()) {
        // Reset state
        lv_subject_set_int(&testing_subject_, 0);
        lv_subject_set_int(&validated_subject_, 0);

        // Set active instance for static callback dispatch
        active_instance_ = this;

        // Register keyboards for text inputs
        lv_obj_t* host_input = lv_obj_find_by_name(dialog(), "host_input");
        if (host_input) {
            helix::ui::modal_register_keyboard(dialog(), host_input);
        }

        lv_obj_t* port_input = lv_obj_find_by_name(dialog(), "port_input");
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

    // Register subjects for XML binding
    lv_xml_register_subject(nullptr, "change_host_ip", &host_ip_subject_);
    lv_xml_register_subject(nullptr, "change_host_port", &host_port_subject_);
    lv_xml_register_subject(nullptr, "change_host_testing", &testing_subject_);
    lv_xml_register_subject(nullptr, "change_host_validated", &validated_subject_);

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

    lv_subject_deinit(&host_ip_subject_);
    lv_subject_deinit(&host_port_subject_);
    lv_subject_deinit(&testing_subject_);
    lv_subject_deinit(&validated_subject_);

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

    if (!ip || strlen(ip) == 0) {
        set_status(nullptr, nullptr, "Please enter a host address");
        return;
    }
    if (!is_valid_ip_or_hostname(ip)) {
        set_status("icon_close_circle", "danger", "Invalid IP address or hostname");
        return;
    }
    if (!is_valid_port(port_clean)) {
        set_status("icon_close_circle", "danger", "Invalid port (must be 1-65535)");
        return;
    }

    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        set_status("icon_close_circle", "danger", "Client not available");
        return;
    }

    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::NORMAL);
    client->disconnect();

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

void ChangeHostModal::handle_save() {
    spdlog::debug("[ChangeHostModal] Save clicked");

    const char* ip = lv_subject_get_string(&host_ip_subject_);
    std::string port_clean = sanitize_port(lv_subject_get_string(&host_port_subject_));

    if (!ip || port_clean.empty()) {
        spdlog::error("[ChangeHostModal] Cannot save - null/empty subjects");
        return;
    }

    // Validate port before saving (defensive — should already be validated)
    int port = 7125;
    try {
        port = std::stoi(port_clean);
    } catch (const std::exception& e) {
        spdlog::error("[ChangeHostModal] Invalid port '{}': {}", port_clean, e.what());
        return;
    }

    // Save to config
    Config* config = Config::get_instance();
    if (config) {
        config->set(config->df() + "moonraker_host", std::string(ip));
        config->set(config->df() + "moonraker_port", port);
        config->save();
        spdlog::info("[ChangeHostModal] Saved new host: {}:{}", ip, port);
        // moonraker_host changed — flush the same-host detection cache so the
        // shutdown widget picks up the new value on next open.
        helix::invalidate_host_identity_cache();
    }

    // Close modal first — on_hide() removes observers and clears state
    hide();

    // Defer the completion callback so the reconnection flood doesn't
    // overlap with the modal exit animation. Without this, manager->connect()
    // triggers a burst of subject notifications while the modal's LVGL
    // widgets are still alive (150ms exit animation) which can cause
    // heap corruption via stale observer dispatch.
    if (completion_callback_) {
        auto callback = completion_callback_;
        helix::ui::queue_update([callback]() { callback(true); });
    }
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

    lv_obj_t* icon_label = lv_obj_find_by_name(dialog(), "status_icon");
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

    lv_obj_t* text_label = lv_obj_find_by_name(dialog(), "status_text");
    if (text_label) {
        lv_label_set_text(text_label, text ? text : "");
    }
}

// ============================================================================
// Input Change Observer
// ============================================================================

void ChangeHostModal::on_input_changed_cb(lv_observer_t* /*observer*/, lv_subject_t* /*subject*/) {
    // Reset validation when user edits host or port after a successful test
    lv_subject_t* validated = lv_xml_get_subject(nullptr, "change_host_validated");
    if (validated && lv_subject_get_int(validated) != 0) {
        lv_subject_set_int(validated, 0);
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

void show_change_host_modal(std::function<void(bool changed)> extra_on_complete) {
    // Function-local static: ChangeHostModal's active_instance_ is a static
    // singleton, so a second owner elsewhere would fight this one. The instance
    // must also outlive the dialog it shows.
    static std::unique_ptr<ChangeHostModal> modal;
    if (!modal) {
        modal = std::make_unique<ChangeHostModal>();
    }

    modal->set_completion_callback([extra = std::move(extra_on_complete)](bool changed) {
        if (!changed) {
            return;
        }

        Config* config = Config::get_instance();
        const std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
        const int port = config->get<int>(config->df() + "moonraker_port", 7125);

        if (extra) {
            extra(true);
        }

        IMoonrakerClient* client = get_moonraker_client();
        MoonrakerManager* manager = get_moonraker_manager();
        if (!client || !manager) {
            spdlog::error("[ChangeHostModal] Cannot reconnect - client or manager unavailable");
            return;
        }

        // The teardown below looks exactly like an unexpected drop; suppress the
        // recovery dialog so an intentional switch doesn't raise one.
        EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::SHORT);
        client->disconnect();

        const std::string ws_url = "ws://" + host + ":" + std::to_string(port) + "/websocket";
        const std::string http_url = "http://" + host + ":" + std::to_string(port);

        spdlog::info("[ChangeHostModal] Reconnecting to {}:{}", host, port);
        manager->connect(ws_url, http_url);
    });

    modal->show_modal(lv_screen_active());
}

void show_connection_failed_modal(const std::string& title, const std::string& message) {
    // Callers include MoonrakerClient::on_ws_close on the libhv event-loop
    // thread. Everything below touches LVGL, so hop to the main thread first —
    // this mirrors what ui_notification_error() does internally for the
    // OK-only path this replaces.
    helix::ui::queue_update([title, message]() {
        // Reconnect first: a wedged transport (reported on Android, where the
        // process outlives its sockets) cannot be revived from outside the app,
        // and a full teardown/rebuild re-resolves the host — the one thing the
        // auto-retry loop cannot do for a changed IP. The prompt's job is to
        // offer that action; address surgery stays one tap away but secondary.
        auto reconnect_and_dismiss = [](lv_event_t*) {
            if (lv_obj_t* top = Modal::get_top()) {
                Modal::hide(top);
            }
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
        std::string host;
        if (Config* cfg = Config::get_instance()) {
            host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
        }
        if (!host.empty() && helix::is_moonraker_on_same_host(host)) {
            helix::ui::modal_show_alert(title.c_str(), message.c_str(), ModalSeverity::Error,
                                        lv_tr("Reconnect"), reconnect_and_dismiss);
            return;
        }

        helix::ui::modal_show_confirmation(
            title.c_str(), message.c_str(), ModalSeverity::Error, lv_tr("Reconnect"),
            reconnect_and_dismiss,
            [](lv_event_t*) {
                // Dismiss this prompt before opening the next dialog. Stacking
                // works, but leaving a live error modal underneath means its
                // buttons stay pressable behind the host form.
                if (lv_obj_t* top = Modal::get_top()) {
                    Modal::hide(top);
                }
                show_change_host_modal();
            },
            nullptr, lv_tr("Change Address"));
    });
}

} // namespace helix::ui
