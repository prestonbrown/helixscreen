// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_ipv4_settings.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_keyboard_manager.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "core/http_executor.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "ethernet_manager.h"
#include "ipv4_config_system.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"
#include "wifi_manager.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix::settings {

namespace {

constexpr uint32_t kCountdownTickMs = 250;

// The sentence the form shows for a validation verdict, translated.
const char* ipv4_problem_text(helix::net::Ipv4Problem problem, helix::net::Ipv4Field field) {
    switch (problem) {
    case helix::net::Ipv4Problem::None:
        return "";
    case helix::net::Ipv4Problem::Empty:
        switch (field) {
        case helix::net::Ipv4Field::Address:
            return lv_tr("Enter an IP address.");
        case helix::net::Ipv4Field::Netmask:
            return lv_tr("Enter a subnet mask.");
        case helix::net::Ipv4Field::Gateway:
            return lv_tr("Enter a gateway address.");
        case helix::net::Ipv4Field::Dns:
        case helix::net::Ipv4Field::None:
            return lv_tr("Enter a DNS server address.");
        }
        break;
    case helix::net::Ipv4Problem::NotAnAddress:
    case helix::net::Ipv4Problem::NotUsable:
        switch (field) {
        case helix::net::Ipv4Field::Gateway:
            return lv_tr("The gateway is not a valid address.");
        case helix::net::Ipv4Field::Dns:
            return lv_tr("The DNS server is not a valid address.");
        default:
            return lv_tr("The IP address is not a valid address.");
        }
    case helix::net::Ipv4Problem::NotAMask:
        return lv_tr("The subnet mask is not valid. Use a form such as 255.255.255.0 or 24.");
    case helix::net::Ipv4Problem::NetworkAddress:
    case helix::net::Ipv4Problem::BroadcastAddress:
        return field == helix::net::Ipv4Field::Gateway
                   ? lv_tr("The gateway cannot be the subnet's network or broadcast address.")
                   : lv_tr("The IP address cannot be the subnet's network or broadcast address.");
    case helix::net::Ipv4Problem::GatewayOutsideSubnet:
        return lv_tr("The gateway is not on the same subnet as the IP address.");
    case helix::net::Ipv4Problem::GatewayIsAddress:
        return lv_tr("The gateway cannot be the printer's own address.");
    }
    return "";
}

void copy_to(lv_subject_t& subject, const std::string& text) {
    lv_subject_copy_string(&subject, text.c_str());
}

} // namespace

std::string ipv4_row_summary(const helix::net::Ipv4Config& config) {
    if (config.dhcp) {
        return lv_tr("DHCP");
    }
    return std::string(lv_tr("Static")) + " " + config.address;
}

// ============================================================================
// Keep-or-revert dialog
// ============================================================================

void Ipv4KeepModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
}

void Ipv4KeepModal::on_ok() {
    auto keep = on_keep_;
    hide(ModalCloseReason::ButtonPress);
    if (keep) {
        keep();
    }
}

void Ipv4KeepModal::on_cancel() {
    auto revert = on_revert_;
    hide(ModalCloseReason::ButtonPress);
    if (revert) {
        revert();
    }
}

void Ipv4KeepModal::on_dismissed() {
    // Unanswered is not a yes: a backdrop tap or a rebuild restores the old config.
    auto revert = on_revert_;
    if (revert) {
        revert();
    }
}

// ============================================================================
// Overlay
// ============================================================================

Ipv4SettingsOverlay& get_ipv4_settings_overlay() {
    return helix::lazy_global<Ipv4SettingsOverlay>("Ipv4SettingsOverlay");
}

void open_ipv4_settings(lv_obj_t* parent_screen, helix::net::Ipv4Link link,
                        const std::string& current_ip) {
    get_ipv4_settings_overlay().open(parent_screen, link, current_ip);
}

Ipv4SettingsOverlay::~Ipv4SettingsOverlay() {
    abandon_change();
    stop_countdown();
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }
}

void Ipv4SettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(title_, title_buf_, "", "ipv4_title", subjects_);
        UI_MANAGED_SUBJECT_STRING(current_, current_buf_, "", "ipv4_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(address_, address_buf_, "", "ipv4_address", subjects_);
        UI_MANAGED_SUBJECT_STRING(netmask_, netmask_buf_, "", "ipv4_netmask", subjects_);
        UI_MANAGED_SUBJECT_STRING(gateway_, gateway_buf_, "", "ipv4_gateway", subjects_);
        UI_MANAGED_SUBJECT_STRING(dns_, dns_buf_, "", "ipv4_dns", subjects_);
        UI_MANAGED_SUBJECT_STRING(error_, error_buf_, "", "ipv4_error", subjects_);
        UI_MANAGED_SUBJECT_INT(busy_, 1, "ipv4_busy", subjects_);
        UI_MANAGED_SUBJECT_STRING(keep_title_, keep_title_buf_, "", "ipv4_keep_title", subjects_);
        UI_MANAGED_SUBJECT_STRING(keep_message_, keep_message_buf_, "", "ipv4_keep_message",
                                  subjects_);
        auto& state = get_printer_state();
        job_observer_ = helix::ui::observe<int>(
            state.print_state().get_job_holds_machine_subject(), this,
            [](Ipv4SettingsOverlay* self, int holds) {
                if (holds) {
                    self->perform(self->flow_.job_started());
                }
            },
            state.get_subjects_lifetime());
    });
}

