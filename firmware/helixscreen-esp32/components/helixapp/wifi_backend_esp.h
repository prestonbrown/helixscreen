// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 13 — companion header for wifi_backend_esp.cpp. Firmware-local (not
// under include/): the one seam app_boot.cpp needs beyond the shared
// create_platform_wifi_backend() declaration in wifi_backend.h.

#pragma once

namespace helix {

// Gate flag for the esp_wifi hardware bring-up (esp_wifi_init/esp_wifi_start
// and their internal-DRAM allocations). Defaults to CLOSED: a WifiBackendEsp
// constructed and start_async()'d before this is opened (e.g. by
// NetworkWidget's ctor, which runs during build_shell() — well before
// app_boot.cpp's internal-DRAM boot gates clear) gets a harmless
// NOT_INITIALIZED WiFiError instead of running esp_wifi_init on the wrong
// thread/stack, preserving THE PATTERN (Task 8): the only >=32KB internal
// allocation on the net path happens on app_net_start()'s dedicated pthread,
// claimed AFTER the boot's internal-DRAM gates.
//
// app_boot.cpp's net thread calls this once, immediately before nudging the
// (possibly already-constructed) shared WiFiManager to retry — see
// helix::WiFiManager::retry_async() in wifi_manager.cpp, which re-invokes
// backend_->start_async() and this time performs the real bring-up.
void wifi_backend_esp_allow_hardware_bringup();

// Task 14 — clears the persisted "wifi" NVS creds (ssid+psk). Used ONLY by
// provisioning's own self-healing rollback: if a first-boot portal-initiated
// join fails DEFINITIVELY (wrong password, etc.), the creds connect_network()
// just wrote (unconditionally, before the assoc result is known) are erased
// again, so a reboot mid-failed-provisioning re-enters the portal instead of
// silently retrying a known-bad password forever in the background. Keeps
// single-writer discipline: this lives in the same file as nvs_write_creds()
// rather than provisioning_esp.cpp reaching into NVS itself. Do NOT call this
// from the Settings > Network path — a user-driven join failure there keeps
// the existing on-glass retry behavior; only provisioning's own out-of-box
// flow rolls back.
void wifi_backend_esp_clear_stored_credentials();

} // namespace helix
