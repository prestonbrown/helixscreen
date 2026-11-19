// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_dir_watcher.h"

#include "core/timer_guard.h"
#include "plugin_manifest.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <vector>

namespace helix::plugin {

namespace {

namespace fs = std::filesystem;

/// The signature of one plugin's directory: every regular file under it.
DirSignature signature_of(const fs::path& plugin_dir) {
    DirSignature sig;
    std::error_code it_ec;
    for (fs::recursive_directory_iterator it(plugin_dir, it_ec), end; it != end;
         it.increment(it_ec)) {
        std::error_code qec;
        if (!it->is_regular_file(qec))
            continue;
        ++sig.files;
        sig.bytes += static_cast<uint64_t>(it->file_size(qec));
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         it->last_write_time(qec).time_since_epoch())
                         .count();
        if (ns > sig.newest_mtime_ns)
            sig.newest_mtime_ns = ns;
    }
    if (it_ec)
        spdlog::debug("[PluginDirWatcher] scanning {} failed: {}", plugin_dir.string(),
                      it_ec.message());
    return sig;
}

} // namespace

std::map<std::string, DirSignature> scan_plugin_dir(const std::string& dir) {
    std::map<std::string, DirSignature> out;
    std::error_code it_ec;
    for (fs::directory_iterator it(dir, it_ec), end; it != end; it.increment(it_ec)) {
        std::error_code qec;
        if (!it->is_directory(qec))
            continue;
        const std::string name = it->path().filename().string();
        if (!is_valid_plugin_id(name))
            continue;
        out[name] = signature_of(it->path());
    }
    return out;
}

PluginDirWatcher::PluginDirWatcher(PluginHost& host, std::string dir, uint32_t interval_ms)
    : host_(host), dir_(std::move(dir)), last_(scan_plugin_dir(dir_)) {
    timer_ = lv_timer_create(&PluginDirWatcher::on_timer, interval_ms, this);
}

PluginDirWatcher::~PluginDirWatcher() {
    stop();
}

void PluginDirWatcher::poll_now() {
    auto now = scan_plugin_dir(dir_);
    std::vector<std::string> changed;
    for (const auto& [id, sig] : now) {
        auto prev = last_.find(id);
        if (prev == last_.end() || !(prev->second == sig))
            changed.push_back(id);
    }
    for (const auto& [id, sig] : last_) {
        (void)sig;
        if (now.find(id) == now.end())
            changed.push_back(id);
    }
    last_ = std::move(now);
    if (changed.empty())
        return;
    spdlog::debug("[PluginDirWatcher] {} plugin(s) changed under {}, rescanning", changed.size(),
                  dir_);
    host_.rescan(changed);
}

void PluginDirWatcher::on_timer(lv_timer_t* timer) {
    auto* self = static_cast<PluginDirWatcher*>(lv_timer_get_user_data(timer));
    if (self)
        self->poll_now();
}

void PluginDirWatcher::stop() {
    helix::ui::lv_timer_cancel_safe(timer_);
    timer_ = nullptr;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
