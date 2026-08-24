// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <string>

namespace helix {

/** @brief Print completion notification mode (Off=0, Notification=1, Alert=2) */
enum class CompletionAlertMode { OFF = 0, NOTIFICATION = 1, ALERT = 2 };

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

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // GETTERS / SETTERS
    // =========================================================================

    /** @brief Get master sound enabled state */
    bool get_sounds_enabled() const;

    /** @brief Set master sound enabled state (updates subject + persists) */
    void set_sounds_enabled(bool enabled);

    /** @brief Get UI interaction sounds enabled state */
    bool get_ui_sounds_enabled() const;

    /** @brief Set UI interaction sounds enabled state (updates subject + persists) */
    void set_ui_sounds_enabled(bool enabled);

    /** @brief Get master volume (0-100) */
    int get_volume() const;

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
    void set_volume(int volume);

    /** @brief Get sound theme name from config */
    std::string get_sound_theme() const;

    /** @brief Set sound theme name (persists to config) */
    void set_sound_theme(const std::string& name);

    /** @brief Get persisted ALSA output device PCM ("" if unset) */
    std::string get_output_device() const;

    /** @brief Set ALSA output device PCM (persists to config) */
    void set_output_device(const std::string& pcm);

    /** @brief Get completion alert mode */
    CompletionAlertMode get_completion_alert_mode() const;

    /** @brief Set completion alert mode (updates subject + persists) */
    void set_completion_alert_mode(CompletionAlertMode mode);

    /**
     * @brief Refresh the audio-device-available subject from the live backend
     *
     * `settings_audio_device_available` cannot be seeded in init_subjects()
     * because subjects are initialized before SoundManager picks its backend.
     * Called once after SoundManager::initialize() so the value is correct
     * before any overlay binds to it. The backend is static after init, so a
     * single refresh is sufficient.
     */
    void refresh_audio_device_available();

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Sounds enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_sounds_enabled() {
        return &sounds_enabled_subject_;
    }

    /** @brief UI sounds enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ui_sounds_enabled() {
        return &ui_sounds_enabled_subject_;
    }

    /** @brief Volume subject (integer: 0-100 percent) */
    lv_subject_t* subject_volume() {
        return &volume_subject_;
    }

    /** @brief Completion alert subject (integer: 0=off, 1=notification, 2=alert) */
    lv_subject_t* subject_completion_alert() {
        return &completion_alert_subject_;
    }

  private:
    AudioSettingsManager();
    ~AudioSettingsManager() = default;

    SubjectManager subjects_;

    lv_subject_t sounds_enabled_subject_;
    lv_subject_t ui_sounds_enabled_subject_;
    lv_subject_t volume_subject_;
    lv_subject_t completion_alert_subject_;
    lv_subject_t audio_device_available_subject_;

    bool subjects_initialized_ = false;
};

} // namespace helix
