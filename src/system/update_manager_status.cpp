// SPDX-License-Identifier: GPL-3.0-or-later

#include "update_manager_status.h"

#include "json_utils.h"

#include <cctype>

namespace helix::fwupdate {

namespace {

using helix::json_util::find_member;
using helix::json_util::safe_bool;
using helix::json_util::safe_int;
using helix::json_util::safe_string;

bool remote_known(const std::string& remote) {
    return !remote.empty() && remote != "?";
}

/// Commits the checkout is behind its remote, when the entry says; nullopt when it
/// carries neither count nor list (older Moonraker, or not a git entry).
std::optional<int> commits_behind(const nlohmann::json& info) {
    if (const auto* count = find_member(info, "commits_behind_count");
        count != nullptr && count->is_number_integer()) {
        return count->get<int>();
    }
    if (const auto* list = find_member(info, "commits_behind");
        list != nullptr && list->is_array()) {
        return static_cast<int>(list->size());
    }
    return std::nullopt;
}

ManagedEntry parse_entry(const std::string& name, const nlohmann::json& info) {
    ManagedEntry e;
    e.name = name;
    e.kind = safe_string(info, "configured_type");
    if (e.kind.empty() && name == "system") {
        e.kind = "system";
    }
    e.version = safe_string(info, "version");
    e.remote_version = safe_string(info, "remote_version");
    if (e.kind == "system") {
        e.package_count = safe_int(info, "package_count", 0);
        e.update_available = e.package_count > 0;
        return e;
    }
    e.valid = safe_bool(info, "is_valid", true) && !safe_bool(info, "corrupt", false);
    if (!e.valid) {
        return e;
    }
    if (const auto behind = commits_behind(info)) {
        e.update_available = *behind > 0;
    } else {
        e.update_available = remote_known(e.remote_version) && e.remote_version != e.version;
    }
    return e;
}

} // namespace

std::optional<UpdateStatus> parse_update_status(const nlohmann::json& result) {
    if (!result.is_object()) {
        return std::nullopt;
    }
    const auto* info = find_member(result, "version_info");
    if (info == nullptr || !info->is_object()) {
        return std::nullopt;
    }
    UpdateStatus status;
    status.busy = safe_bool(result, "busy", true);
    for (auto it = info->begin(); it != info->end(); ++it) {
        if (it.key() == kHelixScreenEntry || !it.value().is_object()) {
            continue;
        }
        status.entries.push_back(parse_entry(it.key(), it.value()));
    }
    return status;
}

std::vector<ConfirmPoint> confirm_points(const ManagedEntry& e) {
    if (e.kind == "system") {
        return {ConfirmPoint::UpgradesOsPackages, ConfirmPoint::MayNeedReboot,
                ConfirmPoint::StayOnUntilDone};
    }
    return {ConfirmPoint::ServicesRestart, ConfirmPoint::StayOnUntilDone};
}

std::optional<UpdateResponse> parse_update_response(const nlohmann::json& notification) {
    const nlohmann::json* payload = helix::json_util::notification_payload(notification);
    if (payload == nullptr) {
        return std::nullopt;
    }
    const auto* application = find_member(*payload, "application");
    const auto* complete = find_member(*payload, "complete");
    if (application == nullptr || !application->is_string() || complete == nullptr ||
        !complete->is_boolean()) {
        return std::nullopt;
    }
    UpdateResponse r;
    r.application = application->get<std::string>();
    r.message = safe_string(*payload, "message");
    r.complete = complete->get<bool>();
    return r;
}

std::optional<int> percent_in_message(std::string_view message) {
    std::optional<int> last;
    for (size_t pos = message.find('%'); pos != std::string_view::npos;
         pos = message.find('%', pos + 1)) {
        size_t start = pos;
        while (start > 0 && std::isdigit(static_cast<unsigned char>(message[start - 1]))) {
            --start;
        }
        const size_t digits = pos - start;
        if (digits == 0 || digits > 3) {
            continue;
        }
        int value = 0;
        for (size_t i = start; i < pos; ++i) {
            value = value * 10 + (message[i] - '0');
        }
        if (value <= 100) {
            last = value;
        }
    }
    return last;
}

bool response_reports_failure(const UpdateResponse& response) {
    constexpr std::string_view kError = "error";
    const std::string_view msg = response.message;
    if (msg.size() < kError.size()) {
        return false;
    }
    for (size_t i = 0; i < kError.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(msg[i])) != kError[i]) {
            return false;
        }
    }
    return true;
}

} // namespace helix::fwupdate
