// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipv4_config.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

using namespace helix::net;

namespace {

Ipv4Form form(const char* address, const char* mask, const char* gateway, const char* dns) {
    return Ipv4Form{address, mask, gateway, dns};
}

Ipv4Config static_config(const char* address, int prefix, const char* gateway,
                         std::vector<std::string> dns) {
    Ipv4Config c;
    c.dhcp = false;
    c.address = address;
    c.prefix = prefix;
    c.gateway = gateway;
    c.dns = std::move(dns);
    return c;
}

// The dhcpcd transforms, for input they must accept.
std::string with_ipv4(const std::string& conf, const std::string& iface, const Ipv4Config& c) {
    const auto out = dhcpcd_conf_with_ipv4(conf, iface, c);
    REQUIRE(out);
    return *out;
}
Ipv4Config read_ipv4(const std::string& conf, const std::string& iface) {
    const auto out = dhcpcd_conf_read_ipv4(conf, iface);
    REQUIRE(out);
    return *out;
}

} // namespace

// ============================================================================
// Address arithmetic
// ============================================================================

TEST_CASE("parse_ipv4 accepts only a strict dotted quad", "[ipv4]") {
    REQUIRE(parse_ipv4("192.168.1.50") == 0xC0A80132u);
    REQUIRE(parse_ipv4(" 10.0.0.1 ") == 0x0A000001u);
    REQUIRE(parse_ipv4("0.0.0.0") == 0u);
    REQUIRE(parse_ipv4("255.255.255.255") == 0xFFFFFFFFu);

    REQUIRE_FALSE(parse_ipv4(""));
    REQUIRE_FALSE(parse_ipv4("192.168.1"));
    REQUIRE_FALSE(parse_ipv4("192.168.1.1.1"));
    REQUIRE_FALSE(parse_ipv4("192.168.1.256"));
    REQUIRE_FALSE(parse_ipv4("192.168..1"));
    REQUIRE_FALSE(parse_ipv4("192.168.1."));
    REQUIRE_FALSE(parse_ipv4("1922.168.1.1"));
    REQUIRE_FALSE(parse_ipv4("192.168. 1.1"));
    REQUIRE_FALSE(parse_ipv4("+1.2.3.4"));
    REQUIRE_FALSE(parse_ipv4("a.b.c.d"));
    REQUIRE_FALSE(parse_ipv4("192.168.1.1/24"));
}

TEST_CASE("format_ipv4 round-trips parse_ipv4", "[ipv4]") {
    REQUIRE(format_ipv4(0xC0A80132u) == "192.168.1.50");
    REQUIRE(format_ipv4(0) == "0.0.0.0");
    REQUIRE(format_ipv4(*parse_ipv4("172.16.254.3")) == "172.16.254.3");
}

TEST_CASE("parse_netmask takes a dotted mask or CIDR", "[ipv4]") {
    REQUIRE(parse_netmask("255.255.255.0") == 24);
    REQUIRE(parse_netmask("255.255.254.0") == 23);
    REQUIRE(parse_netmask("255.255.255.255") == 32);
    REQUIRE(parse_netmask("128.0.0.0") == 1);
    REQUIRE(parse_netmask("0.0.0.0") == 0);
    REQUIRE(parse_netmask("/24") == 24);
    REQUIRE(parse_netmask("24") == 24);
    REQUIRE(parse_netmask(" /16 ") == 16);
    REQUIRE(parse_netmask("/32") == 32);
    REQUIRE(parse_netmask("/0") == 0);

    SECTION("non-contiguous masks are refused") {
        REQUIRE_FALSE(parse_netmask("255.0.255.0"));
        REQUIRE_FALSE(parse_netmask("255.255.255.1"));
        REQUIRE_FALSE(parse_netmask("0.255.255.255"));
    }
    SECTION("malformed CIDR is refused") {
        REQUIRE_FALSE(parse_netmask("/33"));
        REQUIRE_FALSE(parse_netmask("/"));
        REQUIRE_FALSE(parse_netmask("/-1"));
        REQUIRE_FALSE(parse_netmask("/2a"));
        REQUIRE_FALSE(parse_netmask(""));
        REQUIRE_FALSE(parse_netmask("255.255.255"));
    }
}

