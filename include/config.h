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

#include "config_storage.h"
#include "json_fwd.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace helix {

namespace config_detail {

/// JSON type that Config::get<T>(ptr, default) needs the stored value to hold.
/// Used only to name the expectation in the warning logged when a persisted
/// value has the wrong type; not a conversion rule.
template <typename T> constexpr const char* expected_json_type() {
    if constexpr (std::is_same_v<T, bool>) {
        return "boolean";
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "string";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return "number";
    } else {
        return "value";
    }
}

} // namespace config_detail

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
static constexpr int CURRENT_CONFIG_VERSION = 21;

class Config {
  private:
    static Config* instance;
    std::string path;
    std::string active_printer_id_;          ///< Currently active printer slug ID
    bool read_only_mode_ = false;            ///< Config directory is on a read-only filesystem
    std::unique_ptr<ConfigStorage> storage_; ///< Document-level persistence backend
    /// True when storage_ was auto-created from `path` rather than injected by
    /// set_storage(). Only an auto-created backend may be rebuilt when `path`
    /// moves — an injected one is the caller's, and its target is not `path`.
    bool storage_is_default_ = false;

    /// Point storage_ at `path`, rebuilding a stale auto-created backend.
    /// `path` moves whenever init() runs against a different file (printer
    /// switch, and every test that re-points the singleton); without this the
    /// first backend keeps writing to the original file forever.
    void ensure_storage();

    /**
     * @brief Point active_printer_id_ at a printer that actually exists
     *
     * Reads /active_printer_id and falls back to the first entry of /printers
     * that is an object (the map also holds plain settings keys). Leaves
     * /active_printer_id untouched when no printer object exists at all.
     *
     * @return true if /active_printer_id was rewritten (config needs saving)
     */
    bool refresh_active_printer_id();

    /// Epoch-seconds stamp for a new /removed_printers entry, forced strictly
    /// newer than every existing stamp so ordering survives a coarse or
    /// backwards-stepping clock.
    int64_t next_archive_stamp() const;

    /// Drop the oldest /removed_printers entries beyond MAX_ARCHIVED_PRINTERS
    void prune_archived_printers();

    /// Warn that the value at @p json_ptr could not be read as the requested
    /// type and the caller's default was substituted. Out-of-line so this
    /// very widely included header does not pull in spdlog.
    static void log_type_mismatch(const std::string& json_ptr, const char* stored_type,
                                  const char* expected_type, const char* detail);

    /// Warn that a set() could not store its value, so the setting will not
    /// persist. Out-of-line for the same reason as log_type_mismatch().
    static void log_set_failed(const std::string& json_ptr, const char* detail);

  protected:
    json data;

    /// Allow test-only accessor to reach protected/private members
    friend class ConfigTestAccess;

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
     * @brief The settings path init() will actually use for @p config_path
     *
     * Applies the HELIX_CONFIG_DIR override — the directory comes from the
     * env var, the filename from @p config_path, so the
     * settings.json / settings-test.json distinction survives. Pure: creates
     * nothing and touches no state, so callers that only want to *report* the
     * effective path (the --test banner) resolve it the same way init() does
     * instead of printing the unresolved compile-time constant.
     *
     * @param config_path Default path, e.g. RuntimeConfig::TEST_CONFIG_PATH
     * @return @p config_path when HELIX_CONFIG_DIR is unset or empty
     */
    static std::string resolve_path(const std::string& config_path);

    /**
     * @brief Reset state set by init() for test isolation
     *
     * Empties the persistence path and the active-printer slug so
     * subsequent tests aren't surprised by lingering state. Used by
     * test fixtures that init() the singleton with a temp directory
     * and then delete that directory — without this, a later save()
     * in another test would fail (parent dir gone), trigger
     * CONFIG_RECORD_ERROR, and enqueue a phantom telemetry event in
     * tests that expect a clean queue. Clearing active_printer_id_
     * also keeps is_wizard_required() reading the root-level key
     * rather than a stale per-printer one (FirstRunTour gate tests).
     */
    void clear_path() {
        path.clear();
        active_printer_id_.clear();
        storage_.reset();
        storage_is_default_ = false;
    }

