// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 14 — first-boot SoftAP captive-portal WiFi provisioning. Activates only
// when Task 13's NVS "wifi" namespace has never held an SSID (out-of-box /
// factory-reset case). app_boot.cpp's app_net_thread_main() calls these two
// entry points, in order:
//
//   1. provisioning_needed() — read-only NVS check. Does not touch the radio.
//   2. provisioning_run_portal() — blocking: stands up a SoftAP + captive
//      portal ("HelixScreen-XXXX", MAC-derived), shows on-screen instructions
//      via an info modal, and waits until either a successful STA join lands
//      (through the shared WiFiManager — Task 13's one credential writer) or
//      the user dismisses to fall back on Settings > Network. Either way the
//      AP/httpd/DNS are torn down before returning.
//
// Placed in helixapp (not main/) so app_net_thread_main() — the natural call
// site — can #include it directly: main/ depends on helixapp, not the other
// way around, and helixapp already owns the Task 13 WifiBackend this reuses.
//
// Compiled only for !CONFIG_HELIX_MOCK_PRINTER builds; mock builds have no
// radio and never call these (both are cheap inert stubs there regardless).

#pragma once

namespace helix {

bool provisioning_needed();
bool provisioning_run_portal();

} // namespace helix
