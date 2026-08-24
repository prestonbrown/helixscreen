// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace helix {

/// Persistent per-file cache of the tools-used set recovered from a gcode
/// scan/viewer parse. Keyed by (path, size, mtime) so re-sliced files
/// invalidate naturally. JSON map in <cache>/tools_used/cache.json,
/// LRU-bounded. Pure logic — no LVGL, safe on any thread (internally
/// serialized with a mutex if a mutex already exists; single-threaded use
/// from the detail view today, so keep it simple: document main-thread use).
class ToolsUsedCache {
  public:
    ToolsUsedCache() = default;

    /// nullopt = miss / stale / malformed. An EMPTY set is a legitimate
    /// cached value (single-extruder file) and is returned as such.
    std::optional<std::set<int>> lookup(const std::string& file_path, uint64_t size_bytes,
                                        time_t modified);

    /// Persist an entry (also compacts when over MAX_ENTRIES — drop
    /// least-recently-LOOKED-UP first). Writes through to disk immediately;
    /// a failed write logs a warning and keeps the in-memory entry.
    void store(const std::string& file_path, uint64_t size_bytes, time_t modified,
               const std::set<int>& tools);

    static constexpr size_t MAX_ENTRIES = 256;

    /// Test seam + cheap explicit reset. Not needed in production flow.
    void invalidate() {
        entries_.clear();
    }

  private:
    struct Entry {
        uint64_t size_bytes = 0;
        time_t modified = 0;
        std::set<int> tools;
        uint64_t last_used_ctr = 0; // higher = more recent
    };
    bool load_from_disk();
    void save_to_disk();

    std::map<std::string, Entry> entries_;
    uint64_t next_ctr_ = 1;
    bool loaded_ = false;
};

} // namespace helix
