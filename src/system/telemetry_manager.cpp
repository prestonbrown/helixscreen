// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/telemetry_manager.h"

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
#include "hv/requests.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_types.h"
#include "platform_capabilities.h"
#include "printer_state.h"
#include "system/crash_handler.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "temperature_sensor_manager.h"
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

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace helix;

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
    connect_count_ = 0;
    disconnect_count_ = 0;
    total_connected_sec_ = 0;
    total_disconnected_sec_ = 0;
    longest_disconnect_sec_ = 0;
    klippy_error_count_ = 0;
    klippy_shutdown_count_ = 0;
    connection_tracking_connected_ = false;
    error_rate_limit_.clear();

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

    // Load enabled state from config (before crash check so opt-in is respected)
    try {
        std::string config_path = config_dir_ + "/telemetry_config.json";
        std::ifstream config_file(config_path);
        if (config_file.good()) {
            json config = json::parse(config_file);
            if (config.contains("enabled") && config["enabled"].is_boolean()) {
                enabled_.store(config["enabled"].get<bool>());
                spdlog::info("[TelemetryManager] Loaded enabled state: {}",
                             enabled_.load() ? "true" : "false");
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to load config, defaulting to disabled: {}",
                     e.what());
        enabled_.store(false);
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

    // Record session-summary events before shutting down
    record_panel_usage();
    record_connection_stability();

    shutting_down_.store(true);

    // Stop auto-send timer first (LVGL call — skip if LVGL already torn down)
    if (lv_is_initialized()) {
        stop_auto_send();
    } else {
        auto_send_timer_ = nullptr;
    }

    // Persist queue to disk
    save_queue();

    // Join background send thread if active
    if (send_thread_.joinable()) {
        spdlog::debug("[TelemetryManager] Joining send thread...");
        send_thread_.join();
    }

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

    // Update LVGL subject via queue to ensure thread safety — set_enabled()
    // may be called from background threads (telemetry callbacks)
    if (subjects_initialized_) {
        helix::ui::queue_update(
            [this, enabled]() { lv_subject_set_int(&enabled_subject_, enabled ? 1 : 0); });
    }

    // Persist to telemetry_config.json
    try {
        json config;
        config["enabled"] = enabled;

        std::string config_path = config_dir_ + "/telemetry_config.json";
        std::ofstream config_file(config_path);
        if (config_file.good()) {
            config_file << config.dump(2);
            spdlog::debug("[TelemetryManager] Persisted enabled state to {}", config_path);
        } else {
            spdlog::warn("[TelemetryManager] Failed to write config to {}", config_path);
        }
    } catch (const std::exception& e) {
        spdlog::error("[TelemetryManager] Failed to persist enabled state: {}", e.what());
    }
}

bool TelemetryManager::is_enabled() const {
    return enabled_.load();
}

// =============================================================================
// Event Recording
// =============================================================================

void TelemetryManager::record_session() {
    if (!enabled_.load() || !initialized_.load()) {
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
    if (!enabled_.load() || !initialized_.load()) {
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
    if (!enabled_.load() || !initialized_.load()) {
        return;
    }

    spdlog::info("[TelemetryManager] Recording update failure: reason={} version={}", reason,
                 version);
    auto event =
        build_update_failed_event(reason, version, platform, http_code, file_size, exit_code);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_memory_snapshot(const std::string& trigger) {
    if (!enabled_.load() || !initialized_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording memory snapshot (trigger={})", trigger);
    auto event = build_memory_snapshot_event(trigger);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_hardware_profile() {
    if (!enabled_.load() || !initialized_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording hardware profile");
    auto event = build_hardware_profile_event();
    enqueue_event(std::move(event));
}

void TelemetryManager::record_settings_snapshot() {
    if (!enabled_.load() || !initialized_.load()) {
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

    spdlog::trace("[TelemetryManager] Panel changed to '{}' (visits={})", panel_name,
                  panel_visits_[panel_name]);
}

void TelemetryManager::notify_overlay_opened() {
    overlay_open_count_++;
    spdlog::trace("[TelemetryManager] Overlay opened (count={})", overlay_open_count_);
}

void TelemetryManager::record_panel_usage() {
    if (!enabled_.load() || !initialized_.load()) {
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
    if (!enabled_.load() || !initialized_.load()) {
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

    if (enabled_.load()) {
        auto event = build_update_success_event(
            flag.value("version", "unknown"), flag.value("from_version", "unknown"),
            flag.value("platform", "unknown"), flag.value("timestamp", get_timestamp()));
        enqueue_event(std::move(event));
        save_queue();
        spdlog::info("[TelemetryManager] Enqueued update_success event (version={})",
                     flag.value("version", "unknown"));
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

void TelemetryManager::try_send() {
    if (!enabled_.load() || !initialized_.load() || shutting_down_.load()) {
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

    if (last_send_time_.time_since_epoch().count() > 0 && now - last_send_time_ < interval) {
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

    // Send on background thread; joined on next try_send() call or shutdown()
    send_thread_ = std::thread([this, batch = std::move(batch)]() { do_send(batch); });
}

void TelemetryManager::do_send(const nlohmann::json& batch) {
    try {
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
}

void TelemetryManager::stop_auto_send() {
    if (auto_send_timer_) {
        lv_timer_delete(auto_send_timer_);
        auto_send_timer_ = nullptr;
        spdlog::info("[TelemetryManager] Auto-send timer stopped");
    }
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

    spdlog::info("[TelemetryManager] Found crash file from previous session");

    auto crash_data = crash_handler::read_crash_file(crash_path);
    if (crash_data.is_null()) {
        spdlog::warn("[TelemetryManager] Failed to parse crash file, skipping telemetry event");
        return;
    }

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

    // Add platform (not in crash file — determined at runtime)
    event["app_platform"] = UpdateChecker::get_platform_key();

    // Only enqueue if telemetry is enabled (respect user opt-in)
    if (enabled_.load()) {
        enqueue_event(std::move(event));
        save_queue();
        spdlog::info("[TelemetryManager] Enqueued crash event (signal={}, name={})",
                     crash_data.value("signal", 0), crash_data.value("signal_name", "unknown"));
    } else {
        spdlog::debug("[TelemetryManager] Crash event discarded (telemetry disabled)");
    }

    // Note: crash file is NOT removed here — CrashReporter owns the lifecycle
    // and removes it after the user interacts with the crash report modal.
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
    return fans;
}

nlohmann::json TelemetryManager::build_hw_sensors_section() {
    json sensors;
    sensors["filament"] = static_cast<int>(FilamentSensorManager::instance().sensor_count());
    sensors["temperature_extra"] =
        static_cast<int>(sensors::TemperatureSensorManager::instance().sensor_count());
    sensors["color"] = static_cast<int>(sensors::ColorSensorManager::instance().sensor_count());
    sensors["accel"] = static_cast<int>(sensors::AccelSensorManager::instance().sensor_count());
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
    case AmsType::VALGACE:
        ams["type"] = "valgace";
        break;
    case AmsType::TOOL_CHANGER:
        ams["type"] = "tool_changer";
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
            const auto& bv = hw.build_volume();
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
            event["steppers"] = steppers;

            // ---- leds section ----
            json leds;
            leds["count"] = static_cast<int>(hw.leds().size());
            leds["has_led_effects"] = hw.has_led_effects();
            event["leds"] = leds;

            event["sensors"] = build_hw_sensors_section();
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

    event["theme"] = DisplaySettingsManager::instance().get_dark_mode() ? "dark" : "light";
    event["brightness_pct"] = DisplaySettingsManager::instance().get_brightness();
    event["screensaver_timeout_sec"] = DisplaySettingsManager::instance().get_display_dim_sec();
    event["screen_blank_timeout_sec"] = DisplaySettingsManager::instance().get_display_sleep_sec();
    event["locale"] = SystemSettingsManager::instance().get_language();
    event["sound_enabled"] = AudioSettingsManager::instance().get_sounds_enabled();
    event["auto_update_channel"] = SystemSettingsManager::instance().get_update_channel();
    event["animations_enabled"] = DisplaySettingsManager::instance().get_animations_enabled();
    event["time_format"] =
        DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12 ? "12h" : "24h";

    return event;
}

nlohmann::json TelemetryManager::build_panel_usage_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "panel_usage";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();

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

    event["overlay_open_count"] = overlay_open_count_;

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
    if (!enabled_.load() || !initialized_.load()) {
        return;
    }

    spdlog::debug("[TelemetryManager] Recording print start context: source={}, slicer={}", source,
                  slicer);
    auto event = build_print_start_context_event(source, has_thumbnail, file_size_bytes,
                                                 estimated_duration_sec, slicer, tool_count_used,
                                                 ams_active);
    enqueue_event(std::move(event));
}

void TelemetryManager::record_error(const std::string& category, const std::string& code,
                                    const std::string& context) {
    if (!enabled_.load() || !initialized_.load()) {
        return;
    }

    // Validate category against allow-list to prevent unbounded map growth
    static const std::array<std::string_view, 4> ALLOWED_CATEGORIES = {"moonraker_api", "websocket",
                                                                       "file_io", "display"};
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
// Print Outcome Observer
// =============================================================================

namespace {

/// Tracks the previous print state to detect transitions to terminal states
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
    auto current = static_cast<PrintJobState>(lv_subject_get_int(subject));

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

                            // Record print start context with metadata
                            std::string source = "local";
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

        // Temperatures: subjects store centidegrees (value * 10), divide by 10
        int nozzle_temp_centi = lv_subject_get_int(ps.get_active_extruder_target_subject());
        int bed_temp_centi = lv_subject_get_int(ps.get_bed_target_subject());
        int nozzle_temp = nozzle_temp_centi / 10;
        int bed_temp = bed_temp_centi / 10;

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
    s_telemetry_prev_state = PrintJobState::STANDBY;
    s_telemetry_max_phase = 0;
    s_telemetry_filament_type.clear();
    s_telemetry_filament_used_mm = 0.0f;

    spdlog::debug("[Telemetry] Print outcome observer registered");
    return ObserverGuard(get_printer_state().get_print_state_enum_subject(),
                         on_print_state_changed_for_telemetry, nullptr);
}
