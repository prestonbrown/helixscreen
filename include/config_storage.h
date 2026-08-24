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

    /// Whole-document read. nullopt = document does not exist (first boot).
    /// If the document exists but could not be read (permission denied, I/O
    /// error), throw (e.g. std::runtime_error) rather than returning
    /// nullopt — callers must be able to tell "absent" from "present but
    /// unreadable" so they don't silently treat a locked-down existing
    /// config as first-boot and reset it to defaults. Config::init() routes
    /// a thrown load() into the same corrupt-preserve + backup-restore path
    /// as a parse failure.
    virtual std::optional<std::string> load() = 0;

    /// Atomic, durable whole-document write. False on failure (caller logs).
    virtual bool store(const std::string& bytes) = 0;

    /// Set the current (corrupt) document aside so load() stops returning
    /// it, preserving it for diagnosis where the backend can (.corrupt file).
    virtual void preserve_corrupt() = 0;

    /// True when the backing store cannot accept writes (RO filesystem).
    virtual bool read_only() = 0;

    /// Human-readable location for logs ("config/settings.json", "nvs://…").
    virtual std::string describe() const = 0;
};

/// Atomic-rename file implementation; behavior extracted verbatim from the
/// pre-seam Config::save() / Config::init().
std::unique_ptr<ConfigStorage> make_file_config_storage(const std::string& path);

} // namespace helix
