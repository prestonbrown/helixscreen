// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef __HELIX_CONFIG_H__
#define __HELIX_CONFIG_H__

#include "spdlog/spdlog.h"

#include <string>

#include "hv/json.hpp"

using json = nlohmann::json;

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
 * std::string ip = cfg->get<std::string>("/printers/default/ip", "127.0.0.1");
 *
 * // Set and save
 * cfg->set<int>("/printers/default/port", 7125);
 * cfg->save();
 * ```
 */
class Config {
  private:
    static Config* instance;
    std::string path;

  protected:
    json data;
    std::string default_printer;

    /// Allow test fixture to access protected members
    friend class ConfigTestFixture;

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
     * @param json_ptr JSON pointer path (e.g., "/printers/default/ip")
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
     * @param json_ptr JSON pointer path (e.g., "/printers/default/ip")
     * @param default_value Fallback value if path not found
     * @return Configuration value or default_value
     */
    template <typename T> T get(const std::string& json_ptr, const T& default_value) {
        json::json_pointer ptr(json_ptr);
        if (data.contains(ptr)) {
            return data[ptr].template get<T>();
        }
        return default_value;
    };

    /**
     * @brief Set configuration value at JSON pointer path
     *
     * Creates intermediate paths if they don't exist.
     * Changes are in-memory only until save() is called.
     *
     * @tparam T Value type to store
     * @param json_ptr JSON pointer path (e.g., "/printers/default/port")
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
     * @brief Save current configuration to file
     *
     * Writes in-memory config to disk with pretty formatting.
     * Includes error handling and validation.
     *
     * @return true if save succeeded, false on error
     */
    bool save();

    /**
     * @brief Get default printer path prefix
     *
     * Returns JSON pointer prefix for the default printer configuration.
     * Useful for constructing full paths to printer settings.
     *
     * @return JSON pointer prefix (e.g., "/printers/default_printer/")
     */
    std::string& df();

    /**
     * @brief Get configuration file path
     *
     * @return Absolute path to the loaded configuration file
     */
    std::string get_path();

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
     * @brief Reset configuration to factory defaults
     *
     * Clears all user settings and restores the config to initial state.
     * This will require the setup wizard to run again.
     * Call save() after this to persist the reset.
     */
    void reset_to_defaults();

    /**
     * @brief Get singleton instance
     *
     * @return Pointer to global Config instance
     */
    static Config* get_instance();
};

#endif // __HELIX_CONFIG_H__
