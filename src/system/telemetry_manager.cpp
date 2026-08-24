// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/telemetry_manager.h"

#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "accel_sensor_manager.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "audio_settings_manager.h"
#include "color_sensor_manager.h"
#include "config.h"
#include "display_backend.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "filament_sensor_manager.h"
#include "helix_plugin_installer.h"
#include "host_identity.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "moonraker_client.h"
#include "moonraker_types.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "platform_capabilities.h"
#include "printer_state.h"
#include "screen_locality.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "temperature_sensor_manager.h"
#include "theme_loader.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

// glibc ptmalloc introspection for heap-fragmentation telemetry (#758 class).
// mallinfo2() (glibc 2.33+) returns size_t fields — preferred.
// mallinfo() (older glibc) returns ints that wrap above 2 GB, which is fine on
// the small-RAM devices we're actually chasing.
#if defined(__GLIBC__)
#include <malloc.h>
#if defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 33)
#define HELIX_HAS_MALLINFO2 1
#else
#define HELIX_HAS_MALLINFO 1
#endif
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace helix;

namespace {
/// Strip instance suffix for aggregation (e.g., "favorite_macro:2" -> "favorite_macro")
std::string strip_instance_suffix(const std::string& id) {
    auto colon = id.find(':');
    return (colon != std::string::npos) ? id.substr(0, colon) : id;
}
} // namespace

namespace helix {
namespace telemetry_context {
std::atomic<int> print_state_int{-1};
std::atomic<int> active_panel_int{-1};
std::atomic<bool> gcode_renderer_loaded{false};
} // namespace telemetry_context
} // namespace helix

// =============================================================================
// SHA-256 implementation
// =============================================================================

#ifdef __APPLE__

static std::string sha256_hex(const std::string& input) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), hash);

    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(hex, CC_SHA256_DIGEST_LENGTH * 2);
}

#else

// Minimal portable SHA-256 implementation (public domain)
// Based on RFC 6234 / FIPS 180-4

namespace {

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t gamma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t gamma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    unsigned char buf[64];
};

static void sha256_init(Sha256Ctx& ctx) {
    ctx.state[0] = 0x6a09e667;
    ctx.state[1] = 0xbb67ae85;
    ctx.state[2] = 0x3c6ef372;
    ctx.state[3] = 0xa54ff53a;
    ctx.state[4] = 0x510e527f;
    ctx.state[5] = 0x9b05688c;
    ctx.state[6] = 0x1f83d9ab;
    ctx.state[7] = 0x5be0cd19;
    ctx.count = 0;
}

static void sha256_transform(uint32_t state[8], const unsigned char block[64]) {
    uint32_t W[64];
    for (int i = 0; i < 16; ++i) {
        W[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        W[i] = gamma1(W[i - 2]) + W[i - 7] + gamma0(W[i - 15]) + W[i - 16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K256[i] + W[i];
        uint32_t t2 = sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256_update(Sha256Ctx& ctx, const unsigned char* data, size_t len) {
    size_t buf_len = static_cast<size_t>(ctx.count % 64);
    ctx.count += len;

    // Fill existing buffer first
    if (buf_len > 0) {
        size_t fill = 64 - buf_len;
        if (len < fill) {
            std::memcpy(ctx.buf + buf_len, data, len);
            return;
        }
        std::memcpy(ctx.buf + buf_len, data, fill);
        sha256_transform(ctx.state, ctx.buf);
        data += fill;
        len -= fill;
    }

    // Process full blocks
    while (len >= 64) {
        sha256_transform(ctx.state, data);
        data += 64;
        len -= 64;
    }

    // Buffer remaining
    if (len > 0) {
        std::memcpy(ctx.buf, data, len);
    }
}

static void sha256_final(Sha256Ctx& ctx, unsigned char hash[32]) {
    uint64_t total_bits = ctx.count * 8;
    size_t buf_len = static_cast<size_t>(ctx.count % 64);

    // Padding
    ctx.buf[buf_len++] = 0x80;
    if (buf_len > 56) {
        std::memset(ctx.buf + buf_len, 0, 64 - buf_len);
        sha256_transform(ctx.state, ctx.buf);
        buf_len = 0;
    }
    std::memset(ctx.buf + buf_len, 0, 56 - buf_len);

    // Append length in bits (big-endian)
    for (int i = 0; i < 8; ++i) {
        ctx.buf[56 + i] = static_cast<unsigned char>(total_bits >> (56 - i * 8));
    }
    sha256_transform(ctx.state, ctx.buf);

    // Output hash (big-endian)
    for (int i = 0; i < 8; ++i) {
        hash[i * 4] = static_cast<unsigned char>(ctx.state[i] >> 24);
        hash[i * 4 + 1] = static_cast<unsigned char>(ctx.state[i] >> 16);
        hash[i * 4 + 2] = static_cast<unsigned char>(ctx.state[i] >> 8);
        hash[i * 4 + 3] = static_cast<unsigned char>(ctx.state[i]);
    }
}

} // anonymous namespace

static std::string sha256_hex(const std::string& input) {
    Sha256Ctx ctx;
    sha256_init(ctx);
    sha256_update(ctx, reinterpret_cast<const unsigned char*>(input.data()), input.size());

    unsigned char hash[32];
    sha256_final(ctx, hash);

    char hex[65];
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(hex, 64);
}

#endif // !__APPLE__

// =============================================================================
// Singleton
// =============================================================================

TelemetryManager& TelemetryManager::instance() {
    static TelemetryManager inst;
    return inst;
}

std::optional<bool>
TelemetryManager::classify_moonraker_locality(const std::string& websocket_url) {
    if (websocket_url.empty()) {
        return std::nullopt;
    }

    const std::string host = helix::extract_host_from_websocket_url(websocket_url);
    if (host.empty()) {
        return std::nullopt;
    }

    return helix::is_moonraker_on_same_host(host);
}

TelemetryManager::~TelemetryManager() {
    if (initialized_.load()) {
        shutdown();
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

void TelemetryManager::init(const std::string& config_dir) {
    if (initialized_.load()) {
        spdlog::debug("[TelemetryManager] Already initialized, skipping");
        return;
    }

    spdlog::info("[TelemetryManager] Initializing with config dir: {}", config_dir);

    config_dir_ = config_dir;

    // Reset in-memory state for clean initialization
    enabled_.store(false);
    shutting_down_.store(false);
    had_update_restart_ = false;
    crash_event_queued_ = false;
    init_time_ = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    // Reset session trackers
    panel_time_sec_.clear();
    panel_visits_.clear();
    current_panel_.clear();
    overlay_open_count_ = 0;
    overlay_visits_.clear();
    connect_count_ = 0;
    disconnect_count_ = 0;
    total_connected_sec_ = 0;
    total_disconnected_sec_ = 0;
    longest_disconnect_sec_ = 0;
    klippy_error_count_ = 0;
    klippy_shutdown_count_ = 0;
    connection_tracking_connected_ = false;
    error_rate_limit_.clear();
    snapshot_seq_ = 0;
    frame_ring_count_ = 0;
    frame_ring_idx_ = 0;
    panel_names_.clear();
    current_panel_id_ = 0;
    pending_settings_changes_.clear();
    is_shutdown_snapshot_ = false;

    // Ensure config directory exists
    try {
        fs::create_directories(config_dir_);
    } catch (const fs::filesystem_error& e) {
        spdlog::error("[TelemetryManager] Failed to create config dir '{}': {}", config_dir_,
                      e.what());
    }

    // Load or generate device identity
    ensure_device_id();

    // Restore persisted event queue
    load_queue();

    // Recover snapshot state from previous session (e.g., after crash)
    load_snapshot_state();

    // Load enabled state from settings.json via Config singleton (the single
    // source of truth shared with SystemSettingsManager). Previously this
    // was loaded from a separate telemetry_config.json, and a sync line in
    // application.cpp clobbered it with SystemSettingsManager's value on
    // every startup — silently disabling telemetry for any user whose
    // settings.json didn't have the key set. Legacy file is migrated by
    // config.cpp's migrate_v13_to_v14(), which runs before Config finishes
    // loading, so by now /telemetry_enabled is already authoritative.
    {
        Config* cfg = Config::get_instance();
        if (cfg) {
            enabled_.store(cfg->get<bool>("/telemetry_enabled", false));
            spdlog::info("[TelemetryManager] Loaded enabled state: {}",
                         enabled_.load() ? "true" : "false");
        } else {
            enabled_.store(false);
        }
    }

    // Check for crash file from a previous session (respects opt-in)
    check_previous_crash();

    // Check for update success flag from a previous session
    check_previous_update();

    // Initialize LVGL subject for settings UI binding
    if (!subjects_initialized_) {
        UI_MANAGED_SUBJECT_INT(enabled_subject_, enabled_.load() ? 1 : 0, "telemetry_enabled",
                               subjects_);
        subjects_initialized_ = true;
        spdlog::debug("[TelemetryManager] LVGL subject initialized");
    }

    initialized_.store(true);
    spdlog::info("[TelemetryManager] Initialization complete (enabled={}, queue={})",
                 enabled_.load() ? "true" : "false", queue_size());
}

void TelemetryManager::shutdown() {
    if (!initialized_.load()) {
        spdlog::debug("[TelemetryManager] Not initialized, skipping shutdown");
        return;
    }

    spdlog::info("[TelemetryManager] Shutting down...");

    // Mark events recorded during shutdown as shutdown snapshots
    is_shutdown_snapshot_ = true;

    // Flush any pending settings changes before shutdown
    flush_settings_changes();

    // Record session-summary events before shutting down
    record_panel_usage();
    record_connection_stability();
    record_performance_snapshot();
    // Drain the final AsyncLifetime skip window. is_shutdown_snapshot_ is true
    // so record_async_lifetime_snapshot emits even if this window's count is
    // zero — that zero confirms no orphan work survived to shutdown.
    record_async_lifetime_snapshot();

    shutting_down_.store(true);

    // Stop auto-send timer first (LVGL call — skip if LVGL already torn down)
    if (lv_is_initialized()) {
        stop_auto_send();
    } else {
        auto_send_timer_ = nullptr;
        snapshot_timer_ = nullptr;
        feature_adoption_timer_ = nullptr;
        settings_debounce_timer_ = nullptr;
    }

    // Persist queue to disk
    save_queue();

    // Remove snapshot file — clean shutdown, no recovery needed
    auto snap_path = fs::path(config_dir_) / "telemetry_snapshot.json";
    std::error_code ec;
    fs::remove(snap_path, ec);

    // Join background send thread if active
    if (send_thread_.joinable()) {
        spdlog::debug("[TelemetryManager] Joining send thread...");
        send_thread_.join();
    }

    // Expire the deferred subject write before the subject goes away (#1165).
    // Unconditional: the callback must be dropped even on the LVGL-already-torn-
    // down path, where deinit_all() is skipped but the subject is just as dead.
    async_lifetime_.invalidate();

    // Deinitialize LVGL subjects (skip if LVGL already torn down)
    if (subjects_initialized_ && lv_is_initialized()) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    initialized_.store(false);
    shutting_down_.store(false);
    spdlog::info("[TelemetryManager] Shutdown complete");
}

// =============================================================================
// Enable / Disable
// =============================================================================

void TelemetryManager::set_enabled(bool enabled) {
    enabled_.store(enabled);
    spdlog::info("[TelemetryManager] Telemetry {}", enabled ? "enabled" : "disabled");

    // Start/stop auto-send timer based on enabled state.
    // The timer triggers DNS resolution (HTTP request to telemetry.helixscreen.org)
    // which crashes on statically-linked platforms (AD5M/CC1) where glibc NSS
    // modules are dynamically loaded with ABI mismatches. Only run when enabled.
    if (enabled && discovery_complete_) {
        start_auto_send();
    } else if (!enabled) {
        stop_auto_send();
    }

    // Update LVGL subject via queue to ensure thread safety.
    // NOTE: set_enabled() must be called from the LVGL thread since it
    // creates/deletes LVGL timers above. enabled_ is atomic for safe reads
    // from any thread, but the function itself is LVGL-thread-only.
    if (subjects_initialized_) {
        async_lifetime_.defer("TelemetryManager::set_enabled", [this, enabled]() {
            lv_subject_set_int(&enabled_subject_, enabled ? 1 : 0);
        });
    }

    // Persist to settings.json via Config (single source of truth).
    // SystemSettingsManager::set_telemetry_enabled() also calls this path,
    // so there is exactly one writer now — application.cpp no longer needs
    // to clobber our state on every startup.
    Config* cfg = Config::get_instance();
    if (cfg) {
        cfg->set<bool>("/telemetry_enabled", enabled);
        cfg->save();
        spdlog::debug("[TelemetryManager] Persisted enabled state to settings.json");
    } else {
        spdlog::warn("[TelemetryManager] Config not available; enabled state not persisted");
    }
}

bool TelemetryManager::is_enabled() const {
    return enabled_.load();
}

// =============================================================================
// Event Recording
// =============================================================================

void TelemetryManager::record_session() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording session event");
    auto event = build_session_event();
    enqueue_event(std::move(event));
}

void TelemetryManager::record_print_outcome(const std::string& outcome, int duration_sec,
                                            int phases_completed, float filament_used_mm,
                                            const std::string& filament_type, int nozzle_temp,
                                            int bed_temp) {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording print outcome: {} ({}s)", outcome, duration_sec);
    auto event = build_print_outcome_event(outcome, duration_sec, phases_completed,
                                           filament_used_mm, filament_type, nozzle_temp, bed_temp);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_update_failure(const std::string& reason, const std::string& version,
                                             const std::string& platform, int http_code,
                                             int64_t file_size, int exit_code) {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::info("[TelemetryManager] Recording update failure: reason={} version={}", reason,
                 version);
    auto event =
        build_update_failed_event(reason, version, platform, http_code, file_size, exit_code);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_memory_snapshot(const std::string& trigger) {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording memory snapshot (trigger={})", trigger);
    auto event = build_memory_snapshot_event(trigger);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_memory_warning(const helix::MemoryWarningEvent& warning) {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording memory warning (level={})",
                  helix::pressure_level_to_string(warning.level));
    auto event = build_memory_warning_event(warning);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_hardware_profile() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording hardware profile");
    auto event = build_hardware_profile_event();
    enqueue_event(std::move(event));
}

void TelemetryManager::record_settings_snapshot() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording settings snapshot");
    auto event = build_settings_snapshot_event();
    enqueue_event(std::move(event));
}

void TelemetryManager::notify_panel_changed(const std::string& panel_name) {
    auto now = std::chrono::steady_clock::now();

    // Flush time for the previous panel
    if (!current_panel_.empty()) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - panel_start_time_).count();
        panel_time_sec_[current_panel_] += static_cast<int>(elapsed);
    }

    // Start tracking the new panel
    current_panel_ = panel_name;
    panel_start_time_ = now;
    panel_visits_[panel_name]++;

    // Maintain panel ID for frame time ring buffer
    auto it = std::find(panel_names_.begin(), panel_names_.end(), panel_name);
    if (it != panel_names_.end()) {
        current_panel_id_ = static_cast<uint16_t>(std::distance(panel_names_.begin(), it));
    } else {
        panel_names_.push_back(panel_name);
        current_panel_id_ = static_cast<uint16_t>(panel_names_.size() - 1);
    }

    spdlog::trace("[TelemetryManager] Panel changed to '{}' (visits={})", panel_name,
                  panel_visits_[panel_name]);
}

void TelemetryManager::notify_overlay_opened(const std::string& overlay_name) {
    overlay_open_count_++;
    overlay_visits_[overlay_name]++;
    spdlog::trace("[TelemetryManager] Overlay '{}' opened (total={}, visits={})", overlay_name,
                  overlay_open_count_, overlay_visits_[overlay_name]);
}

void TelemetryManager::notify_widget_interaction(const std::string& widget_id) {
    if (!enabled_.load() || !initialized_.load())
        return;
    // Strip instance suffix for aggregation (e.g., "favorite_macro:2" -> "favorite_macro")
    std::string base_id = widget_id;
    auto colon = base_id.find(':');
    if (colon != std::string::npos) {
        base_id = base_id.substr(0, colon);
    }
    widget_interactions_[base_id]++;
    spdlog::trace("[TelemetryManager] Widget interaction '{}' (count={})", base_id,
                  widget_interactions_[base_id]);
}

void TelemetryManager::record_panel_usage() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    // Finalize: flush time for the current panel
    if (!current_panel_.empty()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - panel_start_time_).count();
        panel_time_sec_[current_panel_] += static_cast<int>(elapsed);
        panel_start_time_ = now;
    }

    spdlog::debug("[TelemetryManager] Recording panel usage event");
    auto event = build_panel_usage_event();
    enqueue_event(std::move(event));
}

void TelemetryManager::notify_connection_state_changed(int state) {
    auto now = std::chrono::steady_clock::now();

    if (state == 2 && !connection_tracking_connected_) {
        // Transitioning to connected from disconnected
        if (connection_state_start_time_.time_since_epoch().count() > 0) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - connection_state_start_time_)
                    .count();
            int elapsed_sec = static_cast<int>(elapsed);
            total_disconnected_sec_ += elapsed_sec;
            if (elapsed_sec > longest_disconnect_sec_) {
                longest_disconnect_sec_ = elapsed_sec;
            }
        }
        connect_count_++;
        connection_tracking_connected_ = true;
        connection_state_start_time_ = now;

        spdlog::trace("[TelemetryManager] Connection state: connected (count={})", connect_count_);
    } else if (state != 2 && connection_tracking_connected_) {
        // Transitioning from connected to disconnected
        if (connection_state_start_time_.time_since_epoch().count() > 0) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - connection_state_start_time_)
                    .count();
            total_connected_sec_ += static_cast<int>(elapsed);
        }
        disconnect_count_++;
        connection_tracking_connected_ = false;
        connection_state_start_time_ = now;

        spdlog::trace("[TelemetryManager] Connection state: disconnected (count={})",
                      disconnect_count_);
    } else if (connection_state_start_time_.time_since_epoch().count() == 0) {
        // First state notification — initialize tracking
        connection_tracking_connected_ = (state == 2);
        connection_state_start_time_ = now;
        if (state == 2) {
            connect_count_++;
        }
        spdlog::trace("[TelemetryManager] Connection tracking initialized (connected={})",
                      connection_tracking_connected_);
    }
}

