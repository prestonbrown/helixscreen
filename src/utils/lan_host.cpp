// SPDX-License-Identifier: GPL-3.0-or-later

#include "utils/lan_host.h"

#include "core/log_redact.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace helix::net {

namespace {

bool is_lan_class(redact::IpClass c) {
    return c == redact::IpClass::Private || c == redact::IpClass::LinkLocal ||
           c == redact::IpClass::UniqueLocal;
}

bool ends_with_local_label(std::string_view host) {
    if (!host.empty() && host.back() == '.')
        host.remove_suffix(1);
    constexpr std::string_view kSuffix = ".local";
    if (host.size() <= kSuffix.size())
        return false;
    const std::string_view tail = host.substr(host.size() - kSuffix.size());
    for (size_t i = 0; i < kSuffix.size(); ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != kSuffix[i])
            return false;
    }
    return true;
}

} // namespace

bool is_lan_host(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (const size_t zone = host.find('%'); zone != std::string_view::npos)
        host = host.substr(0, zone);

    const redact::IpClass c = redact::ip_class(host);
    if (c != redact::IpClass::NotAnIp)
        return is_lan_class(c);
    return ends_with_local_label(host);
}

bool is_lan_address(const struct sockaddr* addr) {
    if (addr == nullptr)
        return false;
    char text[INET6_ADDRSTRLEN] = {};
    if (addr->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        if (!inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)))
            return false;
    } else if (addr->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (!inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)))
            return false;
    } else {
        return false;
    }
    return is_lan_class(redact::ip_class(text));
}

} // namespace helix::net
