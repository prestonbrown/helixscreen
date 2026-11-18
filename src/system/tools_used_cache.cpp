// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools_used_cache.h"

#include "app_globals.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <utility>

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

    const auto j = nlohmann::json::parse(data, nullptr, false);
    if (j.is_discarded()) {
        spdlog::warn("[ToolsUsedCache] Corrupt cache file, starting cold: not valid JSON");
        entries_.clear();
        return false;
    }
    const auto it = j.find("entries");
    if (it == j.end() || !it->is_object()) {
        return false;
    }
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        const auto& e = entry.value();
        const auto size = helix::json_util::read_field<uint64_t>(e, "size");
        const auto mtime = helix::json_util::read_field<int64_t>(e, "mtime");
        const nlohmann::json* tools_arr = helix::json_util::find_array(e, "tools");
        if (!size || !mtime || !tools_arr) {
            continue; // drop malformed entry, keep the rest
        }
        const FileVersion version{*size, static_cast<time_t>(*mtime)};
        std::set<int> tools;
        bool ok = true;
        for (const auto& t : *tools_arr) {
            if (!t.is_number() || t.get<int64_t>() < 0) {
                ok = false;
                break;
            }
            tools.insert(static_cast<int>(t.get<int64_t>()));
        }
        if (ok) {
            entries_.put(entry.key(), version, std::move(tools));
        }
    }
    return true;
}

void ToolsUsedCache::save_to_disk() {
    nlohmann::json j = {{"v", 1}, {"entries", nlohmann::json::object()}};
    for (const auto& [key, e] : entries_.entries()) {
        nlohmann::json tools = nlohmann::json::array();
        for (int t : e.value)
            tools.push_back(t);
        j["entries"][key] = {{"size", e.version.size_bytes},
                             {"mtime", static_cast<int64_t>(e.version.modified)},
                             {"tools", std::move(tools)}};
    }
    const std::string path = cache_file_path();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        spdlog::warn("[ToolsUsedCache] Cannot write {}", path);
        return;
    }
    const std::string out = helix::json_util::safe_dump(j);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
}

std::optional<std::set<int>> ToolsUsedCache::lookup(const std::string& file_path,
                                                    FileVersion version) {
    if (!loaded_) {
        load_from_disk();
    }
    // A miss, or stale: same key, changed file.
    if (const std::set<int>* tools = entries_.lookup(file_path, version)) {
        return *tools;
    }
    return std::nullopt;
}

void ToolsUsedCache::store(const std::string& file_path, FileVersion version,
                           const std::set<int>& tools) {
    if (!loaded_) {
        load_from_disk();
    }
    entries_.put(file_path, version, tools);
    entries_.evict(MAX_ENTRIES);
    save_to_disk();
}

ToolsUsedCache& tools_used_cache() {
    static ToolsUsedCache cache;
    return cache;
}

} // namespace helix