TEST_CASE("prefix_to_netmask covers both ends", "[ipv4]") {
    REQUIRE(prefix_to_netmask(24) == "255.255.255.0");
    REQUIRE(prefix_to_netmask(0) == "0.0.0.0");
    REQUIRE(prefix_to_netmask(32) == "255.255.255.255");
    REQUIRE(prefix_to_netmask(20) == "255.255.240.0");
    REQUIRE(prefix_to_netmask(40) == "255.255.255.255");
    REQUIRE(prefix_to_netmask(-3) == "0.0.0.0");
}

// ============================================================================
// Form validation
// ============================================================================

TEST_CASE("validate_ipv4_form accepts a good form and builds the config", "[ipv4]") {
    const auto v =
        validate_ipv4_form(form("192.168.1.50", "255.255.255.0", "192.168.1.1", "1.1.1.1"));
    REQUIRE(v.ok());
    REQUIRE(v.field == Ipv4Field::None);
    REQUIRE(v.config == static_config("192.168.1.50", 24, "192.168.1.1", {"1.1.1.1"}));

    SECTION("CIDR mask and surrounding whitespace normalize") {
        const auto c = validate_ipv4_form(form(" 10.0.0.9", "/8", "10.0.0.1 ", " 10.0.0.1"));
        REQUIRE(c.ok());
        REQUIRE(c.config == static_config("10.0.0.9", 8, "10.0.0.1", {"10.0.0.1"}));
    }
    SECTION("a loopback DNS resolver is allowed") {
        REQUIRE(validate_ipv4_form(form("192.168.1.50", "/24", "192.168.1.1", "127.0.0.53")).ok());
    }
    SECTION("a DNS server outside the subnet is allowed") {
        REQUIRE(validate_ipv4_form(form("192.168.1.50", "/24", "192.168.1.1", "8.8.8.8")).ok());
    }
}

TEST_CASE("validate_ipv4_form names the first bad field", "[ipv4]") {
    struct Case {
        Ipv4Form form;
        Ipv4Problem problem;
        Ipv4Field field;
    };
    const Case cases[] = {
        {form("", "/24", "192.168.1.1", "1.1.1.1"), Ipv4Problem::Empty, Ipv4Field::Address},
        {form("192.168.1", "/24", "192.168.1.1", "1.1.1.1"), Ipv4Problem::NotAnAddress,
         Ipv4Field::Address},
        {form("127.0.0.5", "/8", "127.0.0.1", "1.1.1.1"), Ipv4Problem::NotUsable,
         Ipv4Field::Address},
        {form("0.1.2.3", "/8", "0.0.0.1", "1.1.1.1"), Ipv4Problem::NotUsable, Ipv4Field::Address},
        {form("224.0.0.5", "/24", "224.0.0.1", "1.1.1.1"), Ipv4Problem::NotUsable,
         Ipv4Field::Address},
        {form("240.0.0.5", "/24", "240.0.0.1", "1.1.1.1"), Ipv4Problem::NotUsable,
         Ipv4Field::Address},
        {form("192.168.1.50", "", "192.168.1.1", "1.1.1.1"), Ipv4Problem::Empty,
         Ipv4Field::Netmask},
        {form("192.168.1.50", "255.0.255.0", "192.168.1.1", "1.1.1.1"), Ipv4Problem::NotAMask,
         Ipv4Field::Netmask},
        {form("192.168.1.50", "/0", "192.168.1.1", "1.1.1.1"), Ipv4Problem::NotAMask,
         Ipv4Field::Netmask},
        {form("192.168.1.0", "/24", "192.168.1.1", "1.1.1.1"), Ipv4Problem::NetworkAddress,
         Ipv4Field::Address},
        {form("192.168.1.255", "/24", "192.168.1.1", "1.1.1.1"), Ipv4Problem::BroadcastAddress,
         Ipv4Field::Address},
        {form("192.168.1.50", "/24", "", "1.1.1.1"), Ipv4Problem::Empty, Ipv4Field::Gateway},
        {form("192.168.1.50", "/24", "192.168.2.1", "1.1.1.1"), Ipv4Problem::GatewayOutsideSubnet,
         Ipv4Field::Gateway},
        {form("192.168.1.50", "/24", "192.168.1.255", "1.1.1.1"), Ipv4Problem::BroadcastAddress,
         Ipv4Field::Gateway},
        {form("192.168.1.50", "/24", "192.168.1.0", "1.1.1.1"), Ipv4Problem::NetworkAddress,
         Ipv4Field::Gateway},
        {form("192.168.1.50", "/24", "192.168.1.50", "1.1.1.1"), Ipv4Problem::GatewayIsAddress,
         Ipv4Field::Gateway},
        {form("192.168.1.50", "/24", "192.168.1.1", ""), Ipv4Problem::Empty, Ipv4Field::Dns},
        {form("192.168.1.50", "/24", "192.168.1.1", "dns.example"), Ipv4Problem::NotAnAddress,
         Ipv4Field::Dns},
        {form("192.168.1.50", "/24", "192.168.1.1", "0.0.0.0"), Ipv4Problem::NotUsable,
         Ipv4Field::Dns},
        {form("192.168.1.50", "/24", "192.168.1.1", "255.255.255.255"), Ipv4Problem::NotUsable,
         Ipv4Field::Dns},
    };
    for (const auto& c : cases) {
        CAPTURE(c.form.address, c.form.netmask, c.form.gateway, c.form.dns);
        const auto v = validate_ipv4_form(c.form);
        REQUIRE_FALSE(v.ok());
        REQUIRE(v.problem == c.problem);
        REQUIRE(v.field == c.field);
    }
}

