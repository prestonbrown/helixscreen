// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file config.h
 * @brief JSON configuration singleton with RFC 6901 pointer syntax accessors
 *
 * @pattern Singleton with template accessors and default fallbacks
 * @threading Main thread only (not thread-safe)
 *
 * @see Friend test access pattern for unit testing
 */

#pragma once

#include "json_fwd.h"

#include <string>
#include <vector>

namespace helix {

/**
 * @brief Configuration for a user-customizable macro button
 *
 * Stores both the display label (shown on button) and the G-code
 * command to execute. Supports backward compatibility with string-only
 * config entries where the string is used as both label and gcode.
 */
struct MacroConfig {
    std::string label; ///< Human-readable button label
    std::string gcode; ///< G-code macro command to execute
};

/**
 * @brief Application configuration manager (singleton)
 *
 * Loads and manages application configuration from JSON file.
 * Uses JSON pointer syntax (RFC 6901) for nested value access.
 *
 * Thread safety: Not thread-safe. Should be initialized once at startup
 * and accessed from main thread only.
 *
 * Example usage:
 * ```cpp
 * Config* cfg = Config::get_instance();
 * cfg->init("/path/to/config.json");
 *
 * // Get with default fallback
 * std::string ip = cfg->get<std::string>(cfg->df() + "moonraker_host", "127.0.0.1");
 *
 * // Set and save
 * cfg->set<int>(cfg->df() + "moonraker_port", 7125);
 * cfg->save();
 * ```
 */
/// Current config schema version — bump when adding new migrations
static constexpr int CURRENT_CONFIG_VERSION = 4;

class Config {
  private:
    static Config* instance;
    std::string path;
    std::string active_printer_id_; ///< Currently active printer slug ID

  protected:
    json data;

    /// Allow test fixtures to access protected members
    friend class ConfigTestFixture;
    friend class ChangeHostConfigFixture;
    friend class HardwareValidatorConfigFixture;
    friend class MmuDetectionFixture;
    friend class PanelWidgetConfigFixture;
    friend class ThermistorConfigFixture;

  public:
    /**
     * @brief Construct configuration manager
     *
     * Use get_instance() to obtain singleton instance.
     */
    Config();

    Config(Config& o) = delete;
    void operator=(const Config&) = delete;

    /**
     * @brief Initialize configuration from file
     *
     * Loads JSON configuration file and sets up default printer path.
     * Creates config file with defaults if it doesn't exist.
     *
     * @param config_path Absolute path to JSON configuration file
     */
    void init(const std::string& config_path);

    /**
     * @brief Get configuration value at JSON pointer path
     *
     * Throws nlohmann::json::exception if path doesn't exist.
     * Use the overload with default_value for safer access.
     *
     * @tparam T Value type to retrieve
     * @param json_ptr JSON pointer path (e.g., "/printer/moonraker_host")
     * @return Configuration value of type T
     * @throws nlohmann::json::exception if path not found
     */
    template <typename T> T get(const std::string& json_ptr) {
        return data[json::json_pointer(json_ptr)].template get<T>();
    };

    /**
     * @brief Get configuration value with default fallback
     *
     * Safe accessor that returns default_value if path doesn't exist.
     *
     * @tparam T Value type to retrieve
     * @param json_ptr JSON pointer path (e.g., "/printer/moonraker_host")
     * @param default_value Fallback value if path not found
     * @return Configuration value or default_value
     */
    template <typename T> T get(const std::string& json_ptr, const T& default_value) {
        json::json_pointer ptr(json_ptr);
        if (data.contains(ptr) && !data[ptr].is_null()) {
            return data[ptr].template get<T>();
        }
        return default_value;
    };

    /**
     * @brief Check if a configuration key exists
     *
     * @param json_ptr JSON pointer path (e.g., "/display/rotate")
     * @return true if the key exists in the configuration
     */
    bool exists(const std::string& json_ptr) {
        return data.contains(json::json_pointer(json_ptr));
    }

    /**
     * @brief Set configuration value at JSON pointer path
     *
     * Creates intermediate paths if they don't exist.
     * Changes are in-memory only until save() is called.
     *
     * @tparam T Value type to store
     * @param json_ptr JSON pointer path (e.g., "/printer/moonraker_port")
     * @param v Value to set
     * @return The value that was set
     */
    template <typename T> T set(const std::string& json_ptr, T v) {
        return data[json::json_pointer(json_ptr)] = v;
    };

    /**
     * @brief Get JSON sub-object at path
     *
     * Returns mutable reference to JSON object for complex operations.
     *
     * @param json_path JSON pointer path
     * @return Reference to JSON object at path
     */
    json& get_json(const std::string& json_path);

