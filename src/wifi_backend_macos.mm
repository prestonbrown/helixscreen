// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef __APPLE__

#include "wifi_backend_macos.h"

#include "core/log_redact.h"
#include "safe_log.h"

#include <spdlog/spdlog.h>

#import <CoreLocation/CoreLocation.h>
#import <CoreWLAN/CoreWLAN.h>
#import <Foundation/Foundation.h>

// ============================================================================
// Constructor / Destructor
// ============================================================================

WifiBackendMacOS::WifiBackendMacOS()
    : running_(false), wifi_client_(nullptr), interface_(nullptr), scan_timer_(nullptr),
      connect_timer_(nullptr), connection_in_progress_(false) {
    spdlog::debug("[WiFiMacOS] Backend created");
}

WifiBackendMacOS::~WifiBackendMacOS() {
    stop();

    @autoreleasepool {
        if (wifi_client_) {
            CWWiFiClient* client = (__bridge CWWiFiClient*)wifi_client_;
            [client release];
            wifi_client_ = nullptr;
        }
        // Note: interface_ is owned by wifi_client_, no need to release
        interface_ = nullptr;
    }

    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WiFiMacOS] Backend destroyed\n");
}

// ============================================================================
// Lifecycle Management
// ============================================================================

WiFiError WifiBackendMacOS::start() {
    if (running_) {
        spdlog::warn("[WiFiMacOS] Backend already running");
        return WiFiErrorHelper::success();
    }

    spdlog::debug("[WiFiMacOS] Starting CoreWLAN backend");

    // Check system prerequisites
    WiFiError prereq_check = check_system_prerequisites();
    if (!prereq_check.success()) {
        spdlog::error("[WiFiMacOS] System prerequisites check failed: {}",
                      prereq_check.technical_msg);
        return prereq_check;
    }

    @autoreleasepool {
        // Initialize CoreWLAN client
        CWWiFiClient* client = [[CWWiFiClient alloc] init];
        if (!client) {
            spdlog::error("[WiFiMacOS] Failed to create CWWiFiClient");
            return WiFiError(WiFiResult::BACKEND_ERROR, "Failed to create CWWiFiClient",
                             "WiFi system initialization failed", "Restart the application");
        }
        wifi_client_ = (__bridge void*)client;

        // Get primary WiFi interface
        CWInterface* iface = [client interface];
        if (!iface) {
            spdlog::error("[WiFiMacOS] No WiFi interface found");
            [client release];
            wifi_client_ = nullptr;
            return WiFiErrorHelper::hardware_not_available();
        }
        interface_ = (__bridge void*)iface;

        NSString* ifaceName = [iface interfaceName];
        spdlog::info("[WiFiMacOS] Using interface: {}",
                     ifaceName ? [ifaceName UTF8String] : "unknown");
    }

    running_ = true;

    // Resolve 5GHz support from hardware capabilities
    resolve_5ghz_support();

    spdlog::info("[WiFiMacOS] CoreWLAN backend started successfully");

    return WiFiErrorHelper::success();
}

void WifiBackendMacOS::stop() {
    if (!running_) {
        return;
    }

    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WiFiMacOS] Stopping CoreWLAN backend\n");

    // Cancel any pending timers
    // Check lv_is_initialized() to avoid crash during static destruction
    if (lv_is_initialized()) {
        if (scan_timer_) {
            lv_timer_delete(scan_timer_);
            scan_timer_ = nullptr;
        }
        if (connect_timer_) {
            lv_timer_delete(connect_timer_);
            connect_timer_ = nullptr;
        }
    }

    running_ = false;
    connection_in_progress_ = false;

    fprintf(stderr, "[WiFiMacOS] CoreWLAN backend stopped\n");
}

bool WifiBackendMacOS::is_running() const {
    return running_;
}

// ============================================================================
// Network Scanning
// ============================================================================

WiFiError WifiBackendMacOS::trigger_scan() {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not initialized", "");
    }

    spdlog::info("[WiFiMacOS] Triggering network scan");

    // Cancel any existing scan timer
    if (scan_timer_) {
        lv_timer_delete(scan_timer_);
        scan_timer_ = nullptr;
    }

    // Perform scan in background (simulate async with timer)
    // Use lambda capture to avoid accessing private timer fields
    scan_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            // Get backend pointer from timer user_data (set below)
            WifiBackendMacOS* backend =
                static_cast<WifiBackendMacOS*>(lv_timer_get_user_data(timer));
            if (backend) {
                backend->scan_timer_callback(timer);
            }
        },
        100, nullptr);

    if (scan_timer_) {
        lv_timer_set_user_data(scan_timer_, this);
        lv_timer_set_repeat_count(scan_timer_, 1); // Fire once
    }

    return WiFiErrorHelper::success();
}