void TelemetryManager::notify_klippy_state_changed(int state) {
    if (state == 2) {
        klippy_shutdown_count_++;
        spdlog::trace("[TelemetryManager] Klippy shutdown (count={})", klippy_shutdown_count_);
    } else if (state == 3) {
        klippy_error_count_++;
        spdlog::trace("[TelemetryManager] Klippy error (count={})", klippy_error_count_);
    }
}

void TelemetryManager::record_connection_stability() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    // Finalize: flush time for the current connection state
    auto now = std::chrono::steady_clock::now();
    if (connection_state_start_time_.time_since_epoch().count() > 0) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - connection_state_start_time_)
                .count();
        int elapsed_sec = static_cast<int>(elapsed);
        if (connection_tracking_connected_) {
            total_connected_sec_ += elapsed_sec;
        } else {
            total_disconnected_sec_ += elapsed_sec;
            if (elapsed_sec > longest_disconnect_sec_) {
                longest_disconnect_sec_ = elapsed_sec;
            }
        }
        connection_state_start_time_ = now;
    }

    spdlog::debug("[TelemetryManager] Recording connection stability event");
    auto event = build_connection_stability_event();
    enqueue_event(std::move(event));
}

void TelemetryManager::write_update_success_flag(const std::string& config_dir,
                                                 const std::string& version,
                                                 const std::string& from_version,
                                                 const std::string& platform) {
    json flag;
    flag["version"] = version;
    flag["from_version"] = from_version;
    flag["platform"] = platform;

    // Use ISO 8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc {};
    gmtime_r(&tt, &utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    flag["timestamp"] = buf;

    std::string path = config_dir + "/update_success.json";
    std::ofstream ofs(path);
    if (ofs) {
        ofs << flag.dump();
        spdlog::info("[TelemetryManager] Wrote update success flag: {}", path);
    } else {
        spdlog::error("[TelemetryManager] Failed to write update success flag: {}", path);
    }
}

void TelemetryManager::check_previous_update() {
    std::string flag_path = config_dir_ + "/update_success.json";

    if (!fs::exists(flag_path)) {
        return;
    }

    spdlog::info("[TelemetryManager] Found update success flag from previous session");

    // Read and parse the flag file
    json flag;
    {
        std::ifstream ifs(flag_path);
        if (!ifs) {
            spdlog::warn("[TelemetryManager] Could not open update success flag");
            std::remove(flag_path.c_str());
            return;
        }
        try {
            flag = json::parse(ifs);
        } catch (const json::parse_error& e) {
            spdlog::warn("[TelemetryManager] Malformed update success flag: {}", e.what());
            std::remove(flag_path.c_str());
            return;
        }
    }

    // Always clean up the flag file
    std::remove(flag_path.c_str());

    // The try above covers only json::parse, and it catches parse_error only —
    // neither protects the reads below. A flag file that parsed but isn't an
    // object carries nothing worth reporting, so drop it rather than enqueue an
    // event whose every field reads "unknown".
    if (!flag.is_object()) {
        spdlog::warn("[TelemetryManager] Update success flag is not an object; discarding");
        return;
    }

    if (enabled_.load()) {
        // safe_string, not .value(): the installer writes this file, and a null
        // field (e.g. an unresolved platform) would throw type_error.302 out of
        // .value() — uncaught here, and this runs during telemetry init.
        const std::string version = helix::json_util::safe_string(flag, "version", "unknown");
        auto event = build_update_success_event(
            version, helix::json_util::safe_string(flag, "from_version", "unknown"),
            helix::json_util::safe_string(flag, "platform", "unknown"),
            helix::json_util::safe_string(flag, "timestamp", get_timestamp()));
        enqueue_event(std::move(event));
        save_queue();
        spdlog::info("[TelemetryManager] Enqueued update_success event (version={})", version);
    } else {
        spdlog::debug("[TelemetryManager] Update success event discarded (telemetry disabled)");
    }
}

// =============================================================================
// Queue Management
// =============================================================================

size_t TelemetryManager::queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

nlohmann::json TelemetryManager::get_queue_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return json(queue_);
}

void TelemetryManager::clear_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    spdlog::info("[TelemetryManager] Queue cleared");
}

// =============================================================================
// Transmission
// =============================================================================

nlohmann::json TelemetryManager::build_batch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t batch_size = std::min(queue_.size(), MAX_BATCH_SIZE);
    return json(std::vector<json>(queue_.begin(), queue_.begin() + static_cast<long>(batch_size)));
}

void TelemetryManager::remove_sent_events(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t to_remove = std::min(count, queue_.size());
    queue_.erase(queue_.begin(), queue_.begin() + static_cast<long>(to_remove));
    spdlog::debug("[TelemetryManager] Removed {} sent events, {} remaining", to_remove,
                  queue_.size());
}

void TelemetryManager::try_send(bool force) {
    // `force` (crash-bundle consent path) bypasses the opt-in gate and the
    // send-interval backoff, but never the initialized/shutting-down guards.
    if ((!enabled_.load() && !force) || !initialized_.load() || shutting_down_.load()) {
        return;
    }

    if (queue_size() == 0) {
        spdlog::debug("[TelemetryManager] try_send: queue empty, nothing to send");
        return;
    }

    // Check send interval with backoff
    auto now = std::chrono::steady_clock::now();
    int backoff = backoff_multiplier_.load();
    auto interval = SEND_INTERVAL * backoff;
    // Cap backoff at 7 days
    auto max_interval = std::chrono::hours{24 * 7};
    if (interval > max_interval) {
        interval = max_interval;
    }

    if (!force && last_send_time_.time_since_epoch().count() > 0 &&
        now - last_send_time_ < interval) {
        spdlog::debug("[TelemetryManager] try_send: too soon (backoff={}x), skipping", backoff);
        return;
    }

    // Join previous send thread if it completed (prevents std::terminate on reassignment)
    if (send_thread_.joinable()) {
        send_thread_.join();
    }

    auto batch = build_batch();
    if (batch.empty()) {
        return;
    }

    last_send_time_ = now;

    spdlog::info("[TelemetryManager] Sending batch of {} events", batch.size());

    // Send on background thread; joined on next try_send() call or shutdown().
    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    // build_batch() above copies from queue_ without removing — events are
    // only removed via remove_sent_events() inside do_send on success — so on
    // spawn failure the queue is already in the right state. Just log; the
    // next try_send tick will retry.
    try {
        send_thread_ = std::thread([this, batch = std::move(batch)]() { do_send(batch); });
    } catch (const std::system_error& e) {
        spdlog::error("[TelemetryManager] Failed to spawn send thread: {} — events remain "
                      "queued for next try_send",
                      e.what());
    }
}

