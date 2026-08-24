// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/crash_reporter.h"

#include "helix_version.h"
#include "hv/requests.h"
#include "platform_capabilities.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/log_collector.h"
#include "system/update_checker.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <SDL.h>
#include <jni.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/// Parse ISO 8601 UTC timestamp ("2026-04-19T03:55:36Z") to epoch seconds.
/// Returns -1 on failure.
int64_t parse_iso_utc_epoch(const std::string& iso) {
    std::tm tm{};
    std::istringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail())
        return -1;
    return static_cast<int64_t>(timegm(&tm));
}

/// Try to extract a timestamp from a log line, returning epoch seconds.
/// Supports two formats, both assumed to be in LOCAL time (matches spdlog's
/// default and syslog/journalctl output):
///   - File-log / spdlog:  "[YYYY-MM-DD HH:MM:SS..."
///   - Syslog / journalctl: "MMM DD HH:MM:SS ..." (year is inferred)
/// Returns -1 if the line does not start with a recognizable timestamp.
int64_t parse_line_local_epoch(const std::string& line, int crash_year) {
    // File-log format: "[YYYY-MM-DD HH:MM:SS..."
    if (line.size() > 20 && line[0] == '[') {
        std::tm tm{};
        std::istringstream ss(line.substr(1, 19));
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (!ss.fail()) {
            tm.tm_isdst = -1;
            auto t = std::mktime(&tm);
            if (t != -1)
                return static_cast<int64_t>(t);
        }
    }
    // Syslog / journalctl format: "MMM DD HH:MM:SS host prog[pid]: ..."
    if (line.size() >= 15) {
        std::tm tm{};
        std::istringstream ss(line.substr(0, 15));
        ss >> std::get_time(&tm, "%b %d %H:%M:%S");
        if (!ss.fail()) {
            tm.tm_year = crash_year - 1900;
            tm.tm_isdst = -1;
            auto t = std::mktime(&tm);
            // Year-boundary fallback: if the parsed date is more than ~6 months
            // after the crash, assume it's actually from the previous year
            // (e.g., crash Jan 2, line "Dec 30"). Rare, but cheap to handle.
            if (t != -1) {
                return static_cast<int64_t>(t);
            }
        }
    }
    return -1;
}

/// Filter newline-separated `content` to lines whose parsed local-time
/// timestamp is less than or equal to `cutoff_epoch`. Lines without a
/// parseable timestamp are kept (don't silently drop context).
///
/// The cutoff is the crash timestamp — everything after that is from the
/// restarted session reporting the crash, not from the crashed process.
std::string filter_lines_before(const std::string& content, int64_t cutoff_epoch, int crash_year) {
    std::istringstream stream(content);
    std::string line;
    std::ostringstream out;
    bool first = true;
    int dropped = 0;
    while (std::getline(stream, line)) {
        auto line_epoch = parse_line_local_epoch(line, crash_year);
        // Allow a small skew window: lines up to 2s after the cutoff are kept
        // (helps when the crash handler log entry has a slightly later stamp
        // than the crash timestamp itself).
        if (line_epoch > 0 && line_epoch > cutoff_epoch + 2) {
            ++dropped;
            continue;
        }
        if (!first)
            out << '\n';
        out << line;
        first = false;
    }
    if (dropped > 0) {
        spdlog::debug("[CrashReporter] Filtered {} post-crash log lines", dropped);
    }
    return out.str();
}

} // namespace

// =============================================================================
// Singleton
// =============================================================================

bool heap_snapshot_age_is_plausible(long age_ms, int uptime_sec) {
    if (age_ms < 0) {
        return false;
    }
    if (uptime_sec <= 0) {
        return true; // nothing to check against
    }
    // uptime is truncated to whole seconds, so allow a couple of seconds of
    // slack before calling an age impossible.
    constexpr long SLACK_MS = 2000;
    return age_ms <= static_cast<long>(uptime_sec) * 1000 + SLACK_MS;
}

CrashReporter& CrashReporter::instance() {
    static CrashReporter instance;
    return instance;
}

// =============================================================================
// Lifecycle
// =============================================================================

void CrashReporter::init(const std::string& config_dir) {
    if (initialized_) {
        shutdown();
    }
    config_dir_ = config_dir;
    initialized_ = true;
    spdlog::debug("[CrashReporter] Initialized with config dir: {}", config_dir_);
}

void CrashReporter::shutdown() {
    config_dir_.clear();
    initialized_ = false;
}

// =============================================================================
// Detection
// =============================================================================

bool CrashReporter::has_crash_report() const {
    if (!initialized_) {
        return false;
    }
    return crash_handler::has_crash_file(crash_file_path());
}

std::string CrashReporter::crash_file_path() const {
    return config_dir_ + "/crash.txt";
}

std::string CrashReporter::report_file_path() const {
    return config_dir_ + "/crash_report.txt";
}

// =============================================================================
// Report Collection
// =============================================================================