void Ipv4SettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_ipv4_apply", [](lv_event_t*) { get_ipv4_settings_overlay().handle_apply(); }},
        {"on_ipv4_use_dhcp", [](lv_event_t*) { get_ipv4_settings_overlay().handle_use_dhcp(); }},
        {"on_ipv4_edit_address",
         [](lv_event_t*) { get_ipv4_settings_overlay().edit_field(Ipv4FormField::Address); }},
        {"on_ipv4_edit_netmask",
         [](lv_event_t*) { get_ipv4_settings_overlay().edit_field(Ipv4FormField::Netmask); }},
        {"on_ipv4_edit_gateway",
         [](lv_event_t*) { get_ipv4_settings_overlay().edit_field(Ipv4FormField::Gateway); }},
        {"on_ipv4_edit_dns",
         [](lv_event_t*) { get_ipv4_settings_overlay().edit_field(Ipv4FormField::Dns); }},
    });
}

void Ipv4SettingsOverlay::open(lv_obj_t* parent_screen, helix::net::Ipv4Link link,
                               const std::string& current_ip) {
    if (change_pending()) {
        spdlog::info("[{}] A change is still waiting for Keep; not reopening", get_name());
        return;
    }
    link_ = link;
    current_ip_ = current_ip;
    show(parent_screen);
}

void Ipv4SettingsOverlay::before_show() {
    // The fields are typed on the numeric keypad, so they opt out of the software
    // keyboard <text_input> gives every textarea; without this a tap raises both.
    for (const char* name :
         {"ipv4_address_input", "ipv4_netmask_input", "ipv4_gateway_input", "ipv4_dns_input"}) {
        if (lv_obj_t* field = helix::ui::find_required(overlay_root_, name, get_name())) {
            KeyboardManager::instance().unregister_textarea(field);
        }
    }
}

void Ipv4SettingsOverlay::edit_field(Ipv4FormField field) {
    if (!loaded_ || change_pending() || lv_subject_get_int(&busy_)) {
        return;
    }
    lv_subject_t* subject = &address_;
    const char* title = lv_tr("IP address");
    switch (field) {
    case Ipv4FormField::Address:
        break;
    case Ipv4FormField::Netmask:
        subject = &netmask_;
        title = lv_tr("Subnet mask");
        break;
    case Ipv4FormField::Gateway:
        subject = &gateway_;
        title = lv_tr("Gateway");
        break;
    case Ipv4FormField::Dns:
        subject = &dns_;
        title = lv_tr("DNS server");
        break;
    }
    ui_keypad_config_t config{};
    config.title_label = title;
    config.unit_label = "";
    config.mode = helix::ui::KeypadInput::Mode::Ipv4;
    config.initial_text = lv_subject_get_string(subject);
    // The overlay is process-wide, so the subject outlives the keypad.
    config.on_confirm_text = [this, subject](const std::string& text) {
        lv_subject_copy_string(subject, text.c_str());
        set_error("");
    };
    ui_keypad_show(&config);
}

void Ipv4SettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    // After create: the header's static title is applied over the subject's
    // value, and only a publish retitles it.
    copy_to(title_,
            link_ == helix::net::Ipv4Link::Wifi ? lv_tr("WiFi IPv4") : lv_tr("Ethernet IPv4"));
    if (!change_pending()) {
        load_current();
    }
}

void Ipv4SettingsOverlay::cleanup() {
    OverlayBase::cleanup();
    stop_countdown();
    ethernet_manager_.reset();
}

// ----------------------------------------------------------------------------
// Link plumbing
// ----------------------------------------------------------------------------

bool Ipv4SettingsOverlay::link_supported() {
    if (link_ == helix::net::Ipv4Link::Wifi) {
        auto wifi = get_wifi_manager();
        return wifi && wifi->supports_ipv4_config();
    }
    if (!ethernet_manager_) {
        ethernet_manager_ = std::make_unique<EthernetManager>();
    }
    return ethernet_manager_->supports_ipv4_config();
}

