// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// True when the given Moonraker host string refers to the machine running
/// helixscreen. Checked in order: loopback literals → gethostname() →
/// getifaddrs() scan. Result cached per distinct host string.
bool is_moonraker_on_same_host(std::string_view host);

/// Extract the host portion of a websocket URL ("ws://host:port/..." or
/// "wss://..."; bracketed IPv6 handled). Returns "" for empty input or an
/// unknown scheme. Canonical home for "which host are we talking to" —
/// keep URL parsing and host-identity checks together.
[[nodiscard]] std::string extract_host_from_websocket_url(const std::string& url);

/// The host portion of an http, https, ws or wss URL, parsed as
/// extract_host_from_websocket_url() does. "" for any other scheme.
[[nodiscard]] std::string extract_host_from_url(const std::string& url);

/// True for the names this machine gives its own loopback ("localhost",
/// 127.0.0.1, ::1; any case), which no other machine can use to reach it.
[[nodiscard]] bool is_loopback_host(std::string_view host);

/// `ip` in the form addresses are compared in: an IPv4-mapped IPv6 address as plain IPv4,
/// with no IPv6 zone suffix.
[[nodiscard]] std::string canonical_ip(std::string ip);

/// Every address assigned to one of this machine's interfaces, IPv4 and IPv6, in
/// canonical numeric form. Empty where interfaces cannot be enumerated.
[[nodiscard]] std::vector<std::string> local_interface_ips();

/// The address a LAN client most likely reaches this machine on: the first IPv4
/// in @p ips that is neither loopback nor link-local. "" when there is none.
[[nodiscard]] std::string select_lan_ipv4(const std::vector<std::string>& ips);

/// Drop the detection cache. Call when moonraker_host changes at runtime.
void invalidate_host_identity_cache();

} // namespace helix