CrashReporter::CrashReport CrashReporter::collect_report() {
    CrashReport report;

    // Parse crash.txt via existing crash_handler
    auto crash_data = crash_handler::read_crash_file(crash_file_path());
    if (crash_data.is_null()) {
        spdlog::warn("[CrashReporter] Failed to parse crash file");
        return report;
    }

    // Extract crash data fields
    report.signal = crash_data.value("signal", 0);
    report.signal_name = crash_data.value("signal_name", "UNKNOWN");
    report.app_version = crash_data.value("app_version", "unknown");
    report.timestamp = crash_data.value("timestamp", "");
    report.uptime_sec = crash_data.value("uptime_sec", 0);

    if (crash_data.contains("exception"))
        report.exception_what = crash_data["exception"];

    if (crash_data.contains("abort_msg"))
        report.abort_msg = crash_data["abort_msg"];

    if (crash_data.contains("abort_msg_state"))
        report.abort_msg_state = crash_data["abort_msg_state"];

    if (crash_data.contains("terminate_msg"))
        report.terminate_msg = crash_data["terminate_msg"];

    if (crash_data.contains("recent_errors") && crash_data["recent_errors"].is_array()) {
        for (const auto& e : crash_data["recent_errors"]) {
            report.recent_errors.push_back(e.get<std::string>());
        }
    }

    if (crash_data.contains("backtrace") && crash_data["backtrace"].is_array()) {
        for (const auto& addr : crash_data["backtrace"]) {
            report.backtrace.push_back(addr.get<std::string>());
        }
    }

    // Fault info
    if (crash_data.contains("fault_addr"))
        report.fault_addr = crash_data["fault_addr"];
    if (crash_data.contains("fault_code"))
        report.fault_code = crash_data["fault_code"];
    if (crash_data.contains("fault_code_name"))
        report.fault_code_name = crash_data["fault_code_name"];

    // Memory map (from /proc/self/maps)
    if (crash_data.contains("memory_map") && crash_data["memory_map"].is_array()) {
        for (const auto& line : crash_data["memory_map"]) {
            report.memory_map.push_back(line.get<std::string>());
        }
    }

    // Register state (Phase 2)
    if (crash_data.contains("reg_pc"))
        report.reg_pc = crash_data["reg_pc"];
    if (crash_data.contains("reg_sp"))
        report.reg_sp = crash_data["reg_sp"];
    if (crash_data.contains("reg_lr"))
        report.reg_lr = crash_data["reg_lr"];
    else if (crash_data.contains("reg_ra"))
        report.reg_lr = crash_data["reg_ra"]; // MIPS spells link register "ra"
    if (crash_data.contains("reg_bp"))
        report.reg_bp = crash_data["reg_bp"];

    // ASLR load base (for symbol resolution)
    if (crash_data.contains("load_base"))
        report.load_base = crash_data["load_base"];

    // UpdateQueue callback tag
    if (crash_data.contains("queue_callback"))
        report.queue_callback = crash_data["queue_callback"];

    // Current LVGL event (from event_send_core hook)
    if (crash_data.contains("event_target"))
        report.event_target = crash_data["event_target"];
    if (crash_data.contains("event_original_target"))
        report.event_original_target = crash_data["event_original_target"];
    if (crash_data.contains("event_code"))
        report.event_code = crash_data["event_code"];

    // Heap snapshot (cached from main loop)
    auto copy_heap = [&](const char* key, long& dst) {
        if (crash_data.contains(key)) {
            dst = crash_data[key].get<long>();
            report.heap.present = true;
        }
    };
    auto copy_heap_int = [&](const char* key, int& dst) {
        if (crash_data.contains(key)) {
            dst = static_cast<int>(crash_data[key].get<long>());
            report.heap.present = true;
        }
    };
    copy_heap("heap_snapshot_age_ms", report.heap.age_ms);
    copy_heap("heap_snapshot_ts_ms", report.heap.snapshot_ts_ms);
    copy_heap("heap_mono_ms_now", report.heap.mono_ms_now);
    copy_heap("heap_rss_kb", report.heap.rss_kb);
    copy_heap("heap_vsz_kb", report.heap.vsz_kb);
    copy_heap("heap_arena_kb", report.heap.arena_kb);
    copy_heap("heap_used_kb", report.heap.used_kb);
    copy_heap("heap_free_kb", report.heap.free_kb);
    copy_heap("heap_mmap_kb", report.heap.mmap_kb);
    copy_heap("lv_heap_total_kb", report.heap.lv_total_kb);
    copy_heap_int("lv_heap_used_pct", report.heap.lv_used_pct);
    copy_heap_int("lv_heap_frag_pct", report.heap.lv_frag_pct);
    copy_heap("lv_heap_free_biggest_kb", report.heap.lv_free_biggest_kb);

    // Activity breadcrumbs from the in-process ring buffer
    if (crash_data.contains("breadcrumbs") && crash_data["breadcrumbs"].is_array()) {
        for (const auto& line : crash_data["breadcrumbs"]) {
            report.breadcrumbs.push_back(line.get<std::string>());
        }
    }

    // Stack-scanned backtrace metadata
    if (crash_data.contains("bt_source"))
        report.bt_source = crash_data["bt_source"];
    if (crash_data.contains("text_start"))
        report.text_start = crash_data["text_start"];
    if (crash_data.contains("text_end"))
        report.text_end = crash_data["text_end"];

    // Stack dump (ARM32/MIPS: raw stack words for offline analysis)
    if (crash_data.contains("stack_base"))
        report.stack_base = crash_data["stack_base"];
    if (crash_data.contains("stack_dump") && crash_data["stack_dump"].is_array()) {
        for (const auto& word : crash_data["stack_dump"]) {
            report.stack_dump.push_back(word.get<std::string>());
        }
    }

    // Extra registers (reg_r0, reg_r1, ..., reg_fp, reg_ip, etc.)
    static const std::vector<std::string> extra_reg_names = {
        "reg_r0", "reg_r1", "reg_r2", "reg_r3",  "reg_r4", "reg_r5", "reg_r6",
        "reg_r7", "reg_r8", "reg_r9", "reg_r10", "reg_fp", "reg_ip", "reg_ra",
    };
    for (const auto& reg : extra_reg_names) {
        if (crash_data.contains(reg)) {
            report.extra_registers.emplace_back(reg.substr(4), crash_data[reg].get<std::string>());
        }
    }

    // Collect additional system context
    report.platform = UpdateChecker::get_platform_key();
#ifdef HELIX_BINARY_VARIANT
    report.display_info = HELIX_BINARY_VARIANT;
#else
    report.display_info = "unknown";
#endif

    auto caps = helix::PlatformCapabilities::detect();
    report.ram_total_mb = static_cast<int>(caps.total_ram_mb);
    report.cpu_cores = caps.cpu_cores;

    // Log tail — collect a generous buffer (the new reporting session writes
    // its own boot lines, which flood a small tail). We then filter to lines
    // at or before the crash timestamp, so the report contains pre-crash
    // context rather than post-restart noise (see crash report #827).
    report.log_tail = get_log_tail(500);
    if (!report.log_tail.empty() && !report.timestamp.empty()) {
        auto cutoff = parse_iso_utc_epoch(report.timestamp);
        if (cutoff > 0) {
            // crash_year: extract YYYY from "YYYY-MM-DDT..." for syslog lines
            int crash_year = 0;
            try {
                crash_year = std::stoi(report.timestamp.substr(0, 4));
            } catch (...) {
                crash_year = 0;
            }
            if (crash_year > 0) {
                auto filtered = filter_lines_before(report.log_tail, cutoff, crash_year);
                // Safety net: if filtering drops everything (e.g., clock skew or
                // unexpected line format), keep the unfiltered tail.
                if (!filtered.empty()) {
                    report.log_tail = std::move(filtered);
                }
            }
        }
    }

    // Printer/Klipper info — these may not be available at startup
    // (no Moonraker connection yet), so left empty until connected
    // The modal or caller can populate these later if Moonraker is available

    spdlog::info(
        "[CrashReporter] Collected report: {} (signal {}), platform={}, RAM={}MB, cores={}",
        report.signal_name, report.signal, report.platform, report.ram_total_mb, report.cpu_cores);

    return report;
}

