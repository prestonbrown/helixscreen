// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

#include <string>

namespace helix {

/**
 * @brief Domain-specific manager for system-level settings
 *
 * Owns all system-related LVGL subjects and persistence:
 * - language (index into language list)
 * - update_channel (Stable=0, Beta=1, Dev=2)
 * - telemetry_enabled (opt-in toggle)
 * - wifi_enabled (radio on/off choice, survives restarts)
 * - wifi_country (regulatory country, "" = leave it to the system)
 * - wifi_country_last_seen (last real country the radio reported)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class SystemSettingsManager {
  public:
    static SystemSettingsManager& instance();

    /// The static PersistedSetting table this manager is built from.
    static settings::TableView persisted_table();

    // Non-copyable
    SystemSettingsManager(const SystemSettingsManager&) = delete;
    SystemSettingsManager& operator=(const SystemSettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // LANGUAGE SETTINGS
    // =========================================================================

    /**
     * @brief Get current language code
     * @return Language code (e.g., "en", "de", "fr", "es", "ru")
     */
    std::string get_language() const;

    /**
     * @brief Set language and apply translations
     *
     * Updates subject, calls lv_translation_set_language() for hot-reload,
     * syncs lv_i18n system, and persists to Config.
     *
     * @param lang Language code (e.g., "en", "de", "fr", "es", "ru")
     */
    void set_language(const std::string& lang);

    /**
     * @brief Set language by dropdown index
     * @param index Index in language options dropdown (0=English, 1=German, etc.)
     */
    void set_language_by_index(int index);

    /**
     * @brief Get current language dropdown index
     * @return Index of current language in dropdown options
     */
    int get_language_index() const;

    /** @brief Get dropdown options string "English\nDeutsch\nFrancais\n..." */
    static const char* get_language_options();

    /** @brief Get the current language's native display name (e.g. "Deutsch") */
    std::string get_language_display_name() const;

    /** @brief Get language code for dropdown index */
    static std::string language_index_to_code(int index);

    /** @brief Get dropdown index for language code */
    static int language_code_to_index(const std::string& code);

    // =========================================================================
    // UPDATE CHANNEL SETTINGS
    // =========================================================================

    /** @brief Get current update channel (0=Stable, 1=Beta, 2=Dev) */
    int get_update_channel() const {
        return settings_.get(Key::UpdateChannel);
    }

    /** @brief Set update channel, persist, and clear update cache */
    void set_update_channel(int channel);

    // =========================================================================
    // TELEMETRY SETTINGS
    // =========================================================================

    bool get_telemetry_enabled() const {
        return settings_.get_bool(Key::TelemetryEnabled);
    }

    /** @brief Set telemetry enabled state (persists to config + notifies TelemetryManager) */
    void set_telemetry_enabled(bool enabled);

    // =========================================================================
    // WIFI SETTINGS
    // =========================================================================

    /**
     * @brief Stored WiFi radio on/off choice (default true, also before init_subjects())
     *
     * WiFiManager's READY handler reads this to decide whether to force the
     * radio off, so a wrong "off" here switches WiFi off on a remote printer.
     */
    bool get_wifi_enabled() const {
        return settings_.get_bool(Key::WifiEnabled);
    }

    /**
     * @brief Persist the WiFi radio on/off choice
     *
     * Persists only and does NOT call into WiFiManager. WiFiManager reasserts
     * this stored value against the radio itself once its backend is ready;
     * calling back into it from here would create a feedback loop.
     */
    void set_wifi_enabled(bool enabled) {
        settings_.set(Key::WifiEnabled, enabled);
    }

    /**
     * @brief The user's WiFi regulatory country (ISO 3166-1 alpha-2), "" when
     *        never chosen. Readable before init_subjects().
     *
     * "" means HelixScreen leaves the radio's country to the system.
     */
    std::string get_wifi_country() const;

    /**
     * @brief Persist the WiFi country choice ("" clears it)
     *
     * Persists only, like set_wifi_enabled(): WiFiManager applies it.
     */
    void set_wifi_country(const std::string& code);

    /**
     * @brief The last real country the radio reported (ISO 3166-1 alpha-2),
     *        "" when it never reported one. Readable before init_subjects().
     *
     * Not a choice: WiFiManager records it, and sends it back when the radio
     * comes up in the world domain with no saved choice.
     */
    std::string get_wifi_country_last_seen() const;

    /// Persist the last real country the radio reported.
    void set_wifi_country_last_seen(const std::string& code);

    // =========================================================================
    // LOG LEVEL SETTINGS
    // =========================================================================

    /** @brief Get current log level index (0=Warn, 1=Info, 2=Debug, 3=Trace) */
    int get_log_level_index() const;

    /** @brief Set log level by dropdown index, apply immediately, and persist */
    void set_log_level_by_index(int index);

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Language subject (integer: index into language options) */
    lv_subject_t* subject_language() {
        return &language_subject_;
    }

    /// Death signal for this manager's subjects, for observers of them.
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /** @brief Update channel subject (integer: 0=Stable, 1=Beta, 2=Dev) */
    lv_subject_t* subject_update_channel() {
        return settings_.subject(Key::UpdateChannel);
    }

    /** @brief Telemetry enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_telemetry_enabled() {
        return settings_.subject(Key::TelemetryEnabled);
    }

    /** @brief WiFi radio enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_wifi_enabled() {
        return settings_.subject(Key::WifiEnabled);
    }

    /** @brief Log level subject (integer: 0=Warn, 1=Info, 2=Debug, 3=Trace) */
    lv_subject_t* subject_log_level() {
        return &log_level_subject_;
    }

  private:
    SystemSettingsManager();
    ~SystemSettingsManager() = default;

    enum class Key : uint8_t { UpdateChannel, TelemetryEnabled, WifiEnabled, COUNT };

    SubjectManager subjects_;
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;

    lv_subject_t language_subject_{};
    lv_subject_t log_level_subject_{};

    bool subjects_initialized_ = false;
};

} // namespace helix