void Ipv4SettingsOverlay::read_link_config(
    std::function<void(bool ok, const helix::net::Ipv4Config&)> on_done) {
    if (!link_supported()) {
        on_done(false, helix::net::Ipv4Config{});
        return;
    }
    // The object's token, not the view's: an answer still matters after Back.
    auto token = object_lifetime_.token();
    if (link_ == helix::net::Ipv4Link::Wifi) {
        get_wifi_manager()->get_ipv4_config_async(token, std::move(on_done));
    } else {
        ethernet_manager_->get_ipv4_config_async(token, std::move(on_done));
    }
}

// ----------------------------------------------------------------------------
// Form
// ----------------------------------------------------------------------------

void Ipv4SettingsOverlay::set_error(const char* text) {
    lv_subject_copy_string(&error_, text ? text : "");
}

void Ipv4SettingsOverlay::set_busy(bool busy) {
    lv_subject_set_int(&busy_, busy ? 1 : 0);
}

void Ipv4SettingsOverlay::load_current() {
    loaded_ = false;
    set_busy(true);
    set_error("");
    copy_to(current_, lv_tr("Reading the current settings..."));
    const helix::net::Ipv4Link link = link_;
    read_link_config([this, link](bool ok, const helix::net::Ipv4Config& config) {
        if (link != link_ || change_pending()) {
            return;
        }
        if (!ok) {
            copy_to(current_, "");
            set_error(lv_tr("Could not read the current IPv4 settings."));
            return; // stays busy: nothing to change from
        }
        current_config_ = config;
        loaded_ = true;
        fill_form(config);
        set_busy(false);
    });
}

void Ipv4SettingsOverlay::fill_form(const helix::net::Ipv4Config& config) {
    helix::net::Ipv4Form form = helix::net::ipv4_form_from_config(config);
    if (config.dhcp && form.address.empty()) {
        form.address = current_ip_;
    }
    copy_to(address_, form.address);
    copy_to(netmask_, form.netmask);
    copy_to(gateway_, form.gateway);
    copy_to(dns_, form.dns);
    copy_to(current_, std::string(lv_tr("Currently:")) + " " + ipv4_row_summary(config));
}

// ----------------------------------------------------------------------------
// Change flow
// ----------------------------------------------------------------------------

void Ipv4SettingsOverlay::handle_apply() {
    if (!loaded_ || change_pending() || lv_subject_get_int(&busy_)) {
        return;
    }
    helix::net::Ipv4Form form;
    form.address = lv_subject_get_string(&address_);
    form.netmask = lv_subject_get_string(&netmask_);
    form.gateway = lv_subject_get_string(&gateway_);
    form.dns = lv_subject_get_string(&dns_);
    const helix::net::Ipv4Verdict verdict = helix::net::validate_ipv4_form(form);
    if (!verdict.ok()) {
        set_error(ipv4_problem_text(verdict.problem, verdict.field));
        return;
    }
    start_change(helix::net::ipv4_keep_unshown(current_config_, verdict.config));
}

void Ipv4SettingsOverlay::handle_use_dhcp() {
    if (!loaded_ || change_pending() || lv_subject_get_int(&busy_)) {
        return;
    }
    // DHCP hands out DNS servers too; an override is left to a static config.
    start_change(helix::net::Ipv4Config{});
}

void Ipv4SettingsOverlay::start_change(const helix::net::Ipv4Config& next) {
    set_error("");
    previous_config_ = current_config_;
    perform(flow_.begin(current_config_, next));
}

void Ipv4SettingsOverlay::perform(helix::net::Ipv4ChangeFlow::Action action) {
    using Action = helix::net::Ipv4ChangeFlow::Action;
    switch (action) {
    case Action::None:
        return;
    case Action::Close:
        close();
        return;
    case Action::Apply:
        run_apply();
        return;
    case Action::ShowKeepPrompt:
        show_keep_prompt();
        return;
    case Action::Revert:
        stop_countdown();
        // Reached from the dialog's own callbacks too: clearing them first means
        // a dialog already closing cannot ask for a second revert.
        keep_modal_.set_callbacks(nullptr, nullptr);
        if (keep_modal_.is_visible()) {
            keep_modal_.hide();
        }
        run_revert();
        return;
    case Action::KeptForJob:
    case Action::Kept:
        stop_countdown();
        if (action == Action::KeptForJob) {
            keep_modal_.set_callbacks(nullptr, nullptr);
            if (keep_modal_.is_visible()) {
                keep_modal_.hide();
            }
        }
        current_config_ = flow_.next();
        set_busy(false);
        spdlog::info("[{}] Kept {}", get_name(), helix::net::ipv4_config_text(current_config_));
        if (!helix::net::clear_pending_ipv4_revert()) {
            NOTIFY_ERROR("{}", lv_tr("The new IPv4 settings could not be marked as kept, so the "
                                     "next start undoes them."));
        } else if (action == Action::KeptForJob) {
            NOTIFY_INFO(lv_tr("Kept {}: a print started."), current_config_.dhcp
                                                                ? std::string(lv_tr("DHCP"))
                                                                : current_config_.address);
        } else {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("IPv4 settings saved"),
                                          2500);
        }
        close();
        return;
    case Action::Restored:
        current_config_ = previous_config_;
        fill_form(previous_config_);
        set_busy(false);
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Previous IPv4 settings restored"),
                                      3000);
        return;
    case Action::RestoreFailed:
        // The record stays, so the next start tries again. What the link runs
        // now is unknown, so nothing more is applied until the screen reads it.
        spdlog::error("[{}] Could not restore {}", get_name(),
                      helix::net::ipv4_config_text(previous_config_));
        set_error(lv_tr("Could not restore the previous IPv4 settings. They are restored at the "
                        "next start."));
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Could not restore the previous IPv4 settings"));
        loaded_ = false;
        return;
    }
}