// =============================================================================
// Log Tail
// =============================================================================

std::string CrashReporter::get_log_tail(int num_lines) {
    // Build the file-log search list: defaults plus the test-visible config_dir
    // log (tests redirect config_dir_ to a tmp path and write helix.log there).
    // Then cascade through syslog + journalctl via helix::logs::tail_best so
    // embedded platforms (AD5M/AD5X via syslog, pi/pi32 via journald) stop
    // returning empty when they have no file sink.
    //
    // Tests set isolated_log_paths_ via CrashReporterTestAccess to skip system
    // log sources entirely, otherwise journalctl/syslog from the dev host
    // leaks into assertions that expect an empty tail.
    std::vector<std::string> paths;
    if (!isolated_log_paths_) {
        paths = helix::logs::default_file_paths();
    }
    if (!config_dir_.empty()) {
        paths.push_back(config_dir_ + "/helix.log");
    }
    auto result = isolated_log_paths_ ? helix::logs::tail_file(paths, num_lines)
                                      : helix::logs::tail_best(num_lines, paths);
    if (result.empty()) {
        spdlog::debug("[CrashReporter] No log source produced content for log tail");
    }
    return result;
}

// =============================================================================
// Report Formatting
// =============================================================================

nlohmann::json CrashReporter::report_to_json(const CrashReport& report) {
    json j;
    j["signal"] = report.signal;
    j["signal_name"] = report.signal_name;
    j["app_version"] = report.app_version;
    j["timestamp"] = report.timestamp;
    j["uptime_seconds"] = report.uptime_sec;
    j["backtrace"] = report.backtrace;
    j["platform"] = report.platform;
    j["printer_model"] = report.printer_model;
    j["klipper_version"] = report.klipper_version;
    j["display_backend"] = report.display_info;
    j["ram_mb"] = report.ram_total_mb;
    j["cpu_cores"] = report.cpu_cores;

    // Exception message (for EXCEPTION crashes)
    if (!report.exception_what.empty()) {
        j["exception"] = report.exception_what;
    }

    // glibc abort reason for SIGABRT (e.g. "free(): invalid pointer")
    if (!report.abort_msg.empty()) {
        j["abort_msg"] = report.abort_msg;
    }
    if (!report.abort_msg_state.empty()) {
        j["abort_msg_state"] = report.abort_msg_state;
    }
    if (!report.terminate_msg.empty()) {
        j["terminate_msg"] = report.terminate_msg;
    }
    if (!report.recent_errors.empty()) {
        j["recent_errors"] = report.recent_errors;
    }

    // Fault info (only when present)
    if (!report.fault_addr.empty()) {
        j["fault_addr"] = report.fault_addr;
        j["fault_code"] = report.fault_code;
        j["fault_code_name"] = report.fault_code_name;
    }

    // Register state (only when present)
    if (!report.reg_pc.empty()) {
        json regs;
        regs["pc"] = report.reg_pc;
        regs["sp"] = report.reg_sp;
        if (!report.reg_lr.empty())
            regs["lr"] = report.reg_lr;
        if (!report.reg_bp.empty())
            regs["bp"] = report.reg_bp;
        j["registers"] = regs;
    }

    // ASLR load base (for symbol resolution)
    if (!report.load_base.empty()) {
        j["load_base"] = report.load_base;
    }

    // UpdateQueue callback tag (crash context)
    if (!report.queue_callback.empty()) {
        j["queue_callback"] = report.queue_callback;
    }

    // Current LVGL event at crash time
    if (!report.event_target.empty()) {
        j["event_target"] = report.event_target;
        if (!report.event_original_target.empty())
            j["event_original_target"] = report.event_original_target;
        j["event_code"] = report.event_code;
    }

    // Cached heap snapshot
    if (report.heap.present) {
        json h;
        h["age_ms"] = report.heap.age_ms;
        // Raw stamps the age came from, so a reader can tell a 32-bit monotonic
        // wrap from a stalled refresh when the age looks wrong.
        h["snapshot_ts_ms"] = report.heap.snapshot_ts_ms;
        h["mono_ms_now"] = report.heap.mono_ms_now;
        h["age_plausible"] = heap_snapshot_age_is_plausible(report.heap.age_ms, report.uptime_sec);
        h["rss_kb"] = report.heap.rss_kb;
        h["vsz_kb"] = report.heap.vsz_kb;
        if (report.heap.arena_kb) {
            h["arena_kb"] = report.heap.arena_kb;
            h["used_kb"] = report.heap.used_kb;
            h["free_kb"] = report.heap.free_kb;
            h["mmap_kb"] = report.heap.mmap_kb;
        }
        if (report.heap.lv_total_kb) {
            h["lv_total_kb"] = report.heap.lv_total_kb;
            h["lv_used_pct"] = report.heap.lv_used_pct;
            h["lv_frag_pct"] = report.heap.lv_frag_pct;
            h["lv_free_biggest_kb"] = report.heap.lv_free_biggest_kb;
        }
        j["heap"] = h;
    }

    // Activity breadcrumbs (in-process ring buffer)
    if (!report.breadcrumbs.empty()) {
        j["breadcrumbs"] = report.breadcrumbs;
    }

    // Stack-scanned backtrace metadata
    if (!report.bt_source.empty())
        j["bt_source"] = report.bt_source;
    if (!report.text_start.empty()) {
        j["text_start"] = report.text_start;
        j["text_end"] = report.text_end;
    }

    // Memory map (all mappings — worker needs full map to identify crash address regions)
    if (!report.memory_map.empty()) {
        j["memory_map"] = report.memory_map;
    }

    // Stack dump (raw stack words for return-address scanning)
    if (!report.stack_base.empty()) {
        j["stack_base"] = report.stack_base;
    }
    if (!report.stack_dump.empty()) {
        j["stack_dump"] = report.stack_dump;
    }

    // Extra registers (ARM32: r0-r12, fp, ip)
    if (!report.extra_registers.empty()) {
        json extra_regs = json::object();
        for (const auto& [name, value] : report.extra_registers) {
            extra_regs[name] = value;
        }
        j["extra_registers"] = extra_regs;
    }

    // Worker expects log_tail as an array of lines
    if (!report.log_tail.empty()) {
        json lines = json::array();
        std::istringstream stream(report.log_tail);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }
        j["log_tail"] = lines;
    }

    // Debug bundle share code (only set when the crash modal auto-attached a
    // bundle — the worker uses this to link the issue to R2-stored context).
    if (!report.debug_bundle_share_code.empty()) {
        j["debug_bundle_share_code"] = report.debug_bundle_share_code;
    }

    return j;
}

