// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "http_transport.h"

#include "remote_control_server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

static constexpr size_t MAX_HTTP_REQUEST = 1024 * 1024; // 1MB request cap.

namespace helix {

namespace {

/// Parse a numeric IPv4 address. False when @p host is not one.
bool parse_ipv4(const std::string& host, struct in_addr& out) {
    return inet_pton(AF_INET, host.c_str(), &out) == 1;
}

/// Compare without an early exit, so a wrong token leaks no position.
bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

} // namespace

HttpBindDecision decide_http_bind(const std::string& bind_host, const std::string& token) {
    struct in_addr addr {};
    if (!parse_ipv4(bind_host, addr)) {
        return HttpBindDecision::InvalidHost;
    }
    // 127/8 in its entirety, not just 127.0.0.1.
    if ((ntohl(addr.s_addr) >> 24) == 127) {
        return HttpBindDecision::Allow;
    }
    if (token.empty()) {
        return HttpBindDecision::TokenRequired;
    }
    if (token.size() < kMinHttpTokenLength) {
        return HttpBindDecision::TokenTooWeak;
    }
    return HttpBindDecision::Allow;
}

bool http_token_matches(const std::string& expected, const std::string& authorization) {
    if (expected.empty()) {
        return false;
    }
    static constexpr char kScheme[] = "bearer ";
    static constexpr size_t kSchemeLen = sizeof(kScheme) - 1;
    if (authorization.size() <= kSchemeLen) {
        return false;
    }
    for (size_t i = 0; i < kSchemeLen; i++) {
        if (std::tolower(static_cast<unsigned char>(authorization[i])) != kScheme[i]) {
            return false;
        }
    }
    return constant_time_equal(expected, authorization.substr(kSchemeLen));
}

HttpTransport::HttpTransport(std::string bind_host, int port, std::string token)
    : bind_host_(std::move(bind_host)), port_(port), token_(std::move(token)) {}

int HttpTransport::create_listener() {
    switch (decide_http_bind(bind_host_, token_)) {
    case HttpBindDecision::Allow:
        break;
    case HttpBindDecision::InvalidHost:
        spdlog::error("[RemoteControl] Invalid HTTP bind address: {}", bind_host_);
        return -1;
    case HttpBindDecision::TokenRequired:
        spdlog::error("[RemoteControl] Refusing to bind {}:{}: reachable from off-box and "
                      "no token is set. Export HELIX_REMOTE_HTTP_TOKEN (>= {} characters) "
                      "or bind 127.0.0.1.",
                      bind_host_, port_, kMinHttpTokenLength);
        return -1;
    case HttpBindDecision::TokenTooWeak:
        spdlog::error("[RemoteControl] Refusing to bind {}:{}: HELIX_REMOTE_HTTP_TOKEN is "
                      "shorter than {} characters.",
                      bind_host_, port_, kMinHttpTokenLength);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        spdlog::error("[RemoteControl] Failed to create TCP socket: {}", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, bind_host_.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("[RemoteControl] Invalid HTTP bind address: {}", bind_host_);
        close(fd);
        return -1;
    }

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("[RemoteControl] Failed to bind {}:{}: {}", bind_host_, port_,
                      strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        spdlog::error("[RemoteControl] Failed to listen on {}:{}: {}", bind_host_, port_,
                      strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

namespace {

// Case-insensitive search for a header value; returns "" if absent.
std::string header_value(const std::string& headers, const std::string& name) {
    std::string lc_headers = headers;
    std::transform(lc_headers.begin(), lc_headers.end(), lc_headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string needle = name;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    needle += ":";

    size_t pos = lc_headers.find("\r\n" + needle);
    if (pos == std::string::npos) {
        if (lc_headers.compare(0, needle.size(), needle) == 0) {
            pos = 0;
        } else {
            return "";
        }
    } else {
        pos += 2; // Skip the leading CRLF.
    }
    size_t val_start = pos + needle.size();
    size_t val_end = headers.find("\r\n", val_start);
    if (val_end == std::string::npos) {
        val_end = headers.size();
    }
    std::string value = headers.substr(val_start, val_end - val_start);
    // Trim surrounding whitespace.
    size_t b = value.find_first_not_of(" \t");
    size_t e = value.find_last_not_of(" \t");
    if (b == std::string::npos) {
        return "";
    }
    return value.substr(b, e - b + 1);
}

std::string http_response(int status, const char* status_text, const std::string& content_type,
                          const std::string& body) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

} // namespace

void HttpTransport::serve_client(int client_fd) {
    // One request per connection (Connection: close) — simple and sufficient for
    // a control channel. Read headers, then the Content-Length-delimited body.
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buffer;
    char chunk[4096];
    size_t header_end = std::string::npos;

    // Read until the end of the headers.
    while (running_.load()) {
        ssize_t n = read(client_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            return; // Disconnected before a complete request.
        }
        buffer.append(chunk, static_cast<size_t>(n));
        if (buffer.size() > MAX_HTTP_REQUEST) {
            std::string resp = http_response(413, "Payload Too Large", "text/plain", "too large\n");
            write_all(client_fd, resp.c_str(), resp.size());
            return;
        }
        header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
    }
    if (header_end == std::string::npos) {
        return;
    }

    std::string request_line = buffer.substr(0, buffer.find("\r\n"));
    std::string headers = buffer.substr(0, header_end);
    std::string body = buffer.substr(header_end + 4);

    // Parse "METHOD PATH HTTP/x.y".
    std::string method, path;
    {
        size_t sp1 = request_line.find(' ');
        size_t sp2 =
            (sp1 == std::string::npos) ? std::string::npos : request_line.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            method = request_line.substr(0, sp1);
            path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }

    if (method != "POST") {
        std::string resp = http_response(405, "Method Not Allowed", "text/plain",
                                         "POST a JSON-RPC request to /rpc\n");
        write_all(client_fd, resp.c_str(), resp.size());
        return;
    }
    if (path != "/rpc" && path != "/") {
        std::string resp = http_response(404, "Not Found", "text/plain", "not found\n");
        write_all(client_fd, resp.c_str(), resp.size());
        return;
    }

    // A token gates every request once configured, not just off-box binds: a
    // loopback listener started with one is still meant to be gated.
    if (!token_.empty() && !http_token_matches(token_, header_value(headers, "Authorization"))) {
        std::string resp =
            http_response(401, "Unauthorized", "text/plain", "missing or invalid bearer token\n");
        write_all(client_fd, resp.c_str(), resp.size());
        return;
    }

    // Read the rest of the body up to Content-Length.
    size_t content_length = 0;
    std::string cl = header_value(headers, "Content-Length");
    if (!cl.empty()) {
        try {
            content_length = static_cast<size_t>(std::stoul(cl));
        } catch (...) {
            content_length = 0;
        }
    }
    while (running_.load() && body.size() < content_length) {
        ssize_t n = read(client_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        body.append(chunk, static_cast<size_t>(n));
        if (body.size() > MAX_HTTP_REQUEST) {
            std::string resp = http_response(413, "Payload Too Large", "text/plain", "too large\n");
            write_all(client_fd, resp.c_str(), resp.size());
            return;
        }
    }

    // Strip a trailing newline the client may have appended (curl --data-binary
    // of a here-doc, etc.) — the dispatcher expects one bare JSON object.
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }

    std::string result = body.empty() ? std::string("{}") : handler_(body);
    std::string resp = http_response(200, "OK", "application/json", result);
    if (!write_all(client_fd, resp.c_str(), resp.size())) {
        spdlog::warn("[RemoteControl] Failed to write HTTP response: {}", strerror(errno));
    }
}

} // namespace helix
