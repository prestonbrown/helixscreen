// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/afc_message_dedup.h"

#include "config.h"
#include "core/text_io.h"
#include "exception_policy.h"
#include "helix_fs.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>

using json = nlohmann::json;

namespace helix {

namespace tio = ::helix::text_io;

AfcMessageDedup& AfcMessageDedup::instance() {
    static AfcMessageDedup instance;
    return instance;
}

std::string AfcMessageDedup::active_printer_key() {
    auto* config = Config::get_instance();
    const std::string id = config->get_active_printer_id();
    return id.empty() ? std::string("default") : id;
}

std::string AfcMessageDedup::seed_path_locked() const {
    return config_dir_ + "/afc_message_dedup.json";
}

void AfcMessageDedup::init(const std::string& config_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_dir_ = config_dir;
    last_error_by_printer_.clear();
    initialized_ = true;
    warned_uninitialized_ = false;

    const std::string path = seed_path_locked();
    {
        if (!tio::open_file(path, "rb")) {
            // Absent is the common first-boot case, not an error.
            spdlog::debug("[AfcMessageDedup] No seed file at {}", path);
            return;
        }
        // file_size from stat: a path that is not a readable regular file
        // reports here rather than as an empty parse.
        const auto size = tio::file_size(path);
        if (size && *size > kMaxSeedBytes) {
            spdlog::warn("[AfcMessageDedup] Seed file {} is {} bytes, past the {}-byte "
                         "cap; ignoring it",
                         path, *size, kMaxSeedBytes);
            return;
        }
        if (!size) {
            spdlog::warn("[AfcMessageDedup] Cannot load seed file {}: {}", path,
                         std::strerror(errno));
            last_error_by_printer_.clear();
        } else {
            // Read only after the cap check: the cap exists so an oversized file is
            // never pulled into memory.
            // A corrupt seed costs one extra toast; say so and move on.
            json data = json::parse(tio::read_file(path).value_or(""), nullptr, false);
            if (data.is_discarded()) {
                spdlog::warn("[AfcMessageDedup] Unreadable seed file {}: not valid JSON", path);
                last_error_by_printer_.clear();
            } else if (data.contains("printers") && data["printers"].is_object()) {
                for (const auto& [printer_id, text] : data["printers"].items()) {
                    if (text.is_string()) {
                        last_error_by_printer_[printer_id] = text.get<std::string>();
                    }
                }
            }
        }
    }

    // A seed this store cannot retract must not be armed: record_cleared()
    // could never take the text back off disk, so every later session would
    // keep suppressing it. Refusing it costs one toast.
    if (!last_error_by_printer_.empty() && !can_write_locked()) {
        spdlog::warn("[AfcMessageDedup] Seed file {} is not writable; ignoring it so a "
                     "recurrence still surfaces",
                     path);
        last_error_by_printer_.clear();
    }

    spdlog::debug("[AfcMessageDedup] Initialized ({} printer records) from {}",
                  last_error_by_printer_.size(), config_dir_);
}

void AfcMessageDedup::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_dir_.clear();
    last_error_by_printer_.clear();
    initialized_ = false;
}

std::string AfcMessageDedup::last_error_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (warn_uninitialized_locked("last_error_text")) {
        return {};
    }
    auto it = last_error_by_printer_.find(active_printer_key());
    return it == last_error_by_printer_.end() ? std::string() : it->second;
}

void AfcMessageDedup::record_error(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (warn_uninitialized_locked("record_error")) {
        return;
    }
    const std::string key = active_printer_key();
    auto it = last_error_by_printer_.find(key);
    if (it != last_error_by_printer_.end() && it->second == text) {
        return;
    }
    last_error_by_printer_[key] = text;
    // A lost record costs one extra toast on the next start, which is the
    // direction this store is allowed to fail in.
    save_locked();
}

void AfcMessageDedup::record_cleared() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (warn_uninitialized_locked("record_cleared")) {
        return;
    }
    if (last_error_by_printer_.erase(active_printer_key()) == 0) {
        return;
    }
    if (save_locked()) {
        return;
    }

    // The queue is empty, so a recurrence is a new event — but disk still
    // names the old text and would seed the next session with it. Delete the
    // file: an absent record is the state every load path fails open on, and
    // an unwritable file in a writable directory can still be removed.
    const std::string path = seed_path_locked();
    if (!helix::fs::remove(path) && errno != ENOENT) {
        spdlog::error("[AfcMessageDedup] Cannot clear the seed at {} ({}); a later session "
                      "may not toast this error's first sighting",
                      path, std::strerror(errno));
        return;
    }
    spdlog::warn("[AfcMessageDedup] Could not rewrite the seed at {}; deleted it so a "
                 "recurrence still surfaces",
                 path);
}

bool AfcMessageDedup::save_locked() {
    const std::string path = seed_path_locked();
    // safe_dump: message text carries whatever the printer printed, and
    // strict-mode dump would throw on invalid UTF-8 and cost the file.
    std::string data;
    if (!helix::contain_exceptions(
            fmt::format("[AfcMessageDedup] Serializing the seed for {}", path), [&] {
                json printers = json::object();
                for (const auto& [printer_id, text] : last_error_by_printer_) {
                    printers[printer_id] = text;
                }
                data = helix::json_util::safe_dump(json{{"printers", printers}}) + "\n";
            })) {
        return false;
    }
    if (!tio::write_file(path, data)) {
        spdlog::warn("[AfcMessageDedup] Cannot write {}", path);
        return false;
    }
    return true;
}

bool AfcMessageDedup::can_write_locked() const {
    return static_cast<bool>(tio::open_file(seed_path_locked(), "ab"));
}

bool AfcMessageDedup::warn_uninitialized_locked(const char* op) const {
    if (initialized_) {
        return false;
    }
    if (!warned_uninitialized_) {
        warned_uninitialized_ = true;
        spdlog::warn("[AfcMessageDedup] {} before init(): nothing persists, so a latched AFC "
                     "message re-toasts at every start",
                     op);
    }
    return true;
}

} // namespace helix
