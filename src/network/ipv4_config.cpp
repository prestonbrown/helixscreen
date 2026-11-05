// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipv4_config.h"

#include "core/base64_field.h"
#include "core/data_root_resolver.h"
#include "core/text_io.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "hv/json.hpp"

namespace helix::net {

namespace {

constexpr const char* kPendingRevertFile = "ipv4_pending_revert.json";
constexpr const char* kDhcpcdMarkerBegin = "# HelixScreen static IPv4 begin: ";
constexpr const char* kDhcpcdMarkerEnd = "# HelixScreen static IPv4 end: ";

std::string_view trim(std::string_view s) {
    return helix::text_io::trim(s);
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

uint32_t prefix_mask(int prefix) {
    if (prefix <= 0) {
        return 0;
    }
    if (prefix >= 32) {
        return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu << (32 - prefix);
}

// Unicast addresses a host can hold: not 0.0.0.0/8, loopback, multicast or
// the reserved 240/4 block (which holds the limited broadcast address).
bool is_usable_host(uint32_t a) {
    const uint32_t first = a >> 24;
    return first != 0 && first != 127 && first < 224;
}

// A DNS server may be a loopback resolver; it may never be "no address",
// multicast or broadcast.
bool is_usable_dns(uint32_t a) {
    const uint32_t first = a >> 24;
    return first != 0 && first < 224;
}

std::vector<std::string_view> split_on(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(sep, start);
        if (end == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::vector<std::string_view> lines_of(std::string_view s) {
    std::vector<std::string_view> out;
    for (std::string_view line : split_on(s, '\n')) {
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.push_back(line);
    }
    // A trailing newline leaves one empty piece that is not a line.
    if (!out.empty() && out.back().empty()) {
        out.pop_back();
    }
    return out;
}

std::string join_fields(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) {
            out += sep;
        }
        out += items[i];
    }
    return out;
}

// "a.b.c.d/nn" into address and prefix.
bool parse_cidr(std::string_view text, std::string& address, int& prefix) {
    text = trim(text);
    const size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    const auto addr = parse_ipv4(text.substr(0, slash));
    const auto p = parse_netmask(text.substr(slash));
    if (!addr || !p) {
        return false;
    }
    address = format_ipv4(*addr);
    prefix = *p;
    return true;
}

// The first token of a whitespace-separated dhcpcd.conf line.
std::string_view first_word(std::string_view line) {
    line = trim(line);
    const size_t sp = line.find_first_of(" \t");
    return sp == std::string_view::npos ? line : line.substr(0, sp);
}

// The interface a dhcpcd.conf `interface X` line opens, or "" when the line
// opens no stanza. `ssid` and `profile` lines open other stanzas.
bool opens_stanza(std::string_view line, std::string_view& iface) {
    const std::string_view word = first_word(line);
    if (word == "interface") {
        iface = trim(trim(line).substr(word.size()));
        return true;
    }
    if (word == "ssid" || word == "profile") {
        iface = {};
        return true;
    }
    return false;
}

bool is_static_ipv4_line(std::string_view line) {
    line = trim(line);
    if (first_word(line) != "static") {
        return false;
    }
    const std::string_view rest = trim(line.substr(6));
    return starts_with(rest, "ip_address=") || starts_with(rest, "routers=") ||
           starts_with(rest, "domain_name_servers=");
}

} // namespace

bool Ipv4Config::operator==(const Ipv4Config& other) const {
    if (dhcp != other.dhcp || dns != other.dns) {
        return false;
    }
    if (dhcp) {
        return true;
    }
    return address == other.address && prefix == other.prefix && gateway == other.gateway;
}

std::string ipv4_config_text(const Ipv4Config& config) {
    if (config.dhcp) {
        return "DHCP";
    }
    return config.address + "/" + std::to_string(config.prefix) + " via " + config.gateway;
}

const char* ipv4_link_name(Ipv4Link link) {
    return link == Ipv4Link::Ethernet ? "ethernet" : "wifi";
}

std::optional<Ipv4Link> ipv4_link_from_name(std::string_view name) {
    if (name == "wifi") {
        return Ipv4Link::Wifi;
    }
    if (name == "ethernet") {
        return Ipv4Link::Ethernet;
    }
    return std::nullopt;
}

// ============================================================================
// Address arithmetic
// ============================================================================

std::optional<uint32_t> parse_ipv4(std::string_view text) {
    text = trim(text);
    const auto parts = split_on(text, '.');
    if (parts.size() != 4) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (std::string_view part : parts) {
        if (part.empty() || part.size() > 3) {
            return std::nullopt;
        }
        uint32_t octet = 0;
        for (char c : part) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            octet = octet * 10 + static_cast<uint32_t>(c - '0');
        }
        if (octet > 255) {
            return std::nullopt;
        }
        value = (value << 8) | octet;
    }
    return value;
}

std::string format_ipv4(uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (a >> 24) & 0xFF, (a >> 16) & 0xFF,
                  (a >> 8) & 0xFF, a & 0xFF);
    return buf;
}

std::optional<int> parse_netmask(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.find('.') == std::string_view::npos) {
        if (text.front() == '/') {
            text.remove_prefix(1);
        }
        if (text.empty() || text.size() > 2) {
            return std::nullopt;
        }
        int prefix = 0;
        for (char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            prefix = prefix * 10 + (c - '0');
        }
        if (prefix > 32) {
            return std::nullopt;
        }
        return prefix;
    }
    const auto mask = parse_ipv4(text);
    if (!mask) {
        return std::nullopt;
    }
    // Contiguous: the inverted mask plus one is a power of two (or zero).
    const uint32_t inverted = ~*mask;
    if ((inverted & (inverted + 1)) != 0) {
        return std::nullopt;
    }
    int prefix = 0;
    for (uint32_t m = *mask; m & 0x80000000u; m <<= 1) {
        ++prefix;
    }
    return prefix;
}

