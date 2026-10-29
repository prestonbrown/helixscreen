// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_auth.h"

#include "text_io.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace helix::moonraker_auth {

namespace {

std::mutex& credential_mutex() {
    static std::mutex m;
    return m;
}

Credential& stored_credential() {
    static Credential c;
    return c;
}

std::atomic<bool>& refused() {
    static std::atomic<bool> flag{false};
    return flag;
}

/// Caller holds credential_mutex().
const std::string* key_for_locked(std::string_view host, int port) {
    const Credential& c = stored_credential();
    if (c.api_key.empty() || c.port != port || !text_io::equals_ignore_case(c.host, host)) {
        return nullptr;
    }
    return &c.api_key;
}

} // namespace

void set_credential(Credential credential) {
    std::lock_guard<std::mutex> lock(credential_mutex());
    stored_credential() = std::move(credential);
    refused().store(false);
}

bool is_plausible_api_key(std::string_view key) {
    if (key.size() != 32) {
        return false;
    }
    for (const char ch : key) {
        const bool hex =
            (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

bool is_unauthorized_error(int code, std::string_view message) {
    return code == 401 || text_io::equals_ignore_case(message, "Unauthorized");
}

bool mark_unauthorized() {
    return !refused().exchange(true);
}

void mark_authorized() {
    refused().store(false);
}

bool unauthorized() {
    return refused().load();
}

std::string key_for(std::string_view host, int port) {
    std::lock_guard<std::mutex> lock(credential_mutex());
    const std::string* key = key_for_locked(host, port);
    return key ? *key : std::string{};
}

std::string key_for_url(std::string_view url) {
    const size_t scheme = url.find("://");
    std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));

    std::string_view host;
    std::string_view port;
    if (!rest.empty() && rest.front() == '[') {
        const size_t close = rest.find(']');
        if (close == std::string_view::npos || close + 1 >= rest.size() || rest[close + 1] != ':') {
            return {};
        }
        host = rest.substr(1, close - 1);
        port = rest.substr(close + 2);
    } else {
        const size_t colon = rest.rfind(':');
        if (colon == std::string_view::npos || rest.find(':') != colon) {
            return {};
        }
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    }

    int port_number = 0;
    for (const char ch : port) {
        if (ch < '0' || ch > '9' || port_number > 65535) {
            return {};
        }
        port_number = port_number * 10 + (ch - '0');
    }
    if (port.empty() || port_number == 0 || port_number > 65535) {
        return {};
    }
    return key_for(host, port_number);
}

HeaderDecision header_action(std::string_view host, int port, std::string_view current) {
    std::lock_guard<std::mutex> lock(credential_mutex());
    if (const std::string* key = key_for_locked(host, port)) {
        if (current == *key) {
            return {};
        }
        return {HeaderAction::Set, *key};
    }
    // Another service's key is that caller's own business; the recorded key
    // reaching a host it does not belong to (a redirect) is ours to take back.
    const std::string& recorded = stored_credential().api_key;
    if (!current.empty() && !recorded.empty() && current == recorded) {
        return {HeaderAction::Remove, {}};
    }
    return {};
}

} // namespace helix::moonraker_auth