std::string CrashReporter::report_to_text(const CrashReport& report) {
    std::ostringstream ss;

    ss << "=== HelixScreen Crash Report ===\n\n";

    ss << "--- Crash Summary ---\n";
    ss << "Signal:    " << report.signal << " (" << report.signal_name << ")\n";
    ss << "Version:   " << report.app_version << "\n";
    ss << "Timestamp: " << report.timestamp << "\n";
    ss << "Uptime:    " << report.uptime_sec << " seconds\n";
    if (!report.exception_what.empty()) {
        ss << "Exception: " << report.exception_what << "\n";
    }
    ss << "\n";

    if (!report.fault_addr.empty()) {
        ss << "Fault Information\n";
        ss << "  Fault Address: " << report.fault_addr << "\n";
        ss << "  Fault Code: " << report.fault_code << " (" << report.fault_code_name << ")\n";
    }

    if (!report.reg_pc.empty()) {
        ss << "Registers\n";
        ss << "  PC: " << report.reg_pc << "\n";
        ss << "  SP: " << report.reg_sp << "\n";
        if (!report.reg_lr.empty())
            ss << "  LR: " << report.reg_lr << "\n";
        if (!report.reg_bp.empty())
            ss << "  BP: " << report.reg_bp << "\n";
    }

    if (!report.load_base.empty()) {
        ss << "Load Base: " << report.load_base << "\n";
    }

    if (!report.queue_callback.empty()) {
        ss << "Queue Callback: " << report.queue_callback << "\n";
    }

    if (!report.event_target.empty()) {
        ss << "LVGL Event: target=" << report.event_target;
        if (!report.event_original_target.empty())
            ss << " original=" << report.event_original_target;
        ss << " code=" << report.event_code << "\n";
    }

    if (report.heap.present) {
        ss << "Heap (age " << report.heap.age_ms << "ms): " << "RSS=" << report.heap.rss_kb
           << "kB VSZ=" << report.heap.vsz_kb << "kB";
        if (report.heap.arena_kb) {
            ss << " arena=" << report.heap.arena_kb << "kB used=" << report.heap.used_kb
               << "kB free=" << report.heap.free_kb << "kB";
        }
        ss << "\n";
        if (!heap_snapshot_age_is_plausible(report.heap.age_ms, report.uptime_sec)) {
            ss << "  WARNING: snapshot age exceeds process uptime (" << report.uptime_sec
               << "s) — heap figures above are NOT trustworthy. ts=" << report.heap.snapshot_ts_ms
               << "ms now=" << report.heap.mono_ms_now << "ms ("
               << (report.heap.snapshot_ts_ms > report.heap.mono_ms_now
                       ? "ts > now: 32-bit monotonic wrap"
                       : "refresh stalled")
               << ")\n";
        }
        if (report.heap.lv_total_kb) {
            ss << "LVGL heap: total=" << report.heap.lv_total_kb
               << "kB used=" << report.heap.lv_used_pct << "% frag=" << report.heap.lv_frag_pct
               << "% biggest_free=" << report.heap.lv_free_biggest_kb << "kB\n";
        }
    }

    if (!report.breadcrumbs.empty()) {
        ss << "--- Breadcrumbs (last " << report.breadcrumbs.size() << ") ---\n";
        for (const auto& line : report.breadcrumbs) {
            ss << "  " << line << "\n";
        }
        ss << "\n";
    }

    ss << "--- System Info ---\n";
    ss << "Platform:  " << report.platform << "\n";
    ss << "RAM:       " << report.ram_total_mb << " MB\n";
    ss << "CPU Cores: " << report.cpu_cores << "\n";
    ss << "Display:   " << report.display_info << "\n";
    ss << "Printer:   " << report.printer_model << "\n";
    ss << "Klipper:   " << report.klipper_version << "\n\n";

    if (!report.backtrace.empty()) {
        ss << "--- Backtrace ---\n";
        for (const auto& addr : report.backtrace) {
            ss << addr << "\n";
        }
        ss << "\n";
    }

    if (!report.memory_map.empty()) {
        ss << "--- Memory Map ---\n";
        for (const auto& line : report.memory_map) {
            ss << line << "\n";
        }
        ss << "\n";
    }

    if (!report.extra_registers.empty()) {
        ss << "--- Extra Registers ---\n";
        for (const auto& [name, value] : report.extra_registers) {
            ss << "  " << name << ": " << value << "\n";
        }
        ss << "\n";
    }

    if (!report.stack_dump.empty()) {
        ss << "--- Stack Dump (" << report.stack_dump.size() << " words from " << report.stack_base
           << ") ---\n";
        for (size_t i = 0; i < report.stack_dump.size(); ++i) {
            ss << "  [SP+0x" << std::hex << (i * 4) << "] " << report.stack_dump[i] << "\n";
        }
        ss << std::dec << "\n";
    }

    if (!report.log_tail.empty()) {
        // Count lines to make the label accurate — the tail is filtered to
        // pre-crash content, so its length varies per report.
        int line_count = 1;
        for (char c : report.log_tail) {
            if (c == '\n')
                ++line_count;
        }
        ss << "--- Log Tail (" << line_count << " pre-crash lines) ---\n";
        ss << report.log_tail << "\n";
    }

    return ss.str();
}

