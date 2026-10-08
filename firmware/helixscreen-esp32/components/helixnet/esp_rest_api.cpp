// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// esp_rest_api.cpp — the ESP32 concrete implementation of MoonrakerRestAPI and
// MoonrakerFileTransferAPI (Task 10), replacing the two excluded desktop TUs
// (src/api/moonraker_rest_api.cpp, src/api/moonraker_file_transfer_api.cpp —
// both libhv/hv::requests-based, see app_srcs.txt "EXCLUDED, and why") the
// same way esp_moonraker_client.cpp replaces moonraker_client.cpp for the
// WebSocket transport. Their ctor/dtor/virtuals used to live as link stubs in
// helixapp/task10_pending_stubs.cpp; this file is their real home now.
//
// R1 enumeration (esp32p4-task-10-report.md has the full call-site ledger):
// of the ten ITransfersAPI/IRestAPI methods, exactly ONE is reachable from the
// v1 print-select thumbnail/metadata surface AND fits the R3 no-materialization
// constraint:
//
//   ITransfersAPI::download_file_partial — REAL (below). Used today by
//   ui_panel_print_select.cpp's gcode-header thumbnail-extraction fallback
//   (100KB Range fetch) and is exactly the "download_file_partial-style
//   in-memory thumbnail fetch" the Task 10 plan entry describes — Task 11
//   ("new design" print-select) reuses this same primitive directly for real
//   thumbnail bytes (root="gcodes", path=the resolved .thumbs/ path, capped
//   at HARD_CAP_BYTES), it just isn't wired to a UI yet.
//
// ITransfersAPI::download_file — REAL, capped at WHOLE_FILE_CAP_BYTES and
// erroring above it: small config files such as AFC.cfg, read into memory.
//
// ITransfersAPI::get_file_metadata / metascan_file are IFilesAPI (NOT this
// file) and are already real: they're pure JSON-RPC over the WebSocket
// transport (src/api/moonraker_file_api.cpp, kept unmodified in
// app_srcs.txt), no HTTP involved.
//
// Plan 4 Task 15 R2 adds one more real method: IRestAPI::call_rest_get,
// needed by the ACE AMS backend's REST poll (behind
// CONFIG_HELIX_AMS_HTTP_POLL_BACKENDS, default n — see ams_backend.cpp's
// http_poll_ams_backends_supported() gate). It rides the same EspHttpLane as
// download_file_partial below. Unconditionally real (not itself
// Kconfig-gated): the only reachable caller on ESP32 is the ACE backend,
// which is only ever constructed when that Kconfig is on, so leaving this
// implementation unconditional changes nothing observable when it's off.
//
// Everything else here is an asserting stub (log + ErrorCallback, matching
// task10_pending_stubs.cpp's existing non-fatal pattern — never abort/reset
// the board on a stray user action):
//
//   ITransfersAPI::download_thumbnail — its CONTRACT implies a file path
//     (takes a cache_path, returns a local path via StringCallback so the
//     caller can lv_image_set_src() it — see moonraker_file_transfer_api.h's
//     doc comment). That's exactly R3's "if an interface method's contract
//     implies a file path, it gets an asserting stub instead" carve-out.
//     Task 11's new PSRAM-thumbnail design calls download_file_partial
//     directly instead of going through this file-path-shaped method.
//   ITransfersAPI::download_file_to_path, upload_file,
//     upload_file_with_name, upload_file_from_path — not reachable from the
//     v1 print-select thumbnail/metadata surface (used elsewhere: macro
//     editor, AD5X polling — itself real again as of Task 15 via
//     download_file_partial, see ams_backend_ad5x_ifs.cpp — timelapse,
//     klipper.conf editor, print-start prep — all still out of scope), and
//     download_file_to_path specifically is HARD-BANNED by R3 regardless
//     (file materialization).
//   IRestAPI::call_rest_post/wled_*/get_server_config — no print-select call
//     site; WLED is excluded from v1; call_rest_post's only AMS caller
//     (Snapmaker's optional filament_detect/set action) already degrades
//     gracefully on failure and stays out of Task 15's scope (that flag only
//     covers the two HTTP-*polling* backends, ACE and AD5X IFS).