std::string prefix_to_netmask(int prefix) {
    return format_ipv4(prefix_mask(prefix));
}

// ============================================================================
// Form validation
// ============================================================================

Ipv4Verdict validate_ipv4_form(const Ipv4Form& form) {
    Ipv4Verdict v;
    auto fail = [&v](Ipv4Problem problem, Ipv4Field field) {
        v.problem = problem;
        v.field = field;
        v.config = {};
        return v;
    };

    if (trim(form.address).empty()) {
        return fail(Ipv4Problem::Empty, Ipv4Field::Address);
    }
    const auto address = parse_ipv4(form.address);
    if (!address) {
        return fail(Ipv4Problem::NotAnAddress, Ipv4Field::Address);
    }
    if (!is_usable_host(*address)) {
        return fail(Ipv4Problem::NotUsable, Ipv4Field::Address);
    }

    if (trim(form.netmask).empty()) {
        return fail(Ipv4Problem::Empty, Ipv4Field::Netmask);
    }
    const auto prefix = parse_netmask(form.netmask);
    if (!prefix || *prefix == 0) {
        return fail(Ipv4Problem::NotAMask, Ipv4Field::Netmask);
    }

    const uint32_t mask = prefix_mask(*prefix);
    const uint32_t network = *address & mask;
    const uint32_t broadcast = network | ~mask;
    // /31 and /32 have no network or broadcast address (RFC 3021).
    const bool has_edges = *prefix <= 30;
    if (has_edges && *address == network) {
        return fail(Ipv4Problem::NetworkAddress, Ipv4Field::Address);
    }
    if (has_edges && *address == broadcast) {
        return fail(Ipv4Problem::BroadcastAddress, Ipv4Field::Address);
    }

    if (trim(form.gateway).empty()) {
        return fail(Ipv4Problem::Empty, Ipv4Field::Gateway);
    }
    const auto gateway = parse_ipv4(form.gateway);
    if (!gateway) {
        return fail(Ipv4Problem::NotAnAddress, Ipv4Field::Gateway);
    }
    if (!is_usable_host(*gateway)) {
        return fail(Ipv4Problem::NotUsable, Ipv4Field::Gateway);
    }
    if (*gateway == *address) {
        return fail(Ipv4Problem::GatewayIsAddress, Ipv4Field::Gateway);
    }
    if ((*gateway & mask) != network) {
        return fail(Ipv4Problem::GatewayOutsideSubnet, Ipv4Field::Gateway);
    }
    if (has_edges && *gateway == network) {
        return fail(Ipv4Problem::NetworkAddress, Ipv4Field::Gateway);
    }
    if (has_edges && *gateway == broadcast) {
        return fail(Ipv4Problem::BroadcastAddress, Ipv4Field::Gateway);
    }

    if (trim(form.dns).empty()) {
        return fail(Ipv4Problem::Empty, Ipv4Field::Dns);
    }
    const auto dns = parse_ipv4(form.dns);
    if (!dns) {
        return fail(Ipv4Problem::NotAnAddress, Ipv4Field::Dns);
    }
    if (!is_usable_dns(*dns)) {
        return fail(Ipv4Problem::NotUsable, Ipv4Field::Dns);
    }

    v.config.dhcp = false;
    v.config.address = format_ipv4(*address);
    v.config.prefix = *prefix;
    v.config.gateway = format_ipv4(*gateway);
    v.config.dns = {format_ipv4(*dns)};
    return v;
}

