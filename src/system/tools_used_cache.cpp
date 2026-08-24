// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools_used_cache.h"

#include "app_globals.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix {

namespace {
std::string cache_file_path() {
    return get_helix_cache_dir("tools_used") + "/cache.json";
}
} // namespace

bool ToolsUsedCache::load_from_disk() {
    loaded_ = true; // never retry within this instance
    const std::string path = cache_file_path();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false; // cold cache — normal on first run
    }
    std::string data;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
    }
    std::fclose(f);

    try {
        const auto j = nlohmann::json::parse(data);
        const auto it = j.find("entries");
        if (it == j.end() || !it->is_object()) {
            return false;
        }
        for (auto entry = it->begin(); entry != it->end(); ++entry) {
            const auto& e = entry.value();
            if (!e.is_object() || !e.contains("size") || !e.contains("mtime") ||
                !e["size"].is_number() || !e["mtime"].is_number() || !e.contains("tools") ||
                !e["tools"].is_array()) {
                continue; // drop malformed entry, keep the rest
            }
            Entry parsed;
            parsed.size_bytes = e["size"].get<uint64_t>();
            parsed.modified = static_cast<time_t>(e["mtime"].get<int64_t>());
            bool ok = true;
            for (const auto& t : e["tools"]) {
                if (!t.is_number() || t.get<int64_t>() < 0) {
                    ok = false;
                    break;
                }
                parsed.tools.insert(static_cast<int>(t.get<int64_t>()));
            }
            if (ok) {
                parsed.last_used_ctr = next_ctr_++;
                entries_[entry.key()] = std::move(parsed);
            }
        }
        return true;
    } catch (const std::exception& ex) {
        spdlog::warn("[ToolsUsedCache] Corrupt cache file, starting cold: {}", ex.what());
        entries_.clear();
        return false;
    }
}

void ToolsUsedCache::save_to_disk() {
    try {
        nlohmann::json j = {{"v", 1}, {"entries", nlohmann::json::object()}};
        for (const auto& [key, e] : entries_) {
            nlohmann::json tools = nlohmann::json::array();
            for (int t : e.tools)
                tools.push_back(t);
            j["entries"][key] = {{"size", e.size_bytes},
                                 {"mtime", static_cast<int64_t>(e.modified)},
                                 {"tools", std::move(tools)}};
        }
        const std::string path = cache_file_path();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            spdlog::warn("[ToolsUsedCache] Cannot write {}", path);
            return;
        }
        const std::string out = j.dump();
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    } catch (const std::exception& ex) {
        spdlog::warn("[ToolsUsedCache] Save failed: {}", ex.what());
    }
}

std::optional<std::set<int>> ToolsUsedCache::lookup(const std::string& file_path,
                                                    uint64_t size_bytes, time_t modified) {
    if (!loaded_ && !load_from_disk()) {
        // cold cache — loaded_ is now true, entries_ empty
    }
    const auto it = entries_.find(file_path);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    const Entry& e = it->second;
    if (e.size_bytes != size_bytes || e.modified != modified) {
        return std::nullopt; // stale — same key, changed file
    }
    it->second.last_used_ctr = next_ctr_++;
    return e.tools;
}

void ToolsUsedCache::store(const std::string& file_path, uint64_t size_bytes, time_t modified,
                           const std::set<int>& tools) {
    if (!loaded_ && !load_from_disk()) {
    }
    Entry& e = entries_[file_path];
    e.size_bytes = size_bytes;
    e.modified = modified;
    e.tools = tools;
    e.last_used_ctr = next_ctr_++;

    if (entries_.size() > MAX_ENTRIES) {
        // Evict lowest last_used_ctr entries down to MAX_ENTRIES.
        // (collect all, partial_sort by ctr ascending, erase the front slice)
        std::vector<std::pair<uint64_t, std::map<std::string, Entry>::iterator>> all;
        all.reserve(entries_.size());
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            all.emplace_back(it->second.last_used_ctr, it);
        }
        const size_t excess = entries_.size() - MAX_ENTRIES;
        // ctrs are unique (one monotonic counter), so first-only compare is total
        std::partial_sort(all.begin(), all.begin() + static_cast<long>(excess), all.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < excess; ++i) {
            entries_.erase(all[i].second);
        }
    }
    save_to_disk();
}

} // namespace helix