void TelemetryManager::do_send(const nlohmann::json& batch) {
    try {
        // Verify SSL certificate availability before HTTPS request.
        // On devices without a CA cert bundle (e.g., AD5M stock firmware),
        // glibc's NSS resolver can crash with SIGSEGV during SSL handshake.
        if (!ssl_verified_) {
            const char* cert_file = getenv("SSL_CERT_FILE");
            const char* cert_dir = getenv("SSL_CERT_DIR");
            bool have_certs = (cert_file && access(cert_file, R_OK) == 0) ||
                              (cert_dir && access(cert_dir, R_OK) == 0) ||
                              access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0;
            if (!have_certs) {
                spdlog::warn("[TelemetryManager] No CA certificate bundle found — "
                             "HTTPS requests may fail. Set SSL_CERT_FILE or install "
                             "ca-certificates.");
                // Don't crash — just disable telemetry sends
                send_disabled_ = true;
            }
            ssl_verified_ = true;
        }
        if (send_disabled_) {
            return;
        }

        // Use libhv HTTP client (same pattern as UpdateChecker and Moonraker API)
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_POST;
        req->url = ENDPOINT_URL;
        req->timeout = 30;
        req->content_type = APPLICATION_JSON;
        req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
        req->headers["X-API-Key"] = API_KEY;
        req->body = batch.dump();

        auto resp = requests::request(req);

        if (shutting_down_.load()) {
            spdlog::debug("[TelemetryManager] Shutting down, aborting send result processing");
            return;
        }

        int status_code = resp ? static_cast<int>(resp->status_code) : 0;

        if (resp && status_code >= 200 && status_code < 300) {
            // Success: remove sent events from queue and persist
            spdlog::info("[TelemetryManager] Successfully sent {} events (HTTP {})", batch.size(),
                         status_code);
            remove_sent_events(batch.size());
            save_queue();
            backoff_multiplier_.store(1);
        } else {
            // Failure: keep events, increase backoff
            int new_backoff = std::min(backoff_multiplier_.load() * 2, 7);
            spdlog::warn("[TelemetryManager] Send failed (HTTP {}), will retry with backoff={}x",
                         status_code, new_backoff);
            backoff_multiplier_.store(new_backoff);
        }
    } catch (const std::exception& e) {
        spdlog::error("[TelemetryManager] Send exception: {}", e.what());
        backoff_multiplier_.store(std::min(backoff_multiplier_.load() * 2, 7));
    }
}

// =============================================================================
// Auto-send Scheduler
// =============================================================================

void TelemetryManager::start_auto_send() {
    discovery_complete_ = true;

    if (!is_enabled()) {
        spdlog::debug("[TelemetryManager] Telemetry disabled, skipping auto-send timer");
        return;
    }

    if (auto_send_timer_) {
        spdlog::debug("[TelemetryManager] Auto-send timer already running");
        return;
    }

    auto_send_initial_fired_ = false;

    auto_send_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (!self)
                return;

            // After the initial delay fires, switch to the normal hourly interval
            if (!self->auto_send_initial_fired_) {
                self->auto_send_initial_fired_ = true;
                lv_timer_set_period(timer, AUTO_SEND_INTERVAL_MS);
            }

            if (self->is_enabled()) {
                spdlog::debug("[TelemetryManager] Auto-send timer fired");
                self->try_send();
                // Record periodic memory snapshot
                self->record_memory_snapshot("hourly");
                // Periodic queue persistence (events no longer save individually)
                self->save_queue();
            }
        },
        INITIAL_SEND_DELAY_MS, this);

    spdlog::info("[TelemetryManager] Auto-send timer started (initial delay: {}s, interval: {}s)",
                 INITIAL_SEND_DELAY_MS / 1000, AUTO_SEND_INTERVAL_MS / 1000);

    start_snapshot_timer();
    start_frame_perf_timer();
    start_feature_adoption_timer();
}

void TelemetryManager::stop_auto_send() {
    if (auto_send_timer_) {
        lv_timer_delete(auto_send_timer_);
        auto_send_timer_ = nullptr;
        spdlog::info("[TelemetryManager] Auto-send timer stopped");
    }
    stop_snapshot_timer();
    stop_frame_perf_timer();
    stop_feature_adoption_timer();
    stop_settings_debounce_timer();
}

// =============================================================================
// Device ID Utilities
// =============================================================================

std::string TelemetryManager::generate_uuid_v4() {
    unsigned char bytes[16];

    // Try /dev/urandom for high-quality randomness
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.good()) {
        urandom.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    } else {
        // Fallback to std::random_device
        spdlog::warn("[TelemetryManager] /dev/urandom unavailable, using std::random_device");
        std::random_device rd;
        for (int i = 0; i < 16; i += 4) {
            uint32_t val = rd();
            bytes[i] = static_cast<unsigned char>(val & 0xFF);
            bytes[i + 1] = static_cast<unsigned char>((val >> 8) & 0xFF);
            bytes[i + 2] = static_cast<unsigned char>((val >> 16) & 0xFF);
            if (i + 3 < 16) {
                bytes[i + 3] = static_cast<unsigned char>((val >> 24) & 0xFF);
            }
        }
    }

    // Set version 4 (bits 12-15 of time_hi_and_version)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;

    // Set variant RFC 4122 (bits 6-7 of clock_seq_hi_and_reserved)
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as 8-4-4-4-12
    char uuid[37];
    std::snprintf(uuid, sizeof(uuid),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                  bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                  bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return std::string(uuid);
}

std::string TelemetryManager::hash_device_id(const std::string& uuid, const std::string& salt) {
    // Double-hash: SHA-256(SHA-256(uuid) + salt)
    std::string first_hash = sha256_hex(uuid);
    std::string combined = first_hash + salt;
    return sha256_hex(combined);
}

// =============================================================================
// Persistence
// =============================================================================

void TelemetryManager::save_queue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::string path = get_queue_path();
        std::string tmp_path = path + ".tmp";

        // Write to temp file first, then atomic rename to prevent
        // empty/corrupt queue file if process is killed mid-write
        std::ofstream file(tmp_path);
        if (file.good()) {
            file << json(queue_).dump(2);
            file.close();
            if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
                spdlog::warn("[TelemetryManager] Failed to rename queue temp file: {}",
                             strerror(errno));
            } else {
                spdlog::trace("[TelemetryManager] Saved {} events to {}", queue_.size(), path);
            }
        } else {
            spdlog::warn("[TelemetryManager] Failed to open queue file for writing: {}", path);
        }
    } catch (const std::exception& e) {
        spdlog::error("[TelemetryManager] Failed to save queue: {}", e.what());
    }
}

void TelemetryManager::load_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::string path = get_queue_path();
        std::ifstream file(path);
        if (!file.good()) {
            spdlog::debug("[TelemetryManager] No queue file at {}, starting with empty queue",
                          path);
            return;
        }

        // Guard against empty file left by interrupted save_queue()
        if (file.peek() == std::ifstream::traits_type::eof()) {
            spdlog::debug("[TelemetryManager] Queue file is empty, starting fresh");
            return;
        }

        json arr = json::parse(file);
        if (!arr.is_array()) {
            spdlog::warn("[TelemetryManager] Queue file is not a JSON array, ignoring");
            return;
        }

        queue_.clear();
        for (auto& event : arr) {
            queue_.push_back(std::move(event));
        }

        // Enforce max queue size
        while (queue_.size() > MAX_QUEUE_SIZE) {
            queue_.erase(queue_.begin());
        }

        spdlog::info("[TelemetryManager] Loaded {} events from queue", queue_.size());
    } catch (const json::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to parse queue file (corrupt?): {}", e.what());
        queue_.clear();
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to load queue: {}", e.what());
    }
}

// =============================================================================
// Crash Reporting
// =============================================================================

void TelemetryManager::check_previous_crash() {
    std::string crash_path = config_dir_ + "/crash.txt";

    if (!crash_handler::has_crash_file(crash_path)) {
        spdlog::debug("[TelemetryManager] No crash file found at {}", crash_path);
        return;
    }

    // If update_success.json exists alongside crash.txt, this crash was from
    // the expected post-update restart (old binary dying after install.sh swap).
    // Don't enqueue a crash telemetry event. Don't delete crash.txt here —
    // Application::run() calls CrashReporter::consume_crash_file() which
    // rotates it to crash_1.txt for offline analysis.
    std::string update_flag = config_dir_ + "/update_success.json";
    std::error_code ec;
    if (fs::exists(update_flag, ec) && !ec) {
        spdlog::info("[TelemetryManager] Crash file is from post-update restart, suppressing");
        had_update_restart_ = true;
        return;
    }

    spdlog::info("[TelemetryManager] Found crash file from previous session");

    auto crash_data = crash_handler::read_crash_file(crash_path);
    if (crash_data.is_null()) {
        spdlog::warn("[TelemetryManager] Failed to parse crash file, skipping telemetry event");
        return;
    }

    // Client-side dedup: skip if we already sent a report with the same fingerprint
    std::string bt0;
    if (crash_data.contains("backtrace") && crash_data["backtrace"].is_array() &&
        !crash_data["backtrace"].empty()) {
        bt0 = crash_data["backtrace"][0].get<std::string>();
    }
    std::string fp = helix::crash_fingerprint(crash_data.value("signal_name", ""),
                                              crash_data.value("app_version", ""), bt0);
    if (helix::CrashHistory::instance().has_fingerprint(fp)) {
        spdlog::info("[TelemetryManager] Duplicate crash ({}), skipping telemetry event", fp);
        return;
    }

    // Only enqueue if telemetry is enabled (respect user opt-in)
    if (enabled_.load()) {
        enqueue_event(build_crash_event(crash_data));
        save_queue();
        crash_event_queued_ = true;
        spdlog::info("[TelemetryManager] Enqueued crash event (signal={}, name={})",
                     crash_data.value("signal", 0), crash_data.value("signal_name", "unknown"));

        // Record fingerprint so we don't re-send this crash on subsequent boots.
        // CrashReporter also records when its own send succeeds, but that path may
        // not run (user dismisses modal, network down, etc.).
        helix::CrashHistoryEntry hist_entry;
        hist_entry.timestamp = get_timestamp();
        hist_entry.signal_name = crash_data.value("signal_name", "");
        hist_entry.signal = crash_data.value("signal", 0);
        hist_entry.app_version = crash_data.value("app_version", "");
        hist_entry.uptime_sec = crash_data.value("uptime_sec", 0);
        hist_entry.fault_addr = crash_data.value("fault_addr", "");
        hist_entry.fault_code_name = crash_data.value("fault_code_name", "");
        hist_entry.abort_msg = crash_data.value("abort_msg", "");
        hist_entry.sent_via = "telemetry";
        hist_entry.fingerprint = fp;
        helix::CrashHistory::instance().add_entry(hist_entry);
    } else {
        spdlog::debug("[TelemetryManager] Crash event discarded (telemetry disabled)");
    }

    // Note: crash file is NOT removed here — CrashReporter owns the lifecycle
    // and removes it after the user interacts with the crash report modal.
}

nlohmann::json TelemetryManager::build_crash_event(const nlohmann::json& crash_data) const {
    // Build a crash event following the telemetry schema
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "crash";
    event["device_id"] = get_hashed_device_id();

    // Use timestamp from crash file if available, otherwise current time
    event["timestamp"] =
        crash_data.contains("timestamp") ? crash_data["timestamp"] : json(get_timestamp());

    // Copy crash-specific fields (signal info, backtrace, register state)
    for (const char* key :
         {"signal", "signal_name", "app_version", "uptime_sec", "backtrace", "fault_addr",
          "fault_code", "fault_code_name", "reg_pc", "reg_sp", "reg_lr", "reg_bp", "load_base"}) {
        if (crash_data.contains(key)) {
            event[key] = crash_data[key];
        }
    }
    // MIPS spells the link register "ra". Forward it under "reg_lr" so the
    // crash worker's Registers table shows the return address alongside PC.
    if (!event.contains("reg_lr") && crash_data.contains("reg_ra")) {
        event["reg_lr"] = crash_data["reg_ra"];
    }

    // Filter memory map to executable mappings (.so files, main binary) to keep
    // telemetry payload small while retaining the data needed to identify which
    // library a crash PC belongs to.
    if (crash_data.contains("memory_map") && crash_data["memory_map"].is_array()) {
        json filtered_maps = json::array();
        for (const auto& line : crash_data["memory_map"]) {
            const auto& s = line.get_ref<const std::string&>();
            // Only include executable mappings (r-xp) that map a file
            if (s.find("r-xp") != std::string::npos &&
                (s.find(".so") != std::string::npos ||
                 s.find("helix-screen") != std::string::npos ||
                 s.find("helix_screen") != std::string::npos)) {
                filtered_maps.push_back(line);
            }
        }
        if (!filtered_maps.empty()) {
            event["memory_map"] = std::move(filtered_maps);
        }
    }

    // Add platform (not in crash file — determined at runtime)
    event["app_platform"] = UpdateChecker::get_platform_key();

    return event;
}

