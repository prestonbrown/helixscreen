// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_webcam.h"

#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "data_root_resolver.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "printer_state.h"
#include "remote_screen_service.h"
#include "remote_screen_settings_manager.h"
#include "webcam_selection.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unistd.h>

namespace helix {

std::string remote_screen_webcam_name(bool moonraker_on_this_host, const std::string& hostname) {
    if (moonraker_on_this_host || hostname.empty()) {
        return kRemoteScreenWebcamName;
    }
    return std::string(kRemoteScreenWebcamName) + " (" + hostname + ")";
}

std::string remote_screen_effective_route(bool moonraker_remote, const std::string& route) {
    return moonraker_remote ? std::string() : route;
}

std::optional<nlohmann::json> remote_screen_webcam_item(const std::string& name, bool enabled,
                                                        bool control, const std::string& route,
                                                        const std::string& lan_ip, int port) {
    if (!enabled) {
        return std::nullopt;
    }
    std::string base;
    if (!route.empty()) {
        base = route.back() == '/' ? route : route + "/";
    } else if (!lan_ip.empty()) {
        base = "http://" + lan_ip + ":" + std::to_string(port) + "/";
    } else {
        return std::nullopt;
    }
    const std::string snapshot = base + "snapshot.jpg";
    nlohmann::json item = {
        {"name", name},
        {"location", "printer"},
        {"enabled", true},
        {"snapshot_url", snapshot},
        {"extra_data", {{kRemoteScreenWebcamMarker, true}}},
    };
    if (control) {
        item["service"] = "iframe";
        item["stream_url"] = base;
    } else {
        item["service"] = "mjpegstreamer";
        item["stream_url"] = base + "stream.mjpg";
        item["target_fps"] = 2;
    }
    return item;
}

namespace {

using StoreUid = std::function<void(const std::string&)>;

void log_webcam_error(const char* what, const MoonrakerError& err) {
    spdlog::warn("[RemoteScreen] Could not {} the screen webcam: {}", what, err.message);
}

/// Post @p item as a new entry, or onto the one already named like it.
void post_by_name(IMoonrakerAPI& api, const nlohmann::json& item, StoreUid store_uid) {
    api.timelapse().post_webcam(
        item,
        [store_uid](const WebcamInfo& cam) {
            spdlog::info("[RemoteScreen] Registered the screen as a webcam (uid '{}')", cam.uid);
            store_uid(cam.uid);
        },
        [](const MoonrakerError& err) { log_webcam_error("register", err); });
}

/// Refresh the entry @p uid with @p item's URLs. Only what the screen decides is sent, so
/// the name, enabled state and frame rate the user gave the entry are kept.
void post_by_uid(IMoonrakerAPI& api, const nlohmann::json& item, const std::string& uid,
                 StoreUid store_uid) {
    nlohmann::json update = {{"uid", uid}};
    for (const char* key : {"service", "stream_url", "snapshot_url", "extra_data"}) {
        const auto it = item.find(key);
        if (it != item.end()) {
            update[key] = *it;
        }
    }
    api.timelapse().post_webcam(
        update,
        [store_uid](const WebcamInfo& cam) {
            spdlog::info("[RemoteScreen] Refreshed the screen webcam '{}'", cam.name);
            if (!cam.uid.empty()) {
                store_uid(cam.uid);
            }
        },
        [](const MoonrakerError& err) {
            if (err.is_not_found()) {
                spdlog::info("[RemoteScreen] The screen webcam was removed in Moonraker; "
                             "not adding it back until sharing is turned on again");
                return;
            }
            // Never re-post by name here: the entry may be renamed, and a new one would
            // duplicate it. The next connect retries.
            log_webcam_error("refresh", err);
        });
}

} // namespace

void reconcile_remote_screen_webcam(IMoonrakerAPI& api, const std::optional<nlohmann::json>& item,
                                    const std::string& name, const std::string& known_uid,
                                    StoreUid store_uid) {
    if (item && !known_uid.empty()) {
        post_by_uid(api, *item, known_uid, std::move(store_uid));
        return;
    }
    if (!item && !known_uid.empty()) {
        WebcamInfo cam;
        cam.uid = known_uid;
        api.timelapse().delete_webcam(
            cam, [] { spdlog::info("[RemoteScreen] Removed the screen webcam"); },
            [](const MoonrakerError& err) {
                if (!err.is_not_found()) {
                    log_webcam_error("remove", err);
                }
            });
        store_uid("");
        return;
    }
    const std::string snapshot =
        item ? json_util::safe_string(*item, "snapshot_url", "") : std::string();
    IMoonrakerAPI* api_ptr = &api;
    api.timelapse().get_webcam_list(
        [api_ptr, item, name, snapshot, store_uid](const std::vector<WebcamInfo>& cams) {
            const auto ours = std::find_if(cams.begin(), cams.end(), [&](const WebcamInfo& cam) {
                return cam.name == name || (!snapshot.empty() && cam.snapshot_url == snapshot);
            });
            if (item) {
                if (ours != cams.end() && !ours->uid.empty()) {
                    post_by_uid(*api_ptr, *item, ours->uid, store_uid);
                } else {
                    post_by_name(*api_ptr, *item, store_uid);
                }
                return;
            }
            // Deleting a missing entry is an error, so look before deleting.
            if (ours != cams.end()) {
                api_ptr->timelapse().delete_webcam(
                    *ours, [] { spdlog::info("[RemoteScreen] Removed the screen webcam"); },
                    [](const MoonrakerError& err) { log_webcam_error("remove", err); });
            }
        },
        [api_ptr, item, store_uid](const MoonrakerError& err) {
            spdlog::debug("[RemoteScreen] Webcam list unavailable: {}", err.message);
            if (item) {
                post_by_name(*api_ptr, *item, store_uid);
            }
        });
}

void sync_remote_screen_webcam(IMoonrakerAPI& api, MoonrakerLocality locality) {
    if (locality == MoonrakerLocality::Unknown) {
        return;
    }
    auto& service = RemoteScreenService::instance();
    const auto& settings = RemoteScreenSettingsManager::instance();
    const bool remote = locality == MoonrakerLocality::Remote;
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        host[0] = '\0';
    }
    const std::string name = remote_screen_webcam_name(!remote, host);
    const std::string route = remote_screen_effective_route(
        remote, remote_screen_route(writable_path(".remote_screen_route")));
    const auto item = remote_screen_webcam_item(
        name, service.running() && settings.get_webcam(), settings.get_control(), route,
        select_lan_ipv4(local_interface_ips()), service.port());
    // The uid belongs to the printer this reconcile ran against, even if the active
    // printer changes before Moonraker answers.
    const std::string printer = Config::get_instance()->df();
    reconcile_remote_screen_webcam(
        api, item, name, settings.get_webcam_uid(), [printer](const std::string& uid) {
            ui::queue_update("remote_screen::webcam_uid", [printer, uid] {
                RemoteScreenSettingsManager::instance().set_webcam_uid(printer, uid);
            });
        });
}

} // namespace helix
