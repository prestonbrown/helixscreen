// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/afc_message_dedup.h"

#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <fstream>

using json = nlohmann::json;

namespace helix {

AfcMessageDedup& AfcMessageDedup::instance() {
    static AfcMessageDedup instance;
    return instance;
}

void AfcMessageDedup::init(const std::string& config_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_dir_ = config_dir;
    last_error_.clear();
    initialized_ = true;

    const std::string path = config_dir_ + "/afc_message_dedup.json";
    std::ifstream in(path);
    if (!in) {
        // Absent is the common first-boot case, not an error.
        spdlog::debug("[AfcMessageDedup] No seed file at {}", path);
        return;
    }
    try {
        json data = json::parse(in);
        last_error_ = helix::json_util::safe_string(data, "last_error");
    } catch (const json::exception& e) {
        // A corrupt seed costs one extra toast; say so and move on.
        spdlog::warn("[AfcMessageDedup] Unreadable seed file {}: {}", path, e.what());
    }
    spdlog::debug("[AfcMessageDedup] Initialized (seed {} bytes) from {}", last_error_.size(),
                  config_dir_);
}

void AfcMessageDedup::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_dir_.clear();
    last_error_.clear();
    initialized_ = false;
}

std::string AfcMessageDedup::last_error_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void AfcMessageDedup::record_error(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || text == last_error_) {
        return;
    }
    last_error_ = text;
    save_locked(text);
}

void AfcMessageDedup::record_cleared() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || last_error_.empty()) {
        return;
    }
    last_error_.clear();
    save_locked("");
}

void AfcMessageDedup::save_locked(const std::string& text) {
    const std::string path = config_dir_ + "/afc_message_dedup.json";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        // A lost record costs one extra toast on the next start.
        spdlog::warn("[AfcMessageDedup] Cannot write {}", path);
        return;
    }
    try {
        // safe_dump: message text carries whatever the printer printed, and
        // strict-mode dump would throw on invalid UTF-8 and cost the file.
        out << helix::json_util::safe_dump(json{{"last_error", text}}) << "\n";
    } catch (const json::exception& e) {
        spdlog::warn("[AfcMessageDedup] Cannot serialize seed for {}: {}", path, e.what());
    } catch (const std::bad_alloc&) {
        spdlog::warn("[AfcMessageDedup] Out of memory serializing seed for {}", path);
    }
}

} // namespace helix
