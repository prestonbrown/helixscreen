// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file crash_reporter.h
 * @brief Standalone crash reporter — sends crash data to developer on next launch
 *
 * When HelixScreen crashes, crash_handler.cpp writes config/crash.txt with signal,
 * version, uptime, and backtrace. On next startup, CrashReporter detects this file,
 * collects additional context (platform, logs, hardware info), and offers the user
 * a dialog to send the report.
 *
 * Delivery priority:
 * 1. Auto-send via CF Worker at crash.helixscreen.org → GitHub issue
 * 2. QR code with pre-filled GitHub issue URL (if no network)
 * 3. File fallback to ~/helixscreen/crash_report.txt (always)
 *
 * Independent of TelemetryManager — works without telemetry opt-in.
 */

#include <string>
#include <vector>

#include "hv/json.hpp"

/**
 * @brief Is a reported heap-snapshot age arithmetically possible?
 *
 * The snapshot is taken by the running process, so it can never be older than
 * the process itself. Bundle ED2YC336 reported an age of 120036989ms (33.3h)
 * against an uptime of 35390s (9.8h), which is not "stale" — it is impossible,
 * and every heap figure in that report was therefore meaningless.
 *
 * Two things produce it, and the raw stamps emitted alongside the age tell them
 * apart: the low 32 bits of CLOCK_MONOTONIC folding (needs ~49.7 days of
 * uptime), or the main loop's 10s refresh having genuinely stalled.
 *
 * Pure; unit-tested. `uptime_sec <= 0` means we have nothing to check against,
 * which counts as plausible rather than as a failure.
 *
 * @param age_ms     Reported heap_snapshot_age_ms
 * @param uptime_sec Reported process uptime in seconds
 */
bool heap_snapshot_age_is_plausible(long age_ms, int uptime_sec);

class CrashReporter {
  public:
    CrashReporter() = default;
    ~CrashReporter() = default;

    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;

    /**
     * @brief Initialize crash reporter with config directory
     * @param config_dir Directory containing crash.txt (e.g., "config" or temp dir for tests)
     */
    void init(const std::string& config_dir);

    /**
     * @brief Reset state for clean re-initialization (used in tests)
     */
    void shutdown();

    /**
     * @brief Check if crash.txt exists from a previous crash
     */
    bool has_crash_report() const;

    /**
     * @brief Structured crash report with all collected context
     */
    struct CrashReport {
        // From crash.txt
        int signal = 0;
        std::string signal_name;
        std::string app_version;
        std::string timestamp;
        int uptime_sec = 0;
        std::vector<std::string> backtrace;

        // Exception message (for signal 0 / EXCEPTION crashes)
        std::string exception_what;

        // glibc abort reason captured via __abort_msg on SIGABRT — e.g.
        // "free(): invalid pointer", "double free or corruption", an assertion
        // string, etc. Empty for non-SIGABRT signals and on non-glibc libcs
        // where __abort_msg doesn't exist.
        std::string abort_msg;

        // Why abort_msg may be blank on SIGABRT (issue #987): "present" (a real
        // glibc reason is in abort_msg), "empty" (glibc stored no message — a
        // bare abort()/raise or std::terminate fall-through, NOT malloc/assert/
        // fortify), or "unresolved" (__abort_msg symbol not found). Empty for
        // non-SIGABRT.
        std::string abort_msg_state;

        // std::terminate reason captured when a re-entrant terminate fell
        // through to a bare abort() (which leaves __abort_msg empty). This is
        // the reason behind an otherwise-blank SIGABRT (issue #987).
        std::string terminate_msg;

        // Most recent ERROR-level log lines before the crash (newest first),
        // captured by CrashErrorLogSink. Last-ditch context when neither
        // abort_msg nor terminate_msg is available (issue #987).
        std::vector<std::string> recent_errors;

        // Fault info (Phase 2 - from siginfo_t)
        std::string fault_addr;
        int fault_code = 0;
        std::string fault_code_name;

        // Register state (Phase 2 - from ucontext_t)
        std::string reg_pc;
        std::string reg_sp;
        std::string reg_lr; // ARM only
        std::string reg_bp; // x86_64 only

        // ASLR load base (for symbol resolution)
        std::string load_base;

        // UpdateQueue callback tag (identifies which queued callback was executing)
        std::string queue_callback;

        // LVGL event under dispatch at crash time (set by event_send_core hook).
        // event_target is a raw pointer hex string; event_code is an lv_event_code_t.
        // event_original_target differs from event_target only for bubbled events
        // (the originator's pointer); populated only when it differs from target.
        std::string event_target;
        std::string event_original_target;
        int event_code = 0;