// =============================================================================
// GitHub URL
// =============================================================================

std::string CrashReporter::generate_github_url(const CrashReport& report) {
    // Build a pre-filled GitHub issue URL
    // Must stay under ~2000 chars for QR code compatibility

    std::string title = "Crash: " + report.signal_name + " in v" + report.app_version;

    std::ostringstream body;
    body << "## Crash Summary\n";
    body << "- **Signal:** " << report.signal << " (" << report.signal_name << ")\n";
    body << "- **Version:** " << report.app_version << "\n";
    body << "- **Platform:** " << report.platform << "\n";
    body << "- **Uptime:** " << report.uptime_sec << "s\n";
    if (!report.exception_what.empty()) {
        body << "- **Exception:** " << report.exception_what << "\n";
    }
    if (!report.abort_msg.empty()) {
        body << "- **Abort Reason:** `" << report.abort_msg << "`\n";
    }
    // The std::terminate reason behind an otherwise-blank SIGABRT (issue #987).
    if (!report.terminate_msg.empty()) {
        body << "- **Terminate Reason:** `" << report.terminate_msg << "`\n";
    }
    // Make a blank abort reason self-explanatory rather than silently missing.
    if (report.abort_msg.empty() && !report.abort_msg_state.empty() &&
        report.abort_msg_state != "present") {
        body << "- **Abort Reason:** _(none captured — " << report.abort_msg_state << ")_\n";
    }
    if (!report.recent_errors.empty()) {
        body << "- **Recent errors (newest first):**\n";
        for (const auto& e : report.recent_errors) {
            body << "  - `" << e << "`\n";
        }
    }
    if (!report.fault_code_name.empty()) {
        body << "- **Fault:** " << report.fault_code_name << " at " << report.fault_addr << "\n";
    }
    if (!report.reg_pc.empty()) {
        body << "- **PC:** " << report.reg_pc;
        if (!report.reg_lr.empty())
            body << " **LR:** " << report.reg_lr;
        body << "\n";
    }
    if (!report.queue_callback.empty()) {
        body << "- **Active Callback:** " << report.queue_callback << "\n";
    }
    if (!report.event_target.empty()) {
        body << "- **LVGL Event:** target=`" << report.event_target << "`";
        if (!report.event_original_target.empty())
            body << " original=`" << report.event_original_target << "`";
        body << " code=" << report.event_code << "\n";
    }
    if (report.heap.present) {
        body << "- **Heap:** RSS " << report.heap.rss_kb << "kB";
        if (report.heap.arena_kb) {
            body << ", arena " << report.heap.arena_kb << "kB (" << report.heap.used_kb
                 << " used / " << report.heap.free_kb << " free)";
        }
        if (report.heap.lv_total_kb) {
            body << ", LVGL " << report.heap.lv_used_pct << "% used " << report.heap.lv_frag_pct
                 << "% frag";
        }
        body << " (age " << report.heap.age_ms << "ms)\n";
        if (!heap_snapshot_age_is_plausible(report.heap.age_ms, report.uptime_sec)) {
            // Say so in the issue itself — this figure was chased as a real
            // signal once already (bundle ED2YC336).
            body << "  - ⚠️ Snapshot age exceeds the process uptime of " << report.uptime_sec
                 << "s, so the heap figures above are not trustworthy. Raw stamps: ts "
                 << report.heap.snapshot_ts_ms << "ms, crash clock " << report.heap.mono_ms_now
                 << "ms — "
                 << (report.heap.snapshot_ts_ms > report.heap.mono_ms_now
                         ? "ts is ahead of the crash clock, i.e. the 32-bit monotonic counter "
                           "wrapped (~49.7 days of uptime)."
                         : "the main loop's 10s snapshot refresh stalled.")
                 << "\n";
        }
    }
    if (!report.load_base.empty()) {
        body << "- **Load Base:** " << report.load_base << "\n";
    }
    body << "\n";

    if (!report.breadcrumbs.empty()) {
        body << "## Breadcrumbs\n```\n";
        // Cap breadcrumb lines to keep the URL under QR-code length.
        size_t max_b = std::min(report.breadcrumbs.size(), static_cast<size_t>(16));
        size_t start = report.breadcrumbs.size() - max_b;
        for (size_t i = start; i < report.breadcrumbs.size(); ++i) {
            body << report.breadcrumbs[i] << "\n";
        }
        if (start > 0) {
            body << "... (" << start << " older breadcrumbs omitted)\n";
        }
        body << "```\n\n";
    }

    if (!report.backtrace.empty()) {
        body << "## Backtrace\n```\n";
        // Limit backtrace entries to keep URL short
        size_t max_bt = std::min(report.backtrace.size(), static_cast<size_t>(10));
        for (size_t i = 0; i < max_bt; ++i) {
            body << report.backtrace[i] << "\n";
        }
        if (report.backtrace.size() > max_bt) {
            body << "... (" << (report.backtrace.size() - max_bt) << " more frames)\n";
        }
        body << "```\n";
    }

    // URL-encode the title and body
    auto url_encode = [](const std::string& str) -> std::string {
        std::ostringstream encoded;
        for (unsigned char c : str) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded << c;
            } else if (c == ' ') {
                encoded << '+';
            } else {
                encoded << '%' << "0123456789ABCDEF"[c >> 4] << "0123456789ABCDEF"[c & 0xF];
            }
        }
        return encoded.str();
    };

    std::string url = "https://github.com/" + std::string(GITHUB_REPO) +
                      "/issues/new?title=" + url_encode(title) + "&body=" + url_encode(body.str()) +
                      "&labels=crash,auto-reported";

    // Truncate body if URL exceeds 2000 chars
    if (url.size() > 2000) {
        // Rebuild with minimal body
        std::string minimal_body = "## Crash: " + report.signal_name + " in v" +
                                   report.app_version + " on " + report.platform;
        url = "https://github.com/" + std::string(GITHUB_REPO) +
              "/issues/new?title=" + url_encode(title) + "&body=" + url_encode(minimal_body) +
              "&labels=crash,auto-reported";
    }

    return url;
}

