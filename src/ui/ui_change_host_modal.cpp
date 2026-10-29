// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_change_host_modal.h"

#include "ui_next_tick.h"
#include "ui_printer_list_overlay.h"
#include "ui_printer_switch_menu.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lan_sweep.h"
#include "lvgl/lvgl.h"
#include "moonraker_events.h"
#include "moonraker_manager.h"
#include "platform_info.h"
#include "printer_discovery_list.h"
#include "printer_retarget.h"
#include "printer_state.h"
#include "text_io.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"
#include "utils/network_validation.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <string>
#include <utility>

using namespace helix;

namespace {

void store_active_printer_address(const std::string& host, int port) {
    Config* config = Config::get_instance();
    config->set(config->df() + "moonraker_host", host);
    config->set(config->df() + "moonraker_port", port);
    config->save();
    spdlog::info("[ChangeHostModal] Saved new host: {}:{}", host, port);
    // moonraker_host changed — flush the same-host detection cache so the
    // shutdown widget picks up the new value on next open.
    helix::invalidate_host_identity_cache();
}

} // namespace

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
    int port =
        adding ? helix::kDefaultMoonrakerPort
               : config->get<int>(config->df() + "moonraker_port", helix::kDefaultMoonrakerPort);

    lv_subject_copy_string(&host_ip_subject_, host.c_str());
    lv_subject_copy_string(&host_port_subject_, std::to_string(port).c_str());
    // The key belongs to the active printer's entry; a printer being added has none yet.
    const std::string api_key = adding ? std::string{} : active_printer_credential().api_key;
    lv_subject_copy_string(&api_key_subject_, api_key.c_str());

    bool result = show(parent);
    if (result && dialog()) {
        // Reset state
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

        // Named for the shared reveal button, which looks for its sibling by this name.
        lv_obj_t* key_input = helix::ui::find_required(dialog(), "password_input", get_name());
        if (key_input) {
            helix::ui::modal_register_keyboard(dialog(), key_input);
        }

        // Observe text input changes to invalidate validation when user edits.
        // Using ObserverGuard for RAII cleanup instead of lv_subject_add_observer_obj
        // which relies on widget deletion timing (unsafe during exit animation).
        host_ip_observer_ = ObserverGuard(&host_ip_subject_, on_input_changed_cb, nullptr);
        host_port_observer_ = ObserverGuard(&host_port_subject_, on_input_changed_cb, nullptr);
        api_key_observer_ = ObserverGuard(&api_key_subject_, on_input_changed_cb, nullptr);
        // Adding an observer notifies it once; the field still holds the filled-in port.
        port_origin_ = helix::PortOrigin::FilledIn;

        start_discovery();
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
    stop_discovery();
    connection_test_.cancel();

    // Remove observers NOW rather than relying on auto-removal when dialog
    // widget is deleted after exit animation.
    host_ip_observer_.reset();
    host_port_observer_.reset();
    api_key_observer_.reset();

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
                           std::to_string(kDefaultMoonrakerPort).c_str());
    lv_subject_init_string(&api_key_subject_, api_key_buf_, nullptr, sizeof(api_key_buf_), "");
    lv_subject_init_int(&testing_subject_, 0);
    lv_subject_init_int(&validated_subject_, 0);
    lv_subject_init_int(&adding_subject_, 0);
    lv_subject_init_int(&save_locked_subject_, 1);
    lv_subject_init_int(&discovery_active_subject_, 0);

    // Register subjects for XML binding
    subjects_.publish("change_host_ip", &host_ip_subject_);
    subjects_.publish("change_host_port", &host_port_subject_);
    subjects_.publish("change_host_api_key", &api_key_subject_);
    subjects_.publish("change_host_testing", &testing_subject_);
    subjects_.publish("change_host_validated", &validated_subject_);
    subjects_.publish("change_host_adding", &adding_subject_);
    subjects_.publish("change_host_save_locked", &save_locked_subject_);
    subjects_.publish("change_host_discovery_active", &discovery_active_subject_);

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
    api_key_observer_.release();

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

    lv_subject_set_int(&testing_subject_, 1);
    set_status("icon_question_circle", "text_muted", "Testing connection...");

    // The live client stays on the saved printer: only Save moves it.
    const std::string host(helix::text_io::trim(ip));
    connection_test_.start(
        host, moonraker_ports_to_try(port_clean, port_origin_),
        [this, host](const MoonrakerConnectionTest::Result& result) {
            if (result.reached) {
                on_test_success(host, result);
            } else {
                on_test_failure(result);
            }
        },
        entered_api_key());
}

