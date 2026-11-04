// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_source_app.h"

#include "app_globals.h"
#include "core/text_io.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <system_error>
#include <utility>
#include <vector>

namespace helix::plugin {

bool is_plugin_filelist_change(const json& msg) {
    const json* payload = helix::json_util::notification_payload(msg);
    if (!payload)
        return false;
    auto in_root = [](const json& item) {
        if (!item.is_object())
            return false;
        std::string root = helix::json_util::safe_string(item, "root");
        std::string path = helix::json_util::safe_string(item, "path");
        std::string_view prefix(kPluginRootPath);
        std::string_view bare = prefix.substr(0, prefix.size() - 1); // the folder itself
        return root == "config" && (path.rfind(prefix, 0) == 0 || path == bare);
    };
    auto it = payload->find("item");
    auto src = payload->find("source_item");
    return (it != payload->end() && in_root(*it)) || (src != payload->end() && in_root(*src));
}

SourceDeps make_moonraker_source_deps(IMoonrakerAPI* api) {
    SourceDeps deps;
    deps.list = [api](std::function<void(bool ok, std::vector<RemoteFile>)> done) {
        api->files().list_files(
            "config", "", false,
            [done](const std::vector<FileInfo>& files) {
                std::vector<RemoteFile> out;
                const size_t prefix_len = std::string_view(kPluginRootPath).size();
                for (const FileInfo& f : files) {
                    if (f.is_dir || f.path.rfind(kPluginRootPath, 0) != 0)
                        continue;
                    out.push_back({f.path.substr(prefix_len), f.size, f.modified});
                }
                done(true, std::move(out));
            },
            [done](const MoonrakerError& error) {
                spdlog::warn("[PluginSource] config listing failed: {}", error.message);
                done(false, {});
            });
    };
    deps.download = [api](const std::string& path, const std::string& dest, size_t max_bytes,
                          std::function<void(bool ok, std::string error)> done) {
        // A partial download never moves more than max_bytes over the wire, so an
        // oversized or stale file cannot fill flash before PluginSource rejects it.
        api->transfers().download_file_partial(
            "config", std::string(kPluginRootPath) + path, max_bytes,
            [dest, done](const std::string& body) {
                if (!text_io::write_file(dest, body)) {
                    done(false, "cannot write " + dest);
                    return;
                }
                done(true, {});
            },
            [done](const MoonrakerError& error) { done(false, error.message); });
    };
    return deps;
}

std::string plugin_cache_dir_for(const std::string& printer_id) {
    // The id comes from user-editable settings, so it is untrusted as a path
    // component: keep every byte in [a-z0-9_-] and map the rest to '-', which
    // cannot express '..', a separator or a leading '/'.
    std::string id;
    for (char c : printer_id)
        id += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c : '-';
    if (id.empty())
        id = "default";
    const std::filesystem::path dir = std::filesystem::path(get_helix_cache_dir("plugins")) / id;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

PluginSyncDriver::PluginSyncDriver(PluginHost& host, SourceDeps deps, std::string cache_dir,
                                   uint32_t debounce_ms)
    : host_(host), source_(std::make_unique<PluginSource>(std::move(deps), std::move(cache_dir))),
      debounce_ms_(debounce_ms) {}

PluginSyncDriver::~PluginSyncDriver() {
    if (!timer_ || !lv_is_initialized())
        return;
    helix::ui::lv_timer_cancel_safe(timer_);
    // The neutered timer has no callback, so handing it back to LVGL's own
    // spent-timer collection is safe and frees the node.
    lv_timer_set_auto_delete(timer_, true);
}

void PluginSyncDriver::sync_now() {
    // A direct sync subsumes whatever the debounce was still waiting to do;
    // a spent timer cannot re-fire until the next request re-arms it.
    if (timer_) {
        lv_timer_set_repeat_count(timer_, 0);
        lv_timer_pause(timer_);
    }
    source_->sync([this](const SyncResult& result) {
        std::vector<std::string> ids = result.changed;
        ids.insert(ids.end(), result.removed.begin(), result.removed.end());
        if (!ids.empty())
            host_.rescan(ids);
        if (on_synced)
            on_synced(result);
    });
}

void PluginSyncDriver::request_sync() {
    if (!timer_) {
        timer_ = lv_timer_create(&PluginSyncDriver::on_debounce, debounce_ms_, this);
        // One fire per arm. auto_delete stays off while the driver owns the
        // timer so a fired debounce leaves timer_ valid for the next arm.
        lv_timer_set_auto_delete(timer_, false);
    }
    lv_timer_set_repeat_count(timer_, 1);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
}

void PluginSyncDriver::on_debounce(lv_timer_t* timer) {
    auto* self = static_cast<PluginSyncDriver*>(lv_timer_get_user_data(timer));
    if (self)
        self->sync_now();
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