Ipv4Form ipv4_form_from_config(const Ipv4Config& config) {
    Ipv4Form f;
    if (!config.dhcp) {
        f.address = config.address;
        f.netmask = prefix_to_netmask(config.prefix);
        f.gateway = config.gateway;
    }
    if (!config.dns.empty()) {
        f.dns = config.dns.front();
    }
    return f;
}

Ipv4Config ipv4_keep_unshown(const Ipv4Config& current, Ipv4Config next) {
    if (next.dns.size() == 1 && !current.dns.empty() && current.dns.front() == next.dns.front()) {
        next.dns = current.dns;
    }
    return next;
}

// ============================================================================
// Mock store
// ============================================================================

Ipv4Result MockIpv4Config::get(Ipv4Config& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out = config_;
    return Ipv4Result::success();
}

Ipv4Result MockIpv4Config::apply(const Ipv4Config& config) {
    const char* fail = std::getenv("HELIX_MOCK_IPV4_APPLY_FAIL");
    if (fail && std::strcmp(fail, "1") == 0) {
        spdlog::info("[MockIpv4] Apply refused (HELIX_MOCK_IPV4_APPLY_FAIL=1)");
        return Ipv4Result::failed("mock apply failure requested");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    spdlog::info("[MockIpv4] Applied {}", ipv4_config_text(config));
    return Ipv4Result::success();
}

MockIpv4Config& mock_ipv4_store(Ipv4Link link) {
    static MockIpv4Config wifi;
    static MockIpv4Config ethernet;
    return link == Ipv4Link::Ethernet ? ethernet : wifi;
}

Ipv4Snapshot mock_ipv4_snapshot(Ipv4Link link) {
    Ipv4Config current;
    mock_ipv4_store(link).get(current);
    nlohmann::json j;
    j["dhcp"] = current.dhcp;
    j["address"] = current.address;
    j["prefix"] = current.prefix;
    j["gateway"] = current.gateway;
    j["dns"] = current.dns;
    Ipv4Snapshot snap;
    snap.store = "mock";
    snap.iface = ipv4_link_name(link);
    snap.original = helix::json_util::safe_dump(j);
    return snap;
}

Ipv4Result restore_mock_ipv4_snapshot(const Ipv4Snapshot& snapshot) {
    const auto link = ipv4_link_from_name(snapshot.iface);
    const nlohmann::json j =
        nlohmann::json::parse(snapshot.original.begin(), snapshot.original.end(), nullptr, false);
    if (!link || j.is_discarded() || !j.is_object()) {
        return Ipv4Result::refused("unreadable mock snapshot");
    }
    Ipv4Config c;
    c.dhcp = helix::json_util::safe_bool(j, "dhcp", true);
    c.address = helix::json_util::safe_string(j, "address");
    c.prefix = helix::json_util::safe_int(j, "prefix");
    c.gateway = helix::json_util::safe_string(j, "gateway");
    if (const nlohmann::json* dns = helix::json_util::find_member(j, "dns");
        dns && dns->is_array()) {
        for (const auto& server : *dns) {
            c.dns.push_back(helix::json_util::as_string(server));
        }
    }
    return mock_ipv4_store(*link).apply(c);
}

std::string MockIpv4Config::static_address() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.dhcp ? std::string() : config_.address;
}

// ============================================================================
// NetworkManager
// ============================================================================

