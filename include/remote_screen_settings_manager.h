// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {

/// Default port of the built-in screen server.
constexpr int kRemoteScreenDefaultPort = 7130;

/**
 * @brief The built-in screen server's settings (prestonbrown/helixscreen#1825).
 *
 * Two opt-ins, both off by default: share the screen (view), then let a browser
 * drive it (control). Control is only ever on while view is. A third choice, on by
 * default, lists the shared screen among Moonraker's webcams for Mainsail and Fluidd.
 * All three belong to the active printer: a switch reloads them from its config.
 */
class RemoteScreenSettingsManager {
  public:
    static RemoteScreenSettingsManager& instance();

    /// The static PersistedSetting table this manager is built from.
    static settings::TableView persisted_table();

    RemoteScreenSettingsManager(const RemoteScreenSettingsManager&) = delete;
    RemoteScreenSettingsManager& operator=(const RemoteScreenSettingsManager&) = delete;

    void init_subjects();
    void deinit_subjects();

    bool get_enabled() const {
        return settings_.get_bool(Key::Enabled);
    }
    /// Turning view off turns control off with it.
    void set_enabled(bool enabled);

    bool get_control() const {
        return settings_.get_bool(Key::Control);
    }
    /// Ignored (stays off) while view is off.
    void set_control(bool control);

    /// Whether a shared screen is listed in Moonraker's webcams. Means nothing while
    /// view is off.
    bool get_webcam() const {
        return settings_.get_bool(Key::Webcam);
    }
    void set_webcam(bool webcam);

    /// Re-read view, control and webcam for the active printer, then tell the printer
    /// handler. Runs on every printer switch, while the previous printer is still connected.
    void on_printer_changed();

    /// `/remote_screen/port`; not shown in the UI. 0 asks for an ephemeral port.
    int get_port() const;

    /// `/remote_screen/pair_secret`: the 6 digits a browser enters to pair; "" until
    /// control is first used facing the LAN.
    std::string get_pair_code() const;
    /// Persist and publish to the settings row. Main thread.
    void set_pair_code(const std::string& code);
    /// `/remote_screen/paired_tokens`: tokens of the paired browsers.
    std::vector<std::string> get_paired_tokens() const;
    void set_paired_tokens(const std::vector<std::string>& tokens);

    /// The uid Moonraker gave the screen's webcam entry on the active printer; "" when the
    /// screen has no entry it knows of. Main thread.
    std::string get_webcam_uid() const;
    /// Store @p uid for the printer whose config prefix (Config::df()) is @p printer.
    void set_webcam_uid(const std::string& printer, const std::string& uid);

    /// Whether the server faces the LAN (pairing rows shown). Set by the server's owner.
    void set_lan_mode(bool lan_mode);

    /// A browser touched the screen recently: drives the on-screen indicator.
    void set_control_active(bool active);

    /// "Forget paired browsers": hands the request to whoever owns the pairing store.
    void forget_paired_browsers();
    void set_forget_handler(std::function<void()> handler) {
        forget_handler_ = std::move(handler);
    }

    /// Called after view, control or webcam changes. The server's owner installs it, so the
    /// settings UI never has to know a server exists (the firmware build has none).
    void set_change_handler(std::function<void()> handler) {
        change_handler_ = std::move(handler);
    }
    /// Called by on_printer_changed(). Unlike the change handler it must not touch
    /// Moonraker: the connection is still the previous printer's.
    void set_printer_handler(std::function<void()> handler) {
        printer_handler_ = std::move(handler);
    }

    /// Whether the rows show at all: false where the firmware serves the screen.
    /// Starts false; RemoteScreenService decides at startup.
    void set_available(bool available);

    lv_subject_t* subject_enabled() {
        return settings_.subject(Key::Enabled);
    }
    lv_subject_t* subject_control() {
        return settings_.subject(Key::Control);
    }
    lv_subject_t* subject_webcam() {
        return settings_.subject(Key::Webcam);
    }

  private:
    RemoteScreenSettingsManager();
    ~RemoteScreenSettingsManager() = default;

    enum class Key : uint8_t { Enabled, Control, Webcam, COUNT };

    SubjectManager subjects_;
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;
    void notify_changed();
    /// A config edited by hand can hold control without view.
    void drop_control_without_view();

    std::function<void()> change_handler_;
    std::function<void()> printer_handler_;
    std::function<void()> forget_handler_;
    helix::Subject available_subject_{};
    helix::Subject lan_mode_subject_{};
    helix::Subject control_active_subject_{};
    helix::Subject pair_code_subject_{};
    char pair_code_buf_[16] = {};
    bool subjects_initialized_ = false;
};

} // namespace helix