TEST_CASE("validate_ipv4_form at the prefix extremes", "[ipv4]") {
    SECTION("/32: no gateway can be inside the subnet") {
        const auto v = validate_ipv4_form(form("192.168.1.50", "/32", "192.168.1.1", "1.1.1.1"));
        REQUIRE(v.problem == Ipv4Problem::GatewayOutsideSubnet);
        REQUIRE(v.field == Ipv4Field::Gateway);
    }
    SECTION("/31: both addresses are hosts and the other is the gateway") {
        REQUIRE(validate_ipv4_form(form("10.0.0.0", "/31", "10.0.0.1", "1.1.1.1")).ok());
        REQUIRE(validate_ipv4_form(form("10.0.0.1", "/31", "10.0.0.0", "1.1.1.1")).ok());
    }
    SECTION("/30: two hosts between network and broadcast") {
        REQUIRE(validate_ipv4_form(form("10.0.0.1", "/30", "10.0.0.2", "1.1.1.1")).ok());
        REQUIRE(validate_ipv4_form(form("10.0.0.3", "/30", "10.0.0.2", "1.1.1.1")).problem ==
                Ipv4Problem::BroadcastAddress);
    }
    SECTION("/1: a very wide subnet still bounds the gateway") {
        REQUIRE(validate_ipv4_form(form("10.0.0.5", "/1", "100.0.0.1", "1.1.1.1")).ok());
        REQUIRE(validate_ipv4_form(form("10.0.0.5", "/1", "192.168.0.1", "1.1.1.1")).problem ==
                Ipv4Problem::GatewayOutsideSubnet);
    }
}

TEST_CASE("ipv4_form_from_config pre-fills the dotted mask and first DNS", "[ipv4]") {
    const auto f =
        ipv4_form_from_config(static_config("10.1.2.3", 16, "10.1.0.1", {"9.9.9.9", "1.1.1.1"}));
    REQUIRE(f.address == "10.1.2.3");
    REQUIRE(f.netmask == "255.255.0.0");
    REQUIRE(f.gateway == "10.1.0.1");
    REQUIRE(f.dns == "9.9.9.9");

    const auto d = ipv4_form_from_config(Ipv4Config{});
    REQUIRE(d.address.empty());
    REQUIRE(d.netmask.empty());
    REQUIRE(d.dns.empty());
}

TEST_CASE("Ipv4Config equality compares every field", "[ipv4]") {
    const auto a = static_config("10.0.0.2", 24, "10.0.0.1", {"1.1.1.1"});
    REQUIRE(a == a);
    auto b = a;
    b.prefix = 23;
    REQUIRE(a != b);
    b = a;
    b.dns.push_back("8.8.8.8");
    REQUIRE(a != b);
    b = a;
    b.dhcp = true;
    REQUIRE(a != b);
    // Static fields are noise on a DHCP config, but DNS is kept.
    Ipv4Config d1, d2;
    d1.address = "1.2.3.4";
    REQUIRE(d1 == d2);
    d2.dns = {"1.1.1.1"};
    REQUIRE(d1 != d2);
}