// =============================================================================
// File Save
// =============================================================================

bool CrashReporter::save_to_file(const CrashReport& report) {
    std::string path = report_file_path();

    // Ensure parent directory exists
    std::error_code ec;
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
    }

    std::ofstream ofs(path);
    if (!ofs.good()) {
        spdlog::error("[CrashReporter] Cannot write report file: {}", path);
        return false;
    }

    ofs << report_to_text(report);
    ofs.close();

    if (ofs.fail()) {
        spdlog::error("[CrashReporter] Failed to write report file: {}", path);
        return false;
    }

    spdlog::info("[CrashReporter] Saved crash report to: {}", path);
    return true;
}

// =============================================================================
// Crash File Lifecycle
// =============================================================================

void CrashReporter::consume_crash_file() {
    // Rotate crash files to keep the last 3 raw crash dumps for analysis:
    //   crash.txt → crash_1.txt → crash_2.txt → crash_3.txt (oldest dropped)
    namespace fs = std::filesystem;
    std::string base = config_dir_ + "/crash";
    std::error_code ec;

    fs::remove(base + "_3.txt", ec);
    for (int i = 2; i >= 1; --i) {
        fs::rename(base + "_" + std::to_string(i) + ".txt",
                   base + "_" + std::to_string(i + 1) + ".txt", ec);
    }
    fs::rename(base + ".txt", base + "_1.txt", ec);

    spdlog::debug("[CrashReporter] Rotated crash file");
}