std::string nm_active_uuid_for_device(const std::string& output, const std::string& iface) {
    if (iface.empty()) {
        return {};
    }
    for (std::string_view line : lines_of(output)) {
        // UUIDs and device names carry no ':', so terse escaping never applies.
        const size_t colon = line.rfind(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        if (line.substr(colon + 1) == iface) {
            return std::string(trim(line.substr(0, colon)));
        }
    }
    return {};
}

std::vector<std::string> nm_ipv4_show_argv(const std::string& uuid) {
    return {"-t",   "-f", "ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns", "connection", "show",
            "uuid", uuid};
}

std::string nm_inactive_profile_for_link(const std::string& output, Ipv4Link link) {
    const std::string_view type = link == Ipv4Link::Ethernet ? "802-3-ethernet" : "802-11-wireless";
    std::string best;
    long long best_stamp = -1;
    for (std::string_view line : lines_of(output)) {
        const auto f = split_on(line, ':');
        if (f.size() != 4 || f[1] != type || !trim(f[3]).empty()) {
            continue;
        }
        const long long stamp = std::atoll(std::string(f[2]).c_str());
        if (stamp > best_stamp) {
            best_stamp = stamp;
            best = std::string(trim(f[0]));
        }
    }
    return best;
}

NmIpv4Values nm_parse_ipv4_values(const std::string& output) {
    NmIpv4Values v;
    for (std::string_view line : lines_of(output)) {
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        std::string value(trim(line.substr(colon + 1)));
        if (value == "--") {
            value.clear();
        }
        if (key == "ipv4.method") {
            v.method = value;
        } else if (key == "ipv4.addresses") {
            v.addresses = value;
        } else if (key == "ipv4.gateway") {
            v.gateway = value;
        } else if (key == "ipv4.dns") {
            v.dns = value;
        }
    }
    return v;
}

std::string nm_encode_values(const NmIpv4Values& v) {
    return "ipv4.method:" + v.method + "\nipv4.addresses:" + v.addresses +
           "\nipv4.gateway:" + v.gateway + "\nipv4.dns:" + v.dns + "\n";
}

NmIpv4Values nm_decode_values(const std::string& text) {
    return nm_parse_ipv4_values(text);
}

NmIpv4Values nm_values_for(const Ipv4Config& config) {
    NmIpv4Values v;
    v.method = config.dhcp ? "auto" : "manual";
    if (!config.dhcp) {
        v.addresses = config.address + "/" + std::to_string(config.prefix);
        v.gateway = config.gateway;
    }
    v.dns = join_fields(config.dns, ",");
    return v;
}

namespace {
// nmcli prints lists as "a,b" or "a, b"; compare them item by item.
std::string normalized_list(const std::string& list) {
    std::string out;
    for (std::string_view item : split_on(list, ',')) {
        item = trim(item);
        if (item.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ',';
        }
        out.append(item);
    }
    return out;
}

bool same_values(const NmIpv4Values& a, const NmIpv4Values& b) {
    return a.method == b.method && normalized_list(a.addresses) == normalized_list(b.addresses) &&
           a.gateway == b.gateway && normalized_list(a.dns) == normalized_list(b.dns);
}
} // namespace

RestoreStep nm_restore_step(const NmIpv4Values& now, const NmIpv4Values& original,
                            const NmIpv4Values& written) {
    if (same_values(now, original)) {
        return RestoreStep::AlreadyThere;
    }
    return same_values(now, written) ? RestoreStep::Write : RestoreStep::Refuse;
}

std::vector<std::string> nm_ipv4_restore_argv(const std::string& uuid, const NmIpv4Values& v) {
    return {"connection",     "modify",    "uuid",         uuid,      "ipv4.method", v.method,
            "ipv4.addresses", v.addresses, "ipv4.gateway", v.gateway, "ipv4.dns",    v.dns};
}

std::optional<Ipv4Config> nm_parse_ipv4(const std::string& output) {
    const NmIpv4Values v = nm_parse_ipv4_values(output);
    const std::string& method = v.method;
    const std::string& addresses = v.addresses;
    const std::string& gateway = v.gateway;
    const std::string& dns = v.dns;

    Ipv4Config c;
    for (std::string_view server : split_on(dns, ',')) {
        server = trim(server);
        if (!server.empty()) {
            c.dns.emplace_back(server);
        }
    }
    if (method == "auto") {
        c.dhcp = true;
        return c;
    }
    if (method != "manual") {
        return std::nullopt;
    }
    c.dhcp = false;
    const std::string_view first = split_on(addresses, ',').front();
    if (!parse_cidr(first, c.address, c.prefix)) {
        return std::nullopt;
    }
    if (const auto gw = parse_ipv4(gateway)) {
        c.gateway = format_ipv4(*gw);
    }
    return c;
}

std::vector<std::string> nm_ipv4_modify_argv(const std::string& uuid, const Ipv4Config& config) {
    std::vector<std::string> argv = {"connection", "modify", "uuid", uuid, "ipv4.method"};
    if (config.dhcp) {
        argv.insert(argv.end(), {"auto", "ipv4.addresses", "", "ipv4.gateway", ""});
    } else {
        argv.insert(argv.end(), {"manual", "ipv4.addresses",
                                 config.address + "/" + std::to_string(config.prefix),
                                 "ipv4.gateway", config.gateway});
    }
    argv.insert(argv.end(), {"ipv4.dns", join_fields(config.dns, ",")});
    return argv;
}

// ============================================================================
// dhcpcd
// ============================================================================

std::optional<std::string> dhcpcd_conf_with_ipv4(const std::string& conf, const std::string& iface,
                                                 const Ipv4Config& config) {
    const std::string begin = kDhcpcdMarkerBegin + iface;
    const std::string end = kDhcpcdMarkerEnd + iface;

    std::string out;
    bool in_marked = false;
    bool in_iface_stanza = false;
    for (std::string_view line : lines_of(conf)) {
        const std::string_view t = trim(line);
        if (in_marked) {
            if (t == end) {
                in_marked = false;
            }
            continue;
        }
        if (t == begin) {
            in_marked = true;
            continue;
        }
        std::string_view opened;
        if (opens_stanza(line, opened)) {
            in_iface_stanza = (opened == iface);
        } else if (in_iface_stanza && is_static_ipv4_line(line)) {
            continue;
        }
        out.append(line);
        out += '\n';
    }
    if (in_marked) {
        return std::nullopt;
    }

    if (!config.dhcp) {
        out += begin + "\n";
        out += "interface " + iface + "\n";
        out += "static ip_address=" + config.address + "/" + std::to_string(config.prefix) + "\n";
        out += "static routers=" + config.gateway + "\n";
        out += "static domain_name_servers=" + join_fields(config.dns, " ") + "\n";
        out += end + "\n";
    }
    return out;
}

std::optional<Ipv4Config> dhcpcd_conf_read_ipv4(const std::string& conf, const std::string& iface) {
    std::vector<std::string> addresses;
    std::string routers, dns;
    bool in_iface_stanza = false;
    for (std::string_view line : lines_of(conf)) {
        std::string_view opened;
        if (opens_stanza(line, opened)) {
            in_iface_stanza = (opened == iface);
            continue;
        }
        if (!in_iface_stanza || !is_static_ipv4_line(line)) {
            continue;
        }
        const std::string_view rest = trim(trim(line).substr(6));
        const size_t eq = rest.find('=');
        const std::string_view key = rest.substr(0, eq);
        const std::string value(trim(rest.substr(eq + 1)));
        if (key == "ip_address") {
            addresses.push_back(value);
        } else if (key == "routers") {
            routers = value;
        } else {
            dns = value;
        }
    }
    Ipv4Config c;
    for (std::string_view server : split_on(dns, ' ')) {
        server = trim(server);
        if (!server.empty()) {
            c.dns.emplace_back(server);
        }
    }
    if (addresses.empty()) {
        return c;
    }
    if (addresses.size() > 1 || !parse_cidr(addresses.front(), c.address, c.prefix)) {
        return std::nullopt;
    }
    c.dhcp = false;
    if (const auto gw = parse_ipv4(split_on(routers, ' ').front())) {
        c.gateway = format_ipv4(*gw);
    }
    return c;
}

// ============================================================================
// Confirm-or-revert
// ============================================================================

void Ipv4RevertCountdown::start(uint32_t now_ms, uint32_t seconds) {
    running_ = true;
    start_ms_ = now_ms;
    total_ms_ = seconds * 1000u;
}

uint32_t Ipv4RevertCountdown::seconds_left(uint32_t now_ms) const {
    if (!running_) {
        return 0;
    }
    const uint32_t elapsed = now_ms - start_ms_;
    if (elapsed >= total_ms_) {
        return 0;
    }
    return (total_ms_ - elapsed + 999u) / 1000u;
}

bool Ipv4RevertCountdown::expired(uint32_t now_ms) const {
    return running_ && (now_ms - start_ms_) >= total_ms_;
}

std::string encode_pending_ipv4_revert(const PendingIpv4Revert& pending) {
    nlohmann::json j;
    j["link"] = ipv4_link_name(pending.link);
    j["dhcp"] = pending.previous.dhcp;
    j["address"] = pending.previous.address;
    j["prefix"] = pending.previous.prefix;
    j["gateway"] = pending.previous.gateway;
    j["dns"] = pending.previous.dns;
    j["owner"] = pending.owner;
    j["failed"] = pending.failed;
    j["failure"] = pending.failure;
    j["store"] = pending.snapshot.store;
    j["iface"] = pending.snapshot.iface;
    j["profile"] = pending.snapshot.profile;
    j["original_b64"] = helix::encode_b64_field(pending.snapshot.original);
    j["written_b64"] = helix::encode_b64_field(pending.snapshot.written);
    return helix::json_util::safe_dump(j, 2);
}

namespace {
// Base64 to bytes; false for text that is not base64 (empty is empty).
bool decode_bytes(const std::string& b64, std::string& out) {
    out = helix::decode_b64_field(b64);
    return b64.empty() || !out.empty();
}
} // namespace

std::optional<PendingIpv4Revert> decode_pending_ipv4_revert(std::string_view text) {
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
    }
    const auto link = ipv4_link_from_name(helix::json_util::safe_string(j, "link"));
    if (!link) {
        return std::nullopt;
    }
    PendingIpv4Revert p;
    p.link = *link;
    p.owner = helix::json_util::safe_string(j, "owner");
    p.failed = helix::json_util::safe_bool(j, "failed");
    p.failure = helix::json_util::safe_string(j, "failure");
    p.previous.dhcp = helix::json_util::safe_bool(j, "dhcp", true);
    p.previous.address = helix::json_util::safe_string(j, "address");
    p.previous.prefix = helix::json_util::safe_int(j, "prefix");
    p.previous.gateway = helix::json_util::safe_string(j, "gateway");
    if (const nlohmann::json* dns = helix::json_util::find_member(j, "dns");
        dns && dns->is_array()) {
        for (const auto& server : *dns) {
            p.previous.dns.push_back(helix::json_util::as_string(server));
        }
    }
    Ipv4Snapshot& snap = p.snapshot;
    snap.store = helix::json_util::safe_string(j, "store");
    snap.iface = helix::json_util::safe_string(j, "iface");
    snap.profile = helix::json_util::safe_string(j, "profile");
    if (!decode_bytes(helix::json_util::safe_string(j, "original_b64"), snap.original) ||
        !decode_bytes(helix::json_util::safe_string(j, "written_b64"), snap.written)) {
        return std::nullopt;
    }
    // What the restore needs, store by store. A failed record is kept only to
    // be shown, so it needs nothing.
    if (p.failed) {
        return p;
    }
    if (snap.store == "networkmanager") {
        if (snap.profile.empty() || snap.iface.empty() || snap.original.empty()) {
            return std::nullopt;
        }
    } else if (snap.store == "dhcpcd") {
        if (snap.iface.empty()) {
            return std::nullopt;
        }
    } else if (snap.store == "mock") {
        if (!ipv4_link_from_name(snap.iface) || snap.original.empty()) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return p;
}

const std::string& ipv4_process_owner() {
    // Two clocks read at first use: a restarted process differs in both, with
    // no entropy source the firmware may lack.
    static const std::string owner = [] {
        const auto wall = std::chrono::system_clock::now().time_since_epoch().count();
        const auto mono = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::to_string(wall) + "-" + std::to_string(mono);
    }();
    return owner;
}

bool is_stale_pending_ipv4_revert(const std::optional<PendingIpv4Revert>& pending, Ipv4Link link) {
    return pending && pending->link == link && pending->owner != ipv4_process_owner();
}

std::string pending_ipv4_revert_path() {
    return helix::get_user_config_dir() + "/" + kPendingRevertFile;
}

bool save_pending_ipv4_revert(const PendingIpv4Revert& pending) {
    const std::string path = pending_ipv4_revert_path();
    if (!helix::text_io::write_file_atomic(path, encode_pending_ipv4_revert(pending),
                                           helix::text_io::Durability::Fsync)) {
        spdlog::error("[Ipv4] Could not write {}: {}", path, std::strerror(errno));
        return false;
    }
    return true;
}

Ipv4RecordLoad read_pending_ipv4_revert() {
    Ipv4RecordLoad out;
    const std::string path = pending_ipv4_revert_path();
    errno = 0;
    const auto text = helix::text_io::read_file(path, kIpv4RecordMaxBytes + 1);
    if (!text) {
        if (errno != ENOENT && access(path.c_str(), F_OK) == 0) {
            out.kind = Ipv4RecordLoad::Kind::Unreadable;
        }
        return out;
    }
    std::optional<PendingIpv4Revert> pending;
    if (text->size() <= kIpv4RecordMaxBytes) {
        pending = decode_pending_ipv4_revert(*text);
    }
    if (!pending) {
        spdlog::error("[Ipv4] {} cannot be used to restore anything", path);
        out.kind = Ipv4RecordLoad::Kind::Unreadable;
        return out;
    }
    out.kind = Ipv4RecordLoad::Kind::Ok;
    out.record = std::move(*pending);
    return out;
}

std::optional<PendingIpv4Revert> load_pending_ipv4_revert() {
    Ipv4RecordLoad r = read_pending_ipv4_revert();
    if (r.kind != Ipv4RecordLoad::Kind::Ok) {
        return std::nullopt;
    }
    return std::move(r.record);
}

bool clear_pending_ipv4_revert() {
    const std::string path = pending_ipv4_revert_path();
    if (std::remove(path.c_str()) == 0 || errno == ENOENT) {
        return true;
    }
    spdlog::error("[Ipv4] Could not remove {}: {}", path, std::strerror(errno));
    return false;
}

std::mutex& ipv4_change_mutex() {
    static std::mutex mutex;
    return mutex;
}

// ============================================================================
// Change flow
// ============================================================================

Ipv4ChangeFlow::Action Ipv4ChangeFlow::begin(const Ipv4Config& current, const Ipv4Config& next) {
    if (state_ != State::Idle) {
        return Action::None;
    }
    if (next == current) {
        return Action::Close;
    }
    next_ = next;
    abandoned_ = std::make_shared<std::atomic<bool>>(false);
    state_ = State::Applying;
    return Action::Apply;
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::applied(Ipv4Result::Status outcome, uint32_t now_ms,
                                               bool job_holds_machine) {
    if (state_ != State::Applying) {
        return Action::None;
    }
    if (outcome == Ipv4Result::Status::Refused || outcome == Ipv4Result::Status::NotSupported) {
        state_ = State::Idle;
        return Action::NotApplied;
    }
    if (outcome != Ipv4Result::Status::Ok) {
        // A failed apply may still have written part of the change.
        state_ = State::Reverting;
        return Action::Revert;
    }
    state_ = State::Confirming;
    countdown_.start(now_ms);
    if (job_holds_machine) {
        return job_started();
    }
    return Action::ShowKeepPrompt;
}

void Ipv4ChangeFlow::handed_off() {
    if (state_ == State::Reverting) {
        state_ = State::Idle;
    }
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::tick(uint32_t now_ms) {
    if (state_ != State::Confirming || !countdown_.expired(now_ms)) {
        return Action::None;
    }
    return reject();
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::keep() {
    if (state_ != State::Confirming) {
        return Action::None;
    }
    countdown_.stop();
    state_ = State::Idle;
    return Action::Kept;
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::job_started() {
    return keep() == Action::Kept ? Action::KeptForJob : Action::None;
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::reject() {
    if (state_ != State::Confirming) {
        return Action::None;
    }
    countdown_.stop();
    state_ = State::Reverting;
    return Action::Revert;
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::reverted(bool ok) {
    if (state_ != State::Reverting) {
        return Action::None;
    }
    state_ = State::Idle;
    return ok ? Action::Restored : Action::RestoreFailed;
}

Ipv4ChangeFlow::Action Ipv4ChangeFlow::teardown() {
    switch (state_) {
    case State::Applying:
        // The answer will find nobody. An apply not started yet refuses; one
        // under way is restored once it lands, by the revert this asks for.
        abandoned_->store(true);
        state_ = State::Reverting;
        return Action::Revert;
    case State::Confirming:
        countdown_.stop();
        state_ = State::Reverting;
        return Action::Revert;
    case State::Idle:
    case State::Reverting:
        return Action::None;
    }
    return Action::None;
}

} // namespace helix::net