TEST_CASE("Ipv4Link names round-trip", "[ipv4]") {
    REQUIRE(ipv4_link_from_name(ipv4_link_name(Ipv4Link::Wifi)) == Ipv4Link::Wifi);
    REQUIRE(ipv4_link_from_name(ipv4_link_name(Ipv4Link::Ethernet)) == Ipv4Link::Ethernet);
    REQUIRE_FALSE(ipv4_link_from_name("bluetooth"));
}

// ============================================================================
// Mock backend store
// ============================================================================

TEST_CASE("MockIpv4Config stores what is applied and can be told to fail", "[ipv4]") {
    unsetenv("HELIX_MOCK_IPV4_APPLY_FAIL");
    MockIpv4Config mock;
    Ipv4Config got;
    REQUIRE(mock.get(got).ok());
    REQUIRE(got.dhcp);
    REQUIRE(mock.static_address().empty());

    const auto s = static_config("192.168.1.50", 24, "192.168.1.1", {"1.1.1.1"});
    REQUIRE(mock.apply(s).ok());
    REQUIRE(mock.get(got).ok());
    REQUIRE(got == s);
    REQUIRE(mock.static_address() == "192.168.1.50");

    setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
    const auto r = mock.apply(Ipv4Config{});
    unsetenv("HELIX_MOCK_IPV4_APPLY_FAIL");
    REQUIRE(r.status == Ipv4Result::Status::Failed);
    REQUIRE(mock.get(got).ok());
    REQUIRE(got == s);
}

// ============================================================================
// NetworkManager
// ============================================================================

TEST_CASE("nm_active_uuid_for_device picks the device's row", "[ipv4]") {
    const std::string out = "aaaa-1111:eth0\n"
                            "bbbb-2222:wlan0\n"
                            "cccc-3333:\n";
    REQUIRE(nm_active_uuid_for_device(out, "wlan0") == "bbbb-2222");
    REQUIRE(nm_active_uuid_for_device(out, "eth0") == "aaaa-1111");
    REQUIRE(nm_active_uuid_for_device(out, "eth1").empty());
    REQUIRE(nm_active_uuid_for_device("", "wlan0").empty());
    REQUIRE(nm_active_uuid_for_device(out, "").empty());
}

TEST_CASE("nm_parse_ipv4 reads manual and auto profiles", "[ipv4]") {
    SECTION("manual") {
        const auto c = nm_parse_ipv4("ipv4.method:manual\n"
                                     "ipv4.addresses:192.168.1.50/24\n"
                                     "ipv4.gateway:192.168.1.1\n"
                                     "ipv4.dns:1.1.1.1,8.8.8.8\n");
        REQUIRE(c);
        REQUIRE(*c == static_config("192.168.1.50", 24, "192.168.1.1", {"1.1.1.1", "8.8.8.8"}));
    }
    SECTION("auto with no DNS, in both empty spellings") {
        const auto c =
            nm_parse_ipv4("ipv4.method:auto\nipv4.addresses:\nipv4.gateway:--\nipv4.dns:\n");
        REQUIRE(c);
        REQUIRE(c->dhcp);
        REQUIRE(c->dns.empty());
    }
    SECTION("auto keeps a manual DNS override") {
        const auto c = nm_parse_ipv4("ipv4.method:auto\nipv4.dns:9.9.9.9\n");
        REQUIRE(c);
        REQUIRE(c->dhcp);
        REQUIRE(c->dns == std::vector<std::string>{"9.9.9.9"});
    }
    SECTION("only the first of several addresses is the config") {
        const auto c = nm_parse_ipv4("ipv4.method:manual\nipv4.addresses:10.0.0.2/8, 10.0.0.3/8\n"
                                     "ipv4.gateway:10.0.0.1\n");
        REQUIRE(c);
        REQUIRE(c->address == "10.0.0.2");
        REQUIRE(c->prefix == 8);
    }
    SECTION("other methods and broken profiles are not a config") {
        REQUIRE_FALSE(nm_parse_ipv4("ipv4.method:shared\n"));
        REQUIRE_FALSE(nm_parse_ipv4("ipv4.method:disabled\n"));
        REQUIRE_FALSE(nm_parse_ipv4("ipv4.method:manual\nipv4.addresses:\n"));
        REQUIRE_FALSE(nm_parse_ipv4("ipv4.method:manual\nipv4.addresses:10.0.0.2\n"));
        REQUIRE_FALSE(nm_parse_ipv4(""));
    }
}

