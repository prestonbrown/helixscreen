// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_update_check.h"

#include "app_globals.h"
#include "core/http_executor.h"
#include "core/notification_api.h"
#include "core/update_queue.h"
#include "helix_version.h"
#include "hv/requests.h"
#include "json_utils.h"
#include "lvgl.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <mutex>

#include "hv/json.hpp"

namespace helix::fwupdate {

namespace {

using json = nlohmann::json;

constexpr int kFetchTimeoutSeconds = 20;
constexpr int kMaxBackoffTicks = 6;

/// The value at JSON pointer @p ptr in @p root, or nullptr. Walks objects and
/// array indices by hand: no exceptions on a missing or malformed path.
const json* at_pointer(const json& root, const char* ptr) {
    if (ptr == nullptr || *ptr != '/') {
        return nullptr;
    }
    const json* node = &root;
    std::string_view rest(ptr + 1);
    while (node != nullptr) {
        const size_t slash = rest.find('/');
        const std::string key(rest.substr(0, slash));
        if (node->is_object()) {
            node = helix::json_util::find_member(*node, key.c_str());
        } else if (node->is_array() && !key.empty() && key.size() < 6 &&
                   std::all_of(key.begin(), key.end(),
                               [](char c) { return c >= '0' && c <= '9'; })) {
            size_t index = 0;
            for (char c : key) {
                index = index * 10 + static_cast<size_t>(c - '0');
            }
            if (index >= node->size()) {
                return nullptr;
            }
            node = &(*node)[index];
        } else {
            return nullptr;
        }
        if (slash == std::string_view::npos) {
            return node;
        }
        rest = rest.substr(slash + 1);
    }
    return nullptr;
}

std::string string_at(const json& root, const char* ptr) {
    const json* v = at_pointer(root, ptr);
    return v != nullptr ? helix::json_util::as_string(*v, "", /*accept_number=*/true) : "";
}

std::string read_first_line(const char* path) {
    if (path == nullptr) {
        return "";
    }
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

std::pair<int, std::string> http_get(const std::string& url) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = kFetchTimeoutSeconds;
    req->headers["User-Agent"] = HELIX_USER_AGENT;
    auto resp = requests::request(req);
    if (!resp) {
        return {0, ""};
    }
    return {resp->status_code, resp->body};
}

/// Main-thread state of the shared check.
struct Checker {
    CheckState state;
    std::optional<RemoteFirmware> found;
    bool in_flight = false;
};

Checker& checker() {
    static Checker c;
    return c;
}

} // namespace

std::optional<RemoteFirmware> parse_check_response(const CheckSource& source,
                                                   std::string_view body) {
    const json root = json::parse(body.begin(), body.end(), nullptr, false);
    if (root.is_discarded()) {
        return std::nullopt;
    }
    RemoteFirmware fw;
    fw.version = string_at(root, source.version_ptr);
    if (fw.version.empty()) {
        return std::nullopt;
    }
    fw.download_url = string_at(root, source.download_url_ptr);
    fw.sha256 = string_at(root, source.sha256_ptr);
    if (const json* size = at_pointer(root, source.size_ptr);
        size != nullptr && size->is_number_unsigned()) {
        fw.size = size->get<uint64_t>();
    } else if (size != nullptr && size->is_number_integer() && size->get<int64_t>() > 0) {
        fw.size = static_cast<uint64_t>(size->get<int64_t>());
    }
    return fw;
}

bool offers_update(const RemoteFirmware& offered, std::string_view installed) {
    return !installed.empty() && !offered.version.empty() && offered.version != installed;
}

int backoff_ticks(int failures) {
    if (failures <= 1) {
        return 0;
    }
    const int shift = std::min(failures - 1, 3);
    return std::min((1 << shift) - 1, kMaxBackoffTicks);
}

bool should_check(CheckState& state, bool checks_suppressed, bool job_holds_machine,
                  bool has_source) {
    if (checks_suppressed || !has_source) {
        return false;
    }
    ++state.ticks_since_attempt;
    if (job_holds_machine) {
        return false;
    }
    return state.ticks_since_attempt > backoff_ticks(state.consecutive_failures);
}

void record_check(CheckState& state, bool succeeded) {
    state.ticks_since_attempt = 0;
    state.consecutive_failures = succeeded ? 0 : state.consecutive_failures + 1;
}

CheckOutcome run_firmware_check(const CheckSource& source, std::string_view installed,
                                const Fetch& fetch) {
    CheckOutcome out;
    if (source.url == nullptr) {
        return out;
    }
    const auto [status, body] = fetch(source.url);
    if (status < 200 || status >= 300) {
        return out;
    }
    const auto offered = parse_check_response(source, body);
    if (!offered) {
        return out;
    }
    out.succeeded = true;
    if (offers_update(*offered, installed)) {
        out.update = offered;
    }
    return out;
}

void on_update_check_tick() {
    const Provider* p = provider_for(current_host_facts());
    const CheckSource* source = p != nullptr ? check_source(*p) : nullptr;
    Checker& c = checker();
    const bool busy = job_holds_machine(get_printer_state().print_state().get_print_lifecycle());
    if (c.in_flight ||
        !should_check(c.state, update_checks_suppressed(), busy, source != nullptr)) {
        return;
    }
    c.in_flight = true;
    const std::string owner = provider_owner(*p);
    helix::http::HttpExecutor::fast().submit([source, owner]() {
        const std::string installed = read_first_line(source->installed_version_file);
        const CheckOutcome outcome = run_firmware_check(*source, installed, http_get);
        helix::ui::queue_update("fwupdate::remote_check_done", [outcome, owner]() {
            Checker& c = checker();
            c.in_flight = false;
            record_check(c.state, outcome.succeeded);
            if (!outcome.succeeded) {
                spdlog::info("[FirmwareUpdate] Remote firmware check failed ({} in a row)",
                             c.state.consecutive_failures);
                return;
            }
            const bool newly_found =
                outcome.update && (!c.found || c.found->version != outcome.update->version);
            c.found = outcome.update;
            if (newly_found) {
                spdlog::info("[FirmwareUpdate] {} firmware {} is available", owner,
                             outcome.update->version);
                ui_notification_info(
                    lv_tr("Firmware Update"),
                    fmt::format(fmt::runtime(lv_tr("{} firmware {} is available. Open Settings > "
                                                   "Firmware Update to install it.")),
                                owner, outcome.update->version)
                        .c_str());
            }
        });
    });
}

std::optional<RemoteFirmware> remote_update_found() {
    return checker().found;
}

} // namespace helix::fwupdate
