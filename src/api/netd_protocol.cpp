// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "netd_protocol.h"

#include "hv/base64.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace helix::netd {
namespace {

// Whitespace the daemon may pad a value with (and the assembler's line tail).
std::string trim_copy(const std::string& s) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && is_space(s[begin]))
        ++begin;
    while (end > begin && is_space(s[end - 1]))
        --end;
    return s.substr(begin, end - begin);
}

std::string ascii_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Lowercased key of a snapshot line ("MODE=ETHERNET" -> "mode"), or empty
/// when the line carries no '='. Internal: only the snapshot parser needs
/// the key spelling now that query_snapshot() derives authority from the
/// merged mode field itself.
std::string snapshot_line_key(const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
        return {};
    return ascii_lower(trim_copy(line.substr(0, eq)));
}

// Strict full-string integer parse: optional sign, digits only, no whitespace,
// bounded magnitude (frequencies and dBm values are small).
std::optional<int> parse_int(const std::string& s) {
    if (s.empty())
        return std::nullopt;
    const size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (start >= s.size())
        return std::nullopt;
    long value = 0;
    for (size_t i = start; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + (c - '0');
        if (value > 1000000000L)
            return std::nullopt;
    }
    return static_cast<int>(s[0] == '-' ? -value : value);
}

// Base64-decode a wire field with validation (the first-party client's
// validate=True semantics): canonical length, alphabet only, '=' confined to
// the trailing pad. Invalid input decodes to an empty string, never an error.
// hv provides the codec; this only guards what it will accept.
std::string decode_b64_field(const std::string& encoded) {
    const size_t len = encoded.size();
    if (len == 0)
        return {};
    if (len % 4 != 0)
        return {};
    for (size_t i = 0; i < len; ++i) {
        const char c = encoded[i];
        const bool in_alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '+' || c == '/';
        // '=' pads only the final quantum (last one or two positions).
        const bool is_trailing_pad = c == '=' && i + 2 >= len;
        if (!in_alphabet && !is_trailing_pad)
            return {};
    }
    return hv::Base64Decode(encoded.c_str(), static_cast<unsigned int>(len));
}

std::string encode_b64_field(const std::string& raw) {
    // hv_base64_encode dereferences its input even at length 0 (verified
    // SIGSEGV), so the empty-string encoding — which is the empty string —
    // must be answered here.
    if (raw.empty())
        return {};
    return hv::Base64Encode(reinterpret_cast<const unsigned char*>(raw.data()),
                            static_cast<unsigned int>(raw.size()));
}

// A partial line strictly larger than this is garbage (no legitimate netd line
// frames a >256 KiB payload) and is discarded rather than hoarded.
constexpr size_t kMaxPartial = 256 * 1024;

} // namespace

Ack parse_ack(const std::string& line) {
    Ack ack;
    if (line == "OK") {
        ack.kind = Ack::Kind::Ok;
        return ack;
    }
    if (line.rfind("OK ", 0) == 0) {
        ack.kind = Ack::Kind::Ok;
        ack.text = trim_copy(line.substr(3));
        return ack;
    }
    if (line.rfind("ERR ", 0) == 0) {
        ack.kind = Ack::Kind::Err;
        ack.text = trim_copy(line.substr(4));
        return ack;
    }
    return ack;
}

bool parse_snapshot_line(const std::string& line, NetdSnapshot& out) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;

    const std::string key = snapshot_line_key(line);
    const std::string value = trim_copy(line.substr(eq + 1));

    // Known keys only; everything else is a newer daemon's addition and is
    // ignored without disturbing what was already merged.
    if (key == "mode") {
        out.mode = value;
    } else if (key == "state") {
        out.state = value;
    } else if (key == "ssid") {
        out.ssid = decode_b64_field(value);
    } else if (key == "signal") {
        out.signal = value;
    } else if (key == "ip") {
        out.ip = value;
    } else if (key == "reason") {
        out.reason = value;
    } else if (key == "progress") {
        out.progress = value;
    } else if (key == "attempt") {
        out.attempt = value;
    } else {
        return false;
    }
    return true;
}

std::optional<ScanRow> parse_scan_row(const std::string& line) {
    if (line.rfind("FREQUENCY=", 0) != 0)
        return std::nullopt;
    const size_t marker = line.find(" NETWORK=");
    if (marker == std::string::npos)
        return std::nullopt;

    ScanRow row;
    bool have_frequency = false;
    bool have_signal = false;

    // KEY=VALUE tokens occupy the head, space-separated, before the marker.
    const std::string head = line.substr(0, marker);
    size_t pos = 0;
    while (pos < head.size()) {
        const size_t space = head.find(' ', pos);
        const std::string token =
            head.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
        if (!token.empty()) {
            const size_t token_eq = token.find('=');
            if (token_eq != std::string::npos) {
                const std::string token_key = token.substr(0, token_eq);
                const std::string token_value = token.substr(token_eq + 1);
                if (token_key == "FREQUENCY") {
                    if (const auto frequency = parse_int(token_value)) {
                        row.frequency_mhz = *frequency;
                        have_frequency = true;
                    }
                } else if (token_key == "SIGNAL") {
                    if (const auto signal = parse_int(token_value)) {
                        row.signal_dbm = *signal;
                        have_signal = true;
                    }
                } else if (token_key == "SECURITY") {
                    row.security = token_value;
                } else if (token_key == "SAVED") {
                    row.saved = token_value == "1";
                }
                // Unknown tokens are forward compatibility; ignored.
            }
        }
        if (space == std::string::npos)
            break;
        pos = space + 1;
    }
    if (!have_frequency || !have_signal)
        return std::nullopt;

    row.ssid = decode_b64_field(line.substr(marker + 9)); // strlen(" NETWORK=")
    if (row.ssid.empty())
        return std::nullopt;
    row.secured = row.security.find("PSK") != std::string::npos;
    return row;
}