// =============================================================================
// Deduplication
// =============================================================================

std::string CrashReporter::fingerprint(const CrashReport& report) {
    std::string first_frame = report.backtrace.empty() ? "" : report.backtrace[0];
    return helix::crash_fingerprint(report.signal_name, report.app_version, first_frame);
}

bool CrashReporter::is_duplicate(const CrashReport& report) const {
    std::string fp = fingerprint(report);
    return helix::CrashHistory::instance().has_fingerprint(fp);
}

// =============================================================================
// Auto-Send
// =============================================================================

#ifdef __ANDROID__
/// HTTPS POST via Android's Java HttpURLConnection (JNI bridge).
/// libhv is built without SSL on Android, so we use the platform TLS stack.
/// Returns {status_code, response_body}; status 0 means network/JNI failure.
static std::pair<int, std::string> android_https_post(const std::string& url,
                                                      const std::string& body,
                                                      const std::string& user_agent,
                                                      const std::string& api_key, int timeout_sec) {
    // SDL_AndroidGetJNIEnv() returns a per-thread JNI env (handles AttachCurrentThread)
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) {
        spdlog::error("[CrashReporter] Failed to get JNI env");
        return {0, "JNI env unavailable"};
    }

    // Cached global ref owned by helix_activity_class() — never released here.
    // The one caller reaches this from the main thread (send_with_bundle is
    // marshaled back via token.defer), but the sole reason it works is that
    // FindClass() needs Java frames on the stack; moving the send onto the
    // bundle worker would silently turn every report into an HTTP 0.
    jclass cls = helix::android::helix_activity_class(env);
    if (!cls) {
        return {0, "HelixActivity class not found"};
    }

    jmethodID method = env->GetStaticMethodID(
        cls, "httpsPost",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)"
        "Ljava/lang/String;");
    if (!method) {
        spdlog::error("[CrashReporter] Failed to find httpsPost method");
        env->ExceptionClear();
        return {0, "httpsPost method not found"};
    }

    jstring j_url = env->NewStringUTF(url.c_str());
    jstring j_body = env->NewStringUTF(body.c_str());
    jstring j_ua = env->NewStringUTF(user_agent.c_str());
    jstring j_key = env->NewStringUTF(api_key.c_str());

    if (!j_url || !j_body || !j_ua || !j_key) {
        if (j_url)
            env->DeleteLocalRef(j_url);
        if (j_body)
            env->DeleteLocalRef(j_body);
        if (j_ua)
            env->DeleteLocalRef(j_ua);
        if (j_key)
            env->DeleteLocalRef(j_key);
        env->ExceptionClear();
        return {0, "JNI string allocation failed"};
    }

    auto j_result = static_cast<jstring>(env->CallStaticObjectMethod(
        cls, method, j_url, j_body, j_ua, j_key, static_cast<jint>(timeout_sec)));

    env->DeleteLocalRef(j_url);
    env->DeleteLocalRef(j_body);
    env->DeleteLocalRef(j_ua);
    env->DeleteLocalRef(j_key);

    if (!j_result || env->ExceptionCheck()) {
        env->ExceptionClear();
        return {0, "JNI call failed"};
    }

    const char* result_cstr = env->GetStringUTFChars(j_result, nullptr);
    std::string result(result_cstr);
    env->ReleaseStringUTFChars(j_result, result_cstr);
    env->DeleteLocalRef(j_result);

    // Parse "STATUS_CODE\nRESPONSE_BODY"
    auto newline = result.find('\n');
    if (newline == std::string::npos) {
        return {0, result};
    }
    int status = 0;
    try {
        status = std::stoi(result.substr(0, newline));
    } catch (...) {
    }
    return {status, result.substr(newline + 1)};
}
#endif // __ANDROID__

