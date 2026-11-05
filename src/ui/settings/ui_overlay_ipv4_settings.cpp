// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_ipv4_settings.h"

#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"

#include "core/static_panel_registry.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix::settings {

namespace {

constexpr uint32_t kCountdownTickMs = 250;

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

Ipv4SettingsOverlay::~Ipv4SettingsOverlay() {
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
    });
}

void Ipv4SettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_ipv4_apply", [](lv_event_t*) { get_ipv4_settings_overlay().handle_apply(); }},
        {"on_ipv4_use_dhcp", [](lv_event_t*) { get_ipv4_settings_overlay().handle_use_dhcp(); }},
    });
}

void Ipv4SettingsOverlay::open(lv_obj_t* parent_screen, helix::net::Ipv4Link link,
                               const std::string& current_ip) {
    if (change_pending_) {
        spdlog::info("[{}] A change is still waiting for Keep; not reopening", get_name());
        return;
    }
    link_ = link;
    current_ip_ = current_ip;
    show(parent_screen);
}

void Ipv4SettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    // After create: the header's static title is applied over the subject's
    // value, and only a publish retitles it.
    copy_to(title_,
            link_ == helix::net::Ipv4Link::Wifi ? lv_tr("WiFi IPv4") : lv_tr("Ethernet IPv4"));
    if (!change_pending_) {
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

void Ipv4SettingsOverlay::apply_link_config(const helix::net::Ipv4Config& config,
                                            std::function<void(bool ok)> on_done) {
    if (!link_supported()) {
        on_done(false);
        return;
    }
    auto token = object_lifetime_.token();
    if (link_ == helix::net::Ipv4Link::Wifi) {
        get_wifi_manager()->apply_ipv4_config_async(config, token, std::move(on_done));
    } else {
        ethernet_manager_->apply_ipv4_config_async(config, token, std::move(on_done));
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
        if (link != link_ || change_pending_) {
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
    if (!loaded_ || change_pending_ || lv_subject_get_int(&busy_)) {
        return;
    }
    helix::net::Ipv4Form form;
    form.address = lv_subject_get_string(&address_);
    form.netmask = lv_subject_get_string(&netmask_);
    form.gateway = lv_subject_get_string(&gateway_);
    form.dns = lv_subject_get_string(&dns_);
    const helix::net::Ipv4Verdict verdict = helix::net::validate_ipv4_form(form);
    if (!verdict.ok()) {
        set_error(lv_tr(helix::net::ipv4_problem_text(verdict.problem, verdict.field)));
        return;
    }
    start_change(verdict.config);
}

void Ipv4SettingsOverlay::handle_use_dhcp() {
    if (!loaded_ || change_pending_ || lv_subject_get_int(&busy_)) {
        return;
    }
    // DHCP hands out DNS servers too; an override is left to a static config.
    start_change(helix::net::Ipv4Config{});
}

void Ipv4SettingsOverlay::start_change(const helix::net::Ipv4Config& next) {
    set_error("");
    if (next == current_config_) {
        close();
        return;
    }
    helix::net::PendingIpv4Revert pending;
    pending.link = link_;
    pending.previous = current_config_;
    pending.owner = helix::net::ipv4_process_owner();
    // Without the record a restart mid-countdown would keep an address nobody
    // confirmed, so nothing is applied unless it is on disk first.
    if (!helix::net::save_pending_ipv4_revert(pending)) {
        set_error(lv_tr("The change was not applied: the current settings could not be saved "
                        "for a revert."));
        return;
    }
    previous_config_ = current_config_;
    next_config_ = next;
    change_pending_ = true;
    set_busy(true);
    spdlog::info("[{}] Applying {} on {}", get_name(), helix::net::ipv4_config_text(next),
                 helix::net::ipv4_link_name(link_));
    apply_link_config(next, [this](bool ok) {
        if (!ok) {
            // A failed apply may still have written part of the change, so the
            // old config goes back on either way.
            set_error(lv_tr("Could not apply the new IPv4 settings."));
            revert_change();
            return;
        }
        show_keep_prompt();
    });
}

void Ipv4SettingsOverlay::show_keep_prompt() {
    char title[sizeof(keep_title_buf_)];
    const std::string what = next_config_.dhcp ? std::string(lv_tr("DHCP")) : next_config_.address;
    std::snprintf(title, sizeof(title), lv_tr("Keep %s?"), what.c_str());
    lv_subject_copy_string(&keep_title_, title);

    countdown_.start(lv_tick_get());
    shown_seconds_ = 0;
    on_countdown_tick();
    keep_modal_.set_callbacks([this]() { keep_change(); }, [this]() { revert_change(); });
    keep_modal_.show(lv_screen_active());
    countdown_timer_.reset(lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<Ipv4SettingsOverlay*>(lv_timer_get_user_data(t))->on_countdown_tick();
        },
        kCountdownTickMs, this));
}

void Ipv4SettingsOverlay::on_countdown_tick() {
    const uint32_t now = lv_tick_get();
    if (countdown_.expired(now)) {
        spdlog::info("[{}] Nobody kept {}: reverting", get_name(),
                     helix::net::ipv4_config_text(next_config_));
        revert_change();
        return;
    }
    const uint32_t left = countdown_.seconds_left(now);
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
    countdown_.stop();
    countdown_timer_.reset();
}

void Ipv4SettingsOverlay::keep_change() {
    if (!change_pending_ || !countdown_.running()) {
        return;
    }
    stop_countdown();
    helix::net::clear_pending_ipv4_revert();
    current_config_ = next_config_;
    change_pending_ = false;
    set_busy(false);
    spdlog::info("[{}] Kept {}", get_name(), helix::net::ipv4_config_text(next_config_));
    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("IPv4 settings saved"), 2500);
    close();
}

void Ipv4SettingsOverlay::revert_change() {
    if (!change_pending_) {
        return;
    }
    stop_countdown();
    // Reached from the dialog's own callbacks too; a dialog already closing
    // ignores this, and the callbacks are cleared so it cannot revert twice.
    keep_modal_.set_callbacks(nullptr, nullptr);
    if (keep_modal_.is_visible()) {
        keep_modal_.hide();
    }
    set_busy(true);
    apply_link_config(previous_config_, [this](bool ok) {
        change_pending_ = false;
        if (!ok) {
            // The record stays, so the next start tries again.
            spdlog::error("[{}] Could not restore {}", get_name(),
                          helix::net::ipv4_config_text(previous_config_));
            set_error(lv_tr("Could not restore the previous IPv4 settings. They are restored at "
                            "the next start."));
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Could not restore the previous IPv4 settings"));
            // What the link runs now is unknown: nothing more is applied until
            // the overlay reads it again.
            loaded_ = false;
            return;
        }
        helix::net::clear_pending_ipv4_revert();
        current_config_ = previous_config_;
        fill_form(previous_config_);
        set_busy(false);
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Previous IPv4 settings restored"),
                                      3000);
    });
}

} // namespace helix::settings