TEST_CASE("nm_ipv4_modify_argv writes manual and auto", "[ipv4]") {
    using V = std::vector<std::string>;
    REQUIRE(nm_ipv4_show_argv("u-1") == V{"-t", "-f",
                                          "ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns",
                                          "connection", "show", "uuid", "u-1"});

    REQUIRE(nm_ipv4_modify_argv(
                "u-1", static_config("192.168.1.50", 24, "192.168.1.1", {"1.1.1.1", "8.8.8.8"})) ==
            V{"connection", "modify", "uuid", "u-1", "ipv4.method", "manual", "ipv4.addresses",
              "192.168.1.50/24", "ipv4.gateway", "192.168.1.1", "ipv4.dns", "1.1.1.1,8.8.8.8"});

    Ipv4Config dhcp;
    dhcp.dns = {"9.9.9.9"};
    REQUIRE(nm_ipv4_modify_argv("u-1", dhcp) == V{"connection", "modify", "uuid", "u-1",
                                                  "ipv4.method", "auto", "ipv4.addresses", "",
                                                  "ipv4.gateway", "", "ipv4.dns", "9.9.9.9"});
}

// ============================================================================
// dhcpcd
// ============================================================================

TEST_CASE("dhcpcd_conf_with_ipv4 appends one marked block", "[ipv4]") {
    const std::string base = "hostname\nclientid\n# a comment\n";
    const auto s = static_config("192.168.1.50", 24, "192.168.1.1", {"1.1.1.1", "8.8.8.8"});
    const std::string out = with_ipv4(base, "wlan0", s);

    REQUIRE(out.rfind(base, 0) == 0);
    REQUIRE(out.find("interface wlan0\n") != std::string::npos);
    REQUIRE(out.find("static ip_address=192.168.1.50/24\n") != std::string::npos);
    REQUIRE(out.find("static routers=192.168.1.1\n") != std::string::npos);
    REQUIRE(out.find("static domain_name_servers=1.1.1.1 8.8.8.8\n") != std::string::npos);
    REQUIRE(read_ipv4(out, "wlan0") == s);

    SECTION("applying again replaces the block rather than adding one") {
        const auto s2 = static_config("192.168.1.60", 24, "192.168.1.1", {"1.1.1.1"});
        const std::string again = with_ipv4(out, "wlan0", s2);
        REQUIRE(again.find("192.168.1.50") == std::string::npos);
        REQUIRE(read_ipv4(again, "wlan0") == s2);
        REQUIRE(again == with_ipv4(base, "wlan0", s2));
    }
    SECTION("DHCP removes the block and restores the file") {
        REQUIRE(with_ipv4(out, "wlan0", Ipv4Config{}) == base);
        REQUIRE(read_ipv4(base, "wlan0").dhcp);
    }
    SECTION("another interface's block is untouched") {
        const auto e = static_config("10.0.0.2", 8, "10.0.0.1", {"10.0.0.1"});
        const std::string both = with_ipv4(out, "eth0", e);
        REQUIRE(read_ipv4(both, "wlan0") == s);
        REQUIRE(read_ipv4(both, "eth0") == e);
        REQUIRE(read_ipv4(with_ipv4(both, "eth0", Ipv4Config{}), "wlan0") == s);
    }
}

TEST_CASE("dhcpcd_conf_with_ipv4 takes over a hand-written stanza", "[ipv4]") {
    const std::string conf = "interface wlan0\n"
                             "static ip_address=10.0.0.5/24\n"
                             "static routers=10.0.0.1\n"
                             "static domain_name_servers=10.0.0.1\n"
                             "metric 300\n"
                             "interface eth0\n"
                             "static ip_address=172.16.0.2/16\n";
    const Ipv4Config read = read_ipv4(conf, "wlan0");
    REQUIRE(read == static_config("10.0.0.5", 24, "10.0.0.1", {"10.0.0.1"}));

    const std::string dhcp = with_ipv4(conf, "wlan0", Ipv4Config{});
    REQUIRE(dhcp == "interface wlan0\n"
                    "metric 300\n"
                    "interface eth0\n"
                    "static ip_address=172.16.0.2/16\n");
    REQUIRE(read_ipv4(dhcp, "wlan0").dhcp);
    REQUIRE_FALSE(read_ipv4(dhcp, "eth0").dhcp);
}