#include "esp_http_lane.h"
#include "esp_log.h"
#include "http_request_epoch.h"
#include "moonraker_file_transfer_api.h"
#include "moonraker_rest_api.h"
#include "moonraker_validation.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {
constexpr char TAG[] = "esp_rest_api";

// Percent-encodes a Moonraker file path the same way desktop's HUrl::escape(
// path, "/.-_") does: alnum and "/.-_" pass through unescaped, everything else
// becomes %XX. libhv's HUrl isn't part of the ESP32 build, so this is a small
// standalone equivalent (no ESP-IDF/libhv dependency, just <cctype>/<cstdio>).
std::string esp_url_escape_path(const std::string& path) {
    static constexpr char UNRESERVED[] = "/.-_";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        if (std::isalnum(c) || std::strchr(UNRESERVED, static_cast<char>(c)) != nullptr) {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Non-fatal: log + surface failure through the API's ErrorCallback. Mirrors
// task10_pending_stubs.cpp's task10_unimplemented_err — a stray user action
// (e.g. Snapmaker's optional call_rest_post action) must fail gracefully, not
// abort/reset the board.
void esp_rest_unimplemented_err(const char* sym,
                                const std::function<void(const MoonrakerError&)>& err) {
    ESP_LOGE(TAG, "esp_rest_api stub: %s", sym);
    if (err) {
        MoonrakerError e;
        e.type = MoonrakerErrorType::VALIDATION_ERROR;
        e.method = sym;
        err(e);
    }
}

// Task 15 R2: validation for call_rest_get's endpoint, mirroring desktop's
// is_safe_endpoint (src/api/moonraker_rest_api.cpp) — rejects directory
// traversal and CRLF/NUL injection. Deliberately NOT moonraker_internal::is_safe_path:
// REST endpoints are absolute paths ("/server/ace/info"), which
// is_safe_path rejects (it's shaped for Moonraker file-root paths).
bool esp_is_safe_endpoint(const std::string& endpoint) {
    if (endpoint.empty()) {
        return false;
    }
    if (endpoint.find("..") != std::string::npos) {
        return false;
    }
    for (char c : endpoint) {
        if (c == '\n' || c == '\r' || c == '\0') {
            return false;
        }
    }
    return true;
}

// Generous enough for ACE's /server/ace/{info,status,slots} JSON bodies
// (a handful of scalar fields + a small slots array) without being wasteful
// of the lane's PSRAM accumulation buffer. EspHttpLane fails loud rather than
// silently truncating an over-cap response (see esp_http_lane.cpp).
constexpr size_t REST_GET_CAP_BYTES = 16 * 1024;
constexpr size_t WHOLE_FILE_CAP_BYTES = 64 * 1024;
} // namespace

// ============================================================================
// MoonrakerFileTransferAPI
// ============================================================================

MoonrakerFileTransferAPI::MoonrakerFileTransferAPI(helix::IMoonrakerClient& client,
                                                   const std::string& http_base_url)
    : client_(client), http_base_url_(http_base_url) {}

MoonrakerFileTransferAPI::~MoonrakerFileTransferAPI() = default;

void MoonrakerFileTransferAPI::download_file_partial(const std::string& root,
                                                     const std::string& path, size_t max_bytes,
                                                     StringCallback on_success,
                                                     ErrorCallback on_error, CancelFlag cancelled) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, "download_file_partial");
    if (moonraker_internal::reject_invalid_path(path, "download_file_partial", on_error))
        return;
    if (moonraker_internal::reject_invalid_file_root(root, "download_file_partial", on_error))
        return;

    if (http_base_url_.empty()) {
        moonraker_internal::report_error(on_error, MoonrakerErrorType::CONNECTION_LOST,
                                         "download_file_partial", "HTTP base URL not configured");
        return;
    }

    std::string url = http_base_url_ + "/server/files/" + root + "/" + esp_url_escape_path(path);
    ESP_LOGD(TAG, "download_file_partial: %s (cap request %u)", url.c_str(), (unsigned)max_bytes);

    bool queued = helix::http::EspHttpLane::instance().submit_get(
        url, max_bytes,
        [on_success](std::string& body) {
            if (on_success) {
                on_success(body);
            }
        },
        [on_error](const std::string& message) {
            moonraker_internal::report_error(on_error, MoonrakerErrorType::UNKNOWN,
                                             "download_file_partial", message);
        },
        std::move(cancelled));

    if (!queued) {
        moonraker_internal::report_error(on_error, MoonrakerErrorType::QUEUE_FULL,
                                         "download_file_partial",
                                         "HTTP request could not be queued — try again");
    }
}