    /**
     * @brief Inject a persistence backend (call BEFORE init()).
     *
     * Default when unset: make_file_config_storage(resolved path). Embedded
     * targets substitute NVS/LittleFS; tests substitute an in-memory mock.
     */
    void set_storage(std::unique_ptr<ConfigStorage> storage) {
        storage_ = std::move(storage);
        storage_is_default_ = false;
    }

    /**
     * @brief Get configuration value at JSON pointer path
     *
     * Throws nlohmann::json::exception if path doesn't exist.
     * Use the overload with default_value for safer access.
     *
     * Non-vivifying: uses at() rather than operator[], so a missing path
     * throws instead of silently inserting nulls along the way (#1129).
     *
     * @tparam T Value type to retrieve
     * @param json_ptr JSON pointer path (e.g., "/printer/moonraker_host")
     * @return Configuration value of type T
     * @throws nlohmann::json::exception if path not found
     */
    template <typename T> T get(const std::string& json_ptr) const {
        return data.at(json::json_pointer(json_ptr)).template get<T>();
    };

    /**
     * @brief Get configuration value with default fallback
     *
     * Safe accessor that returns default_value if the path doesn't exist, holds
     * null, or holds a value of a JSON type that cannot convert to T.
     *
     * The type check matters because settings.json is user-editable: a value
     * like `"home_edit_mode_enabled": "true"` (string where a boolean belongs)
     * makes nlohmann's get<T>() throw type_error.302. That exception used to
     * escape into whatever was reading config at the time, typically a
     * manager's init_subjects() partway through registering its subjects, so a
     * single mistyped key took the whole app down instead of one setting.
     *
     * @tparam T Value type to retrieve
     * @param json_ptr JSON pointer path (e.g., "/printer/moonraker_host")
     * @param default_value Fallback value if path not found or wrongly typed
     * @return Configuration value or default_value
     */
    template <typename T> T get(const std::string& json_ptr, const T& default_value) const {
        json::json_pointer ptr(json_ptr);
        if (!data.contains(ptr)) {
            return default_value;
        }
        const json& node = data.at(ptr);
        if (node.is_null()) {
            return default_value;
        }
        try {
            return node.template get<T>();
        } catch (const json::exception& e) {
            log_type_mismatch(json_ptr, node.type_name(), config_detail::expected_json_type<T>(),
                              e.what());
            return default_value;
        }
    };

    /**
     * @brief Check if a configuration key exists
     *
     * @param json_ptr JSON pointer path (e.g., "/display/rotate")
     * @return true if the key exists in the configuration
     */
    bool exists(const std::string& json_ptr) const {
        return data.contains(json::json_pointer(json_ptr));
    }

    /**
     * @brief Set configuration value at JSON pointer path
     *
     * Creates intermediate paths if they don't exist.
     * Changes are in-memory only until save() is called.
     *
     * A path cannot be created through a component that a corrupted config
     * stores as a scalar (`"input": "oops"` blocks /input/scroll_guard), which
     * nlohmann reports as out_of_range.404. That is logged and the value is
     * dropped rather than thrown: the setting fails to persist, but toggling it
     * does not take the app down.
     *
     * @tparam T Value type to store
     * @param json_ptr JSON pointer path (e.g., "/printer/moonraker_port")
     * @param v Value to set
     * @return The value that was set
     */
    template <typename T> T set(const std::string& json_ptr, T v) {
        try {
            return data[json::json_pointer(json_ptr)] = v;
        } catch (const json::exception& e) {
            log_set_failed(json_ptr, e.what());
            return v;
        }
    };

    /**
     * @brief Get a mutable JSON sub-object at path, CREATING it if absent
     *
     * @warning This vivifies. nlohmann's non-const `operator[](json_pointer)`
     * calls get_and_create(), inserting a `null` at every missing component of
     * the path — and Config::save() writes those nulls to settings.json
     * verbatim (it does no pruning). A config full of authoritative-looking
     * `"led": {"selected_strips": null}` garbage is exactly how #1129 cost a
     * reporter hours of debugging.
     *
     * Use this ONLY when you are about to assign through the returned
     * reference. For read-only access use try_get_json(), get_string_array(),
     * get<T>(path, default) or exists() — all non-vivifying.
     *
     * @param json_path JSON pointer path
     * @return Mutable reference to the JSON node at path (created if missing)
     */
    json& get_json(const std::string& json_path);