bool TelemetryManager::enqueue_crash_event_unconditional() {
    if (!initialized_.load() || shutting_down_.load()) {
        return false;
    }

    // The opt-in boot path (or a prior consent) already captured this crash;
    // don't enqueue a second copy.
    if (crash_event_queued_) {
        spdlog::debug(
            "[TelemetryManager] Crash already queued this session, skipping consent enqueue");
        return false;
    }

    std::string crash_path = config_dir_ + "/crash.txt";
    if (!crash_handler::has_crash_file(crash_path)) {
        return false;
    }

    // A crash alongside update_success.json is the expected post-update restart
    // (old binary dying after the install.sh swap), not a real crash.
    std::string update_flag = config_dir_ + "/update_success.json";
    std::error_code ec;
    if (fs::exists(update_flag, ec) && !ec) {
        spdlog::debug("[TelemetryManager] Consent crash is post-update restart, suppressing");
        return false;
    }

    auto crash_data = crash_handler::read_crash_file(crash_path);
    if (crash_data.is_null()) {
        spdlog::warn("[TelemetryManager] Consent crash: unparseable crash file at {}", crash_path);
        return false;
    }

    // The user explicitly consented to share this crash by sending the bundle,
    // so emit the telemetry crash event regardless of the opt-in setting.
    //
    // Deliberately NO CrashHistory fingerprint dedup here: the caller reaches
    // this only after CrashReporter::try_auto_send() succeeded, which already
    // recorded this crash's fingerprint. A has_fingerprint() check would
    // therefore ALWAYS be true and silently suppress every consent event.
    // Duplicate boot-vs-consent enqueues are prevented by crash_event_queued_
    // above, and the modal is only shown for fresh, non-duplicate crashes
    // (see Application crash-dialog gating on had_update_restart()/is_duplicate()).
    enqueue_event(build_crash_event(crash_data));
    save_queue();
    crash_event_queued_ = true;
    spdlog::info("[TelemetryManager] Enqueued crash event via bundle consent "
                 "(opt-in bypassed, signal={}, name={})",
                 crash_data.value("signal", 0), crash_data.value("signal_name", "unknown"));
    return true;
}

// =============================================================================
// LVGL Subject
// =============================================================================

lv_subject_t* TelemetryManager::enabled_subject() {
    return &enabled_subject_;
}

// =============================================================================
// Internal Helpers
// =============================================================================

void TelemetryManager::enqueue_event(nlohmann::json event) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Drop oldest if at capacity
    if (queue_.size() >= MAX_QUEUE_SIZE) {
        spdlog::debug("[TelemetryManager] Queue at capacity ({}), dropping oldest event",
                      MAX_QUEUE_SIZE);
        queue_.erase(queue_.begin());
    }

    queue_.push_back(std::move(event));
    spdlog::trace("[TelemetryManager] Event enqueued, queue size: {}", queue_.size());
}

nlohmann::json TelemetryManager::build_session_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "session";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();

    // ---- app section ----
    json app;
    app["version"] = HELIX_VERSION;
    app["platform"] = UpdateChecker::get_platform_key();

    if (auto* dm = DisplayManager::instance()) {
        int w = dm->width();
        int h = dm->height();
        if (w > 0 && h > 0) {
            app["display"] = std::to_string(w) + "x" + std::to_string(h);
        }
        if (auto* backend = dm->backend()) {
            app["display_backend"] = display_backend_type_to_string(backend->type());

            // Input type: SDL=mouse, FBDEV/DRM=touch
            if (backend->type() == DisplayBackendType::SDL) {
                app["input_type"] = "mouse";
            } else {
                app["input_type"] = "touch";
            }
        }
        app["has_backlight"] = dm->has_backlight_control();
        app["has_hw_blank"] = dm->uses_hardware_blank();
    }

    // Theme and language (always available, don't depend on DisplayManager)
    app["theme"] = DisplaySettingsManager::instance().get_dark_mode() ? "dark" : "light";
    app["locale"] = SystemSettingsManager::instance().get_language();

    // Which tool (if any) this system can use to read a .zip release archive.
    // Paired with app.version this answers "can this platform go zip-only?":
    // a platform is safe to un-gate once ~100% of its active devices report
    // both a new enough version and zip_tool != "none".
    //
    // Probed at most once per process: available_zip_tool() may exec
    // `python3 -c "import zipfile, zlib"` when no unzip binary exists, which is
    // slow on MIPS hardware, and the answer cannot change while we run.
    // Deliberately OUTSIDE the DisplayManager guard above — this is not a
    // display property, and would silently vanish on headless/early-init.
    static const char* const zip_tool = []() -> const char* {
        switch (UpdateChecker::available_zip_tool()) {
        case UpdateChecker::ZipTool::Unzip:
            return "unzip";
        case UpdateChecker::ZipTool::Python:
            return "python";
        case UpdateChecker::ZipTool::None:
            break;
        }
        return "none";
    }();
    app["zip_tool"] = zip_tool;

    event["app"] = app;

    // ---- host section (always available, doesn't require printer connection) ----
    json host;

    // Architecture from uname
    {
        struct utsname uts;
        if (uname(&uts) == 0) {
            host["arch"] = std::string(uts.machine);
        }
    }

    // CPU model from /proc/cpuinfo (first "model name" or "Hardware" line)
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        if (cpuinfo.good()) {
            std::string line;
            while (std::getline(cpuinfo, line)) {
                // x86: "model name	: Intel(R) Core..."
                // ARM: "Hardware	: BCM2711"
                if (line.rfind("model name", 0) == 0 || line.rfind("Hardware", 0) == 0) {
                    auto pos = line.find(':');
                    if (pos != std::string::npos && pos + 2 < line.size()) {
                        host["cpu_model"] = line.substr(pos + 2);
                    }
                    break;
                }
            }
        }
    }

    // RAM and CPU cores from PlatformCapabilities
    {
        auto caps = helix::PlatformCapabilities::detect();
        if (caps.total_ram_mb > 0) {
            host["ram_total_mb"] = static_cast<int>(caps.total_ram_mb);
        }
        if (caps.cpu_cores > 0) {
            host["cpu_cores"] = caps.cpu_cores;
        }
    }

    // Where the display runs relative to the printer. Derived from the same
    // host-identity check that gates every klippy-UDS feature, so the metric
    // and the gate can never disagree.
    {
        auto* cfg = Config::get_instance();
        const std::string moonraker_host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
        host["screen_locality"] = helix::screen_locality_for_host(moonraker_host);
    }

    // ---- printer & features sections (require discovery data) ----
    auto* client = get_moonraker_client();
    if (client) {
        const auto& hw = client->hardware();

        // printer section
        json printer;
        if (!hw.kinematics().empty()) {
            printer["kinematics"] = hw.kinematics();
        }

        const auto& bv = hw.build_volume();
        if (bv.x_max > 0 && bv.y_max > 0) {
            // Format as "XxYxZ" using integer dimensions
            std::string vol = std::to_string(static_cast<int>(bv.x_max - bv.x_min)) + "x" +
                              std::to_string(static_cast<int>(bv.y_max - bv.y_min));
            if (bv.z_max > 0) {
                vol += "x" + std::to_string(static_cast<int>(bv.z_max));
            }
            printer["build_volume"] = vol;
        }

        if (!hw.mcu().empty()) {
            printer["mcu"] = hw.mcu();
        }
        printer["mcu_count"] = static_cast<int>(hw.mcu_list().empty() ? (hw.mcu().empty() ? 0 : 1)
                                                                      : hw.mcu_list().size());

        // Count extruders from heaters list (names starting with "extruder")
        int extruder_count = 0;
        for (const auto& heater : hw.heaters()) {
            if (heater.rfind("extruder", 0) == 0 && heater.rfind("extruder_stepper", 0) != 0) {
                extruder_count++;
            }
        }
        printer["extruder_count"] = extruder_count;

        printer["has_heated_bed"] = hw.has_heater_bed();
        printer["has_chamber"] = hw.supports_chamber();

        if (!hw.software_version().empty()) {
            printer["klipper_version"] = hw.software_version();
        }
        if (!hw.moonraker_version().empty()) {
            printer["moonraker_version"] = hw.moonraker_version();
        }

        // Detected printer type (generic model name, not PII)
        {
            const auto& ptype = get_printer_state().get_printer_type();
            if (!ptype.empty()) {
                printer["detected_model"] = ptype;
            }
        }

        event["printer"] = printer;

        // features array
        json features = json::array();

        // Leveling
        if (hw.has_bed_mesh())
            features.push_back("bed_mesh");
        if (hw.has_qgl())
            features.push_back("qgl");
        if (hw.has_z_tilt())
            features.push_back("z_tilt");
        if (hw.has_screws_tilt())
            features.push_back("screws_tilt");

        // Hardware
        if (hw.has_probe())
            features.push_back("probe");
        if (hw.has_heater_bed())
            features.push_back("heated_bed");
        if (hw.supports_chamber())
            features.push_back("chamber");
        if (hw.has_accelerometer())
            features.push_back("accelerometer");
        if (hw.has_filament_sensors())
            features.push_back("filament_sensor");
        if (hw.has_led())
            features.push_back("led");
        if (hw.has_speaker())
            features.push_back("speaker");

        // Software
        if (hw.has_firmware_retraction())
            features.push_back("firmware_retraction");
        if (hw.has_exclude_object())
            features.push_back("exclude_object");
        if (hw.has_timelapse())
            features.push_back("timelapse");

        // Spoolman and HelixPlugin from PrinterState
        auto& ps = get_printer_state();
        auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
        if (spoolman_subj && lv_subject_get_int(spoolman_subj) > 0) {
            features.push_back("spoolman");
        }
        if (ps.is_phase_tracking_enabled()) {
            features.push_back("phase_tracking");
        }
        if (ps.service_has_helix_plugin()) {
            features.push_back("helix_plugin");
        }

        // MMU
        switch (hw.mmu_type()) {
        case AmsType::HAPPY_HARE:
            features.push_back("mmu_happy_hare");
            break;
        case AmsType::AFC:
            features.push_back("mmu_afc");
            break;
        case AmsType::TOOL_CHANGER:
            features.push_back("tool_changer");
            break;
        default:
            break;
        }

        event["features"] = features;

        // Add OS from discovery to host section
        if (!hw.os_version().empty()) {
            host["os"] = hw.os_version();
        }
    }

    // Emit host section (always, even without printer connection)
    if (!host.empty()) {
        event["host"] = host;
    }

    return event;
}

nlohmann::json TelemetryManager::build_print_outcome_event(const std::string& outcome,
                                                           int duration_sec, int phases_completed,
                                                           float filament_used_mm,
                                                           const std::string& filament_type,
                                                           int nozzle_temp, int bed_temp) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "print_outcome";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["outcome"] = outcome;
    event["duration_sec"] = duration_sec;
    event["phases_completed"] = phases_completed;
    event["filament_used_mm"] = filament_used_mm;
    event["filament_type"] = filament_type;
    event["nozzle_temp"] = nozzle_temp;
    event["bed_temp"] = bed_temp;

    return event;
}

nlohmann::json TelemetryManager::build_update_failed_event(const std::string& reason,
                                                           const std::string& version,
                                                           const std::string& platform,
                                                           int http_code, int64_t file_size,
                                                           int exit_code) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "update_failed";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["reason"] = reason;
    event["version"] = version;
    event["from_version"] = HELIX_VERSION;
    event["platform"] = platform;

    if (http_code >= 0) {
        event["http_code"] = http_code;
    }
    if (file_size >= 0) {
        event["file_size"] = file_size;
    }
    if (exit_code >= 0) {
        event["exit_code"] = exit_code;
    }

    return event;
}

nlohmann::json TelemetryManager::build_update_success_event(const std::string& version,
                                                            const std::string& from_version,
                                                            const std::string& platform,
                                                            const std::string& timestamp) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "update_success";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = timestamp;
    event["version"] = version;
    event["from_version"] = from_version;
    event["platform"] = platform;

    return event;
}

