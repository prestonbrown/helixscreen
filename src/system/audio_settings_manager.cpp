// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_settings_manager.h"

#include "config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "sound_manager.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>

using namespace helix;

AudioSettingsManager& AudioSettingsManager::instance() {
    static AudioSettingsManager instance;
    return instance;
}

AudioSettingsManager::AudioSettingsManager() {
    spdlog::trace("[AudioSettingsManager] Constructor");
}

void AudioSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[AudioSettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[AudioSettingsManager] Initializing subjects");

    Config* config = Config::get_instance();

    // Sounds master switch (default: false)
    bool sounds = config->get<bool>("/sounds_enabled", false);
    UI_MANAGED_SUBJECT_INT(sounds_enabled_subject_, sounds ? 1 : 0, "settings_sounds_enabled",
                           subjects_);

    // UI sounds (default: true)
    bool ui_sounds = config->get<bool>("/ui_sounds_enabled", true);
    UI_MANAGED_SUBJECT_INT(ui_sounds_enabled_subject_, ui_sounds ? 1 : 0,
                           "settings_ui_sounds_enabled", subjects_);

    // Volume (0-100, default 80)
    int volume = std::clamp(config->get<int>("/sounds/volume", 80), 0, 100);
    UI_MANAGED_SUBJECT_INT(volume_subject_, volume, "settings_volume", subjects_);

    // Completion alert mode (default: ALERT=2)
    int completion_mode = config->get<int>("/completion_alert", 2);
    completion_mode = std::max(0, std::min(2, completion_mode));
    UI_MANAGED_SUBJECT_INT(completion_alert_subject_, completion_mode, "settings_completion_alert",
                           subjects_);

    // Whether an ALSA backend with device selection is active (0/1). Seeded to
    // 0 here because SoundManager has not picked its backend yet at subject-init
    // time; refresh_audio_device_available() sets the real value once it has.
    UI_MANAGED_SUBJECT_INT(audio_device_available_subject_, 0, "settings_audio_device_available",
                           subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup with StaticSubjectRegistry
    StaticSubjectRegistry::instance().register_deinit(
        "AudioSettingsManager", []() { AudioSettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[AudioSettingsManager] Subjects initialized: sounds={}, ui_sounds={}, "
                  "volume={}, completion_alert={}",
                  sounds, ui_sounds, volume, completion_mode);
}

void AudioSettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[AudioSettingsManager] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[AudioSettingsManager] Subjects deinitialized");
}

// =============================================================================
// GETTERS / SETTERS
// =============================================================================

bool AudioSettingsManager::get_sounds_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&sounds_enabled_subject_)) != 0;
}

void AudioSettingsManager::set_sounds_enabled(bool enabled) {
    spdlog::info("[AudioSettingsManager] set_sounds_enabled({})", enabled);

    lv_subject_set_int(&sounds_enabled_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/sounds_enabled", enabled);
    config->save();
}

bool AudioSettingsManager::get_ui_sounds_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&ui_sounds_enabled_subject_)) != 0;
}

void AudioSettingsManager::set_ui_sounds_enabled(bool enabled) {
    spdlog::info("[AudioSettingsManager] set_ui_sounds_enabled({})", enabled);

    lv_subject_set_int(&ui_sounds_enabled_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/ui_sounds_enabled", enabled);
    config->save();
}

int AudioSettingsManager::get_volume() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&volume_subject_));
}

float AudioSettingsManager::get_volume_scaled() const {
    float normalized = static_cast<float>(get_volume()) / 100.0f;
    return normalized * normalized; // Quadratic curve for perceptual loudness
}

int AudioSettingsManager::preview_volume(int volume) {
    volume = std::clamp(volume, 0, 100);
    // Debug, not info: a drag emits one of these per tick.
    spdlog::debug("[AudioSettingsManager] preview_volume({})", volume);

    lv_subject_set_int(&volume_subject_, volume);
    return volume;
}

void AudioSettingsManager::set_volume(int volume) {
    volume = preview_volume(volume);
    spdlog::info("[AudioSettingsManager] set_volume({})", volume);

    Config* config = Config::get_instance();
    if (config) {
        config->set<int>("/sounds/volume", volume);
        config->save();
    }
}

std::string AudioSettingsManager::get_sound_theme() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/sound_theme", "default");
}

void AudioSettingsManager::set_sound_theme(const std::string& name) {
    spdlog::info("[AudioSettingsManager] set_sound_theme('{}')", name);

    Config* config = Config::get_instance();
    config->set<std::string>("/sound_theme", name);
    config->save();
}

std::string AudioSettingsManager::get_output_device() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/sound/output_device", "");
}

void AudioSettingsManager::set_output_device(const std::string& pcm) {
    spdlog::info("[AudioSettingsManager] set_output_device('{}')", pcm);

    Config* config = Config::get_instance();
    config->set<std::string>("/sound/output_device", pcm);
    config->save();
}

CompletionAlertMode AudioSettingsManager::get_completion_alert_mode() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&completion_alert_subject_));
    return static_cast<CompletionAlertMode>(std::max(0, std::min(2, val)));
}

void AudioSettingsManager::set_completion_alert_mode(CompletionAlertMode mode) {
    int val = static_cast<int>(mode);
    spdlog::info("[AudioSettingsManager] set_completion_alert_mode({})", val);
    lv_subject_set_int(&completion_alert_subject_, val);
    Config* config = Config::get_instance();
    config->set<int>("/completion_alert", val);
    config->save();
}

void AudioSettingsManager::refresh_audio_device_available() {
    if (!subjects_initialized_)
        return;
    int available = SoundManager::instance().has_alsa_backend() ? 1 : 0;
    spdlog::info("[AudioSettingsManager] audio_device_available={}", available);
    lv_subject_set_int(&audio_device_available_subject_, available);
}