void WifiBackendMacOS::scan_timer_callback([[maybe_unused]] lv_timer_t* timer) {
    scan_timer_ = nullptr; // Timer will be auto-deleted

    @autoreleasepool {
        CWInterface* iface = (__bridge CWInterface*)interface_;
        if (!iface) {
            spdlog::error("[WiFiMacOS] No WiFi interface available for scan");
            dispatch_event("SCAN_COMPLETE", "");
            return;
        }

        NSError* error = nil;
        NSSet<CWNetwork*>* networks = [iface scanForNetworksWithSSID:nil error:&error];

        if (error) {
            spdlog::error("[WiFiMacOS] Scan failed: {}", [[error localizedDescription] UTF8String]);
            dispatch_event("SCAN_COMPLETE", "");
            return;
        }

        // Convert CoreWLAN networks to our format
        std::vector<WiFiNetwork> discovered;
        for (CWNetwork* network in networks) {
            NSString* ssid = [network ssid];
            if (!ssid || [ssid length] == 0) {
                continue; // Skip hidden networks
            }

            WiFiNetwork wifi_net;
            wifi_net.ssid = [ssid UTF8String];
            wifi_net.signal_strength =
                wifi_signal_percent_from_dbm(static_cast<int>([network rssiValue]));

            // Determine security type
            bool is_secured = false;
            wifi_net.security_type = extract_security_type((__bridge void*)network, is_secured);
            wifi_net.is_secured = is_secured;

            // CoreWLAN reports the band directly; there is no centre frequency
            // on CWChannel, so frequency_mhz stays 0 and only the band is known.
            CWChannel* channel = [network wlanChannel];
            if (channel) {
                if ([channel channelBand] == kCWChannelBand5GHz) {
                    wifi_net.band_mask = WIFI_BAND_5GHZ;
                } else if ([channel channelBand] == kCWChannelBand2GHz) {
                    wifi_net.band_mask = WIFI_BAND_2_4GHZ;
                }
            }

            discovered.push_back(wifi_net);
        }

        // Collapse per-BSS rows to one per SSID, merging their bands, so the
        // dev host behaves like a device (helixscreen#1189).
        discovered = wifi_merge_networks_by_ssid(discovered);

        // Sort by signal strength (strongest first)
        std::sort(discovered.begin(), discovered.end(),
                  [](const WiFiNetwork& a, const WiFiNetwork& b) {
                      return a.signal_strength > b.signal_strength;
                  });

        // Cache results
        {
            std::lock_guard<std::mutex> lock(networks_mutex_);
            cached_networks_ = discovered;
        }

        spdlog::info("[WiFiMacOS] Scan complete: {} networks found", discovered.size());
        dispatch_event("SCAN_COMPLETE", "");
    }
}

WiFiError WifiBackendMacOS::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not initialized", "");
    }

    std::lock_guard<std::mutex> lock(networks_mutex_);
    networks = cached_networks_;

    spdlog::debug("[WiFiMacOS] Returning {} cached scan results", networks.size());
    return WiFiErrorHelper::success();
}

// ============================================================================
// Connection Management
// ============================================================================

WiFiError WifiBackendMacOS::connect_network(const std::string& ssid, const std::string& password,
                                            bool) {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not initialized", "");
    }

    if (connection_in_progress_) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "Connection already in progress",
                         "Please wait for current connection to complete", "");
    }

    spdlog::info("[WiFiMacOS] Connecting to network: {}", helix::redact::ssid(ssid));

    connecting_ssid_ = ssid;
    connecting_password_ = password;
    connection_in_progress_ = true;

    // Cancel any existing connect timer
    if (connect_timer_) {
        lv_timer_delete(connect_timer_);
        connect_timer_ = nullptr;
    }

    // Perform connection in background (use timer with public API)
    connect_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            WifiBackendMacOS* backend =
                static_cast<WifiBackendMacOS*>(lv_timer_get_user_data(timer));
            if (backend) {
                backend->connect_timer_callback(timer);
            }
        },
        100, nullptr);

    if (connect_timer_) {
        lv_timer_set_user_data(connect_timer_, this);
        lv_timer_set_repeat_count(connect_timer_, 1); // Fire once
    }

    return WiFiErrorHelper::success();
}