void ChangeHostModal::on_test_success(const std::string& host,
                                      const MoonrakerConnectionTest::Result& result) {
    spdlog::info("[ChangeHostModal] Moonraker answered at {}:{}", host, result.port);

    if (!is_visible())
        return;

    // A fallback port is where Moonraker answered, so the field and Save carry it.
    const std::string port_text = std::to_string(result.port);
    if (sanitize_port(lv_subject_get_string(&host_port_subject_)) != port_text) {
        port_set_by_app_ = true;
        lv_subject_copy_string(&host_port_subject_, port_text.c_str());
        port_set_by_app_ = false;
    }

    lv_subject_set_int(&testing_subject_, 0);
    if (result.login_required) {
        // Saving an address Moonraker refuses would leave the screen empty.
        set_status("icon_close_circle", result.key_sent ? "danger" : "warning",
                   connection_test_status(host, result).c_str());
        return;
    }
    set_status("icon_check_circle", "success", connection_test_status(host, result).c_str());
    lv_subject_set_int(&validated_subject_, 1);
    update_save_lock();

    spdlog::info("[ChangeHostModal] Test passed, Save button enabled");
}

void ChangeHostModal::on_test_failure(const MoonrakerConnectionTest::Result& result) {
    spdlog::warn("[ChangeHostModal] Test connection failed");

    if (!is_visible())
        return;

    set_status("icon_close_circle", "danger",
               connect_failure_status(result.failure, result.tried).c_str());
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
    if (const std::string problem = api_key_problem(entered_api_key()); !problem.empty()) {
        set_status("icon_close_circle", "danger", problem.c_str());
        return false;
    }
    return true;
}

std::string ChangeHostModal::entered_api_key() {
    return std::string(helix::text_io::trim(lv_subject_get_string(&api_key_subject_)));
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

    // Save to config; the completion callback moves the client to it.
    set_active_printer_api_key(entered_api_key());
    store_active_printer_address(host, port);

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
    // The caller runs on the next tick, outside any UpdateQueue drain, where a switch runs
    // in place and its answer is final.
    hide();
    auto on_add = add_callback_;
    helix::ui::run_next_tick([on_add, host, port]() { on_add(host, port); });
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

void ChangeHostModal::on_input_changed_cb(lv_observer_t* /*observer*/, lv_subject_t* subject) {
    if (active_instance_) {
        // The modal's own write of a probed port is not an edit.
        if (active_instance_->port_set_by_app_) {
            return;
        }
        if (subject == &active_instance_->host_port_subject_) {
            active_instance_->port_origin_ = helix::PortOrigin::Typed;
        }
    }
    // A status line describes the address it was shown for, so an edit clears it.
    if (active_instance_) {
        active_instance_->set_status(nullptr, nullptr, "");
    }
    // Reset validation when user edits the host, port or key after a successful test
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
    lv_xml_register_event_cb(nullptr, "on_change_host_printer_picked", on_printer_picked_cb);

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

void ChangeHostModal::on_printer_picked_cb(lv_event_t* e) {
    if (active_instance_) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        active_instance_->handle_printer_picked(lv_dropdown_get_selected(dropdown));
    }
}

// ============================================================================
// Printer Discovery
// ============================================================================

void ChangeHostModal::start_discovery() {
    stop_discovery();
    discovery_ = helix::make_printer_discovery();
    lv_subject_set_int(&discovery_active_subject_, discovery_ ? 1 : 0);
    if (!discovery_) {
        return;
    }
    show_discovered({});
    // Discovery calls back on the main thread; stop_discovery() runs before this modal
    // hides or goes away.
    auto tok = discovery_lifetime_.token();
    discovery_->start_discovery([this, tok](const std::vector<helix::DiscoveredPrinter>& found) {
        if (tok.expired()) {
            return;
        }
        show_discovered(found);
    });
}

void ChangeHostModal::stop_discovery() {
    discovery_lifetime_.invalidate();
    if (discovery_) {
        discovery_->stop_discovery();
        discovery_.reset();
    }
    discovered_.clear();
}

void ChangeHostModal::show_discovered(const std::vector<helix::DiscoveredPrinter>& printers) {
    discovered_ = printers;
    if (!dialog()) {
        return;
    }
    lv_obj_t* dropdown = helix::ui::find_required(dialog(), "printer_dropdown", get_name());
    if (dropdown) {
        const bool searching = discovery_ && discovery_->is_discovering();
        lv_dropdown_set_options(dropdown,
                                helix::discovered_printer_options(printers, searching).c_str());
    }
}

void ChangeHostModal::handle_printer_picked(uint32_t selected) {
    const helix::DiscoveredPrinter* picked = helix::discovered_printer_at(discovered_, selected);
    if (!picked) {
        return;
    }
    spdlog::info("[ChangeHostModal] Picked discovered printer {}:{}", picked->ip_address,
                 picked->port);
    const std::string host = picked->ip_address;
    const std::string port = std::to_string(picked->port);
    lv_subject_copy_string(&host_ip_subject_, host.c_str());
    // The discovered port is where Moonraker answered, not something the user typed.
    port_set_by_app_ = true;
    lv_subject_copy_string(&host_port_subject_, port.c_str());
    port_set_by_app_ = false;
    port_origin_ = helix::PortOrigin::Discovered;
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

void show_add_printer_modal(std::function<bool(const std::string& host, int port)> on_add) {
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

/// The prompt still describes the connection: the user did not switch printers and it did
/// not come up meanwhile.
bool failure_still_current(const std::string& printer_id) {
    IMoonrakerClient* client = get_moonraker_client();
    return Config::get_instance()->get_active_printer_id() == printer_id &&
           !(client && client->get_connection_state() == ConnectionState::CONNECTED);
}

/// Offers @p moved, the printer found at a new address, in place of the plain prompt.
/// Returns false when there is nothing to offer.
bool offer_moved_address(const std::string& title, const std::string& message,
                         const std::optional<DiscoveredPrinter>& moved,
                         const helix::ui::ConfirmOptions& opts);

void present_connection_failed(const std::string& title, const std::string& message,
                               const std::optional<DiscoveredPrinter>& moved) {
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

    if (offer_moved_address(title, message, moved, opts)) {
        return;
    }

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
    std::optional<DiscoveredPrinter> moved;
    bool held = false;
    lv_timer_t* timer = nullptr;
};

DeferredFailure& deferred_failure() {
    static DeferredFailure d;
    return d;
}

constexpr uint32_t kChooserPollMs = 300;

void defer_connection_failed(const std::string& title, const std::string& message,
                             const std::optional<DiscoveredPrinter>& moved) {
    DeferredFailure& d = deferred_failure();
    d.title = title;
    d.message = message;
    d.moved = moved;
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
            if (!failure_still_current(pending.printer_id)) {
                spdlog::debug(
                    "[ChangeHost] Dropping a connection-failed prompt the user moved past");
                return;
            }
            present_connection_failed(pending.title, pending.message, pending.moved);
        },
        kChooserPollMs, nullptr);
    lv_timer_set_repeat_count(d.timer, 1);
}

void route_connection_failed(const std::string& title, const std::string& message,
                             const std::optional<DiscoveredPrinter>& moved) {
    if (printer_chooser_open()) {
        defer_connection_failed(title, message, moved);
        return;
    }
    present_connection_failed(title, message, moved);
}

// ----------------------------------------------------------------------------
// Rediscovery: an mDNS browse for the printer at a new address. The firmware has no
// mDNS (its MdnsDiscovery is inert) and no inet_pton, so there it is compiled out and
// the prompt always shows the address it has.
// ----------------------------------------------------------------------------

} // namespace

