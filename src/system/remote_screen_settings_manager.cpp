// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_settings_manager.h"

#include "config.h"
#include "core/static_subject_registry.h"
#include "printer_cache_registry.h"
#include "spdlog/spdlog.h"

#include <iterator>

using namespace helix;

using settings::Scope;
// Row order is RemoteScreenSettingsManager::Key. Per printer, so sharing one printer's
// screen never lists it among another printer's webcams.
static constexpr settings::PersistedSetting kRemoteScreenSettings[] = {
    {"settings_remote_screen_enabled", "remote_screen/enabled", Scope::PerPrinter, true, 0, 0, 1,
     "remote_screen_enabled"},
    {"settings_remote_screen_control", "remote_screen/control", Scope::PerPrinter, true, 0, 0, 1,
     "remote_screen_control"},
    {"settings_remote_screen_webcam", "remote_screen/webcam", Scope::PerPrinter, true, 1, 0, 1,
     "remote_screen_webcam"},
};

RemoteScreenSettingsManager& RemoteScreenSettingsManager::instance() {
    static RemoteScreenSettingsManager instance;
    return instance;
}

settings::TableView RemoteScreenSettingsManager::persisted_table() {
    return {kRemoteScreenSettings, std::size(kRemoteScreenSettings)};
}

RemoteScreenSettingsManager::RemoteScreenSettingsManager() : settings_(kRemoteScreenSettings) {}

void RemoteScreenSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    settings_.init(subjects_);
    UI_MANAGED_SUBJECT_INT(available_subject_, 0, "remote_screen_available", subjects_);
    UI_MANAGED_SUBJECT_INT(lan_mode_subject_, 0, "remote_screen_lan_mode", subjects_);
    UI_MANAGED_SUBJECT_INT(control_active_subject_, 0, "remote_screen_control_active", subjects_);
    UI_MANAGED_SUBJECT_STRING(pair_code_subject_, pair_code_buf_, get_pair_code().c_str(),
                              "remote_screen_pair_code", subjects_);
    drop_control_without_view();
    subjects_initialized_ = true;
    PrinterCacheRegistry::instance().register_invalidator("RemoteScreenSettingsManager", [] {
        RemoteScreenSettingsManager::instance().on_printer_changed();
    });
    // RemoteScreenService decides availability once per process and creates the
    // control indicator once on the top layer, so these subjects outlive a printer
    // switch: rebuilt, they would come back hidden and unobserved.
    StaticSubjectRegistry::instance().register_deinit(
        "RemoteScreenSettingsManager",
        []() { RemoteScreenSettingsManager::instance().deinit_subjects(); },
        StaticSubjectRegistry::Lifetime::Process);
}

void RemoteScreenSettingsManager::drop_control_without_view() {
    if (get_control() && !get_enabled()) {
        settings_.set(Key::Control, false);
    }
}

void RemoteScreenSettingsManager::on_printer_changed() {
    if (!subjects_initialized_) {
        return;
    }
    settings_.reload();
    drop_control_without_view();
    spdlog::debug("[RemoteScreenSettings] Active printer changed: share {}, control {}, webcam {}",
                  get_enabled(), get_control(), get_webcam());
    if (printer_handler_) {
        printer_handler_();
    }
}

void RemoteScreenSettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void RemoteScreenSettingsManager::set_enabled(bool enabled) {
    spdlog::info("[RemoteScreenSettings] Share screen {}", enabled ? "on" : "off");
    settings_.set(Key::Enabled, enabled);
    if (!enabled && get_control()) {
        settings_.set(Key::Control, false);
    }
    notify_changed();
}

void RemoteScreenSettingsManager::set_control(bool control) {
    if (control && !get_enabled()) {
        spdlog::debug("[RemoteScreenSettings] Control refused while sharing is off");
        // Re-publish so a toggle that flipped optimistically flips back.
        settings_.set(Key::Control, false);
        return;
    }
    spdlog::info("[RemoteScreenSettings] Remote control {}", control ? "on" : "off");
    settings_.set(Key::Control, control);
    notify_changed();
}

void RemoteScreenSettingsManager::set_webcam(bool webcam) {
    spdlog::info("[RemoteScreenSettings] Moonraker webcam entry {}", webcam ? "on" : "off");
    settings_.set(Key::Webcam, webcam);
    notify_changed();
}

void RemoteScreenSettingsManager::notify_changed() {
    if (change_handler_) {
        change_handler_();
    }
}

void RemoteScreenSettingsManager::set_available(bool available) {
    if (subjects_initialized_) {
        lv_subject_set_int(&available_subject_, available ? 1 : 0);
    }
}

std::string RemoteScreenSettingsManager::get_pair_code() const {
    return Config::get_instance()->get<std::string>("/remote_screen/pair_secret", "");
}

void RemoteScreenSettingsManager::set_pair_code(const std::string& code) {
    Config* config = Config::get_instance();
    config->set<std::string>("/remote_screen/pair_secret", code);
    config->save();
    if (subjects_initialized_) {
        lv_subject_copy_string(&pair_code_subject_, code.c_str());
    }
}

std::vector<std::string> RemoteScreenSettingsManager::get_paired_tokens() const {
    return Config::get_instance()->get_string_array("/remote_screen/paired_tokens");
}

void RemoteScreenSettingsManager::set_paired_tokens(const std::vector<std::string>& tokens) {
    Config* config = Config::get_instance();
    config->set<nlohmann::json>("/remote_screen/paired_tokens", nlohmann::json(tokens));
    config->save();
}

std::string RemoteScreenSettingsManager::get_webcam_uid() const {
    Config* config = Config::get_instance();
    return config->get<std::string>(config->df() + "remote_screen_webcam_uid", "");
}

void RemoteScreenSettingsManager::set_webcam_uid(const std::string& printer,
                                                 const std::string& uid) {
    Config* config = Config::get_instance();
    const std::string key = printer + "remote_screen_webcam_uid";
    if (config->get<std::string>(key, "") == uid) {
        return;
    }
    config->set<std::string>(key, uid);
    config->save();
}

void RemoteScreenSettingsManager::set_lan_mode(bool lan_mode) {
    if (subjects_initialized_) {
        lv_subject_set_int(&lan_mode_subject_, lan_mode ? 1 : 0);
    }
}

void RemoteScreenSettingsManager::set_control_active(bool active) {
    if (subjects_initialized_ && lv_subject_get_int(&control_active_subject_) != (active ? 1 : 0)) {
        lv_subject_set_int(&control_active_subject_, active ? 1 : 0);
    }
}

void RemoteScreenSettingsManager::forget_paired_browsers() {
    if (forget_handler_) {
        forget_handler_();
    }
}

int RemoteScreenSettingsManager::get_port() const {
    return Config::get_instance()->get<int>("/remote_screen/port", kRemoteScreenDefaultPort);
}