        // Cached heap snapshot (refreshed every ~10s from main loop)
        struct HeapSnapshot {
            long age_ms = 0; // ms between snapshot capture and the crash
            // Raw stamps the age was derived from — both low 32 bits of
            // CLOCK_MONOTONIC. Present only on builds that emit them; 0 means
            // the crash predates the fields, not that the clock read zero.
            long snapshot_ts_ms = 0; // when refresh_heap_snapshot() ran
            long mono_ms_now = 0;    // clock reading taken in the signal handler
            long rss_kb = 0;
            long vsz_kb = 0;
            long arena_kb = 0;    // glibc mallinfo total arena
            long used_kb = 0;     // glibc uordblks
            long free_kb = 0;     // glibc fordblks
            long mmap_kb = 0;     // glibc hblkhd
            long lv_total_kb = 0; // LVGL internal heap total
            int lv_used_pct = 0;
            int lv_frag_pct = 0;
            long lv_free_biggest_kb = 0;
            bool present = false; // true if any heap_* was parsed
        };
        HeapSnapshot heap;

        // Breadcrumbs from the in-process ring buffer.
        // Each entry: "<monotonic_ms> <category> <subject>" (space-separated)
        std::vector<std::string> breadcrumbs;

        // Memory map (/proc/self/maps lines, for mapping addresses to libraries)
        std::vector<std::string> memory_map;

        // Stack-scanned backtrace metadata
        std::string bt_source;  // "stack_scan" if backtrace includes scanned entries
        std::string text_start; // Text segment start address
        std::string text_end;   // Text segment end address

        // Stack dump (ARM32/MIPS: 128 raw stack words for return-address scanning)
        std::string stack_base;
        std::vector<std::string> stack_dump;

        // Extra registers (ARM32: r0-r12, fp, ip; MIPS: ra, etc.)
        // Key = register name (e.g., "r0", "fp"), value = hex string
        std::vector<std::pair<std::string, std::string>> extra_registers;

        // Additional context (collected at startup, from the one
        // helix::diagnostics::collect() snapshot; the signal handler never
        // runs any of this — it only writes crash.txt)
        std::string platform;
        std::string printer_model;
        std::string klipper_version;
        std::string log_tail;
        std::string display_info;
        int ram_total_mb = 0;
        int cpu_cores = 0;
        // Which firmware population this is: one platform key ("mips") serves
        // the K1 series and both AD5X firmwares, which only mod_flavor splits.
        std::string mod_flavor;
        // Resolved layout: where the config this process read and the cache it
        // would have used actually live, and which cascade rung won the cache.
        std::string config_dir;
        std::string cache_dir;
        std::string cache_tier; ///< "" for a fall-through rung (XDG, $HOME, /var/tmp, /tmp)

        // Share code of a debug bundle uploaded alongside this report. Empty
        // when no bundle was attached (bundle upload failed, disabled, or the
        // report is being sent via the QR-code fallback path). The worker
        // renders this as a "Debug Bundle: CODE" row in the issue body so the
        // deeper context (sanitized settings, syslog, crash history) is one
        // click away.
        std::string debug_bundle_share_code;
    };

    /**
     * @brief Collect crash data from crash.txt + system context
     * @return Populated CrashReport struct
     */
    CrashReport collect_report();

    /**
     * @brief Attempt to send crash report to CF Worker
     * @return true if report was sent successfully
     */
    bool try_auto_send(const CrashReport& report);

    /**
     * @brief Check if this crash has already been reported (client-side dedup)
     *
     * Computes a fingerprint matching the server-side formula and checks
     * CrashHistory for a previous submission with the same fingerprint.
     */
    bool is_duplicate(const CrashReport& report) const;

    /**
     * @brief Compute the fingerprint for a crash report
     *
     * Format: signal_name/app_version/backtrace[0]
     * Matches the server-side crashFingerprint() in crash-worker.
     */
    static std::string fingerprint(const CrashReport& report);

    /**
     * @brief Generate a pre-filled GitHub issue URL (for QR code)
     *
     * URL is truncated to stay under ~2000 chars for QR code compatibility.
     */
    std::string generate_github_url(const CrashReport& report);

    /**
     * @brief Save human-readable crash report to file
     * @return true if file was written successfully
     */
    bool save_to_file(const CrashReport& report);

    /**
     * @brief Delete crash.txt after handling (prevents re-processing)
     */
    void consume_crash_file();

    /**
     * @brief Convert crash report to JSON (for CF Worker POST)
     */
    nlohmann::json report_to_json(const CrashReport& report);

    /**
     * @brief Convert crash report to human-readable text
     */
    std::string report_to_text(const CrashReport& report);

    /**
     * @brief Read the last N lines from the log file
     * @param num_lines Number of lines to read from end of file
     * @return Last N lines as a string, empty if log not found
     *
     * @note The caller typically requests a generous buffer (e.g., 500) so the
     *       report can filter out post-crash lines written by the reporting
     *       session and still retain pre-crash context (see crash report #827).
     */
    std::string get_log_tail(int num_lines = 500);

    /// Worker endpoint for auto-send
    static constexpr const char* CRASH_WORKER_URL = "https://crash.helixscreen.org/v1/report";

    /// GitHub repo for issue URL generation
    static constexpr const char* GITHUB_REPO = "prestonbrown/helixscreen";

  private:
    std::string config_dir_;
    bool initialized_ = false;
    bool isolated_log_paths_ = false; ///< Test-only: skip system log search

    std::string crash_file_path() const;
    std::string report_file_path() const;

    friend class CrashReporterTestAccess;
};
