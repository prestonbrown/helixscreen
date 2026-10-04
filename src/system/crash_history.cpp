// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/crash_history.h"

#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace helix {

// =============================================================================
// Fingerprint
// =============================================================================

std::string crash_fingerprint(const std::string& signal_name, const std::string& app_version,
                              const std::string& first_bt_frame) {
    std::string sig = signal_name.empty() ? "UNKNOWN" : signal_name;
    std::string ver = app_version.empty() ? "unknown" : app_version;
    std::string frame = first_bt_frame.empty() ? "no-bt" : first_bt_frame;
    return sig + "/" + ver + "/" + frame;
}

// =============================================================================
// Singleton
// =============================================================================

CrashHistory& CrashHistory::instance() {
    static CrashHistory instance;
    return instance;
}

// =============================================================================
// Lifecycle
// =============================================================================

void CrashHistory::init(const std::string& config_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_dir_ = config_dir;
    entries_.clear();
    initialized_ = true;
    load();
    spdlog::debug("[CrashHistory] Initialized with {} entries from {}", entries_.size(),
                  config_dir);
}

void CrashHistory::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    config_dir_.clear();
    initialized_ = false;
}

// =============================================================================
// Public API
// =============================================================================

void CrashHistory::add_entry(const CrashHistoryEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        spdlog::warn("[CrashHistory] add_entry called before init");
        return;
    }

    entries_.push_back(entry);

    // FIFO cap: drop oldest entry when over limit
    if (entries_.size() > MAX_ENTRIES) {
        entries_.erase(entries_.begin());
    }

    save();
}

std::vector<CrashHistoryEntry> CrashHistory::get_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

size_t CrashHistory::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool CrashHistory::has_fingerprint(const std::string& fingerprint) const {
    if (fingerprint.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(
        entries_.begin(), entries_.end(),
        [&fingerprint](const CrashHistoryEntry& e) { return e.fingerprint == fingerprint; });
}

json CrashHistory::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_to_json();
}

// =============================================================================
// Persistence
// =============================================================================

std::string CrashHistory::history_file_path() const {
    return config_dir_ + "/crash_history.json";
}

void CrashHistory::load() {
    // Caller must hold mutex_
    std::string path = history_file_path();
    try {
        std::ifstream file(path);
        if (!file.good()) {
            spdlog::debug("[CrashHistory] No history file at {}", path);
            return;
        }

        json arr = json::parse(file);
        if (!arr.is_array()) {
            spdlog::warn("[CrashHistory] History file is not a JSON array, ignoring");
            return;
        }

        entries_.clear();
        for (const auto& item : arr) {
            try {
                entries_.push_back(entry_from_json(item));
            } catch (const std::exception& e) {
                spdlog::warn("[CrashHistory] Skipping malformed entry: {}", e.what());
            }
        }

        // Enforce cap on load (in case file was manually edited)
        if (entries_.size() > MAX_ENTRIES) {
            entries_.erase(entries_.begin(), entries_.begin() + static_cast<ptrdiff_t>(
                                                                    entries_.size() - MAX_ENTRIES));
        }
    } catch (const json::parse_error& e) {
        spdlog::warn("[CrashHistory] Failed to parse {}: {}", path, e.what());
        entries_.clear();
    } catch (const std::exception& e) {
        spdlog::warn("[CrashHistory] Failed to load {}: {}", path, e.what());
        entries_.clear();
    }
}

void CrashHistory::save() const {
    // Caller must hold mutex_
    std::string path = history_file_path();
    try {
        std::ofstream file(path);
        if (file.good()) {
            file << helix::json_util::safe_dump(entries_to_json(), 2);
            spdlog::trace("[CrashHistory] Saved {} entries to {}", entries_.size(), path);
        } else {
            spdlog::warn("[CrashHistory] Failed to open {} for writing", path);
        }
    } catch (const std::exception& e) {
        spdlog::error("[CrashHistory] Failed to save: {}", e.what());
    }
}

json CrashHistory::entries_to_json() const {
    // Caller must hold mutex_
    json arr = json::array();
    for (const auto& entry : entries_) {
        arr.push_back(entry_to_json(entry));
    }
    return arr;
}

// =============================================================================
// JSON serialization
// =============================================================================

CrashHistoryEntry CrashHistory::entry_from_json(const json& j) {
    CrashHistoryEntry entry;
    entry.timestamp = j.value("timestamp", "");
    entry.signal = j.value("signal", 0);
    entry.signal_name = j.value("signal_name", "");
    entry.app_version = j.value("app_version", "");
    entry.uptime_sec = j.value("uptime_sec", 0);
    entry.fault_addr = j.value("fault_addr", "");
    entry.fault_code_name = j.value("fault_code_name", "");
    entry.abort_msg = j.value("abort_msg", "");
    entry.github_issue = j.value("github_issue", 0);
    entry.github_url = j.value("github_url", "");
    entry.sent_via = j.value("sent_via", "");
    entry.fingerprint = j.value("fingerprint", "");
    return entry;
}

json CrashHistory::entry_to_json(const CrashHistoryEntry& entry) {
    try {
        return json{{"timestamp", entry.timestamp},
                    {"signal", entry.signal},
                    {"signal_name", entry.signal_name},
                    {"app_version", entry.app_version},
                    {"uptime_sec", entry.uptime_sec},
                    {"fault_addr", entry.fault_addr},
                    {"fault_code_name", entry.fault_code_name},
                    {"abort_msg", entry.abort_msg},
                    {"github_issue", entry.github_issue},
                    {"github_url", entry.github_url},
                    {"sent_via", entry.sent_via},
                    {"fingerprint", entry.fingerprint}};
    } catch (const std::exception& e) {
        spdlog::error("[CrashHistory] Failed to serialize entry: {}", e.what());
        return json{{"error", "serialization_failed"}};
    }
}

} // namespace helix