    /**
     * @brief Non-vivifying read-only lookup of a JSON sub-object
     *
     * The safe counterpart to get_json(): returns nullptr when the path is
     * absent and never mutates the config (#1129).
     *
     * @param json_path JSON pointer path
     * @return Pointer to the node at path, or nullptr if it doesn't exist
     */
    const json* try_get_json(const std::string& json_path) const;

    /**
     * @brief Read an array of strings at a JSON pointer path
     *
     * Non-vivifying. Returns an empty vector when the path is absent, is not
     * an array, or holds no string elements; non-string elements are skipped.
     * Collapses the "probe-then-iterate string array" block that was
     * hand-rolled at ~10 call sites.
     *
     * @param json_path JSON pointer path
     * @return String elements of the array at path (empty if absent/not an array)
     */
    std::vector<std::string> get_string_array(const std::string& json_path) const;

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
    std::string df() const;

    /**
     * @brief Get configuration file path
     *
     * @return Absolute path to the loaded configuration file
     */
    std::string get_path();

    /**
     * @brief Check if the config directory is on a read-only filesystem
     *
     * Detected during init() by probing a test file write.
     * When true, save() will skip writes and return false.
     * UI code can use this to show a persistent notification.
     *
     * @return true if filesystem is read-only
     */
    bool is_read_only() const;

    /// Check if the ACTIVE PRINTER was configured from a platform preset
    bool has_preset() const;

    /// Get the active printer's preset name (e.g., "ad5m"), or empty if none.
    /// Stored at df() + "preset"; a legacy root-level marker is lifted here by
    /// init() and is never read directly, so each printer keeps its own value.
    std::string get_preset() const;

    /// Set the active printer's preset name (written during auto-detection from
    /// printer database)
    void set_preset(const std::string& preset_name);

    /// Erase the active printer's preset marker, plus any legacy root-level one
    /// (set_preset("") is a no-op and cannot clear it)
    void clear_preset();

    /**
     * @brief Load a preset file and merge hardware config into active printer
     *
     * Loads assets/config/presets/{preset_name}.json relative to the data root.
     * Merges the preset's "printer" section (fans, heaters, temp_sensors, leds,
     * hardware, filament_sensors, default_macros) into the active printer config.
     * Only applies when wizard_completed is false for the active printer.
     *
     * @param preset_name Name of the preset (without .json extension)
     * @return true if preset was loaded and merged, false if skipped or error
     */
    bool apply_preset_file(const std::string& preset_name);

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
     * @brief Move a printer configuration out of the active list, preserving it
     *
     * Used by automatic recovery paths, which remove printers the user never
     * asked to delete. The node is copied to /removed_printers/<id> before it is
     * erased, so a mistaken recovery costs the user a support round-trip rather
     * than their heaters, fans, sensors, macros and Moonraker host.
     *
     * Prefer this over remove_printer() anywhere the removal is not an explicit
     * user action.
     *
     * Each snapshot is stamped with ARCHIVED_AT_KEY and the archive is trimmed
     * to the MAX_ARCHIVED_PRINTERS most recent entries.
     *
     * @param printer_id Slug ID of the printer to archive
     */
    void archive_printer(const std::string& printer_id);

    /// How many archived printers /removed_printers keeps. Nothing reads the
    /// archive back yet, so it is pure insurance — an unbounded one would grow
    /// settings.json (and every rolling backup of it) without limit.
    static constexpr size_t MAX_ARCHIVED_PRINTERS = 5;

    /// Epoch-seconds field stamped into each /removed_printers entry, used to
    /// decide which entries are the oldest when pruning.
    static constexpr const char* ARCHIVED_AT_KEY = "archived_at";

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
