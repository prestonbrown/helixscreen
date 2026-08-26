// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "webcam_service_health.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace helix::webcam {

namespace {

/// Fold a Moonraker `service` identifier or a systemd unit name down to a bare
/// comparison key: lowercase, ".service" suffix dropped, separators removed.
/// "camera-streamer.service", "camera_streamer" and "CameraStreamer" all become
/// "camerastreamer", so the table below does not have to spell every variant.
std::string normalize(std::string_view s) {
    // Drop the systemd unit-type suffix first, so "crowsnest.service" and
    // "crowsnest" fold together without also eating the tail of an identifier
    // that genuinely ends in those letters.
    static constexpr std::string_view kUnitSuffix = ".service";
    if (s.size() > kUnitSuffix.size() &&
        s.compare(s.size() - kUnitSuffix.size(), kUnitSuffix.size(), kUnitSuffix) == 0) {
        s.remove_suffix(kUnitSuffix.size());
    }
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '-' || c == '_' || c == '.' || c == ' ')
            continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// THE camera-vendor table. Keys are normalized Moonraker webcam `service`
/// identifiers; values are the normalized systemd units that can serve them, in
/// preference order (most specific process first, the wrapper that supervises
/// it last). crowsnest is a supervisor around ustreamer/camera-streamer, which
/// is why it trails those two rather than replacing them.
struct ServiceUnits {
    std::string_view service;
    std::array<std::string_view, 4> units;
};

constexpr std::array<ServiceUnits, 9> kServiceUnits{{
    {"mjpegstreamer", {"ustreamer", "camerastreamer", "mjpgstreamer", "crowsnest"}},
    {"mjpegstreameradaptive", {"ustreamer", "camerastreamer", "mjpgstreamer", "crowsnest"}},
    {"ustreamer", {"ustreamer", "crowsnest", "", ""}},
    {"camerastreamer", {"camerastreamer", "crowsnest", "", ""}},
    {"uv4lmjpeg", {"uv4l", "", "", ""}},
    {"webrtccamerastreamer", {"camerastreamer", "crowsnest", "", ""}},
    {"webrtcgo2rtc", {"go2rtc", "crowsnest", "", ""}},
    {"webrtcmediamtx", {"mediamtx", "", "", ""}},
    {"hlsstream", {"mediamtx", "go2rtc", "", ""}},
}};

/// Candidate units for a webcam `service` identifier, normalized, in preference
/// order. Always ends with the identifier itself so a streamer we have never
/// heard of still matches a same-named unit.
std::vector<std::string> candidate_units(std::string_view service) {
    std::vector<std::string> out;
    const std::string key = normalize(service);
    if (key.empty())
        return out;
    for (const auto& row : kServiceUnits) {
        if (row.service != key)
            continue;
        for (std::string_view u : row.units) {
            if (!u.empty())
                out.emplace_back(u);
        }
        break;
    }
    if (std::find(out.begin(), out.end(), key) == out.end())
        out.push_back(key);
    return out;
}

enum class Health { Unknown, Up, Down };

/// A string member of a JSON object, or "" when absent or not a string.
/// Moonraker has shipped nulls here on hosts where systemd is unavailable, and
/// json::value() throws on a type mismatch rather than falling back.
std::string string_field(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object())
        return {};
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Classify one service_state entry. Only the terminal systemd states count as
/// Down; "activating", "reloading" and systemd's own "unknown" are transient or
/// uninformative and must not hide a camera.
Health health_of(const nlohmann::json& entry) {
    if (!entry.is_object())
        return Health::Unknown;
    const std::string active = lower(string_field(entry, "active_state"));
    const std::string sub = lower(string_field(entry, "sub_state"));
    if (active.empty() && sub.empty())
        return Health::Unknown;
    if (active == "failed" || active == "inactive" || sub == "failed" || sub == "dead")
        return Health::Down;
    if (active == "unknown" && (sub.empty() || sub == "unknown"))
        return Health::Unknown;
    return Health::Up;
}

/// Locate a candidate unit in service_state, tolerating "crowsnest" vs
/// "crowsnest.service" vs "camera-streamer" key spellings.
const nlohmann::json* find_unit(const nlohmann::json& service_state, const std::string& candidate,
                                std::string* out_key) {
    if (!service_state.is_object())
        return nullptr;
    for (auto it = service_state.begin(); it != service_state.end(); ++it) {
        if (normalize(it.key()) != candidate)
            continue;
        if (out_key)
            *out_key = it.key();
        return &it.value();
    }
    return nullptr;
}

struct Match {
    std::string key; ///< key as spelled in service_state ("" when unmatched)
    Health health = Health::Unknown;
    std::string detail; ///< "<active>/<sub>" for logging
    bool any_present = false;
};

/// Resolve the webcam entry to a unit in service_state. Scans candidates in
/// preference order and stops on the first that is NOT Down, so a printer
/// running ustreamer directly is not condemned by a failed crowsnest sitting
/// beside it. Only if every present candidate is Down does the result say Down.
Match resolve(const nlohmann::json& webcam_entry, const nlohmann::json& service_state) {
    Match result;
    if (!webcam_entry.is_object() || !service_state.is_object() || service_state.empty())
        return result;
    const std::string service = string_field(webcam_entry, "service");
    for (const auto& candidate : candidate_units(service)) {
        std::string key;
        const nlohmann::json* entry = find_unit(service_state, candidate, &key);
        if (!entry)
            continue;
        const Health h = health_of(*entry);
        if (!result.any_present || h != Health::Down) {
            result.key = key;
            result.health = h;
            const std::string active = string_field(*entry, "active_state");
            const std::string sub = string_field(*entry, "sub_state");
            result.detail =
                (active.empty() ? "unknown" : active) + "/" + (sub.empty() ? "unknown" : sub);
        }
        result.any_present = true;
        if (h != Health::Down)
            break;
    }
    return result;
}

} // namespace

nlohmann::json extract_service_state(const nlohmann::json& system_info_response) {
    if (!system_info_response.is_object())
        return nlohmann::json::object();
    const nlohmann::json* node = &system_info_response;
    if (node->contains("result") && (*node)["result"].is_object())
        node = &(*node)["result"];
    if (node->contains("system_info") && (*node)["system_info"].is_object())
        node = &(*node)["system_info"];
    if (node->contains("service_state") && (*node)["service_state"].is_object())
        return (*node)["service_state"];
    return nlohmann::json::object();
}

std::string owning_service_unit(const nlohmann::json& webcam_entry,
                                const nlohmann::json& service_state) {
    return resolve(webcam_entry, service_state).key;
}

bool owning_service_is_down(const nlohmann::json& webcam_entry, const nlohmann::json& service_state,
                            std::string* out_reason) {
    const Match m = resolve(webcam_entry, service_state);
    if (!m.any_present || m.health != Health::Down)
        return false;
    if (out_reason)
        *out_reason = m.key + " (" + m.detail + ")";
    return true;
}

} // namespace helix::webcam
