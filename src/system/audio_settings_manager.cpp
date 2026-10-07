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

using settings::Scope;
// Row order is AudioSettingsManager::Key.
static constexpr settings::PersistedSetting AUDIO_SETTINGS[] = {
    {"settings_sounds_enabled", "/sounds_enabled", Scope::Global, true, 0, 0, 1, nullptr},
    {"settings_ui_sounds_enabled", "/ui_sounds_enabled", Scope::Global, true, 1, 0, 1, nullptr},
    {"settings_volume", "/sounds/volume", Scope::Global, false, 80, 0, 100, nullptr},
    // CompletionAlertMode: 0=Off, 1=Notification, 2=Alert
    {"settings_completion_alert", "/completion_alert", Scope::Global, false, 2, 0, 2, nullptr},
};

AudioSettingsManager& AudioSettingsManager::instance() {
    static AudioSettingsManager instance;
    return instance;
}

AudioSettingsManager::AudioSettingsManager() : settings_(AUDIO_SETTINGS) {
    spdlog::trace("[AudioSettingsManager] Constructor");
}

void AudioSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[AudioSettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[AudioSettingsManager] Initializing subjects");

    settings_.init(subjects_);

    // Whether an ALSA backend with device selection is active, and whether the
    // backend plays music (0/1). Seeded to 0 here because SoundManager has not
    // picked its backend yet at subject-init time; refresh_backend_subjects()
    // sets the real values once it has.
    UI_MANAGED_SUBJECT_INT(audio_device_available_subject_, 0, "settings_audio_device_available",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(music_available_subject_, 0, "settings_music_available", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup with StaticSubjectRegistry
    StaticSubjectRegistry::instance().register_deinit(
        "AudioSettingsManager", []() { AudioSettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[AudioSettingsManager] Subjects initialized");
}

void AudioSettingsManager::reload_from_config() {
    if (subjects_initialized_) {
        settings_.reload();
    }
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

float AudioSettingsManager::get_volume_scaled() const {
    float normalized = static_cast<float>(get_volume()) / 100.0f;
    return normalized * normalized; // Quadratic curve for perceptual loudness
}

int AudioSettingsManager::preview_volume(int volume) {
    volume = std::clamp(volume, 0, 100);
    // Debug, not info: a drag emits one of these per tick.
    spdlog::debug("[AudioSettingsManager] preview_volume({})", volume);

    lv_subject_set_int(settings_.subject(Key::Volume), volume);
    return volume;
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

std::string AudioSettingsManager::get_pwm_channel() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/sound/pwm_channel", "");
}

void AudioSettingsManager::set_output_device(const std::string& pcm) {
    spdlog::info("[AudioSettingsManager] set_output_device('{}')", pcm);

    Config* config = Config::get_instance();
    config->set<std::string>("/sound/output_device", pcm);
    config->save();
}

void AudioSettingsManager::refresh_backend_subjects() {
    if (!subjects_initialized_)
        return;
    auto& sm = SoundManager::instance();
    const int device = sm.has_alsa_backend() ? 1 : 0;
    const int music = sm.can_play_music() ? 1 : 0;
    spdlog::info("[AudioSettingsManager] audio_device_available={} music_available={}", device,
                 music);
    lv_subject_set_int(&audio_device_available_subject_, device);
    lv_subject_set_int(&music_available_subject_, music);
}
