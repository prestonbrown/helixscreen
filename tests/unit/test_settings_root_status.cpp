// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_root_status.h"

#include "../catch_amalgamated.hpp"

using namespace helix::settings;

TEST_CASE("status::display", "[settings][root_status]") {
    CHECK(status::display(80, 600, true) == "80% · sleep 10 min");
    CHECK(status::display(80, 0, true) == "80% · never sleeps");
    CHECK(status::display(100, 60, false) == "Sleep 1 min");
    CHECK(status::display(100, 0, false) == "Never sleeps");
}

TEST_CASE("status::appearance", "[settings][root_status]") {
    CHECK(status::appearance(true, "Ocean") == "Dark Mode · Ocean");
    CHECK(status::appearance(false, "Ocean") == "Light Mode · Ocean");
    CHECK(status::appearance(true, "") == "Dark Mode");
}

TEST_CASE("status::sound", "[settings][root_status]") {
    CHECK(status::sound(true, 60) == "Volume 60%");
    CHECK(status::sound(true, 0) == "Muted");
    CHECK(status::sound(false, 60) == "Muted");
}

TEST_CASE("status::devices", "[settings][root_status]") {
    CHECK(status::devices(0) == "All healthy");
    CHECK(status::devices(1) == "Needs attention");
    CHECK(status::devices(2) == "Problem found");
    CHECK(status::devices(7) == "All healthy");
}

TEST_CASE("status::connection", "[settings][root_status][wifi_dhcp_pending]") {
    using helix::ui::wifi::LinkPhase;
    CHECK(status::connection(true, LinkPhase::Online, "HomeNet") == "Ethernet");
    CHECK(status::connection(true, LinkPhase::GettingAddress, "HomeNet") == "Ethernet");
    CHECK(status::connection(false, LinkPhase::Online, "HomeNet") == "Wi-Fi HomeNet");
    CHECK(status::connection(false, LinkPhase::Online, "") == "Wi-Fi");
    CHECK(status::connection(false, LinkPhase::GettingAddress, "HomeNet") ==
          "Connected, getting address…");
    CHECK(status::connection(false, LinkPhase::Disconnected, "HomeNet") == "Not connected");
}

TEST_CASE("status::wifi_address", "[settings][root_status][wifi_dhcp_pending]") {
    using helix::ui::wifi::LinkPhase;
    CHECK(status::wifi_address(LinkPhase::Online, "10.0.0.5") == "10.0.0.5");
    CHECK(status::wifi_address(LinkPhase::GettingAddress, "") == "Getting address…");
    CHECK(status::wifi_address(LinkPhase::Disconnected, "") == "");
}

TEST_CASE("wifi::link_phase classifies a status", "[wifi][wifi_dhcp_pending]") {
    using helix::ui::wifi::link_phase;
    using helix::ui::wifi::LinkPhase;
    WifiBackend::ConnectionStatus s;
    CHECK(link_phase(s) == LinkPhase::Disconnected);
    s.connected = true;
    s.ip_address = "10.0.0.5";
    CHECK(link_phase(s) == LinkPhase::Online);
    s.ip_address.clear();
    s.awaiting_ipv4 = true;
    CHECK(link_phase(s) == LinkPhase::GettingAddress);
    // A stale flag on a dropped link is not a pending address.
    s.connected = false;
    CHECK(link_phase(s) == LinkPhase::Disconnected);
}

TEST_CASE("status::language_time", "[settings][root_status]") {
    CHECK(status::language_time("English", 1) == "English · 24-hour");
    CHECK(status::language_time("Deutsch", 0) == "Deutsch · 12-hour");
}

TEST_CASE("status::updates", "[settings][root_status]") {
    CHECK(status::updates(2, "1.1.1", "1.1.0", false) == "1.1.1 available");
    CHECK(status::updates(3, "", "1.1.0", false) == "Up to date");
    CHECK(status::updates(1, "", "1.1.0", false) == "Checking…");
    CHECK(status::updates(4, "", "1.1.0", false) == "Check failed");
    CHECK(status::updates(0, "", "1.1.0", false) == "Version 1.1.0");
    CHECK(status::updates(2, "1.1.1", "1.1.0", true) == "Managed by firmware");
}