// --- Asserting stubs: not reachable from the v1 print-select surface (see
// file header). download_file_to_path is additionally hard-banned by R3
// regardless of reachability: its contract IS file materialization. ---

// Whole-file reads into memory: config files (AFC.cfg and friends), which are a
// few KB. The lane asks for a bounded prefix, so one byte past the cap is
// requested and receiving it means the file is too big: that is an error, never
// a truncated file handed back as the whole thing.
void MoonrakerFileTransferAPI::download_file(const std::string& root, const std::string& path,
                                             StringCallback on_success, ErrorCallback on_error) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, "download_file");
    if (moonraker_internal::reject_invalid_path(path, "download_file", on_error))
        return;
    if (moonraker_internal::reject_invalid_file_root(root, "download_file", on_error))
        return;
    if (http_base_url_.empty()) {
        moonraker_internal::report_error(on_error, MoonrakerErrorType::CONNECTION_LOST,
                                         "download_file", "HTTP base URL not configured");
        return;
    }

    std::string url = http_base_url_ + "/server/files/" + root + "/" + esp_url_escape_path(path);
    bool queued = helix::http::EspHttpLane::instance().submit_get(
        url, WHOLE_FILE_CAP_BYTES + 1,
        [on_success, on_error](std::string& body) {
            if (body.size() > WHOLE_FILE_CAP_BYTES) {
                moonraker_internal::report_error(
                    on_error, MoonrakerErrorType::UNKNOWN, "download_file",
                    "file exceeds the " + std::to_string(WHOLE_FILE_CAP_BYTES) +
                        "-byte in-memory cap");
                return;
            }
            if (on_success) {
                on_success(body);
            }
        },
        [on_error](const std::string& message) {
            moonraker_internal::report_error(on_error, MoonrakerErrorType::UNKNOWN, "download_file",
                                             message);
        });
    if (!queued) {
        moonraker_internal::report_error(on_error, MoonrakerErrorType::QUEUE_FULL, "download_file",
                                         "HTTP request could not be queued — try again");
    }
}

// The head-range sibling above is real because EspHttpLane::submit_get() asks
// for a bounded prefix ("bytes=0-N"). A tail needs a suffix range ("bytes=-N"),
// which the lane does not express, and nothing on this surface reads slicer
// footers -- so this stays a stub rather than growing the lane an option with
// no caller. Defining it is not optional even so: it is a non-pure virtual, so
// MoonrakerFileTransferAPI's vtable references it and the link fails without a
// definition.
void MoonrakerFileTransferAPI::download_file_tail(const std::string&, const std::string&, size_t,
                                                  StringCallback, ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerFileTransferAPI::download_file_tail", on_error);
}

void MoonrakerFileTransferAPI::download_file_to_path(const std::string&, const std::string&,
                                                     const std::string&, StringCallback,
                                                     ErrorCallback on_error, ProgressCallback) {
    esp_rest_unimplemented_err("MoonrakerFileTransferAPI::download_file_to_path", on_error);
}

void MoonrakerFileTransferAPI::download_thumbnail(const std::string&, const std::string&,
                                                  StringCallback, ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerFileTransferAPI::download_thumbnail", on_error);
}

void MoonrakerFileTransferAPI::upload_file(const std::string&, const std::string&,
                                           const std::string&, SuccessCallback,
                                           ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerFileTransferAPI::upload_file", on_error);
}

void MoonrakerFileTransferAPI::upload_file_with_name(const std::string&, const std::string&,
                                                     const std::string&, const std::string&,
                                                     SuccessCallback, ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerFileTransferAPI::upload_file_with_name", on_error);
}