TEST_CASE("dhcpcd_conf_with_ipv4 handles a file without a trailing newline", "[ipv4]") {
    const auto s = static_config("192.168.1.50", 24, "192.168.1.1", {"1.1.1.1"});
    const std::string out = with_ipv4("hostname", "wlan0", s);
    REQUIRE(out.rfind("hostname\n", 0) == 0);
    REQUIRE(read_ipv4(out, "wlan0") == s);
    REQUIRE(with_ipv4("", "wlan0", Ipv4Config{}).empty());
}

// ============================================================================
// Confirm-or-revert
// ============================================================================

TEST_CASE("Ipv4RevertCountdown counts whole seconds on the caller's clock", "[ipv4]") {
    Ipv4RevertCountdown c;
    REQUIRE_FALSE(c.running());
    REQUIRE(c.seconds_left(0) == 0);
    REQUIRE_FALSE(c.expired(0));

    c.start(1000);
    REQUIRE(c.running());
    REQUIRE(c.seconds_left(1000) == kIpv4ConfirmSeconds);
    REQUIRE(c.seconds_left(1001) == kIpv4ConfirmSeconds);
    REQUIRE(c.seconds_left(2000) == kIpv4ConfirmSeconds - 1);
    REQUIRE(c.seconds_left(1000 + 59'001) == 1);
    REQUIRE_FALSE(c.expired(1000 + 59'999));
    REQUIRE(c.expired(1000 + 60'000));
    REQUIRE(c.seconds_left(1000 + 60'000) == 0);

    c.stop();
    REQUIRE_FALSE(c.expired(1000 + 90'000));

    SECTION("a tick wrap mid-countdown is harmless") {
        Ipv4RevertCountdown w;
        w.start(0xFFFFFC18u, 5); // 1000 ms before the wrap
        REQUIRE(w.seconds_left(0xFFFFFFFFu) == 5);
        REQUIRE(w.seconds_left(3000) == 1);
        REQUIRE_FALSE(w.expired(3999));
        REQUIRE(w.expired(4000));
    }
}

TEST_CASE("PendingIpv4Revert encodes and decodes", "[ipv4]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Ethernet;
    p.previous = static_config("10.0.0.2", 8, "10.0.0.1", {"10.0.0.1", "1.1.1.1"});
    p.owner = "run-1";
    p.snapshot = {"networkmanager", "eth0", "u-1", "ipv4.method:manual\n", "w"};
    const auto back = decode_pending_ipv4_revert(encode_pending_ipv4_revert(p));
    REQUIRE(back);
    REQUIRE(back->link == Ipv4Link::Ethernet);
    REQUIRE(back->previous == p.previous);
    REQUIRE(back->owner == "run-1");
    REQUIRE(back->snapshot.profile == "u-1");
    REQUIRE(back->snapshot.iface == "eth0");
    REQUIRE(back->snapshot.original == p.snapshot.original);

    SECTION("configs the form refuses but a system holds come back") {
        for (const auto& held :
             {static_config("10.0.0.5", 24, "", {}), static_config("10.0.0.5", 32, "", {"1.1.1.1"}),
              static_config("10.0.0.5", 24, "192.168.9.1", {})}) {
            p.previous = held;
            const auto again = decode_pending_ipv4_revert(encode_pending_ipv4_revert(p));
            REQUIRE(again);
            CHECK(again->previous == held);
        }
    }
    SECTION("a DNS server that is not an address is no record") {
        p.previous.dns = {"1.1.1.1\nstatic ip_address=6.6.6.6/8"};
        REQUIRE_FALSE(decode_pending_ipv4_revert(encode_pending_ipv4_revert(p)));
    }

    PendingIpv4Revert d;
    d.previous.dns = {"9.9.9.9"};
    const auto dback = decode_pending_ipv4_revert(encode_pending_ipv4_revert(d));
    REQUIRE(dback);
    REQUIRE(dback->link == Ipv4Link::Wifi);
    REQUIRE(dback->previous == d.previous);

    SECTION("anything unreadable is no record") {
        REQUIRE_FALSE(decode_pending_ipv4_revert(""));
        REQUIRE_FALSE(decode_pending_ipv4_revert("{not json"));
        REQUIRE_FALSE(decode_pending_ipv4_revert("[]"));
        REQUIRE_FALSE(decode_pending_ipv4_revert(R"({"link":"bluetooth","dhcp":true})"));
        REQUIRE_FALSE(decode_pending_ipv4_revert(R"({"link":"wifi"})"));
        // A static record that would not validate is not replayed.
        REQUIRE_FALSE(decode_pending_ipv4_revert(
            R"({"link":"wifi","dhcp":false,"address":"nope","prefix":24,"gateway":"","dns":[]})"));
    }
}

TEST_CASE("only another run's record for the link is stale", "[ipv4]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Wifi;
    p.owner = "an earlier run";
    REQUIRE(is_stale_pending_ipv4_revert(p, Ipv4Link::Wifi));
    REQUIRE_FALSE(is_stale_pending_ipv4_revert(p, Ipv4Link::Ethernet));
    REQUIRE_FALSE(is_stale_pending_ipv4_revert(std::nullopt, Ipv4Link::Wifi));

    p.owner = ipv4_process_owner();
    REQUIRE_FALSE(ipv4_process_owner().empty());
    REQUIRE_FALSE(is_stale_pending_ipv4_revert(p, Ipv4Link::Wifi));
}

// ============================================================================
// Restore targets (prestonbrown/helixscreen#1900 review)
// ============================================================================

TEST_CASE("nm_inactive_profile_for_link picks the newest idle profile of the link's type",
          "[ipv4]") {
    const std::string out = "e-old:802-3-ethernet:100:\n"
                            "e-new:802-3-ethernet:300:\n"
                            "e-up:802-3-ethernet:900:eth1\n"
                            "w-1:802-11-wireless:500:\n";
    CHECK(nm_inactive_profile_for_link(out, Ipv4Link::Ethernet) == "e-new");
    CHECK(nm_inactive_profile_for_link(out, Ipv4Link::Wifi) == "w-1");
    CHECK(nm_inactive_profile_for_link("", Ipv4Link::Wifi).empty());
}

TEST_CASE("NetworkManager values restore exactly, lists intact", "[ipv4]") {
    const std::string shown = "ipv4.method:manual\n"
                              "ipv4.addresses:10.0.0.2/8,10.0.0.3/8\n"
                              "ipv4.gateway:--\n"
                              "ipv4.dns:1.1.1.1,9.9.9.9\n";
    const NmIpv4Values v = nm_decode_values(nm_encode_values(nm_parse_ipv4_values(shown)));
    using V = std::vector<std::string>;
    CHECK(nm_ipv4_restore_argv("u-1", v) == V{"connection", "modify", "uuid", "u-1", "ipv4.method",
                                              "manual", "ipv4.addresses", "10.0.0.2/8,10.0.0.3/8",
                                              "ipv4.gateway", "", "ipv4.dns", "1.1.1.1,9.9.9.9"});
}

TEST_CASE("dhcpcd edits refuse what they cannot represent", "[ipv4]") {
    SECTION("a block with no end marker is never cut to the end of the file") {
        const std::string conf = "# HelixScreen static IPv4 begin: wlan0\n"
                                 "interface wlan0\n"
                                 "static ip_address=10.0.0.5/24\n"
                                 "hostname\n";
        CHECK_FALSE(dhcpcd_conf_with_ipv4(conf, "wlan0", Ipv4Config{}));
    }
    SECTION("an address without a prefix, or several, is not a config") {
        CHECK_FALSE(dhcpcd_conf_read_ipv4("interface eth0\nstatic ip_address=10.0.0.5\n", "eth0"));
        CHECK_FALSE(dhcpcd_conf_read_ipv4("interface eth0\nstatic ip_address=10.0.0.5/24\n"
                                          "static ip_address=10.0.0.6/24\n",
                                          "eth0"));
    }
    SECTION("DHCP keeps its DNS override") {
        const auto c =
            dhcpcd_conf_read_ipv4("interface wlan0\nstatic domain_name_servers=9.9.9.9\n", "wlan0");
        REQUIRE(c);
        CHECK(c->dhcp);
        CHECK(c->dns == std::vector<std::string>{"9.9.9.9"});
    }
}

TEST_CASE("ipv4_keep_unshown keeps the DNS servers the form cannot show", "[ipv4]") {
    const auto current = static_config("10.0.0.2", 24, "10.0.0.1", {"1.1.1.1", "9.9.9.9"});
    CHECK(ipv4_keep_unshown(current, static_config("10.0.0.2", 24, "10.0.0.1", {"1.1.1.1"})) ==
          current);
    CHECK(ipv4_keep_unshown(current, static_config("10.0.0.2", 24, "10.0.0.1", {"8.8.8.8"})).dns ==
          std::vector<std::string>{"8.8.8.8"});
}

TEST_CASE("Ipv4RetrySchedule retries every few seconds, then gives up", "[ipv4]") {
    Ipv4RetrySchedule r;
    r.start(1000);
    CHECK_FALSE(r.due(1000 + kIpv4RetryEveryMs - 1));
    CHECK(r.due(1000 + kIpv4RetryEveryMs));
    r.tried(1000 + kIpv4RetryEveryMs);
    CHECK_FALSE(r.due(1000 + kIpv4RetryEveryMs + 1));
    CHECK(r.exhausted(1000 + kIpv4RetryForMs));
    CHECK_FALSE(r.due(1000 + kIpv4RetryForMs + kIpv4RetryEveryMs));
}

TEST_CASE("Ipv4ChangeFlow: Keep makes a change final, everything else reverts", "[ipv4]") {
    using A = Ipv4ChangeFlow::Action;
    const auto was = static_config("10.0.0.2", 24, "10.0.0.1", {"1.1.1.1"});
    const auto next = static_config("10.0.0.9", 24, "10.0.0.1", {"1.1.1.1"});
    Ipv4ChangeFlow f;
    CHECK(f.begin(was, was) == A::Close);
    REQUIRE(f.begin(was, next) == A::Apply);
    CHECK(f.begin(was, next) == A::None); // one change at a time

    SECTION("keep") {
        REQUIRE(f.applied(true, 0) == A::ShowKeepPrompt);
        CHECK(f.keep() == A::Kept);
        CHECK(f.state() == Ipv4ChangeFlow::State::Idle);
        CHECK(f.reject() == A::None);
    }
    SECTION("the countdown runs out") {
        REQUIRE(f.applied(true, 1000) == A::ShowKeepPrompt);
        CHECK(f.tick(1000 + kIpv4ConfirmSeconds * 1000 - 1) == A::None);
        CHECK(f.tick(1000 + kIpv4ConfirmSeconds * 1000) == A::Revert);
        CHECK(f.keep() == A::None); // too late
        CHECK(f.reverted(true) == A::Restored);
    }
    SECTION("Revert now or a dismissal") {
        REQUIRE(f.applied(true, 0) == A::ShowKeepPrompt);
        CHECK(f.reject() == A::Revert);
        CHECK(f.reject() == A::None);
        CHECK(f.reverted(false) == A::RestoreFailed);
        CHECK(f.state() == Ipv4ChangeFlow::State::Idle);
    }
    SECTION("a failed apply reverts") {
        CHECK(f.applied(false, 0) == A::Revert);
    }
    SECTION("an answer after Back still asks Keep") {
        CHECK(f.applied(true, 0) == A::ShowKeepPrompt);
    }
    SECTION("leaving mid-apply hands the revert to the apply") {
        CHECK(f.teardown() == A::None);
        CHECK(f.abandoned()->load());
        CHECK(f.applied(true, 0) == A::None);
    }
    SECTION("a print starting while asked keeps the change") {
        CHECK(f.job_started() == A::None); // still applying
        REQUIRE(f.applied(true, 0) == A::ShowKeepPrompt);
        CHECK(f.job_started() == A::KeptForJob);
        CHECK(f.tick(kIpv4ConfirmSeconds * 1000) == A::None); // no revert after
        CHECK(f.job_started() == A::None);
    }
    SECTION("leaving while asked reverts") {
        REQUIRE(f.applied(true, 0) == A::ShowKeepPrompt);
        CHECK(f.teardown() == A::Revert);
    }
}