void WifiBackendMacOS::connect_timer_callback([[maybe_unused]] lv_timer_t* timer) {
    connect_timer_ = nullptr; // Timer will be auto-deleted

    @autoreleasepool {
        CWInterface* iface = (__bridge CWInterface*)interface_;
        if (!iface) {
            spdlog::error("[WiFiMacOS] No WiFi interface available for connection");
            connection_in_progress_ = false;
            dispatch_event("DISCONNECTED", "");
            return;
        }

        // Find the network in scan results
        NSString* targetSSID = [NSString stringWithUTF8String:connecting_ssid_.c_str()];
        NSError* error = nil;
        NSData* ssidData = [targetSSID dataUsingEncoding:NSUTF8StringEncoding];
        NSSet<CWNetwork*>* networks = [iface scanForNetworksWithSSID:ssidData error:&error];

        if (error || !networks || [networks count] == 0) {
            spdlog::error("[WiFiMacOS] Network not found: {}",
                          helix::redact::ssid(connecting_ssid_));
            connection_in_progress_ = false;
            dispatch_event("DISCONNECTED", connecting_ssid_);
            return;
        }

        // Get the first matching network
        CWNetwork* network = [[networks allObjects] firstObject];

        // Prepare password if needed
        NSString* password_ns = nil;
        if (!connecting_password_.empty()) {
            password_ns = [NSString stringWithUTF8String:connecting_password_.c_str()];
        }

        // Attempt connection
        BOOL success = [iface associateToNetwork:network password:password_ns error:&error];

        if (success) {
            spdlog::info("[WiFiMacOS] Successfully connected to: {}",
                         helix::redact::ssid(connecting_ssid_));
            dispatch_event("CONNECTED", connecting_ssid_);
        } else {
            spdlog::error("[WiFiMacOS] Connection failed: {}",
                          error ? [[error localizedDescription] UTF8String] : "unknown error");
            dispatch_event("AUTH_FAILED", connecting_ssid_);
        }

        connection_in_progress_ = false;
    }
}

WiFiError WifiBackendMacOS::disconnect_network() {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not initialized", "");
    }

    @autoreleasepool {
        CWInterface* iface = (__bridge CWInterface*)interface_;
        if (!iface) {
            return WiFiError(WiFiResult::BACKEND_ERROR, "No WiFi interface",
                             "WiFi interface unavailable", "");
        }

        [iface disassociate];
        spdlog::info("[WiFiMacOS] Disconnected from network");
        dispatch_event("DISCONNECTED", "");
    }

    return WiFiErrorHelper::success();
}

// ============================================================================
// Radio Power
// ============================================================================

WiFiError WifiBackendMacOS::set_radio_enabled(bool on) {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not initialized", "");
    }

    @autoreleasepool {
        CWInterface* iface = (__bridge CWInterface*)interface_;
        if (!iface) {
            return WiFiError(WiFiResult::BACKEND_ERROR, "No WiFi interface",
                             "WiFi interface unavailable", "");
        }

        // -setPower: is CoreWLAN's own soft radio switch: association stops and
        // the interface stays present, which is what WifiBackend requires.
        NSError* err = nil;
        if (![iface setPower:(on ? YES : NO) error:&err]) {
            std::string detail = err ? std::string([[err localizedDescription] UTF8String])
                                     : std::string("setPower failed");
            spdlog::warn("[WiFiMacOS] setPower:{} failed: {}", on, detail);
            return WiFiError(WiFiResult::PERMISSION_DENIED, "CoreWLAN setPower failed: " + detail,
                             on ? "Could not turn WiFi radio on" : "Could not turn WiFi radio off");
        }

        spdlog::info("[WiFiMacOS] Radio {}", on ? "enabled" : "disabled");
    }

    return WiFiErrorHelper::success();
}

bool WifiBackendMacOS::is_radio_enabled() const {
    if (!interface_) {
        // Nothing to ask. Report off rather than inheriting the base class's
        // unconditional true, which would let the UI claim a radio it cannot
        // see is on.
        return false;
    }

    @autoreleasepool {
        CWInterface* iface = (__bridge CWInterface*)interface_;
        return [iface powerOn] == YES;
    }
}

// ============================================================================
// Status Queries
// ============================================================================

WifiBackend::ConnectionStatus WifiBackendMacOS::get_status() {
    ConnectionStatus status;
    status.connected = false;
    status.signal_strength = 0;

    if (!running_ || !interface_) {
        return status;
    }

    @autoreleasepool {
        CWInterface* iface = (__bridge CWInterface*)interface_;

        // Check if we're associated with a network
        if ([iface serviceActive]) {
            status.connected = true;

            NSString* ssid = [iface ssid];
            if (ssid) {
                status.ssid = [ssid UTF8String];
            }

            NSString* bssid = [iface bssid];
            if (bssid) {
                status.bssid = [bssid UTF8String];
            }

            status.signal_strength =
                wifi_signal_percent_from_dbm(static_cast<int>([iface rssiValue]));

            // macOS is a development host only, never a deployment target, so
            // the address stays blank rather than growing a getifaddrs() walk
            // no user would ever see.
            status.ip_address = "";
        }
    }

    return status;
}