void MoonrakerFileTransferAPI::upload_file_from_path(const std::string&, const std::string&,
                                                     const std::string&, SuccessCallback,
                                                     ErrorCallback on_error, ProgressCallback) {
    esp_rest_unimplemented_err("MoonrakerFileTransferAPI::upload_file_from_path", on_error);
}

// ============================================================================
// MoonrakerRestAPI — call_rest_get is real (Task 15 R2, see file header);
// every other method has no v1 print-select call site and stays a stub.
// ============================================================================

MoonrakerRestAPI::MoonrakerRestAPI(helix::IMoonrakerClient& client,
                                   const std::string& http_base_url)
    : client_(client), http_base_url_(http_base_url) {}

MoonrakerRestAPI::~MoonrakerRestAPI() = default;

void MoonrakerRestAPI::call_rest_get(const std::string& endpoint, RestCallback on_complete) {
    if (!esp_is_safe_endpoint(endpoint)) {
        ESP_LOGE(TAG, "call_rest_get: invalid endpoint '%s'", endpoint.c_str());
        if (on_complete) {
            RestResponse resp;
            resp.success = false;
            resp.error = "Invalid endpoint - contains unsafe characters";
            on_complete(resp);
        }
        return;
    }

    if (http_base_url_.empty()) {
        ESP_LOGE(TAG, "call_rest_get: HTTP base URL not configured");
        if (on_complete) {
            RestResponse resp;
            resp.success = false;
            resp.error = "HTTP base URL not configured";
            on_complete(resp);
        }
        return;
    }

    std::string url = http_base_url_;
    if (!endpoint.empty() && endpoint[0] != '/') {
        url += "/";
    }
    url += endpoint;

    ESP_LOGD(TAG, "call_rest_get: %s", url.c_str());

    bool queued = helix::http::EspHttpLane::instance().submit_get(
        url, REST_GET_CAP_BYTES,
        [on_complete](std::string& body) {
            RestResponse resp;
            resp.success = true;
            resp.status_code = 200; // the lane only succeeds on HTTP 200/206
            if (!body.empty()) {
                resp.data = nlohmann::json::parse(body, nullptr, false);
                if (resp.data.is_discarded()) {
                    resp.data = nlohmann::json::object();
                    resp.data["_raw_body"] = std::move(body);
                }
            }
            if (on_complete) {
                on_complete(resp);
            }
        },
        [on_complete](const std::string& message) {
            RestResponse resp;
            resp.success = false;
            resp.error = message;
            if (on_complete) {
                on_complete(resp);
            }
        });

    if (!queued && on_complete) {
        RestResponse resp;
        resp.success = false;
        resp.error = "HTTP request could not be queued — try again";
        on_complete(resp);
    }
}

void MoonrakerRestAPI::call_rest_post(const std::string&, const json&, RestCallback on_complete) {
    ESP_LOGE(TAG, "esp_rest_api stub: MoonrakerRestAPI::call_rest_post");
    if (on_complete) {
        RestResponse resp;
        resp.success = false;
        resp.error = "not implemented on this platform";
        on_complete(resp);
    }
}

// No WLED on this platform: report an empty strip map, so LED discovery settles
// without an error on every connect.
void MoonrakerRestAPI::wled_get_strips(RestCallback on_success, ErrorCallback) {
    if (on_success) {
        RestResponse resp;
        resp.success = true;
        resp.status_code = 200;
        resp.data = json{{"result", {{"strips", json::object()}}}};
        on_success(resp);
    }
}

void MoonrakerRestAPI::wled_set_strip(const std::string&, const std::string&, int, int,
                                      SuccessCallback, ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerRestAPI::wled_set_strip", on_error);
}

void MoonrakerRestAPI::wled_get_status(RestCallback, ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerRestAPI::wled_get_status", on_error);
}

void MoonrakerRestAPI::get_server_config(RestCallback, ErrorCallback on_error) {
    esp_rest_unimplemented_err("MoonrakerRestAPI::get_server_config", on_error);
}
