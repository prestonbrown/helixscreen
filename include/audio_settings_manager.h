// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "completion_alert_mode.h"
#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

#include <algorithm>
#include <string>

namespace helix {

/**
 * @brief Domain-specific manager for audio/sound settings
 *
 * Owns all audio-related LVGL subjects and persistence:
 * - sounds_enabled (master switch)
 * - ui_sounds_enabled (UI interaction sounds)
 * - volume (0-100)
 * - completion_alert (Off/Notification/Alert)
 * - sound_theme (config-only, no subject)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class AudioSettingsManager {
  public:
    static AudioSettingsManager& instance();

    // Non-copyable
    AudioSettingsManager(const AudioSettingsManager&) = delete;
    AudioSettingsManager& operator=(const AudioSettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Re-read every persisted setting from Config; no-op before init_subjects() */
    void reload_from_config();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // GETTERS / SETTERS
    // =========================================================================

    /** @brief Master sound switch */
    bool get_sounds_enabled() const {
        return settings_.get_bool(Key::SoundsEnabled);
    }
    void set_sounds_enabled(bool enabled) {
        settings_.set(Key::SoundsEnabled, enabled);
    }

    /** @brief UI interaction sounds */
    bool get_ui_sounds_enabled() const {
        return settings_.get_bool(Key::UiSoundsEnabled);
    }
    void set_ui_sounds_enabled(bool enabled) {
        settings_.set(Key::UiSoundsEnabled, enabled);
    }

    /** @brief Master volume (0-100) */
    int get_volume() const {
        return settings_.get(Key::Volume);
    }

    /** @brief Get perceptually-scaled volume as 0.0–1.0 float (quadratic curve) */
    float get_volume_scaled() const;

    /**
     * @brief Apply volume live WITHOUT persisting (clamped 0-100)
     *
     * Updates the subject only. Use for the per-tick handler of a slider drag;
     * set_volume() writes settings.json (double fsync + rolling backup), which
     * has no business running once per drag tick. Pair with set_volume() on
     * release so the final value is durable.
     *
     * @return the clamped value actually applied
     */
    int preview_volume(int volume);

    /** @brief Set master volume (clamped 0-100, updates subject + persists) */
    void set_volume(int volume) {
        settings_.set(Key::Volume, volume);
    }

    /** @brief Get sound theme name from config */
    std::string get_sound_theme() const;

    /** @brief Set sound theme name (persists to config) */
    void set_sound_theme(const std::string& name);

    /** @brief Get persisted ALSA output device PCM ("" if unset) */
    std::string get_output_device() const;

    /** @brief Set ALSA output device PCM (persists to config) */
    void set_output_device(const std::string& pcm);

    /** @brief Persisted PWM buzzer channel as "<chip>:<channel>" ("" if unset) */
    std::string get_pwm_channel() const;

    CompletionAlertMode get_completion_alert_mode() const {
        return static_cast<CompletionAlertMode>(
            std::clamp(settings_.get(Key::CompletionAlert), 0, 2));
    }
    void set_completion_alert_mode(CompletionAlertMode mode) {
        settings_.set(Key::CompletionAlert, static_cast<int>(mode));
    }

    /**
     * @brief Refresh the subjects that describe the live sound backend
     *
     * `settings_audio_device_available` (an ALSA device picker applies) and
     * `settings_music_available` (the backend plays music) cannot be seeded in
     * init_subjects() because subjects are initialized before SoundManager
     * picks its backend. Called once after SoundManager::initialize() so the
     * values are correct before any overlay binds to them. A later M300
     * takeover only swaps one tones-only backend for another, so a single
     * refresh is sufficient.
     */
    void refresh_backend_subjects();

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Sounds enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_sounds_enabled() {
        return settings_.subject(Key::SoundsEnabled);
    }

    /** @brief UI sounds enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ui_sounds_enabled() {
        return settings_.subject(Key::UiSoundsEnabled);
    }

    /** @brief Volume subject (integer: 0-100 percent) */
    lv_subject_t* subject_volume() {
        return settings_.subject(Key::Volume);
    }

    /** @brief Completion alert subject (integer: 0=off, 1=notification, 2=alert) */
    lv_subject_t* subject_completion_alert() {
        return settings_.subject(Key::CompletionAlert);
    }

  private:
    AudioSettingsManager();
    ~AudioSettingsManager() = default;

    enum class Key : uint8_t { SoundsEnabled, UiSoundsEnabled, Volume, CompletionAlert, COUNT };

    SubjectManager subjects_;
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;
    lv_subject_t audio_device_available_subject_{};
    lv_subject_t music_available_subject_{};

    bool subjects_initialized_ = false;
};

} // namespace helix