    /**
     * @brief Get macro configuration with label and G-code command
     *
     * Retrieves a macro definition from the default_macros config section.
     * Handles two formats for backward compatibility:
     * - String: "MACRO_NAME" → used as both label and gcode
     * - Object: {"label": "Display Name", "gcode": "MACRO_NAME"}
     *
     * @param key Macro key name (e.g., "macro_1", "load_filament")
     * @param default_val Fallback if key not found or parse error
     * @return MacroConfig with label and gcode fields populated
     */
    MacroConfig get_macro(const std::string& key, const MacroConfig& default_val);

    /**
     * @brief Save current configuration to file
     *
     * Writes in-memory config to disk with pretty formatting.
     * Includes error handling and validation.
     *
     * @return true if save succeeded, false on error
     */
    bool save();

    /**
     * @brief Get printer config path prefix
     *
     * Returns JSON pointer prefix for the printer configuration.
     * Useful for constructing full paths to printer settings.
     *
     * @return JSON pointer prefix ("/printer/")
     */
    std::string df();

    /**
     * @brief Get configuration file path
     *
     * @return Absolute path to the loaded configuration file
     */
    std::string get_path();

    /// Check if this config was loaded from a platform preset
    bool has_preset() const;

    /// Get the preset name (e.g., "ad5m"), or empty string if no preset
    std::string get_preset() const;

    /**
     * @brief Check if first-run wizard is required
     *
     * Wizard is required if printer configuration is incomplete
     * (missing IP, port, or API key).
     *
     * @return true if wizard should run, false otherwise
     */
    bool is_wizard_required();

    /**
     * @brief Check if WiFi connectivity is expected for this device
     *
     * When true, the UI will show WiFi status and settings even if
     * no WiFi hardware is currently detected (e.g., USB adapter unplugged).
     *
     * @return true if WiFi is expected, false otherwise
     */
    bool is_wifi_expected();

    /**
     * @brief Set whether WiFi connectivity is expected
     *
     * Call save() after this to persist the setting.
     *
     * @param expected true if WiFi should be expected
     */
    void set_wifi_expected(bool expected);

    /**
     * @brief Get the current language code
     *
     * @return Language code (e.g., "en", "de", "fr", "es", "ru")
     */
    std::string get_language();

    /**
     * @brief Set the current language
     *
     * Call save() after this to persist the setting.
     *
     * @param lang Language code (e.g., "en", "de", "fr", "es", "ru")
     */
    void set_language(const std::string& lang);

    /**
     * @brief Check if beta features are enabled
     *
     * Beta features are gated behind this flag to allow testing
     * before public release. Returns true if:
     * - "beta_features" config key is true, OR
     * - Running in --test mode (RuntimeConfig)
     *
     * @return true if beta features should be available
     */
    bool is_beta_features_enabled();

    /**
     * @brief Reset configuration to factory defaults
     *
     * Clears all user settings and restores the config to initial state.
     * This will require the setup wizard to run again.
     * Call save() after this to persist the reset.
     */
    void reset_to_defaults();

    // ========================================================================
    // Multi-printer support
    // ========================================================================

    /**
     * @brief Get the active printer's slug ID
     *
     * @return Active printer ID (e.g., "voronv2", "ender3-pro")
     */
    std::string get_active_printer_id() const;

    /**
     * @brief Switch to a different printer configuration
     *
     * Updates active_printer_id and persists to config.
     * df() will route to the new printer's config section.
     *
     * @param printer_id Slug ID of the printer to activate
     * @return true if printer was found and activated, false otherwise
     */
    bool set_active_printer(const std::string& printer_id);

    /**
     * @brief Get list of all configured printer IDs
     *
     * @return Vector of printer slug IDs (keys of /printers object)
     */
    std::vector<std::string> get_printer_ids() const;

    /**
     * @brief Add a new printer configuration
     *
     * @param printer_id Slug ID for the new printer
     * @param printer_data JSON object with printer configuration
     */
    void add_printer(const std::string& printer_id, const json& printer_data);

    /**
     * @brief Remove a printer configuration
     *
     * If the removed printer is the active one, active_printer_id is cleared.
     *
     * @param printer_id Slug ID of the printer to remove
     */
    void remove_printer(const std::string& printer_id);

    /**
     * @brief Get singleton instance
     *
     * @return Pointer to global Config instance
     */
    static Config* get_instance();

    /**
     * @brief Generate a URL-safe slug ID from a printer name
     *
     * Converts to lowercase, replaces spaces/special chars with hyphens,
     * strips leading/trailing hyphens, collapses consecutive hyphens.
     *
     * @param name Human-readable printer name
     * @return Slug ID (e.g., "Voron 2.4" → "voron-2-4")
     */
    static std::string slugify(const std::string& name);
};

} // namespace helix
