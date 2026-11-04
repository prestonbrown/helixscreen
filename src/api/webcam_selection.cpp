// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "webcam_selection.h"

#include "core/text_io.h"
#include "json_utils.h"

#include <algorithm>
#include <cctype>

namespace helix {

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The path component of a URL (everything before '?' or '#').
std::string url_path(const std::string& url) {
    auto cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}
} // namespace

bool is_usable_snapshot_url(const std::string& snapshot_url) {
    if (snapshot_url.empty())
        return false;
    std::string lower = helix::text_io::to_lower(snapshot_url);
    // Reject HTML pages (e.g. /snapshot.html, /camera.html) — never a JPEG.
    if (ends_with(url_path(lower), ".html"))
        return false;
    // Accept obvious image/snapshot endpoints.
    if (lower.find("action=snapshot") != std::string::npos)
        return true;
    std::string path = url_path(lower);
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg") || ends_with(path, ".png"))
        return true;
    // No HTML marker and no explicit image marker: treat conservatively as usable
    // so atypical-but-real endpoints (bare host, query-only) are not dropped.
    return true;
}

namespace webcam {

bool is_own_screen_webcam(const WebcamInfo& cam) {
    if (cam.screen_server) {
        return true;
    }
    // Ours, or another screen's on the same Moonraker ("HelixScreen Screen (host)").
    const std::string own = kRemoteScreenWebcamName;
    return cam.name == own || cam.name.rfind(own + " (", 0) == 0;
}

WebcamInfo parse_webcam_entry(const nlohmann::json& entry) {
    WebcamInfo cam;
    if (!entry.is_object())
        return cam;
    cam.name = json_util::safe_string(entry, "name", "");
    cam.service = json_util::safe_string(entry, "service", "");
    cam.snapshot_url = json_util::safe_string(entry, "snapshot_url", "");
    cam.stream_url = json_util::safe_string(entry, "stream_url", "");
    cam.uid = json_util::safe_string(entry, "uid", "");
    cam.enabled = json_util::safe_bool(entry, "enabled", true);
    cam.flip_horizontal = json_util::safe_bool(entry, "flip_horizontal", false);
    cam.flip_vertical = json_util::safe_bool(entry, "flip_vertical", false);
    cam.target_fps = json_util::safe_int(entry, "target_fps", 15);
    const auto extra = entry.find("extra_data");
    cam.screen_server = extra != entry.end() && extra->is_object() &&
                        json_util::safe_bool(*extra, kRemoteScreenWebcamMarker, false);
    return cam;
}

bool is_mjpeg_service(const std::string& service) {
    if (service.empty())
        return false;
    return service.find("mjpeg") != std::string::npos ||
           service.find("ustreamer") != std::string::npos;
}

bool is_usable(const WebcamInfo& cam) {
    if (!cam.enabled || !cam.unavailable_reason.empty())
        return false;
    if (is_mjpeg_service(cam.service) && !cam.stream_url.empty())
        return true;
    return is_usable_snapshot_url(cam.snapshot_url);
}

WebcamInfo feed_for(const WebcamInfo& cam) {
    WebcamInfo feed = cam;
    if (!is_mjpeg_service(feed.service)) {
        feed.stream_url.clear();
    }
    if (!is_usable_snapshot_url(feed.snapshot_url)) {
        feed.snapshot_url.clear();
    }
    return feed;
}

std::optional<size_t> auto_pick_index(const std::vector<WebcamInfo>& cams) {
    // First pass: an MJPEG entry gives a real live stream.
    for (size_t i = 0; i < cams.size(); ++i) {
        const auto& cam = cams[i];
        if (cam.enabled && cam.unavailable_reason.empty() && is_mjpeg_service(cam.service) &&
            !cam.stream_url.empty()) {
            return i;
        }
    }
    // Second pass: anything with a snapshot image endpoint, polled.
    for (size_t i = 0; i < cams.size(); ++i) {
        if (is_usable(cams[i])) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<WebcamInfo> auto_pick(const std::vector<WebcamInfo>& cams) {
    auto idx = auto_pick_index(cams);
    if (!idx)
        return std::nullopt;
    return feed_for(cams[*idx]);
}

nlohmann::json view_config_for(const std::string& cam_name,
                               const std::optional<WebcamInfo>& widget_feed,
                               const nlohmann::json& widget_config) {
    if (widget_feed && widget_feed->name == cam_name && widget_config.is_object()) {
        return widget_config;
    }
    return nlohmann::json::object();
}

std::optional<WebcamInfo> photo_pick(const std::vector<WebcamInfo>& cams) {
    const WebcamInfo* first = nullptr;
    for (const auto& cam : cams) {
        if (!cam.enabled || !cam.unavailable_reason.empty() || is_own_screen_webcam(cam) ||
            !is_usable_snapshot_url(cam.snapshot_url)) {
            continue;
        }
        if (is_mjpeg_service(cam.service) && !cam.stream_url.empty()) {
            return cam;
        }
        if (first == nullptr) {
            first = &cam;
        }
    }
    if (first == nullptr) {
        return std::nullopt;
    }
    return *first;
}

namespace {
const WebcamInfo* find_named_usable(const std::vector<WebcamInfo>& cams,
                                    const std::string& source) {
    if (source.empty())
        return nullptr;
    auto it = std::find_if(cams.begin(), cams.end(), [&source](const WebcamInfo& cam) {
        return cam.name == source && is_usable(cam);
    });
    return it == cams.end() ? nullptr : &*it;
}
} // namespace

std::optional<WebcamInfo> select_webcam(const std::vector<WebcamInfo>& cams,
                                        const std::string& source) {
    if (const auto* named = find_named_usable(cams, source)) {
        return feed_for(*named);
    }
    return auto_pick(cams);
}

bool source_is_honored(const std::vector<WebcamInfo>& cams, const std::string& source) {
    return find_named_usable(cams, source) != nullptr;
}

} // namespace webcam
} // namespace helix
