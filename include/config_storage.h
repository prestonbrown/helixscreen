// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <optional>
#include <string>

namespace helix {

/**
 * Document-level persistence backend for Config. Desktop = atomic-rename
 * JSON file (fsync file + parent dir, rolling backup); embedded targets
 * substitute NVS or LittleFS. Config keeps the JSON model, migrations,
 * defaults and multi-printer routing — the backend only moves bytes durably.
 */
class ConfigStorage {
  public:
    virtual ~ConfigStorage() = default;

    /// Whole-document read. nullopt with @p read_error left empty = the
    /// document does not exist (first boot). If it exists but could not be
    /// read (permission denied, I/O error), return nullopt with @p read_error
    /// set: callers must be able to tell "absent" from "present but
    /// unreadable" so they don't silently treat a locked-down existing config
    /// as first-boot and reset it to defaults. Config::init() routes an
    /// unreadable document into the same corrupt-preserve + backup-restore
    /// path as a parse failure.
    virtual std::optional<std::string> load(std::string& read_error) = 0;

    /// Atomic, durable whole-document write. False on failure (caller logs).
    virtual bool store(const std::string& bytes) = 0;

    /// Set the current (corrupt) document aside so load() stops returning
    /// it, preserving it for diagnosis where the backend can (.corrupt file).
    virtual void preserve_corrupt() = 0;

    /// True when the backing store cannot accept writes (RO filesystem).
    virtual bool read_only() = 0;

    /// Human-readable location for logs ("config/settings.json", "nvs://…").
    virtual std::string describe() const = 0;

    /// True on a small flash partition, where Config writes compact JSON and
    /// keeps no side copies of the document.
    virtual bool small_footprint() const {
        return false;
    }
};

/// Standard writes hand-editable pretty JSON. Small is for a partition the size
/// of the K-Touch's 128 KB cfg, where a save must hold the old and new copies at
/// once: compact JSON, about a third of the size, and no .pre-migration copy.
enum class ConfigFootprint { Standard, Small };

/// Atomic-rename file implementation behind Config::save() / Config::init().
std::unique_ptr<ConfigStorage>
make_file_config_storage(const std::string& path,
                         ConfigFootprint footprint = ConfigFootprint::Standard);

} // namespace helix
