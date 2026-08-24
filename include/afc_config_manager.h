// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "klipper_config_parser.h"

#include <functional>
#include <optional>
#include <string>

class IMoonrakerAPI;

/**
 * @brief Manages AFC configuration files with download/upload and dirty tracking
 *
 * Wraps KlipperConfigParser with file transfer operations via IMoonrakerAPI
 * and tracks whether the in-memory config has unsaved modifications.
 *
 * Usage:
 *   AfcConfigManager mgr(&api);
 *   mgr.load("AFC/AFC.cfg", [](bool ok, const std::string& err) { ... });
 *   mgr.parser().set("AFC_hub Turtle_1", "afc_bowden_length", "500");
 *   mgr.mark_dirty();
 *   mgr.save("AFC/AFC.cfg", [](bool ok, const std::string& err) { ... });
 */
class AfcConfigManager {
  public:
    using Callback = std::function<void(bool success, const std::string& error)>;

    explicit AfcConfigManager(IMoonrakerAPI* api);
    AfcConfigManager(IMoonrakerAPI* api, helix::LifetimeToken token);
    ~AfcConfigManager() = default;

    /// Load a config file from the printer via Moonraker.
    /// filename is relative to config root, e.g., "AFC/AFC.cfg"
    void load(const std::string& filename, Callback on_done);

    /// Save current state back to the printer via Moonraker.
    void save(const std::string& filename, Callback on_done);

    /// Load directly from a string (for testing or when content is already available).
    /// Sets loaded state and stores content as the baseline for discard.
    void load_from_string(const std::string& content, const std::string& filename);

    /// Access the parsed config for reading/modifying values.
    KlipperConfigParser& parser();
    const KlipperConfigParser& parser() const;

    /// Returns true if any modifications have been made since last load/save.
    bool has_unsaved_changes() const;

    /// Explicitly mark the config as having unsaved changes.
    void mark_dirty();

    /// Revert to the last-loaded state, clearing all modifications.
    void discard_changes();

    /// Returns true if a config file has been loaded (via load() or load_from_string()).
    bool is_loaded() const;

    /// Returns the filename of the currently loaded config.
    const std::string& loaded_filename() const;

  private:
    IMoonrakerAPI* api_;
    std::optional<helix::LifetimeToken> token_; ///< Lifetime token from owning backend
    KlipperConfigParser parser_;
    std::string original_content_; ///< Content at last load/save (baseline for discard)
    std::string loaded_filename_;
    bool dirty_{false};
    bool loaded_{false};
};