#if !defined(HELIX_PLATFORM_ESP32)

namespace {

bool offer_moved_address(const std::string& title, const std::string& message,
                         const std::optional<DiscoveredPrinter>& moved,
                         const helix::ui::ConfirmOptions& opts) {
    // Offered, never adopted silently: a hostname is not unique, and a lookalike printer
    // answering while this one is off would otherwise take its jobs.
    if (!moved) {
        return false;
    }
    const std::string address = fmt::format("{}:{}", moved->ip_address, moved->port);
    const std::string body =
        message + "\n\n" + fill_placeholders(lv_tr("Found {} at {}."), {moved->name, address});
    auto use_new = [host = moved->ip_address, port = static_cast<int>(moved->port),
                    printer_id = Config::get_instance()->get_active_printer_id()] {
        // The prompt can outlive a printer switch; the address belongs to this one only.
        if (!failure_still_current(printer_id)) {
            spdlog::info("[ChangeHost] Not applying {}:{}: the printer changed or "
                         "reconnected while the prompt was open",
                         host, port);
            return;
        }
        store_active_printer_address(host, port);
        // Past the prompt's exit animation, like the host modal's Save.
        helix::ui::queue_update("ChangeHost::use_rediscovered",
                                [] { retarget_printer_connection(); });
    };
    helix::ui::modal_confirm(title.c_str(), body.c_str(), ModalSeverity::Error,
                             lv_tr("Use New Address"), use_new, opts);
    return true;
}

std::string normalized_hostname(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    const std::string suffix = ".local";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.resize(name.size() - suffix.size());
    }
    return name;
}

/// The one browse a failure escalation runs; `mdns` is set while it is in flight.
struct Rediscovery {
    std::function<std::unique_ptr<IMdnsDiscovery>()> source;
    std::unique_ptr<IMdnsDiscovery> mdns;
    std::string title;
    std::string message;
    std::string printer_id;
};