nlohmann::json TelemetryManager::build_memory_snapshot_event(const std::string& trigger) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "memory_snapshot";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();
    event["trigger"] = trigger;

    // Calculate uptime from init_time_
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - init_time_);
    event["uptime_sec"] = static_cast<int>(uptime.count());

    // Get current memory stats from MemoryMonitor
    auto stats = helix::MemoryMonitor::get_current_stats();
    event["rss_kb"] = static_cast<int>(stats.vm_rss_kb);
    event["vm_size_kb"] = static_cast<int>(stats.vm_size_kb);
    event["vm_data_kb"] = static_cast<int>(stats.vm_data_kb);
    event["vm_swap_kb"] = static_cast<int>(stats.vm_swap_kb);
    event["vm_peak_kb"] = static_cast<int>(stats.vm_peak_kb);
    event["vm_hwm_kb"] = static_cast<int>(stats.vm_hwm_kb);

    // Smaps breakdown
    helix::SmapsRollup smaps;
    if (helix::read_smaps_rollup(smaps)) {
        event["private_dirty_kb"] = static_cast<int>(smaps.private_dirty_kb);
        event["private_clean_kb"] = static_cast<int>(smaps.private_clean_kb);
        event["shared_clean_kb"] = static_cast<int>(smaps.shared_clean_kb);
        event["pss_kb"] = static_cast<int>(smaps.pss_kb);
    }

    // System memory
    auto sys = helix::get_system_memory_info();
    event["system_total_mb"] = static_cast<int>(sys.total_mb());
    event["system_available_mb"] = static_cast<int>(sys.available_mb());

    // glibc ptmalloc arena stats — fragmentation fingerprint for heap-abort
    // crash classes (#758, #771). uordblks = allocated bytes; fordblks = bytes
    // stranded in the free list. Flat uordblks + growing fordblks is classic
    // fragmentation and is the signal we can't get from RSS alone.
#if defined(HELIX_HAS_MALLINFO2)
    {
        struct mallinfo2 mi = mallinfo2();
        event["malloc_arena_kb"] = static_cast<int>(mi.arena / 1024);
        event["malloc_uordblks_kb"] = static_cast<int>(mi.uordblks / 1024);
        event["malloc_fordblks_kb"] = static_cast<int>(mi.fordblks / 1024);
        event["malloc_hblks"] = static_cast<int>(mi.hblks);
        event["malloc_hblkhd_kb"] = static_cast<int>(mi.hblkhd / 1024);
    }
#elif defined(HELIX_HAS_MALLINFO)
    {
        struct mallinfo mi = mallinfo();
        event["malloc_arena_kb"] = static_cast<int>(mi.arena / 1024);
        event["malloc_uordblks_kb"] = static_cast<int>(mi.uordblks / 1024);
        event["malloc_fordblks_kb"] = static_cast<int>(mi.fordblks / 1024);
        event["malloc_hblks"] = static_cast<int>(mi.hblks);
        event["malloc_hblkhd_kb"] = static_cast<int>(mi.hblkhd / 1024);
    }
#endif

    return event;
}

nlohmann::json
TelemetryManager::build_memory_warning_event(const helix::MemoryWarningEvent& warning) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "memory_warning";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();
    event["level"] = helix::pressure_level_to_string(warning.level);
    event["reason"] = warning.reason;

    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - init_time_);
    event["uptime_sec"] = static_cast<int>(uptime.count());

    // Process stats
    event["rss_kb"] = static_cast<int>(warning.stats.vm_rss_kb);
    event["vm_size_kb"] = static_cast<int>(warning.stats.vm_size_kb);
    event["vm_hwm_kb"] = static_cast<int>(warning.stats.vm_hwm_kb);
    event["vm_swap_kb"] = static_cast<int>(warning.stats.vm_swap_kb);

    // System memory
    event["system_total_mb"] = static_cast<int>(warning.system_info.total_mb());
    event["system_available_mb"] = static_cast<int>(warning.system_info.available_mb());

    // Growth
    event["growth_5min_kb"] = static_cast<int>(warning.growth_5min_kb);

    // Smaps breakdown
    event["private_dirty_kb"] = static_cast<int>(warning.smaps.private_dirty_kb);
    event["private_clean_kb"] = static_cast<int>(warning.smaps.private_clean_kb);
    event["shared_clean_kb"] = static_cast<int>(warning.smaps.shared_clean_kb);
    event["shared_dirty_kb"] = static_cast<int>(warning.smaps.shared_dirty_kb);
    event["pss_kb"] = static_cast<int>(warning.smaps.pss_kb);
    event["swap_pss_kb"] = static_cast<int>(warning.smaps.swap_pss_kb);

    // Context fields — populated by main-thread producers via plain atomics so
    // the monitor thread can read them without locking. Lets us distinguish
    // "3D-renderer-during-print holds memory" from "slow idle creep" without
    // cross-referencing other event streams. (No string mirror — racing reads
    // of std::string across threads is UB; the int IDs are sufficient.)
    event["print_state"] =
        helix::telemetry_context::print_state_int.load(std::memory_order_relaxed);
    event["active_panel"] =
        helix::telemetry_context::active_panel_int.load(std::memory_order_relaxed);
    event["gcode_renderer_loaded"] =
        helix::telemetry_context::gcode_renderer_loaded.load(std::memory_order_relaxed);

    return event;
}

// ---------------------------------------------------------------------------
// Hardware profile helper methods
// ---------------------------------------------------------------------------

nlohmann::json TelemetryManager::build_hw_fans_section(const helix::PrinterDiscovery& hw) {
    json fans;
    fans["total"] = static_cast<int>(hw.fans().size());
    int part_cooling = 0, heater_fan = 0, controller_fan = 0, generic = 0;
    for (const auto& fan_name : hw.fans()) {
        if (fan_name.rfind("controller_fan", 0) == 0) {
            controller_fan++;
        } else if (fan_name.rfind("heater_fan", 0) == 0) {
            heater_fan++;
        } else if (fan_name.rfind("fan", 0) == 0) {
            part_cooling++;
        } else {
            generic++;
        }
    }
    fans["part_cooling"] = part_cooling;
    fans["heater_fan"] = heater_fan;
    fans["controller_fan"] = controller_fan;
    fans["generic"] = generic;

    // Name-level data for printer detection analysis
    json names = json::array();
    size_t cap = 200;
    for (const auto& fan_name : hw.fans()) {
        if (names.size() >= cap)
            break;
        names.push_back(fan_name);
    }
    fans["names"] = std::move(names);

    return fans;
}

nlohmann::json TelemetryManager::build_hw_sensors_section(const helix::PrinterDiscovery& hw) {
    json sensors;
    sensors["filament"] = static_cast<int>(FilamentSensorManager::instance().sensor_count());
    sensors["temperature_extra"] =
        static_cast<int>(sensors::TemperatureSensorManager::instance().sensor_count());
    sensors["color"] = static_cast<int>(sensors::ColorSensorManager::instance().sensor_count());
    sensors["accel"] = static_cast<int>(sensors::AccelSensorManager::instance().sensor_count());

    // Name-level data for printer detection analysis
    constexpr size_t cap = 200;
    json temp_names = json::array();
    for (const auto& name : hw.sensors()) {
        if (temp_names.size() >= cap)
            break;
        temp_names.push_back(name);
    }
    sensors["temperature_names"] = std::move(temp_names);

    json fil_names = json::array();
    for (const auto& name : hw.filament_sensor_names()) {
        if (fil_names.size() >= cap)
            break;
        fil_names.push_back(name);
    }
    sensors["filament_names"] = std::move(fil_names);

    return sensors;
}

nlohmann::json TelemetryManager::build_hw_probe_section(const helix::PrinterDiscovery& hw) {
    json probe;
    probe["has_probe"] = hw.has_probe();
    probe["has_bed_mesh"] = hw.has_bed_mesh();
    probe["has_qgl"] = hw.has_qgl();
    probe["has_z_tilt"] = hw.has_z_tilt();
    probe["has_screws_tilt"] = hw.has_screws_tilt();
    return probe;
}

nlohmann::json TelemetryManager::build_hw_capabilities_section(const helix::PrinterDiscovery& hw) {
    json capabilities;
    capabilities["has_chamber"] = hw.supports_chamber();
    capabilities["has_accelerometer"] = hw.has_accelerometer();
    capabilities["has_firmware_retraction"] = hw.has_firmware_retraction();
    capabilities["has_exclude_object"] = hw.has_exclude_object();
    capabilities["has_timelapse"] = hw.has_timelapse();
    capabilities["has_klippain_shaketune"] = hw.has_klippain_shaketune();
    capabilities["has_speaker"] = hw.has_speaker();
    return capabilities;
}

nlohmann::json TelemetryManager::build_hw_ams_section(const helix::PrinterDiscovery& hw) const {
    json ams;
    switch (hw.mmu_type()) {
    case AmsType::HAPPY_HARE:
        ams["type"] = "happy_hare";
        break;
    case AmsType::AFC:
        ams["type"] = "afc";
        break;
    case AmsType::ACE:
        ams["type"] = "ace";
        break;
    case AmsType::TOOL_CHANGER:
        ams["type"] = "tool_changer";
        break;
    case AmsType::AD5X_IFS:
        ams["type"] = "ifs";
        break;
    case AmsType::CFS:
        ams["type"] = "cfs";
        break;
    default:
        ams["type"] = "unknown";
        break;
    }
    ams["unit_count"] = lv_subject_get_int(AmsState::instance().get_backend_count_subject());
    ams["total_slots"] = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    return ams;
}

nlohmann::json TelemetryManager::build_hw_macros_section(const helix::PrinterDiscovery& hw) {
    json macros;
    macros["total_count"] = static_cast<int>(hw.macros().size());
    macros["has_helix_macros"] = !hw.helix_macros().empty();
    macros["has_print_start"] = hw.has_macro("PRINT_START");
    macros["has_nozzle_clean"] = hw.has_macro("CLEAN_NOZZLE") || hw.has_macro("NOZZLE_CLEAN") ||
                                 hw.has_macro("NOZZLE_WIPE") || hw.has_macro("WIPE_NOZZLE") ||
                                 hw.has_macro("PURGE_NOZZLE");
    macros["has_heat_soak"] = hw.has_macro("HEAT_SOAK") || hw.has_macro("CHAMBER_SOAK") ||
                              hw.has_macro("SOAK") || hw.has_macro("BED_SOAK");
    macros["has_purge_line"] = hw.has_macro("PURGE_LINE") || hw.has_macro("PRIME_LINE") ||
                               hw.has_macro("INTRO_LINE") || hw.has_macro("LINE_PURGE");
    macros["led_macro_count"] = static_cast<int>(hw.led_macros().size());

    // Name-level data for printer detection analysis
    json names = json::array();
    constexpr size_t cap = 200;
    for (const auto& macro_name : hw.macros()) {
        if (names.size() >= cap)
            break;
        names.push_back(macro_name);
    }
    macros["names"] = std::move(names);

    return macros;
}

// ---------------------------------------------------------------------------

