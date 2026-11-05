// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_ipv4_settings.h
 * @brief Static or DHCP IPv4 for one network interface, with keep-or-revert
 *
 * One overlay serves both cards of Network Settings; open() names the link.
 * Every change, static or back to DHCP, is applied and then held for
 * helix::net::kIpv4ConfirmSeconds behind a "Keep <address>?" dialog: Keep
 * makes it final, anything else (Revert now, a dismissal, the countdown
 * running out) restores the config it replaced. The config to restore is
 * written to disk before the change is applied, so a restart mid-countdown
 * restores it at the next start (WiFiManager / EthernetManager).
 *
 * Reverting is the default because a wrong address can leave the printer
 * unreachable from the network, and this screen is then the only way back.
 */

#pragma once

#include "ui_modal.h"
#include "ui_timer_guard.h"

#include "ethernet_manager.h"
#include "ipv4_config.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <functional>
#include <memory>
#include <string>

namespace helix::settings {

/// "Keep <address>?" with the countdown; [Revert now] [Keep].
class Ipv4KeepModal : public Modal {
  public:
    const char* get_name() const override {
        return "IPv4 Keep";
    }
    const char* component_name() const override {
        return "ipv4_keep_modal";
    }

    /// Keep runs @p on_keep; Revert now, ESC and every dismissal run @p on_revert.
    void set_callbacks(std::function<void()> on_keep, std::function<void()> on_revert) {
        on_keep_ = std::move(on_keep);
        on_revert_ = std::move(on_revert);
    }

  protected:
    void on_show() override;
    void on_ok() override;
    void on_cancel() override;
    void on_dismissed() override;

  private:
    std::function<void()> on_keep_;
    std::function<void()> on_revert_;
};

/// The four fields of the IPv4 form, each typed on the numeric keypad.
enum class Ipv4FormField { Address, Netmask, Gateway, Dns };

class Ipv4SettingsOverlay : public OverlayBase {
  public:
    Ipv4SettingsOverlay() = default;
    ~Ipv4SettingsOverlay() override;

    Ipv4SettingsOverlay(const Ipv4SettingsOverlay&) = delete;
    Ipv4SettingsOverlay& operator=(const Ipv4SettingsOverlay&) = delete;

    const char* get_name() const override {
        return "IPv4 Settings";
    }
    const char* xml_component() const override {
        return "ipv4_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void init_subjects() override;
    void register_callbacks() override;
    void before_show() override;
    void on_activate() override;
    void cleanup() override;

    /// Opens the keypad on @p field, prefilled with its text.
    void edit_field(Ipv4FormField field);

    /**
     * @brief Show the overlay for @p link
     *
     * @param current_ip The address the link holds now; it pre-fills the IP
     *        field of an interface on DHCP. May be empty.
     */
    void open(lv_obj_t* parent_screen, helix::net::Ipv4Link link, const std::string& current_ip);

    /// A change is being applied, waits for Keep, or is being reverted.
    bool change_pending() const {
        return flow_.state() != helix::net::Ipv4ChangeFlow::State::Idle;
    }

  private:
    // --- Link plumbing: one call per action, whichever manager owns the link.
    bool link_supported();
    void read_link_config(std::function<void(bool ok, const helix::net::Ipv4Config&)> on_done);

    // --- Form
    void load_current();
    void fill_form(const helix::net::Ipv4Config& config);
    void set_error(const char* text);
    void set_busy(bool busy);

    // --- Change flow: Ipv4ChangeFlow decides, perform() carries it out
    void handle_apply();
    void handle_use_dhcp();
    void start_change(const helix::net::Ipv4Config& next);
    void perform(helix::net::Ipv4ChangeFlow::Action action);
    void run_apply();
    void run_revert();
    /// The overlay is going away: an unconfirmed change is reverted, and one
    /// still being applied reverts itself when it lands.
    void abandon_change();
    void show_keep_prompt();
    void on_countdown_tick();
    void stop_countdown();

    SubjectManager subjects_;
    lv_subject_t title_{};
    lv_subject_t current_{};
    lv_subject_t address_{};
    lv_subject_t netmask_{};
    lv_subject_t gateway_{};
    lv_subject_t dns_{};
    lv_subject_t error_{};
    lv_subject_t busy_{}; ///< 1 while a read or apply runs, or the form has nothing loaded
    lv_subject_t keep_title_{};
    lv_subject_t keep_message_{};

    char title_buf_[64] = {};
    char current_buf_[96] = {};
    char address_buf_[24] = {};
    char netmask_buf_[24] = {};
    char gateway_buf_[24] = {};
    char dns_buf_[24] = {};
    char error_buf_[192] = {};
    char keep_title_buf_[96] = {};
    char keep_message_buf_[192] = {};

    helix::net::Ipv4Link link_ = helix::net::Ipv4Link::Wifi;
    std::string current_ip_;
    helix::net::Ipv4Config current_config_; ///< What the link runs now, as last read
    bool loaded_ = false;

    helix::net::Ipv4Config previous_config_; ///< What a revert goes back to
    helix::net::Ipv4ChangeFlow flow_;
    helix::ui::LvglTimerGuard countdown_timer_;
    uint32_t shown_seconds_ = 0;
    Ipv4KeepModal keep_modal_;

    std::unique_ptr<EthernetManager> ethernet_manager_;
};

/// The process-wide IPv4 overlay. It outlives its widget tree, so a countdown
/// keeps running whatever the navigation stack does.
Ipv4SettingsOverlay& get_ipv4_settings_overlay();

/// Opens the overlay for @p link: what Network Settings' rows call.
void open_ipv4_settings(lv_obj_t* parent_screen, helix::net::Ipv4Link link,
                        const std::string& current_ip);

/// Network Settings' row text for @p config: "DHCP" or "Static <address>".
std::string ipv4_row_summary(const helix::net::Ipv4Config& config);

} // namespace helix::settings