void Ipv4SettingsOverlay::run_apply() {
    set_busy(true);
    const helix::net::Ipv4Config next = flow_.next();
    spdlog::info("[{}] Applying {} on {}", get_name(), helix::net::ipv4_config_text(next),
                 helix::net::ipv4_link_name(link_));
    // The object's token: after Back the answer still raises the Keep dialog.
    auto token = object_lifetime_.token();
    auto on_done = [this](bool ok) {
        if (!ok) {
            set_error(lv_tr("Could not apply the new IPv4 settings."));
        }
        perform(flow_.applied(ok, lv_tick_get()));
    };
    if (!link_supported()) {
        on_done(false);
    } else if (link_ == helix::net::Ipv4Link::Wifi) {
        get_wifi_manager()->apply_ipv4_change_async(previous_config_, next, flow_.abandoned(),
                                                    token, on_done);
    } else {
        ethernet_manager_->apply_ipv4_change_async(previous_config_, next, flow_.abandoned(), token,
                                                   on_done);
    }
}

void Ipv4SettingsOverlay::run_revert() {
    set_busy(true);
    auto token = object_lifetime_.token();
    const helix::net::Ipv4Link link = link_;
    helix::http::HttpExecutor::fast().start();
    helix::http::HttpExecutor::fast().submit([token, link]() {
        const bool ok = helix::net::restore_pending_ipv4(link, /*stale_only=*/false).ok();
        token.defer("Ipv4SettingsOverlay::reverted", [ok]() {
            auto& self = get_ipv4_settings_overlay();
            self.perform(self.flow_.reverted(ok));
        });
    });
}

void Ipv4SettingsOverlay::abandon_change() {
    if (flow_.teardown() != helix::net::Ipv4ChangeFlow::Action::Revert) {
        return;
    }
    // Nobody is left to answer: restore without waiting for the result.
    spdlog::info("[{}] Leaving with a change unconfirmed: reverting it", get_name());
    stop_countdown();
    const helix::net::Ipv4Link link = link_;
    helix::http::HttpExecutor::fast().start();
    helix::http::HttpExecutor::fast().submit(
        [link]() { helix::net::restore_pending_ipv4(link, /*stale_only=*/false); });
}

void Ipv4SettingsOverlay::show_keep_prompt() {
    char title[sizeof(keep_title_buf_)];
    const std::string what = flow_.next().dhcp ? std::string(lv_tr("DHCP")) : flow_.next().address;
    std::snprintf(title, sizeof(title), lv_tr("Keep %s?"), what.c_str());
    lv_subject_copy_string(&keep_title_, title);

    shown_seconds_ = 0;
    on_countdown_tick();
    keep_modal_.set_callbacks([this]() { perform(flow_.keep()); },
                              [this]() { perform(flow_.reject()); });
    keep_modal_.show(lv_screen_active());
    countdown_timer_.reset(lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<Ipv4SettingsOverlay*>(lv_timer_get_user_data(t))->on_countdown_tick();
        },
        kCountdownTickMs, this));
}

void Ipv4SettingsOverlay::on_countdown_tick() {
    const uint32_t now = lv_tick_get();
    const auto action = flow_.tick(now);
    if (action != helix::net::Ipv4ChangeFlow::Action::None) {
        spdlog::info("[{}] Nobody kept {}: reverting", get_name(),
                     helix::net::ipv4_config_text(flow_.next()));
        perform(action);
        return;
    }
    const uint32_t left = flow_.countdown().seconds_left(now);
    if (left == shown_seconds_) {
        return;
    }
    shown_seconds_ = left;
    char message[sizeof(keep_message_buf_)];
    std::snprintf(message, sizeof(message),
                  lv_tr("The previous settings come back in %d seconds unless you keep these."),
                  static_cast<int>(left));
    lv_subject_copy_string(&keep_message_, message);
}

void Ipv4SettingsOverlay::stop_countdown() {
    countdown_timer_.reset();
}

} // namespace helix::settings