nlohmann::json TelemetryManager::build_hardware_profile_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "hardware_profile";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();

    try {
        // ---- printer section ----
        json printer;
        {
            const auto& ptype = get_printer_state().get_printer_type();
            if (!ptype.empty()) {
                printer["detected_model"] = ptype;
            }
        }

        // ---- Moonraker topology ----
        // Whether HelixScreen runs on the printer or drives it over the network.
        // Omitted, never guessed, when no URL is known: absent has to stay
        // distinguishable from a measured "remote".
        if (auto* topology_api = get_moonraker_api()) {
            if (auto is_local = classify_moonraker_locality(topology_api->get_websocket_url())) {
                event["moonraker_is_local"] = *is_local;
            }
        }

        auto* client = get_moonraker_client();
        if (client) {
            const auto& hw = client->hardware();

            if (!hw.kinematics().empty()) {
                printer["kinematics"] = hw.kinematics();
            }
            event["printer"] = printer;

            // ---- mcus section ----
            json mcus;
            mcus["primary"] = hw.mcu();
            mcus["count"] = static_cast<int>(hw.mcu_list().empty() ? (hw.mcu().empty() ? 0 : 1)
                                                                   : hw.mcu_list().size());
            if (!hw.mcu_list().empty()) {
                mcus["chips"] = json(hw.mcu_list());
            }
            event["mcus"] = mcus;

            // ---- build_volume section ----
            // Read from IMoonrakerAPI (not client) because set_build_volume()
            // updates api->hardware(), not the discovery sequence's copy
            auto* api = get_moonraker_api();
            const auto& bv = api ? api->hardware().build_volume() : hw.build_volume();
            if (bv.x_max > 0 && bv.y_max > 0) {
                json build_volume;
                build_volume["x_mm"] = static_cast<int>(bv.x_max - bv.x_min);
                build_volume["y_mm"] = static_cast<int>(bv.y_max - bv.y_min);
                build_volume["z_mm"] = static_cast<int>(bv.z_max);
                event["build_volume"] = build_volume;
            }

            // ---- extruders section ----
            json extruders;
            int extruder_count = 0;
            for (const auto& heater : hw.heaters()) {
                if (heater.rfind("extruder", 0) == 0 && heater.rfind("extruder_stepper", 0) != 0) {
                    extruder_count++;
                }
            }
            extruders["count"] = extruder_count;
            extruders["has_chamber_heater"] = hw.has_chamber_heater();
            extruders["has_heater_bed"] = hw.has_heater_bed();
            event["extruders"] = extruders;

            event["fans"] = build_hw_fans_section(hw);

            // ---- steppers section ----
            json steppers;
            steppers["count"] = static_cast<int>(hw.steppers().size());
            json stepper_names = json::array();
            constexpr size_t stepper_cap = 200;
            for (const auto& name : hw.steppers()) {
                if (stepper_names.size() >= stepper_cap)
                    break;
                stepper_names.push_back(name);
            }
            steppers["names"] = std::move(stepper_names);
            event["steppers"] = steppers;

            // ---- leds section ----
            json leds;
            leds["count"] = static_cast<int>(hw.leds().size());
            leds["has_led_effects"] = hw.has_led_effects();
            json led_names = json::array();
            constexpr size_t led_cap = 200;
            for (const auto& name : hw.leds()) {
                if (led_names.size() >= led_cap)
                    break;
                led_names.push_back(name);
            }
            leds["names"] = std::move(led_names);
            event["leds"] = leds;

            event["sensors"] = build_hw_sensors_section(hw);
            event["probe"] = build_hw_probe_section(hw);
            event["capabilities"] = build_hw_capabilities_section(hw);

            // ---- ams section (only if MMU detected) ----
            if (hw.mmu_type() != AmsType::NONE) {
                event["ams"] = build_hw_ams_section(hw);
            }

            // ---- tools section ----
            json tools;
            tools["count"] = ToolState::instance().tool_count();
            tools["is_multi_tool"] = ToolState::instance().is_multi_tool();
            event["tools"] = tools;

            event["macros"] = build_hw_macros_section(hw);

            // Full Klipper object list for detection analysis
            json objects = json::array();
            constexpr size_t obj_cap = 500;
            for (const auto& obj : hw.printer_objects()) {
                if (objects.size() >= obj_cap)
                    break;
                objects.push_back(obj);
            }
            event["printer_objects"] = std::move(objects);
        } else {
            event["printer"] = printer;
        }

        // ---- plugins section ----
        json plugins;
        plugins["helix_plugin_installed"] = get_printer_state().service_has_helix_plugin();
        plugins["phase_tracking_enabled"] = get_printer_state().is_phase_tracking_enabled();
        event["plugins"] = plugins;

        // ---- display_backend ----
        if (auto* dm = DisplayManager::instance()) {
            if (auto* backend = dm->backend()) {
                event["display_backend"] = display_backend_type_to_string(backend->type());
            }
        }

    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Error building hardware profile: {}", e.what());
    }

    return event;
}

nlohmann::json TelemetryManager::build_settings_snapshot_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "settings_snapshot";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    const auto& active_theme = theme_manager_get_active_theme();
    bool dark_mode = DisplaySettingsManager::instance().get_dark_mode();
    // theme_name: full theme identifier e.g. "Nord (Dark)", "Catppuccin (Light)"
    event["theme_name"] = active_theme.name + (dark_mode ? " (Dark)" : " (Light)");
    event["theme"] = dark_mode ? "dark" : "light";
    event["brightness_pct"] = DisplaySettingsManager::instance().get_brightness();
    event["screensaver_timeout_sec"] = DisplaySettingsManager::instance().get_display_dim_sec();
    event["screen_blank_timeout_sec"] = DisplaySettingsManager::instance().get_display_sleep_sec();
    event["locale"] = SystemSettingsManager::instance().get_language();
    event["sound_enabled"] = AudioSettingsManager::instance().get_sounds_enabled();
    event["auto_update_channel"] = SystemSettingsManager::instance().get_update_channel();
    event["animations_enabled"] = DisplaySettingsManager::instance().get_animations_enabled();
    event["time_format"] =
        DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12 ? "12h" : "24h";

    // Home widget placement snapshot
    try {
        json widgets = json::array();
        auto& widget_config = helix::PanelWidgetManager::instance().get_widget_config("home");
        for (const auto& page : widget_config.pages()) {
            for (const auto& entry : page.widgets) {
                if (entry.enabled) {
                    // Strip instance suffix for aggregation
                    std::string base_id = entry.id;
                    auto colon = base_id.find(':');
                    if (colon != std::string::npos) {
                        base_id = base_id.substr(0, colon);
                    }
                    widgets.push_back(base_id);
                }
            }
        }
        event["home_widgets"] = widgets;
    } catch (const std::exception& e) {
        spdlog::debug("[TelemetryManager] Failed to snapshot home widgets: {}", e.what());
    }

    return event;
}

nlohmann::json TelemetryManager::build_panel_usage_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "panel_usage";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    // Session duration from init_time_
    auto now = std::chrono::steady_clock::now();
    auto session_dur = std::chrono::duration_cast<std::chrono::seconds>(now - init_time_);
    event["session_duration_sec"] = static_cast<int>(session_dur.count());

    // Per-panel time and visit maps
    json time_map = json::object();
    for (const auto& [name, sec] : panel_time_sec_) {
        time_map[name] = sec;
    }
    event["panel_time_sec"] = time_map;

    json visit_map = json::object();
    for (const auto& [name, count] : panel_visits_) {
        visit_map[name] = count;
    }
    event["panel_visits"] = visit_map;

    json widget_map = json::object();
    for (const auto& [name, count] : widget_interactions_) {
        widget_map[name] = count;
    }
    event["widget_interactions"] = widget_map;

    event["overlay_open_count"] = overlay_open_count_;

    json overlay_visit_map = json::object();
    for (const auto& [name, count] : overlay_visits_) {
        overlay_visit_map[name] = count;
    }
    event["overlay_visits"] = overlay_visit_map;

    event["snapshot_seq"] = snapshot_seq_;
    event["is_shutdown"] = is_shutdown_snapshot_;

    return event;
}

nlohmann::json TelemetryManager::build_connection_stability_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "connection_stability";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();

    // Session duration from init_time_
    auto now = std::chrono::steady_clock::now();
    auto session_dur = std::chrono::duration_cast<std::chrono::seconds>(now - init_time_);
    event["session_duration_sec"] = static_cast<int>(session_dur.count());

    event["connect_count"] = connect_count_;
    event["disconnect_count"] = disconnect_count_;
    event["total_connected_sec"] = total_connected_sec_;
    event["total_disconnected_sec"] = total_disconnected_sec_;
    event["longest_disconnect_sec"] = longest_disconnect_sec_;
    event["klippy_error_count"] = klippy_error_count_;
    event["klippy_shutdown_count"] = klippy_shutdown_count_;
    event["snapshot_seq"] = snapshot_seq_;
    event["is_shutdown"] = is_shutdown_snapshot_;

    return event;
}

// =============================================================================
// Phase 3: Print Start Context + Error Tracking
// =============================================================================

void TelemetryManager::record_print_start_context(const std::string& source, bool has_thumbnail,
                                                  int64_t file_size_bytes,
                                                  int estimated_duration_sec,
                                                  const std::string& slicer, int tool_count_used,
                                                  bool ams_active) {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording print start context: source={}, slicer={}", source,
                  slicer);
    auto event = build_print_start_context_event(source, has_thumbnail, file_size_bytes,
                                                 estimated_duration_sec, slicer, tool_count_used,
                                                 ams_active);
    enqueue_event(std::move(event));
}

void TelemetryManager::notify_print_started_in_app() {
    print_started_in_app_.store(true);
}

bool TelemetryManager::consume_print_started_in_app() {
    return print_started_in_app_.exchange(false);
}

void TelemetryManager::record_error(const std::string& category, const std::string& code,
                                    const std::string& context) {
    // Check shutdown/initialized BEFORE touching the mutex — background threads
    // (libhv health timer) can call this after shutdown() has run, and locking a
    // destroyed mutex causes SIGBUS on ARM ([884f9172]).
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    // Validate category against allow-list to prevent unbounded map growth
    static const std::array<std::string_view, 5> ALLOWED_CATEGORIES = {
        "moonraker_api", "websocket", "file_io", "display", "memory"};
    if (std::find(ALLOWED_CATEGORIES.begin(), ALLOWED_CATEGORIES.end(), category) ==
        ALLOWED_CATEGORIES.end()) {
        spdlog::trace("[TelemetryManager] Ignoring error with unknown category: {}", category);
        return;
    }

    // Rate limit: max 1 event per category per 5 minutes
    // Guarded by mutex_ since record_error() is called from background threads
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = error_rate_limit_.find(category);
        if (it != error_rate_limit_.end() && now - it->second < ERROR_RATE_LIMIT_INTERVAL) {
            spdlog::trace("[TelemetryManager] Error event rate-limited: category={}", category);
            return;
        }
        error_rate_limit_[category] = now;

        // Prune expired entries to prevent unbounded growth
        if (error_rate_limit_.size() > 200) {
            for (auto prune_it = error_rate_limit_.begin(); prune_it != error_rate_limit_.end();) {
                if (now - prune_it->second >= ERROR_RATE_LIMIT_INTERVAL) {
                    prune_it = error_rate_limit_.erase(prune_it);
                } else {
                    ++prune_it;
                }
            }
        }
    }

    spdlog::debug("[TelemetryManager] Recording error: category={}, code={}, context={}", category,
                  code, context);
    auto event = build_error_event(category, code, context);
    enqueue_event(std::move(event));
}

nlohmann::json TelemetryManager::build_print_start_context_event(
    const std::string& source, bool has_thumbnail, int64_t file_size_bytes,
    int estimated_duration_sec, const std::string& slicer, int tool_count_used,
    bool ams_active) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "print_start_context";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["source"] = source;
    event["has_thumbnail"] = has_thumbnail;
    event["file_size_bucket"] = bucket_file_size(file_size_bytes);
    event["estimated_duration_bucket"] = bucket_duration(estimated_duration_sec);
    event["slicer"] = slicer;
    event["tool_count_used"] = tool_count_used;
    event["ams_active"] = ams_active;

    return event;
}

nlohmann::json TelemetryManager::build_error_event(const std::string& category,
                                                   const std::string& code,
                                                   const std::string& context) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "error_encountered";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["category"] = category;
    event["code"] = code;
    event["context"] = context;

    // Uptime from init_time_
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - init_time_);
    event["uptime_sec"] = static_cast<int>(uptime.count());

    return event;
}

std::string TelemetryManager::bucket_file_size(int64_t bytes) {
    if (bytes < 1024 * 1024)
        return "<1MB";
    if (bytes < 10 * 1024 * 1024)
        return "1-10MB";
    if (bytes < 50 * 1024 * 1024)
        return "10-50MB";
    if (bytes < 100LL * 1024 * 1024)
        return "50-100MB";
    return "100MB+";
}

std::string TelemetryManager::bucket_duration(int sec) {
    if (sec < 1800)
        return "<30min";
    if (sec < 3600)
        return "30min-1hr";
    if (sec < 14400)
        return "1-4hr";
    if (sec < 43200)
        return "4-12hr";
    return "12hr+";
}

std::string TelemetryManager::get_hashed_device_id() const {
    // Safe without mutex: device_uuid_ and device_salt_ are immutable after
    // init() completes, and callers verify initialized_ flag before calling
    return hash_device_id(device_uuid_, device_salt_);
}

std::string TelemetryManager::get_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    struct tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &time_t_now);
#else
    gmtime_r(&time_t_now, &utc_tm);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
    return std::string(buf);
}

void TelemetryManager::ensure_device_id() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string device_path = get_device_id_path();

    // Try to load existing device identity
    try {
        std::ifstream file(device_path);
        if (file.good()) {
            json data = json::parse(file);
            if (data.contains("uuid") && data["uuid"].is_string() && data.contains("salt") &&
                data["salt"].is_string()) {
                device_uuid_ = data["uuid"].get<std::string>();
                device_salt_ = data["salt"].get<std::string>();
                spdlog::info("[TelemetryManager] Loaded device identity from {}", device_path);
                return;
            }
            spdlog::warn("[TelemetryManager] Device file missing uuid/salt, regenerating");
        }
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to load device identity: {}", e.what());
    }

    // Generate new device identity
    device_uuid_ = generate_uuid_v4();
    device_salt_ = generate_uuid_v4(); // Salt is also a random UUID for simplicity

    spdlog::info("[TelemetryManager] Generated new device identity");

    // Persist to disk
    try {
        json data;
        data["uuid"] = device_uuid_;
        data["salt"] = device_salt_;

        std::ofstream file(device_path);
        if (file.good()) {
            file << data.dump(2);
            spdlog::debug("[TelemetryManager] Saved device identity to {}", device_path);
        } else {
            spdlog::error("[TelemetryManager] Failed to write device identity to {}", device_path);
        }
    } catch (const std::exception& e) {
        spdlog::error("[TelemetryManager] Failed to persist device identity: {}", e.what());
    }
}