bool CrashReporter::try_auto_send(const CrashReport& report) {
    // Best-effort POST to crash worker — failure falls through to QR/file
    try {
        json payload = report_to_json(report);
        std::string body = payload.dump();
        std::string user_agent = std::string("HelixScreen/") + HELIX_VERSION;

        int status = 0;
        std::string resp_body;

#ifdef __ANDROID__
        // Android: use JNI bridge to Java's HttpURLConnection (libhv has no SSL)
        auto [s, b] = android_https_post(CRASH_WORKER_URL, body, user_agent, INGEST_API_KEY, 15);
        status = s;
        resp_body = std::move(b);
#else
        // Desktop/embedded: use libhv directly
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_POST;
        req->url = CRASH_WORKER_URL;
        req->timeout = 15;
        req->content_type = APPLICATION_JSON;
        req->headers["User-Agent"] = user_agent;
        req->headers["X-API-Key"] = INGEST_API_KEY;
        req->body = std::move(body);

        auto resp = requests::request(req);
        status = resp ? static_cast<int>(resp->status_code) : 0;
        resp_body = resp ? resp->body : "";
#endif

        if (status >= 200 && status < 300) {
            spdlog::info("[CrashReporter] Crash report sent to worker (HTTP {})", status);

            // Record in crash history for debug bundle cross-referencing
            helix::CrashHistoryEntry hist_entry;
            hist_entry.timestamp = report.timestamp;
            hist_entry.signal = report.signal;
            hist_entry.signal_name = report.signal_name;
            hist_entry.app_version = report.app_version;
            hist_entry.uptime_sec = report.uptime_sec;
            hist_entry.fault_addr = report.fault_addr;
            hist_entry.fault_code_name = report.fault_code_name;
            hist_entry.abort_msg = report.abort_msg;
            hist_entry.sent_via = "crash_reporter";
            hist_entry.fingerprint = fingerprint(report);

            // Parse optional GitHub metadata from crash worker response
            try {
                json resp_json = json::parse(resp_body);
                hist_entry.github_issue = resp_json.value("issue_number", 0);
                hist_entry.github_url = resp_json.value("issue_url", "");
            } catch (const std::exception&) {
                // Response not JSON -- record without GitHub info
            }

            helix::CrashHistory::instance().add_entry(hist_entry);
            spdlog::debug("[CrashReporter] Recorded crash in history (issue #{})",
                          hist_entry.github_issue);

            return true;
        }

        spdlog::warn("[CrashReporter] Worker returned HTTP {} (body: {})", status,
                     resp_body.substr(0, 200));
        return false;
    } catch (const std::exception& e) {
        spdlog::warn("[CrashReporter] Auto-send failed: {}", e.what());
        return false;
    }
}