bool WifiBackendMacOS::supports_5ghz() const {
    return supports_5ghz_cached_;
}

void WifiBackendMacOS::resolve_5ghz_support() {
    @try {
        @autoreleasepool {
            CWInterface* iface = [(__bridge CWWiFiClient*)wifi_client_ interface];
            if (!iface) {
                spdlog::warn("[WiFiMacOS] No interface for 5GHz detection, defaulting to true");
                return;
            }

            NSSet<CWChannel*>* channels = [iface supportedWLANChannels];
            if (!channels || [channels count] == 0) {
                spdlog::warn("[WiFiMacOS] No supported channels reported, defaulting to true");
                return;
            }

            bool has_5ghz = false;
            for (CWChannel* channel in channels) {
                if ([channel channelBand] == kCWChannelBand5GHz) {
                    has_5ghz = true;
                    break;
                }
            }

            supports_5ghz_cached_ = has_5ghz;
            spdlog::debug("[WiFiMacOS] 5GHz support: {} ({} channels queried)",
                          has_5ghz ? "yes" : "no", [channels count]);
        }
    } @catch (NSException* exception) {
        spdlog::warn("[WiFiMacOS] Exception querying 5GHz support: {}, defaulting to true",
                     [[exception reason] UTF8String]);
        supports_5ghz_cached_ = true;
    }
}

// ============================================================================
// System Validation
// ============================================================================

WiFiError WifiBackendMacOS::check_system_prerequisites() {
    // Check WiFi hardware
    WiFiError hw_check = check_wifi_hardware();
    if (!hw_check.success()) {
        return hw_check;
    }

    // Check location permission (required on macOS 10.15+)
    WiFiError perm_check = check_location_permission();
    if (!perm_check.success()) {
        // Location permission required for WiFi scanning - fail startup
        return perm_check;
    }

    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMacOS::check_wifi_hardware() {
    @autoreleasepool {
        CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
        CWInterface* iface = [client interface];

        if (!iface) {
            return WiFiErrorHelper::hardware_not_available();
        }

        NSString* ifaceName = [iface interfaceName];
        spdlog::debug("[WiFiMacOS] WiFi interface found: {}",
                      ifaceName ? [ifaceName UTF8String] : "unknown");
    }

    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMacOS::check_location_permission() {
    @autoreleasepool {
// Use class method (deprecated but compatible with macOS 10.15)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CLAuthorizationStatus status = [CLLocationManager authorizationStatus];
#pragma clang diagnostic pop

        switch (status) {
        case kCLAuthorizationStatusAuthorizedAlways:
            spdlog::debug("[WiFiMacOS] Location permission granted");
            return WiFiErrorHelper::success();

        case kCLAuthorizationStatusNotDetermined:
            spdlog::warn("[WiFiMacOS] Location permission not determined");
            spdlog::warn("[WiFiMacOS] Grant location permission: System Preferences → Security & "
                         "Privacy → Location Services");
            return WiFiError(WiFiResult::PERMISSION_DENIED, "Location permission not determined",
                             "Location access required for WiFi scanning",
                             "Grant location permission in System Preferences");

        case kCLAuthorizationStatusDenied:
        case kCLAuthorizationStatusRestricted:
            spdlog::warn("[WiFiMacOS] Location permission denied/restricted");
            spdlog::warn("[WiFiMacOS] Grant location permission: System Preferences → Security & "
                         "Privacy → Location Services");
            return WiFiError(WiFiResult::PERMISSION_DENIED, "Location permission denied",
                             "Location access required for WiFi scanning",
                             "Grant location permission in System Preferences > Privacy");

        default:
            return WiFiErrorHelper::success();
        }
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string WifiBackendMacOS::extract_security_type(void* network_ptr, bool& is_secured) {
    @autoreleasepool {
        CWNetwork* network = (__bridge CWNetwork*)network_ptr;

        // Check if network is open
        if (![network supportsSecurity:kCWSecurityWPAPersonal] &&
            ![network supportsSecurity:kCWSecurityWPA2Personal] &&
            ![network supportsSecurity:kCWSecurityWPA3Personal] &&
            ![network supportsSecurity:kCWSecurityWEP]) {
            is_secured = false;
            return "Open";
        }

        is_secured = true;

        // Determine specific security type
        if ([network supportsSecurity:kCWSecurityWPA3Personal]) {
            return "WPA3";
        }
        if ([network supportsSecurity:kCWSecurityWPA2Personal]) {
            return "WPA2";
        }
        if ([network supportsSecurity:kCWSecurityWPAPersonal]) {
            return "WPA";
        }
        if ([network supportsSecurity:kCWSecurityWEP]) {
            return "WEP";
        }

        return "Secured"; // Unknown security type but requires password
    }
}

#endif // __APPLE__
