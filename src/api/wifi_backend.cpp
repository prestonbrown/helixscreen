// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend.h"

#include "runtime_config.h"
#include "spdlog/spdlog.h"

#include <cctype>
#include <unistd.h>
#include <unordered_map>
#ifdef HELIX_ENABLE_MOCKS
#include "wifi_backend_mock.h"
#endif

#ifdef __APPLE__
#include "wifi_backend_macos.h"
#elif defined(ESP_PLATFORM)
// esp_wifi backend lives in the firmware tree; only the declaration in
// wifi_backend.h is needed here.
#elif !defined(__ANDROID__)
#include "wifi_backend_networkmanager.h"
#include "wifi_backend_wpa_supplicant.h"
#endif

uint8_t wifi_band_flag_from_frequency(int frequency_mhz) {
    // Channel 1 (2412) through channel 14 (2484), with slack for the channel width.
    if (frequency_mhz >= 2400 && frequency_mhz <= 2500) {
        return WIFI_BAND_2_4GHZ;
    }
    // 4900-5000 is the Japanese/public-safety 4.9GHz allocation, reported by
    // wpa_supplicant as part of the 5GHz band; 5150-5895 covers UNII-1..UNII-4.
    if (frequency_mhz >= 4900 && frequency_mhz <= 5895) {
        return WIFI_BAND_5GHZ;
    }
    // Wi-Fi 6E: 5925-7125 MHz.
    if (frequency_mhz >= 5925 && frequency_mhz <= 7125) {
        return WIFI_BAND_6GHZ;
    }
    return WIFI_BAND_NONE;
}

std::optional<bool> wifi_parse_nm_radio_state(const std::string& output) {
    // Trim whitespace and lowercase — nmcli pads non-terse output and always
    // appends a newline.
    const std::string ws = " \t\r\n";
    const size_t first = output.find_first_not_of(ws);
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const size_t last = output.find_last_not_of(ws);
    std::string word = output.substr(first, last - first + 1);
    for (char& c : word) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    if (word == "enabled") {
        return true;
    }
    if (word == "disabled") {
        return false;
    }
    // "missing", "unavailable", an error string, or empty: inconclusive.
    return std::nullopt;
}

std::vector<WiFiNetwork> wifi_merge_networks_by_ssid(const std::vector<WiFiNetwork>& networks) {
    // First-seen order keeps the output deterministic; ssid -> index into `kept`.
    std::unordered_map<std::string, size_t> slot_by_ssid;
    std::vector<WiFiNetwork> kept;
    kept.reserve(networks.size());

    for (const auto& net : networks) {
        auto [it, inserted] = slot_by_ssid.try_emplace(net.ssid, kept.size());
        if (inserted) {
            kept.push_back(net);
            continue;
        }

        WiFiNetwork& existing = kept[it->second];
        // The losing BSS still contributes its band: this is the whole point of
        // the merge. Without it a 5GHz twin simply disappears (helixscreen#1189).
        uint8_t merged_bands = static_cast<uint8_t>(existing.band_mask | net.band_mask);
        if (net.signal_strength > existing.signal_strength) {
            existing = net;
        }
        existing.band_mask = merged_bands;
    }

    return kept;
}

std::unique_ptr<WifiBackend> WifiBackend::create(bool silent) {
    // In test mode, always use mock unless --real-wifi was specified
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_wifi()) {
        spdlog::debug("[WifiBackend] Test mode: using mock backend");
        auto mock = std::make_unique<WifiBackendMock>();
        mock->set_silent(silent);
        // Non-blocking: mock fires READY immediately from start_async().
        // We intentionally do NOT call it here — the test case explicitly
        // invokes start_async() after registering its READY callback. For
        // production callers, WiFiManager will call start_async() after
        // registering its own event handlers.
        return mock;
    }
#endif

#ifdef __APPLE__
    // macOS: Construct CoreWLAN backend and return immediately. The
    // base-class default start_async() falls back to a synchronous start()
    // — acceptable on macOS until we port the async pattern there.
    spdlog::debug("[WifiBackend] Constructing CoreWLAN backend for macOS");
    auto backend = std::make_unique<WifiBackendMacOS>();
    backend->set_silent(silent);
    return backend;
#elif defined(ESP_PLATFORM)
    // Embedded: the platform tree owns the backend (esp_wifi).
    return helix::create_platform_wifi_backend(silent);
#elif defined(__ANDROID__)
    // Android: WiFi managed by the OS, not by us
    spdlog::info("[WifiBackend] Android platform - WiFi not managed natively");
    return nullptr;
#else
    // Linux: pick between NetworkManager and wpa_supplicant using CHEAP
    // probes (no subprocess, no socket I/O). The actual initialization
    // happens in the caller via start_async().
    //
    // Committing to NM requires BOTH the nmcli binary AND a liveness signal
    // for the NM daemon — its runtime directory. Vanilla Pi OS (and
    // Klipper-derived images like RatOS) ship nmcli in the base image but
    // run dhcpcd + wpa_supplicant as the active network stack, so binary
    // presence alone is not sufficient. NM creates /run/NetworkManager when
    // active and removes it when masked/stopped, making a filesystem probe
    // a reliable proxy without forking a subprocess.
    const bool has_nmcli = (access("/usr/bin/nmcli", X_OK) == 0) ||
                           (access("/bin/nmcli", X_OK) == 0) ||
                           (access("/usr/local/bin/nmcli", X_OK) == 0);
    const bool nm_daemon_active = (access("/run/NetworkManager", F_OK) == 0) ||
                                  (access("/var/run/NetworkManager", F_OK) == 0);

    if (has_nmcli && nm_daemon_active) {
        spdlog::debug(
            "[WifiBackend] Selecting NetworkManager backend (nmcli + /run/NetworkManager){}",
            silent ? " (silent)" : "");
        auto backend = std::make_unique<WifiBackendNetworkManager>();
        backend->set_silent(silent);
        return backend;
    }

    // Default to wpa_supplicant: either nmcli is missing, or it's installed
    // but the NM daemon is inactive (the common Pi / RatOS case). Going
    // straight here avoids the NM→wpa async fallback, which silently fails
    // when the shared WiFiManager is constructed with silent=true.
    spdlog::debug("[WifiBackend] Selecting wpa_supplicant backend "
                  "(nmcli={}, nm_daemon_active={}){}",
                  has_nmcli, nm_daemon_active, silent ? " (silent)" : "");
    auto backend = std::make_unique<WifiBackendWpaSupplicant>();
    backend->set_silent(silent);
    return backend;
#endif
}