// =============================================================================
// Persistence Paths
// =============================================================================

std::string TelemetryManager::get_queue_path() const {
    return config_dir_ + "/telemetry_queue.json";
}

std::string TelemetryManager::get_device_id_path() const {
    return config_dir_ + "/telemetry_device.json";
}

// =============================================================================
// Frame Time Sampling
// =============================================================================

void TelemetryManager::record_frame_time(uint32_t frame_time_us) {
    // Skip idle frames where LVGL did no rendering work.
    // Validated on Pi 4 (DRM/dumb buffers): minimum real render is ~1.3ms,
    // giving 2.6x safety margin above 500µs threshold.
    if (frame_time_us < IDLE_FRAME_THRESHOLD_US)
        return;

    // Always record even when telemetry is disabled to keep the buffer warm —
    // if telemetry is re-enabled mid-session the first snapshot has real data.
    // No mutex needed — LVGL thread only.
    auto& sample = frame_ring_[frame_ring_idx_ % FRAME_RING_SIZE];
    sample.frame_time_us = frame_time_us;
    sample.panel_id = current_panel_id_;
    frame_ring_idx_++;
    frame_ring_count_++;
}

nlohmann::json TelemetryManager::build_performance_snapshot_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "performance_snapshot";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();
    event["snapshot_seq"] = snapshot_seq_;
    event["is_shutdown"] = is_shutdown_snapshot_;

    size_t count = std::min(frame_ring_count_, FRAME_RING_SIZE);
    if (count == 0) {
        event["frame_time_p50_ms"] = 0;
        event["frame_time_p95_ms"] = 0;
        event["frame_time_p99_ms"] = 0;
        event["dropped_frame_count"] = 0;
        event["total_frame_count"] = 0;
        event["worst_panel"] = "";
        event["worst_panel_p95_ms"] = 0;
        event["task_handler_max_ms"] = 0;
        return event;
    }

    std::vector<uint32_t> times;
    times.reserve(count);
    int dropped = 0;
    std::unordered_map<uint16_t, std::vector<uint32_t>> per_panel;

    size_t start = (frame_ring_count_ > FRAME_RING_SIZE) ? (frame_ring_idx_ % FRAME_RING_SIZE) : 0;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (start + i) % FRAME_RING_SIZE;
        uint32_t ft = frame_ring_[idx].frame_time_us;
        times.push_back(ft);
        if (ft > DROPPED_FRAME_THRESHOLD_US)
            dropped++;
        per_panel[frame_ring_[idx].panel_id].push_back(ft);
    }

    std::sort(times.begin(), times.end());

    auto percentile = [&](double p) -> int {
        size_t idx = static_cast<size_t>(p * static_cast<double>(times.size() - 1));
        return static_cast<int>(times[idx] / 1000);
    };

    event["frame_time_p50_ms"] = percentile(0.50);
    event["frame_time_p95_ms"] = percentile(0.95);
    event["frame_time_p99_ms"] = percentile(0.99);
    event["dropped_frame_count"] = dropped;
    event["total_frame_count"] = static_cast<int64_t>(frame_ring_count_);

    // Per-panel breakdown
    json panels_obj = json::object();
    std::string worst_panel;
    int worst_p95 = 0;

    for (auto& panel_entry : per_panel) {
        uint16_t pid = panel_entry.first;
        auto& ptimes = panel_entry.second;
        if (pid >= panel_names_.size())
            continue;

        std::sort(ptimes.begin(), ptimes.end());
        size_t n = ptimes.size();

        auto panel_pct = [&](double p) -> int {
            size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1));
            return static_cast<int>(ptimes[idx] / 1000);
        };

        int p95_ms = panel_pct(0.95);
        if (p95_ms > worst_p95) {
            worst_p95 = p95_ms;
            worst_panel = panel_names_[pid];
        }

        int over_budget = 0;
        for (auto ft : ptimes) {
            if (ft > DROPPED_FRAME_THRESHOLD_US)
                over_budget++;
        }

        json panel_stats;
        panel_stats["p50_ms"] = panel_pct(0.50);
        panel_stats["p95_ms"] = p95_ms;
        panel_stats["p99_ms"] = panel_pct(0.99);
        panel_stats["max_ms"] = static_cast<int>(ptimes.back() / 1000);
        panel_stats["frames"] = static_cast<int>(n);
        panel_stats["over_budget_pct"] =
            std::round(static_cast<double>(over_budget) / static_cast<double>(n) * 1000.0) / 10.0;

        panels_obj[panel_names_[pid]] = panel_stats;
    }

    event["worst_panel"] = worst_panel;
    event["worst_panel_p95_ms"] = worst_p95;
    event["panels"] = panels_obj;
    event["task_handler_max_ms"] = 0;

    return event;
}

void TelemetryManager::record_performance_snapshot() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load())
        return;

    // Skip if no active frames were recorded since last snapshot
    if (frame_ring_count_ == 0) {
        spdlog::debug("[TelemetryManager] Skipping frame perf snapshot — no active frames");
        return;
    }

    spdlog::debug("[TelemetryManager] Recording performance snapshot (seq={})", snapshot_seq_);
    auto event = build_performance_snapshot_event();
    enqueue_event(event);

    frame_ring_count_ = 0;
    frame_ring_idx_ = 0;
}

// =============================================================================
// Feature Adoption Event
// =============================================================================

nlohmann::json TelemetryManager::build_feature_adoption_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "feature_adoption";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    auto has_panel = [&](const std::string& name) -> bool {
        auto it = panel_visits_.find(name);
        return it != panel_visits_.end() && it->second > 0;
    };

    auto has_widget = [&](const std::string& name) -> bool {
        auto it = widget_interactions_.find(name);
        return it != widget_interactions_.end() && it->second > 0;
    };

    json features;
    features["macros"] = has_panel("macros") || has_widget("favorite_macro");
    features["filament_management"] = has_panel("filament") || has_widget("filament");
    features["camera"] = has_panel("camera");
    features["console_gcode"] = has_panel("console");
    features["bed_mesh"] = has_panel("bed_mesh");
    features["input_shaper"] = has_panel("input_shaper");
    features["manual_probe"] = has_widget("manual_probe");
    features["spoolman"] = has_widget("spoolman");
    features["led_control"] = has_widget("led_control");
    features["power_devices"] = has_widget("power_device");
    features["multi_printer"] = has_widget("printer_switcher");
    features["theme_changed"] = (theme_manager_get_active_theme().name != helix::DEFAULT_THEME);
    features["timelapse"] = has_widget("timelapse");
    features["favorites"] = has_widget("favorite_macro");
    features["pid_calibration"] = has_panel("pid_calibration");
    features["firmware_retraction"] = has_panel("firmware_retraction");

    event["features"] = features;
    return event;
}

void TelemetryManager::record_feature_adoption() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load())
        return;

    spdlog::debug("[TelemetryManager] Recording feature adoption");
    auto event = build_feature_adoption_event();
    enqueue_event(event);
}

void TelemetryManager::start_feature_adoption_timer() {
    if (feature_adoption_timer_)
        return;

    feature_adoption_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->record_feature_adoption();
            }
            lv_timer_delete(timer);
            if (self)
                self->feature_adoption_timer_ = nullptr;
        },
        FEATURE_ADOPTION_DELAY_MS, this);
    lv_timer_set_repeat_count(feature_adoption_timer_, 1);
}

void TelemetryManager::stop_feature_adoption_timer() {
    if (feature_adoption_timer_) {
        lv_timer_delete(feature_adoption_timer_);
        feature_adoption_timer_ = nullptr;
    }
}

// =============================================================================
// Settings Changes Event
// =============================================================================

void TelemetryManager::notify_setting_changed(const std::string& setting_name,
                                              const std::string& old_value,
                                              const std::string& new_value) {
    if (!enabled_.load() || !initialized_.load() || shutting_down_.load())
        return;

    pending_settings_changes_.push_back({setting_name, old_value, new_value});
    spdlog::trace("[TelemetryManager] Setting changed: {}='{}' -> '{}'", setting_name, old_value,
                  new_value);

    start_settings_debounce_timer();
}

void TelemetryManager::flush_settings_changes() {
    if (pending_settings_changes_.empty() || !enabled_.load())
        return;

    auto event = build_settings_changes_event();
    enqueue_event(event);
    pending_settings_changes_.clear();
}

nlohmann::json TelemetryManager::build_settings_changes_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "settings_changes";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    json changes = json::array();
    for (const auto& c : pending_settings_changes_) {
        changes.push_back(
            {{"setting", c.setting}, {"old_value", c.old_value}, {"new_value", c.new_value}});
    }
    event["changes"] = changes;
    return event;
}

void TelemetryManager::start_settings_debounce_timer() {
    if (settings_debounce_timer_) {
        lv_timer_reset(settings_debounce_timer_);
        return;
    }

    settings_debounce_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->flush_settings_changes();
            }
            lv_timer_delete(timer);
            if (self)
                self->settings_debounce_timer_ = nullptr;
        },
        SETTINGS_DEBOUNCE_MS, this);
    lv_timer_set_repeat_count(settings_debounce_timer_, 1);
}

void TelemetryManager::stop_settings_debounce_timer() {
    if (settings_debounce_timer_) {
        lv_timer_delete(settings_debounce_timer_);
        settings_debounce_timer_ = nullptr;
    }
}

// =============================================================================
// AsyncLifetimeGuard skip-rate telemetry
//
// `helix::async_lifetime::note_skipped` is called from the three skip paths in
// include/async_lifetime_guard.h. The counters accumulate per-tag and are
// drained here on the periodic snapshot timer (and once at shutdown) so each
// event represents the delta window since the previous flush. A hot producer
// in this event is the early signal that an owner is repeatedly dying with
// pending work — the shape of bug 5KNWUEKY before it crashed (#1165).
// =============================================================================

void TelemetryManager::record_async_lifetime_snapshot() {
    // Drain first, unconditionally. The counters are global and always
    // accumulating, so an early return that skipped the drain would let a
    // telemetry-disabled span pile up and the next opt-in
    // (`set_enabled(true)`) would report a whole-session total as if it were
    // one window.
    auto snap = helix::async_lifetime::take_snapshot();

    if (shutting_down_.load() || !initialized_.load() || !enabled_.load()) {
        return;
    }

    // Skip empty windows: no deferred callbacks were dropped since the last
    // flush, so the event carries no signal and only burns an enqueue slot.
    // The shutdown flush is exempt — a zero-count shutdown event confirms the
    // session ended cleanly with no orphan work.
    if (snap.total == 0 && !is_shutdown_snapshot_) {
        return;
    }

    enqueue_event(build_async_lifetime_snapshot_event(snap));
}

nlohmann::json TelemetryManager::build_async_lifetime_snapshot_event(
    const helix::async_lifetime::SkipSnapshot& snap) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "async_lifetime_skips";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - init_time_);
    event["uptime_sec"] = static_cast<int>(uptime.count());
    event["is_shutdown"] = is_shutdown_snapshot_;

    event["total_skips"] = snap.total;
    event["other_count"] = snap.other_count;

    json tags = json::array();
    for (const auto& entry : snap.entries) {
        tags.push_back({{"tag", entry.tag}, {"count", entry.count}});
    }
    event["tags"] = std::move(tags);

    if (snap.total > 0) {
        spdlog::info("[TelemetryManager] AsyncLifetime skip snapshot: total={} distinct={}",
                     snap.total, snap.entries.size());
    } else {
        spdlog::debug("[TelemetryManager] AsyncLifetime skip snapshot: zero skips in window");
    }

    return event;
}

// =============================================================================
// Periodic Snapshot
// =============================================================================

void TelemetryManager::fire_periodic_snapshot() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load())
        return;

    spdlog::info("[TelemetryManager] Firing periodic snapshot (seq={})", snapshot_seq_);

    record_panel_usage();
    record_connection_stability();
    record_async_lifetime_snapshot();

    snapshot_seq_++;
    save_snapshot_state();
}

void TelemetryManager::start_snapshot_timer() {
    if (snapshot_timer_)
        return;

    spdlog::debug("[TelemetryManager] Starting snapshot timer (interval={}ms)",
                  SNAPSHOT_INTERVAL_MS);

    snapshot_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->fire_periodic_snapshot();
            }
        },
        SNAPSHOT_INTERVAL_MS, this);
}

void TelemetryManager::stop_snapshot_timer() {
    if (snapshot_timer_) {
        lv_timer_delete(snapshot_timer_);
        snapshot_timer_ = nullptr;
    }
}