Rediscovery& rediscovery() {
    static Rediscovery r;
    return r;
}

void finish_rediscovery() {
    Rediscovery& r = rediscovery();
    const std::vector<DiscoveredPrinter> found = r.mdns->get_discovered_printers();
    r.mdns->stop_discovery();
    r.mdns.reset();
    if (!failure_still_current(r.printer_id)) {
        spdlog::debug("[ChangeHost] Dropping a connection-failed prompt the user moved past");
        return;
    }

    Config* cfg = Config::get_instance();
    const std::string host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
    const int port = cfg->get<int>(cfg->df() + "moonraker_port", helix::kDefaultMoonrakerPort);
    const auto moved =
        find_moved_printer(found, host, port,
                           {cfg->get<std::string>(cfg->df() + wizard::kHostname, ""),
                            cfg->get<std::string>(cfg->df() + wizard::PRINTER_NAME, "")});
    if (moved) {
        spdlog::info("[ChangeHost] {}:{} unreachable; mDNS finds '{}' at {}:{}", host, port,
                     moved->hostname, moved->ip_address, moved->port);
    } else {
        spdlog::info("[ChangeHost] mDNS browse found {} Moonraker(s), none of them this printer "
                     "at a new address",
                     found.size());
    }
    route_connection_failed(r.title, r.message, moved);
}

/// Browses once before the prompt shows, so it can offer the printer's new address.
/// Returns false when there is nothing to browse and the prompt should show now.
bool start_rediscovery(const std::string& title, const std::string& message) {
    Rediscovery& r = rediscovery();
    Config* cfg = Config::get_instance();
    const std::string host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
    if (!r.source || host.empty() || helix::is_moonraker_on_same_host(host)) {
        return false;
    }
    if (r.mdns) {
        spdlog::debug("[ChangeHost] A browse is already in flight for this failure");
        return true;
    }
    r.mdns = r.source();
    if (!r.mdns) {
        return false;
    }
    r.title = title;
    r.message = message;
    r.printer_id = cfg->get_active_printer_id();
    spdlog::info("[ChangeHost] Browsing mDNS for {} before the connection-failed prompt", host);
    r.mdns->start_discovery({});
    // TIMER_DTOR_OK: process-lifetime state with no owner object; a one-shot LVGL deletes.
    lv_timer_t* t =
        lv_timer_create([](lv_timer_t*) { finish_rediscovery(); }, kRediscoveryWindowMs, nullptr);
    lv_timer_set_repeat_count(t, 1);
    return true;
}

} // namespace

std::optional<DiscoveredPrinter> find_moved_printer(const std::vector<DiscoveredPrinter>& found,
                                                    const std::string& saved_host, int saved_port,
                                                    const std::vector<std::string>& identities) {
    // A saved name re-resolves to wherever the printer went; swapping it for the literal IP
    // mDNS reports would trade an address that heals for one that cannot.
    in_addr v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, saved_host.c_str(), &v4) != 1 &&
        inet_pton(AF_INET6, saved_host.c_str(), &v6) != 1) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (const auto& id : identities) {
        if (!id.empty()) {
            names.push_back(normalized_hostname(id));
        }
    }
    const DiscoveredPrinter* moved = nullptr;
    int matches = 0;
    for (const auto& p : found) {
        if (std::find(names.begin(), names.end(), normalized_hostname(p.hostname)) == names.end()) {
            continue;
        }
        if (p.ip_address == saved_host && p.port == saved_port) {
            return std::nullopt;
        }
        moved = &p;
        ++matches;
    }
    if (matches != 1) {
        return std::nullopt;
    }
    return *moved;
}

void set_printer_rediscovery_source(std::function<std::unique_ptr<IMdnsDiscovery>()> source) {
    rediscovery().source = std::move(source);
}

#else // HELIX_PLATFORM_ESP32

namespace {

bool offer_moved_address(const std::string&, const std::string&,
                         const std::optional<DiscoveredPrinter>&,
                         const helix::ui::ConfirmOptions&) {
    return false;
}

bool start_rediscovery(const std::string&, const std::string&) {
    return false;
}

} // namespace

std::optional<DiscoveredPrinter> find_moved_printer(const std::vector<DiscoveredPrinter>&,
                                                    const std::string&, int,
                                                    const std::vector<std::string>&) {
    return std::nullopt;
}

void set_printer_rediscovery_source(std::function<std::unique_ptr<IMdnsDiscovery>()>) {}

#endif // HELIX_PLATFORM_ESP32

namespace {

void begin_connection_failed(const std::string& title, const std::string& message) {
    if (!start_rediscovery(title, message)) {
        route_connection_failed(title, message, std::nullopt);
    }
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
                            [title, message]() { begin_connection_failed(title, message); });
}

} // namespace helix::ui