std::vector<std::string> LineAssembler::feed(std::string_view chunk) {
    partial_.append(chunk.data(), chunk.size());

    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        const size_t newline = partial_.find('\n', start);
        if (newline == std::string::npos)
            break;
        std::string line = partial_.substr(start, newline - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
        start = newline + 1;
    }
    if (start > 0)
        partial_.erase(0, start);
    if (partial_.size() > kMaxPartial)
        partial_.clear();
    return lines;
}

void LineAssembler::reset() {
    partial_.clear();
}

std::string encode_get() {
    return "GET";
}
std::string encode_subscribe() {
    return "SUBSCRIBE";
}
std::string encode_scan() {
    return "SCAN";
}
std::string encode_cancel() {
    return "CANCEL";
}

bool is_auth_failure_reason(const std::string& reason) {
    return reason == "WRONG_KEY" || reason == "AUTH_FAILED" || reason == "INVALID_PSK";
}

std::string encode_connect_wifi(const std::string& ssid, const std::string& psk) {
    std::string command = "CONNECT_WIFI ssid=" + encode_b64_field(ssid);
    if (!psk.empty()) {
        command += " psk=" + encode_b64_field(psk);
    }
    return command;
}

std::string socket_path() {
    if (const char* value = ::getenv("HELIX_NETD_SOCKET")) {
        if (*value != '\0')
            return value;
    }
    return "/run/netd.sock";
}

std::string binary_path() {
    if (const char* value = ::getenv("HELIX_NETD_BIN")) {
        if (*value != '\0')
            return value;
    }
    return "/opt/config/mod/.bin/exec/netd";
}

bool available() {
    struct stat st {};
    if (::stat(socket_path().c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK) {
        return true;
    }
    return ::access(binary_path().c_str(), X_OK) == 0;
}

int connect_unix(const std::string& path, int timeout_ms, std::string* error_out) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        if (error_out)
            *error_out = std::string("socket(): ") + std::strerror(errno);
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        if (error_out)
            *error_out = "socket path too long (" + std::to_string(path.size()) + " bytes)";
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        if (error_out)
            *error_out = "connect(\"" + path + "\"): " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (rc != 0) {
        pollfd pfd{fd, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, timeout_ms);
        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (ready <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0 ||
            so_error != 0) {
            if (error_out) {
                *error_out = ready <= 0
                                 ? "connect(\"" + path + "\"): timed out after " +
                                       std::to_string(timeout_ms) + " ms"
                                 : "connect(\"" + path +
                                       "\"): " + std::strerror(so_error != 0 ? so_error : errno);
            }
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

QueryResult query_snapshot(int timeout_ms) {
    QueryResult result;

    const int fd = connect_unix(socket_path(), timeout_ms);
    if (fd < 0)
        return result;

    // Back to blocking mode so the SO_RCVTIMEO read loop below keeps its
    // per-read timeout semantics.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const std::string request = encode_get() + "\n";
    if (::write(fd, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
        ::close(fd);
        return result;
    }

    // Read until an ack ends the reply, the daemon closes, or the socket goes
    // quiet (per-read timeout). An overall deadline bounds a daemon that
    // drips lines without ever acking: 2x the per-read budget. get_info()
    // callers run on the shared HttpExecutor fast lane, so this bound is the
    // most a wedged daemon can pin one of those workers.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms) * 2;
    LineAssembler assembler;
    bool got_any_bytes = false;
    bool ack_seen = false;
    while (!ack_seen && std::chrono::steady_clock::now() < deadline) {
        char buffer[2048];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; // EAGAIN => quiet => done
        }
        if (n == 0)
            break; // daemon closed
        got_any_bytes = true;

        for (const std::string& line : assembler.feed(std::string_view(buffer, n))) {
            const Ack ack = parse_ack(line);
            if (ack.kind != Ack::Kind::None) {
                ack_seen = true;
                break;
            }
            if (parse_snapshot_line(line, result.snapshot)) {
                continue;
            }
            (void)parse_scan_row(line); // rows are not part of a GET reply; tolerated
        }
    }

    ::close(fd);
    result.reached = got_any_bytes;
    // The snapshot starts empty and only a merged MODE= line assigns mode, so
    // a non-empty mode IS "the daemon said something authoritative about the
    // transport". An explicit MODE= with an empty value stays NOT
    // authoritative — treating it as authoritative is the exact kernel-truth
    // blanking the flag exists to prevent.
    result.saw_mode = !result.snapshot.mode.empty();
    return result;
}

} // namespace helix::netd