void TelemetryManager::start_frame_perf_timer() {
    if (frame_perf_timer_)
        return;

    spdlog::debug("[TelemetryManager] Starting frame perf timer (interval={}ms)",
                  FRAME_PERF_INTERVAL_MS);

    frame_perf_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->record_performance_snapshot();
            }
        },
        FRAME_PERF_INTERVAL_MS, this);
}

void TelemetryManager::stop_frame_perf_timer() {
    if (frame_perf_timer_) {
        lv_timer_delete(frame_perf_timer_);
        frame_perf_timer_ = nullptr;
    }
}

void TelemetryManager::save_snapshot_state() const {
    if (config_dir_.empty())
        return;

    json state;
    state["snapshot_seq"] = snapshot_seq_;

    json time_map = json::object();
    for (const auto& [name, sec] : panel_time_sec_) {
        time_map[name] = sec;
    }
    state["panel_time_sec"] = time_map;

    json visit_map = json::object();
    for (const auto& [name, count] : panel_visits_) {
        visit_map[name] = count;
    }
    state["panel_visits"] = visit_map;

    json widget_map = json::object();
    for (const auto& [name, count] : widget_interactions_) {
        widget_map[name] = count;
    }
    state["widget_interactions"] = widget_map;

    state["overlay_open_count"] = overlay_open_count_;

    json overlay_visit_map = json::object();
    for (const auto& [name, count] : overlay_visits_) {
        overlay_visit_map[name] = count;
    }
    state["overlay_visits"] = overlay_visit_map;

    state["connect_count"] = connect_count_;
    state["disconnect_count"] = disconnect_count_;
    state["total_connected_sec"] = total_connected_sec_;
    state["total_disconnected_sec"] = total_disconnected_sec_;
    state["longest_disconnect_sec"] = longest_disconnect_sec_;
    state["klippy_error_count"] = klippy_error_count_;
    state["klippy_shutdown_count"] = klippy_shutdown_count_;

    auto path = fs::path(config_dir_) / "telemetry_snapshot.json";
    auto tmp_path = fs::path(config_dir_) / "telemetry_snapshot.json.tmp";

    try {
        std::ofstream ofs(tmp_path);
        ofs << state.dump(2);
        ofs.close();
        fs::rename(tmp_path, path);
        spdlog::debug("[TelemetryManager] Snapshot state saved to {}", path.string());
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to save snapshot state: {}", e.what());
    }
}

void TelemetryManager::load_snapshot_state() {
    auto path = fs::path(config_dir_) / "telemetry_snapshot.json";
    if (!fs::exists(path))
        return;

    try {
        std::ifstream ifs(path);
        auto state = json::parse(ifs);

        snapshot_seq_ = state.value("snapshot_seq", 0);

        if (state.contains("panel_time_sec")) {
            for (auto& [key, val] : state["panel_time_sec"].items()) {
                panel_time_sec_[key] = val.get<int>();
            }
        }
        if (state.contains("panel_visits")) {
            for (auto& [key, val] : state["panel_visits"].items()) {
                panel_visits_[key] = val.get<int>();
            }
        }
        if (state.contains("widget_interactions")) {
            for (auto& [key, val] : state["widget_interactions"].items()) {
                widget_interactions_[key] = val.get<int>();
            }
        }

        overlay_open_count_ = state.value("overlay_open_count", 0);
        if (state.contains("overlay_visits")) {
            for (auto& [key, val] : state["overlay_visits"].items()) {
                overlay_visits_[key] = val.get<int>();
            }
        }
        connect_count_ = state.value("connect_count", 0);
        disconnect_count_ = state.value("disconnect_count", 0);
        total_connected_sec_ = state.value("total_connected_sec", 0);
        total_disconnected_sec_ = state.value("total_disconnected_sec", 0);
        longest_disconnect_sec_ = state.value("longest_disconnect_sec", 0);
        klippy_error_count_ = state.value("klippy_error_count", 0);
        klippy_shutdown_count_ = state.value("klippy_shutdown_count", 0);

        spdlog::info("[TelemetryManager] Recovered snapshot state (seq={})", snapshot_seq_);
        fs::remove(path);
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to load snapshot state: {}", e.what());
    }
}

// =============================================================================
// Print Outcome Observer
// =============================================================================

namespace {

/// Tracks the previous print state to detect transitions to terminal states
// RAW_PRINT_STATE_OK: telemetry classifies the printer's own transitions.
// FIRST-TICK: seeded to STANDBY, so connecting to an already-running printer
// reads as one STANDBY -> PRINTING edge and resets the phase tracker. Harmless -
// nothing has been collected at that point - but do not copy the pattern into a
// site where a false first edge would discard data.
PrintJobState s_telemetry_prev_state = PrintJobState::STANDBY;

/// Guards against false completion on startup (first update may be stale)
bool s_telemetry_first_update = false;

/// Tracks the highest print start phase reached during the current print.
/// PrintStartPhase resets to IDLE after startup completes, so we capture the
/// max value seen to report how many phases were completed.
int s_telemetry_max_phase = 0;

/// Cached filament metadata from file metadata fetch at print start.
/// Written via ui_queue_update (main thread), read on main thread at print end.
std::string s_telemetry_filament_type;
float s_telemetry_filament_used_mm = 0.0f;

/// Observer callback for print state transitions (telemetry recording)
void on_print_state_changed_for_telemetry(lv_observer_t* observer, lv_subject_t* subject) {
    (void)observer;
    // PRINT_STATE_CAST_OK: `subject` IS print_state_enum. Telemetry's terminal
    // classification is deliberately on the wire.
    auto current = static_cast<PrintJobState>(lv_subject_get_int(subject));

    // Publish to off-main memory_warning context (relaxed: telemetry only).
    // Stored as the raw PrintJobState int — small integer enum, value space
    // matches what the receiver decodes.
    helix::telemetry_context::print_state_int.store(static_cast<int>(current),
                                                    std::memory_order_relaxed);

    // Skip the first callback -- state may be stale before Moonraker updates arrive
    if (!s_telemetry_first_update) {
        s_telemetry_first_update = true;
        s_telemetry_prev_state = current;
        spdlog::debug("[Telemetry] Print observer armed (initial state={})",
                      static_cast<int>(current));
        return;
    }

    // Track the highest print start phase reached during this print.
    // Read it on every state change so we capture the max before it resets to IDLE.
    auto& ps = get_printer_state();
    int phase = lv_subject_get_int(ps.get_print_start_phase_subject());
    if (phase > s_telemetry_max_phase) {
        s_telemetry_max_phase = phase;
    }

    // RAW_PRINT_STATE_OK: load-bearing. On the wire this is one reset at the
    // real start (STANDBY -> PRINTING). On the lifecycle it becomes
    // Idle -> Preparing -> Printing, so the reset would fire at
    // Preparing -> Printing and WIPE the pre-print phase data this tracker
    // exists to collect.
    // When a new print starts (transition to PRINTING from non-PAUSED), reset tracking
    if (current == PrintJobState::PRINTING && s_telemetry_prev_state != PrintJobState::PAUSED) {
        s_telemetry_max_phase = 0;
        // Re-read phase in case it's already set
        phase = lv_subject_get_int(ps.get_print_start_phase_subject());
        if (phase > s_telemetry_max_phase) {
            s_telemetry_max_phase = phase;
        }

        // Reset filament cache (prevent stale data from previous print)
        s_telemetry_filament_type.clear();
        s_telemetry_filament_used_mm = 0.0f;

        // Fetch file metadata to populate filament info for this print.
        // Note: if print ends before the async callback arrives, filament data
        // will be empty — this is acceptable (benign race, telemetry best-effort).
        const char* filename = lv_subject_get_string(ps.get_print_filename_subject());
        if (filename && filename[0] != '\0') {
            std::string fname(filename);
            spdlog::debug("[Telemetry] Fetching metadata for filament info: {}", fname);

            auto* api = get_moonraker_api();
            if (api) {
                api->files().get_file_metadata(
                    fname,
                    [](const FileMetadata& metadata) {
                        // Callback runs on background WebSocket thread —
                        // marshal cache write to main thread via ui_queue_update
                        std::string ftype = metadata.filament_type;
                        float ftotal = static_cast<float>(metadata.filament_total);
                        bool has_thumb = !metadata.thumbnails.empty();
                        int64_t fsize = metadata.size;
                        int est_dur = static_cast<int>(metadata.estimated_time);
                        std::string slicer_name = metadata.slicer;
                        helix::ui::queue_update([ftype = std::move(ftype), ftotal, has_thumb, fsize,
                                                 est_dur, slicer_name = std::move(slicer_name)]() {
                            s_telemetry_filament_type = ftype;
                            s_telemetry_filament_used_mm = ftotal;
                            spdlog::debug("[Telemetry] Cached filament: type='{}', total={:.1f}mm",
                                          s_telemetry_filament_type, s_telemetry_filament_used_mm);

                            // Determine print source: "in_app" if we started it,
                            // "external" otherwise (Mainsail, Fluidd, Obico, etc.)
                            bool in_app =
                                TelemetryManager::instance().consume_print_started_in_app();
                            std::string source = in_app ? "in_app" : "external";
                            int tools = ToolState::instance().tool_count();
                            bool ams =
                                lv_subject_get_int(AmsState::instance().get_ams_type_subject()) !=
                                static_cast<int>(AmsType::NONE);

                            TelemetryManager::instance().record_print_start_context(
                                source, has_thumb, fsize, est_dur, slicer_name, tools, ams);
                        });
                    },
                    [](const MoonrakerError& error) {
                        spdlog::warn(
                            "[Telemetry] Failed to fetch file metadata for filament info: {}",
                            error.message);
                    },
                    true // silent — don't log 404s for missing metadata
                );
            }
        }
    }

    // RAW_PRINT_STATE_OK: terminal-outcome classification is about what the
    // printer reported, and a preparing job that never confirms is retired by
    // PrinterPrintState rather than ending here.
    // Detect transitions from active (PRINTING/PAUSED) to terminal states
    bool was_active = (s_telemetry_prev_state == PrintJobState::PRINTING ||
                       s_telemetry_prev_state == PrintJobState::PAUSED);
    bool is_terminal = (current == PrintJobState::COMPLETE || current == PrintJobState::CANCELLED ||
                        current == PrintJobState::ERROR);

    if (was_active && is_terminal) {
        // Map PrintJobState to telemetry outcome string
        std::string outcome;
        switch (current) {
        case PrintJobState::COMPLETE:
            outcome = "success";
            break;
        case PrintJobState::CANCELLED:
            outcome = "cancelled";
            break;
        case PrintJobState::ERROR:
            outcome = "failure";
            break;
        default:
            outcome = "unknown";
            break;
        }

        // Gather data from PrinterState subjects
        int duration_sec = lv_subject_get_int(ps.get_print_elapsed_subject());
        int phases_completed = s_telemetry_max_phase;

        // Temperatures: subjects store decidegrees (value * 10), divide by 10
        int nozzle_temp_deci = lv_subject_get_int(ps.get_active_extruder_target_subject());
        int bed_temp_deci = lv_subject_get_int(ps.get_bed_target_subject());
        int nozzle_temp = helix::ui::temperature::deci_to_degrees(nozzle_temp_deci);
        int bed_temp = helix::ui::temperature::deci_to_degrees(bed_temp_deci);

        // Use filament data cached at print start from file metadata
        float filament_used_mm = s_telemetry_filament_used_mm;
        std::string filament_type = s_telemetry_filament_type;

        spdlog::info("[Telemetry] Print {} - duration={}s, phases={}, nozzle={}C, bed={}C, "
                     "filament='{}' {:.0f}mm",
                     outcome, duration_sec, phases_completed, nozzle_temp, bed_temp, filament_type,
                     filament_used_mm);

        TelemetryManager::instance().record_print_outcome(outcome, duration_sec, phases_completed,
                                                          filament_used_mm, filament_type,
                                                          nozzle_temp, bed_temp);

        // Reset phase tracking for next print
        s_telemetry_max_phase = 0;
    }

    s_telemetry_prev_state = current;
}

} // anonymous namespace

ObserverGuard TelemetryManager::init_print_outcome_observer() {
    // Reset state tracking on (re)initialization
    s_telemetry_first_update = false;
    // RAW_PRINT_STATE_OK: re-seed on disconnect; see the declaration.
    s_telemetry_prev_state = PrintJobState::STANDBY;
    s_telemetry_max_phase = 0;
    s_telemetry_filament_type.clear();
    s_telemetry_filament_used_mm = 0.0f;

    spdlog::debug("[Telemetry] Print outcome observer registered");
    return ObserverGuard(get_printer_state().get_print_state_enum_subject(),
                         on_print_state_changed_for_telemetry, nullptr);
}
