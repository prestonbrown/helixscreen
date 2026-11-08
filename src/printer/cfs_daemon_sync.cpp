// SPDX-License-Identifier: GPL-3.0-or-later
#include "cfs_daemon_sync.h"

#include "ams_types.h"
#include "cfs_status_parse.h"
#include "core/http_executor.h"
#include "filament_catalog.h"
#include "filament_op_dispatch.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

#include "hv/json.hpp"

#if !defined(HELIX_PLATFORM_ESP32)
#include "hv/base64.h"
#include "hv/hsocket.h"
#include "hv/wsdef.h"

#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#endif

namespace helix::cfs {

namespace {
constexpr int kBaysPerUnit = 4;
constexpr int kUnitCount = 4;
} // namespace

std::optional<DaemonAddress> daemon_address(int global_index, int external_slot) {
    if (global_index == external_slot) {
        return DaemonAddress{0, 0};
    }
    if (global_index < 0 || global_index >= kBaysPerUnit * kUnitCount) {
        return std::nullopt;
    }
    return DaemonAddress{global_index / kBaysPerUnit + 1, global_index % kBaysPerUnit};
}

std::string daemon_color(uint32_t color_rgb) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#0%06x", color_rgb & 0xFFFFFFu);
    return buf;
}

std::string encode_modify_material(const DaemonMaterial& m) {
    nlohmann::json record = {
        {"boxId", m.box_id},
        {"id", m.slot_id},
        {"rfid", m.rfid},
        {"type", m.type},
        {"vendor", m.vendor},
        {"name", m.name},
        {"color", daemon_color(m.color_rgb)},
        {"minTemp", static_cast<double>(m.min_temp_c)},
        {"maxTemp", static_cast<double>(m.max_temp_c)},
        {"pressure", kDaemonDefaultPressure},
    };
    nlohmann::json frame = {{"method", "set"}, {"params", {{"modifyMaterial", record}}}};
    // Replacement, not a throw, on a malformed UTF-8 label: this build has no
    // exceptions to catch one with.
    return frame.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// The daemon's record for a bay just written, from what the write carried and
// the slot as it now stands. rfid is the 5-char catalog code of the material
// written ("" for a color-only write or a code the catalog does not know, as
// Creality Print sends for an untagged spool). Temps and the product label prefer the slot's own
// values and fall back to the catalog product of the code written, or of the user's pick.
std::optional<DaemonMaterial> daemon_material_for(int global_index, const SlotInfo* slot,
                                                  const std::string& material,
                                                  const std::string& brand,
                                                  const std::string& catalog_id,
                                                  const std::string& mat_code, uint32_t color_rgb) {
    const auto address = daemon_address(global_index, helix::ui::kExternalSpoolSlot);
    if (!address) {
        return std::nullopt;
    }
    const auto catalog = helix::printer::FilamentCatalog::load_codes_cached("cfs");
    // Only a code the catalog knows is a material id; anything else the
    // firmware reported (a raw tag UID) would name nothing to the daemon.
    const std::string code = helix::printer::CfsMaterialDb::strip_code(mat_code);
    const helix::printer::EffectiveFilament* coded =
        code.empty() ? nullptr : catalog->resolve_code("cfs", code);
    const helix::printer::EffectiveFilament* entry =
        coded ? coded : (catalog_id.empty() ? nullptr : catalog->resolve_id(catalog_id));
    const std::string usable_brand = is_raw_tag_uid(brand) ? std::string{} : brand;

    DaemonMaterial record;
    record.box_id = address->box_id;
    record.slot_id = address->slot_id;
    record.rfid = coded ? code : std::string{};
    record.color_rgb = color_rgb;
    record.type = !material.empty() ? material : (entry ? entry->type : std::string{});
    record.vendor = !usable_brand.empty() ? usable_brand : (entry ? entry->brand : std::string{});
    if (slot && !slot->product_name.empty()) {
        record.name = slot->product_name;
    } else if (entry && !entry->name.empty()) {
        record.name = entry->name;
    } else {
        record.name = record.type;
    }
    const bool slot_temps = slot && slot->nozzle_temp_min > 0 && slot->nozzle_temp_max > 0;
    record.min_temp_c = slot_temps ? slot->nozzle_temp_min : (entry ? entry->nozzle_min : 0);
    record.max_temp_c = slot_temps ? slot->nozzle_temp_max : (entry ? entry->nozzle_max : 0);
    return record;
}

const char* to_string(DaemonSendResult result) {
    switch (result) {
    case DaemonSendResult::Delivered:
        return "delivered";
    case DaemonSendResult::ConnectFailed:
        return "nothing listening";
    case DaemonSendResult::HandshakeFailed:
        return "handshake refused";
    case DaemonSendResult::SendFailed:
        return "send failed";
    case DaemonSendResult::TimedOut:
        return "timed out";
    case DaemonSendResult::Unsupported:
        return "no transport in this build";
    }
    return "unknown";
}

#if defined(HELIX_PLATFORM_ESP32)

DaemonSendResult send_daemon_frame(const DaemonEndpoint&, const std::string&) {
    return DaemonSendResult::Unsupported;
}

#else

namespace {

using Clock = std::chrono::steady_clock;

int remaining_ms(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

/// Write all of @p data, waiting for the socket at most until @p deadline.
bool send_all(int fd, const char* data, size_t len, Clock::time_point deadline) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL; // a peer that hung up must not raise SIGPIPE
#else
    constexpr int kFlags = 0;
#endif
    while (len > 0) {
        pollfd pfd{fd, POLLOUT, 0};
        const int wait = remaining_ms(deadline);
        if (wait == 0 || ::poll(&pfd, 1, wait) <= 0) {
            return false;
        }
        const ssize_t n = ::send(fd, data, len, kFlags);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

/// Read until the end of the HTTP response head. False on timeout or close.
bool read_response_head(int fd, std::string& head, Clock::time_point deadline) {
    char buf[512];
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > 8192) {
            return true; // oversized head: let the status-line check decide
        }
        pollfd pfd{fd, POLLIN, 0};
        const int wait = remaining_ms(deadline);
        if (wait == 0 || ::poll(&pfd, 1, wait) <= 0) {
            return false;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        head.append(buf, static_cast<size_t>(n));
    }
    return true;
}

std::string make_client_key() {
    unsigned char raw[16];
    for (auto& b : raw) {
        b = static_cast<unsigned char>(std::rand() & 0xFF);
    }
    char encoded[32] = {0};
    hv_base64_encode(raw, sizeof(raw), encoded);
    return encoded;
}

bool build_client_frame(std::vector<char>& out, const char* data, int len, ws_opcode opcode) {
    out.resize(static_cast<size_t>(ws_calc_frame_size(len, true)));
    const int n = ws_client_build_frame(out.data(), data, len, opcode, true);
    if (n <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(n));
    return true;
}

} // namespace

DaemonSendResult send_daemon_frame(const DaemonEndpoint& endpoint, const std::string& payload) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(endpoint.total_budget_ms);

    const int connect_ms = std::min(endpoint.connect_timeout_ms, remaining_ms(deadline));
    if (connect_ms <= 0) {
        return DaemonSendResult::TimedOut;
    }
    const int fd = ConnectTimeout(endpoint.host.c_str(), endpoint.port, connect_ms);
    if (fd < 0) {
        return DaemonSendResult::ConnectFailed;
    }
    struct FdCloser {
        int fd;
        ~FdCloser() {
            ::close(fd);
        }
    } closer{fd};

    std::string request = "GET / HTTP/1.1\r\nHost: " + endpoint.host + ":" +
                          std::to_string(endpoint.port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                          make_client_key() + "\r\n\r\n";
    if (!send_all(fd, request.data(), request.size(), deadline)) {
        return remaining_ms(deadline) == 0 ? DaemonSendResult::TimedOut
                                           : DaemonSendResult::SendFailed;
    }

    std::string head;
    if (!read_response_head(fd, head, deadline)) {
        return remaining_ms(deadline) == 0 ? DaemonSendResult::TimedOut
                                           : DaemonSendResult::HandshakeFailed;
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
        return DaemonSendResult::HandshakeFailed;
    }

    std::vector<char> frame;
    if (!build_client_frame(frame, payload.data(), static_cast<int>(payload.size()),
                            WS_OPCODE_TEXT) ||
        !send_all(fd, frame.data(), frame.size(), deadline)) {
        return DaemonSendResult::SendFailed;
    }

    // A close frame, then wait for the peer to hang up. Closing with unread
    // data from the peer would send a reset, which can discard the frame
    // before the daemon reads it.
    std::vector<char> close_frame;
    if (build_client_frame(close_frame, nullptr, 0, WS_OPCODE_CLOSE)) {
        (void)send_all(fd, close_frame.data(), close_frame.size(), deadline);
    }
    ::shutdown(fd, SHUT_WR);
    char sink[512];
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        const int wait = remaining_ms(deadline);
        if (wait == 0 || ::poll(&pfd, 1, wait) <= 0 || ::recv(fd, sink, sizeof(sink), 0) <= 0) {
            break;
        }
    }
    return DaemonSendResult::Delivered;
}

#endif // HELIX_PLATFORM_ESP32

void post_daemon_frame(const DaemonEndpoint& endpoint, std::string payload) {
    // The work item owns copies of everything it reads, so it outlives any
    // backend that queued it.
    (void)helix::http::HttpExecutor::fast().submit([endpoint, payload = std::move(payload)]() {
        const auto started = std::chrono::steady_clock::now();
        const auto result = send_daemon_frame(endpoint, payload);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        spdlog::debug("[AMS CFS] daemon slot notice to {}:{}: {} ({} ms)", endpoint.host,
                      endpoint.port, to_string(result), ms);
    });
}

} // namespace helix::cfs
