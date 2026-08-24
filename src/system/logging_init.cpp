// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "logging_init.h"

#include <spdlog/pattern_formatter.h>

#ifndef HELIX_WATCHDOG
#include "hv/hlog.h"
#endif
#include "lvgl_assert_handler.h"
#include "lvgl_log_handler.h"
#include "platform_capabilities.h"
#include "system/helix_paths.h"
#ifndef HELIX_WATCHDOG
#include "system/crash_error_log_sink.h"
#include "system/crash_handler.h"
#endif

#include "system/monotonic_ring_sink.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <lvgl.h>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Define the global callback pointer for LVGL assert handler
helix_assert_callback_t g_helix_assert_cpp_callback = nullptr;

#ifdef __linux__
#ifdef HELIX_HAS_SYSTEMD
#include <spdlog/sinks/systemd_sink.h>
#endif
#include <spdlog/sinks/syslog_sink.h>
#endif

#ifdef HELIX_PLATFORM_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif

namespace helix {
namespace logging {

// %* is our custom flag: the CLOCK_MONOTONIC offset since process start, plus a
// CLOCK_STEP annotation on any line the wall clock jumped across. Every sink
// carries it — a bundle may arrive from the ring buffer, the rotating file, or
// /var/log/messages on a syslog-target device, and a wall-clock-only stamp is
// untrustworthy in all three (#1218).
const char* pattern_for_sink(SinkKind kind) {
    switch (kind) {
    case SinkKind::Console:
        // ms timestamp, monotonic offset, colored level, thread id, message.
        return "[%H:%M:%S.%e] [%*] [%^%l%$] [%t] %v";
    case SinkKind::File:
        // As Console, plus the date: the file and the ring buffer are what a
        // debug bundle carries, and a session crossing midnight is otherwise
        // ambiguous. (%^…%$ are no-ops on the non-color file sink.)
        return "[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%l%$] [%t] %v";
    case SinkKind::Journald:
    case SinkKind::Syslog:
        // No wall-clock token — journald/syslog stamp their own time, and a
        // second one would double up. The monotonic offset is not a second
        // clock reading, and it is the only field in these streams that a clock
        // step cannot corrupt, so it stays.
        return "[%*] [%l] [%t] %v";
    case SinkKind::Android:
        // logcat already prefixes its own timestamp/level/tag metadata.
        return "[%*] [%t] %v";
    case SinkKind::CrashBreadcrumb:
        // Feeds crash context — keep a full line with thread id. The crash
        // ring reads msg.payload (the raw message), not this formatted output,
        // so the pattern is for any other consumer of this sink's stream.
        return "[%H:%M:%S.%e] [%*] [%l] [%t] %v";
    }
    return "[%*] [%t] %v";
}

namespace {

/// Raw CLOCK_MONOTONIC reading in seconds, or 0.0 if unavailable.
double raw_monotonic_seconds() {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
    }
#endif
    return 0.0;
}

/// The anchor monotonic_seconds() subtracts. A function-local static so it is
/// immune to static-init order; init_early() touches it at the top of main() so
/// the anchor really is process start and not first-log-line.
double process_start_monotonic() {
    static const double t = raw_monotonic_seconds();
    return t;
}

/// spdlog custom flag `%*`: the monotonic column, plus a CLOCK_STEP annotation
/// on the line across which the wall clock diverged from monotonic time.
///
/// State is per-instance and each sink holds its own formatter, so no sink
/// consumes another's annotation. Safe without additional locking: every sink
/// we install formats under its own mutex — the base_sink<std::mutex> family
/// (file, ring, syslog, journald, android, crash breadcrumb) and ansicolor_sink,
/// which takes the shared console mutex around formatter_->format().
class MonotonicFlagFormatter : public spdlog::custom_flag_formatter {
  public:
    void format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/,
                spdlog::memory_buf_t& dest) override {
        // A ring dump replays the offset captured when the line was logged;
        // every other sink formats on the logging thread, where reading the
        // clock here IS the log time. reset_sequence makes each dump
        // self-contained so its first line is not diffed against whatever this
        // formatter saw on a previous dump.
        auto& replay = detail::monotonic_replay();
        double mono = monotonic_seconds();
        if (replay.active) {
            mono = replay.value;
            if (replay.reset_sequence) {
                have_prev_ = false;
                replay.reset_sequence = false;
            }
        }
        const double wall = std::chrono::duration<double>(msg.time.time_since_epoch()).count();

        std::string out = format_monotonic(mono);
        if (have_prev_ && is_clock_step(wall - prev_wall_, mono - prev_mono_)) {
            // The delta between the two deltas IS the step size — the amount of
            // apparent elapsed time in this log that never actually elapsed.
            fmt::format_to(std::back_inserter(out), " CLOCK_STEP{:+.3f}s",
                           (wall - prev_wall_) - (mono - prev_mono_));
        }
        prev_wall_ = wall;
        prev_mono_ = mono;
        have_prev_ = true;

        dest.append(out.data(), out.data() + out.size());
    }

    std::unique_ptr<spdlog::custom_flag_formatter> clone() const override {
        return spdlog::details::make_unique<MonotonicFlagFormatter>();
    }

  private:
    double prev_wall_ = 0.0;
    double prev_mono_ = 0.0;
    bool have_prev_ = false;
};

// Snapshot of the resolved log destination after init(), used by
// effective_destination() to surface the active sink in the About panel.
LogTarget g_effective_target = LogTarget::Auto;
std::string g_effective_file_path;

// Process-global handle to the in-memory ring-buffer sink. Installed on every
// platform by init_early() (and by init() for the watchdog build, which has no
// early phase) and read by tail_ring_buffer() (debug-bundle log_tail). Rebuilt
// on each init() that does not follow an init_early() — a shared_ptr so a logger
// swap never frees it out from under a concurrent tail read. Null before init.
std::shared_ptr<MonotonicRingSink> g_ring_sink;

// True between init_early() and the init() that takes over its ring sink.
//
// init() ADOPTS that sink instead of building a new one, so everything logged
// during startup Phase 2 — Config load and its corrupt/restore diagnostics — is
// still in the buffer the debug bundle reads. Bundle XGVDYEB5 is what this
// exists for: a user-visible "settings were corrupted, restored from backup"
// toast, and not one line of the config trail anywhere in a 20,000-line
// log_tail, because the early logger had no ring and init() then replaced it.
//
// Cleared on adoption, so any LATER init() rebuilds the ring exactly as before.
// Production calls init() once (Application::init_logging, helix_watchdog), so
// nothing real re-enters; tests re-initialize the logger constantly and rely on
// each init() handing them a clean buffer.
bool g_early_ring_unadopted = false;

// Level of the console sink init_early() installs, and the level the early ring
// falls back to when HELIX_BUNDLE_LOG_DEBUG=0 declines debug capture. Also the
// production default log level, so an early line that clears this floor would
// have been kept by the full logger too.
constexpr spdlog::level::level_enum EARLY_CONSOLE_LEVEL = spdlog::level::warn;

// The user-configured level the persistent sinks run at (the logger floor may
// be lower so the ring captures debug). Recorded for the bundle's log_meta.
spdlog::level::level_enum g_effective_log_level = spdlog::level::warn;

// Floor ring capacity (messages), and the fallback when RAM detection fails.
// Capacity otherwise scales with the device — see ring_capacity_for_ram(). This
// is the historical fixed size, kept as the floor so the smallest boards (AD5M
// at ~107 MB) keep exactly what they had: ~2000 lines ≈ 300 KB at typical line
// lengths. Tunable in both directions via HELIX_LOG_RING_LINES.
constexpr size_t MIN_RING_LINES = 2000;

/// Upper bound: past a few thousand lines a bundle reader is scrolling, not
/// diagnosing, and the memory stops paying for itself.
constexpr size_t MAX_RING_LINES = 20000;

/// ~150 bytes per retained line (≈119 bytes of text plus std::string overhead,
/// measured against real AD5X bundles), so 16 lines/MB budgets ~0.24% of RAM.
constexpr size_t RING_LINES_PER_MB = 16;

size_t resolve_ring_capacity() {
    // Explicit override always wins — a constrained board can pin it down and a
    // developer chasing something can pin it up.
    if (const char* env = std::getenv("HELIX_LOG_RING_LINES")) {
        char* end = nullptr;
        unsigned long long v = std::strtoull(env, &end, 10);
        if (end != env && v > 0) {
            return static_cast<size_t>(v);
        }
    }
    return ring_capacity_for_ram(PlatformCapabilities::detect().total_ram_mb);
}

// Whether the ring buffer captures DEBUG (the diagnostic win) or matches the
// persistent sinks' level (lower formatting cost). Default ON: the formatting
// cost of debug-level emission into a memory ring is modest even on MIPS/AD5X,
// and the bundle diagnostic value — recovering the live debug context that the
// WARN-level file/syslog sinks never persisted — is the entire point of this
// sink. Set HELIX_BUNDLE_LOG_DEBUG=0 to fall back to the configured level.
bool ring_captures_debug() {
    if (const char* env = std::getenv("HELIX_BUNDLE_LOG_DEBUG")) {
        return !(env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N');
    }
    return true;
}

/// Check if a path is writable (for file logging location selection).
/// Probes the parent directory of `path` (the target file may not exist yet)
/// with a real access(W_OK) check rather than inspecting the owner-write
/// permission bit, which reports nothing about the current process's access.
bool is_path_writable(const std::string& path) {
    std::filesystem::path p(path);
    std::filesystem::path dir = p.parent_path();
    return helix::paths::is_writable_dir(dir.empty() ? "." : dir.string());
}

#ifndef HELIX_WATCHDOG
/// Non-owning shared_ptr to the process-lifetime crash error-log sink, so it
/// can join a logger's sink list without the logger ever freeing it (the sink
/// outlives every logger swap, keeping the crash-handler pointers valid).
spdlog::sink_ptr crash_error_log_sink() {
    auto& sink = CrashErrorLogSink::instance();
    sink.set_formatter(make_formatter(SinkKind::CrashBreadcrumb));
    return spdlog::sink_ptr(&sink, [](spdlog::sinks::sink*) {});
}
#endif

/// Get XDG_DATA_HOME or default ~/.local/share, with a /tmp last-resort fallback
std::string get_xdg_data_home() {
    const std::string base = helix::paths::xdg_data_home();
    if (!base.empty()) {
        return base;
    }
    return "/tmp"; // Last resort fallback (module returns "" when HOME is unusable)
}

/// Resolve log file path with fallback logic
std::string resolve_log_file_path(const std::string& override_path) {
    if (!override_path.empty()) {
        return override_path;
    }

    // Try /var/log first (requires permissions, typical for system services)
    const std::string var_log = "/var/log/helix-screen.log";
    if (is_path_writable(var_log)) {
        return var_log;
    }

    // Fallback to user directory
    std::string user_dir = get_xdg_data_home() + "/helix-screen";
    std::error_code ec;
    std::filesystem::create_directories(user_dir, ec);

    return user_dir + "/helix.log";
}

/// Detect best available logging target at runtime
LogTarget detect_best_target() {
#ifdef HELIX_PLATFORM_ANDROID
    // Android: use logcat sink (stdout is invisible in adb logcat)
    return LogTarget::Android;
#elif defined(__linux__)
#ifdef HELIX_HAS_SYSTEMD
    // Check for systemd journal socket
    std::error_code ec;
    if (std::filesystem::exists("/run/systemd/journal/socket", ec)) {
        return LogTarget::Journal;
    }
#endif
    // Syslog is always available on Linux
    return LogTarget::Syslog;
#else
    // macOS/other: console only by default
    return LogTarget::Console;
#endif
}

/// Add system sink based on target
void add_system_sink(std::vector<spdlog::sink_ptr>& sinks, LogTarget target,
                     const std::string& file_path) {
    switch (target) {
#ifdef __linux__
#ifdef HELIX_HAS_SYSTEMD
    case LogTarget::Journal: {
        auto sink = std::make_shared<spdlog::sinks::systemd_sink_mt>("helix-screen");
        sink->set_formatter(make_formatter(SinkKind::Journald));
        sinks.push_back(std::move(sink));
        break;
    }
#endif
    case LogTarget::Syslog: {
        auto sink = std::make_shared<spdlog::sinks::syslog_sink_mt>("helix-screen", LOG_PID,
                                                                    LOG_USER, false);
        sink->set_formatter(make_formatter(SinkKind::Syslog));
        sinks.push_back(std::move(sink));
        break;
    }
#endif
    case LogTarget::File: {
        std::string path = resolve_log_file_path(file_path);
        // Default: 5 MiB per file × 3 files (~15 MiB cap). Constrained-flash
        // platforms tune lower via HELIX_LOG_ROTATE_BYTES / _FILES env vars
        // (e.g., CC1 sets 1 MiB × 3 in hooks-cc1.sh).
        size_t max_bytes = 5 * 1024 * 1024;
        size_t max_files = 3;
        if (const char* env = std::getenv("HELIX_LOG_ROTATE_BYTES")) {
            char* end = nullptr;
            unsigned long long v = std::strtoull(env, &end, 10);
            if (end != env && v > 0) {
                max_bytes = static_cast<size_t>(v);
            }
        }
        if (const char* env = std::getenv("HELIX_LOG_ROTATE_FILES")) {
            char* end = nullptr;
            unsigned long long v = std::strtoull(env, &end, 10);
            if (end != env && v > 0) {
                max_files = static_cast<size_t>(v);
            }
        }
        try {
            auto sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, max_bytes, max_files);
            sink->set_formatter(make_formatter(SinkKind::File));
            sinks.push_back(std::move(sink));
        } catch (const spdlog::spdlog_ex& e) {
            // rotating_file_sink_mt THROWS when the path cannot be opened —
            // missing parent directory, read-only mount, full flash. That
            // exception used to propagate out of init() and out of
            // Application::run(), so a bad HELIX_LOG_FILE took the whole UI
            // down. Logging is never worth refusing to boot over: degrade to
            // whatever this platform would have picked on its own and say so.
            // Matters now that platform hooks steer the log at firmware-owned
            // directories that may not exist on every variant (#1249).
            LogTarget fallback = detect_best_target();
            spdlog::warn("[Logging] Cannot open log file '{}' ({}); falling back to {}", path,
                         e.what(), log_target_name(fallback));
            // Guard the recursion: detect_best_target() never returns File
            // today, but a future platform branch that did would loop forever.
            if (fallback != LogTarget::File) {
                add_system_sink(sinks, fallback, "");
            }
            // Keep effective_destination()/effective_log_file_path() honest —
            // the crash reporter and debug-bundle collector read the latter to
            // find the live log, and a path with no sink behind it would send
            // them at a file that does not exist.
            g_effective_target = fallback;
            g_effective_file_path.clear();
        }
        break;
    }
#ifdef HELIX_PLATFORM_ANDROID
    case LogTarget::Android: {
        auto sink = std::make_shared<spdlog::sinks::android_sink_mt>("HelixScreen");
        sink->set_formatter(make_formatter(SinkKind::Android));
        sinks.push_back(std::move(sink));
        break;
    }
#endif
    case LogTarget::Console:
    case LogTarget::Auto:
        // Console-only or auto (which would have been resolved already)
        // No additional sink needed
        break;
#ifdef __linux__
    default:
        // Handle Journal case when HELIX_HAS_SYSTEMD is not defined
        // Fall back to syslog
        if (target == LogTarget::Journal) {
            auto sink = std::make_shared<spdlog::sinks::syslog_sink_mt>("helix-screen", LOG_PID,
                                                                        LOG_USER, false);
            sink->set_formatter(make_formatter(SinkKind::Syslog));
            sinks.push_back(std::move(sink));
        }
        break;
#else
    default:
        break;
#endif
    }
}

/// C++ assert callback that logs via spdlog and dumps backtrace.
/// IMPORTANT: Do NOT call any LVGL functions here — this callback may fire
/// during rendering or layout, and re-entrant LVGL calls cause cascading
/// assertions and SIGSEGV (crash signature 0997d072).
void lvgl_assert_spdlog_callback(const char* file, int line, const char* func) {
#ifndef HELIX_WATCHDOG
    // LVGL asserts log-and-continue (never abort), but leave a durable crumb:
    // if this assert later contributes to a crash, the breadcrumb names where
    // it fired (issue #987). Runs on the LVGL thread — satisfies breadcrumb's
    // single-producer contract.
    crash_handler::breadcrumb::note("assert", func);
#endif
    // Log via spdlog for consistent logging across all outputs
    spdlog::critical("╔═══════════════════════════════════════════════════════════╗");
    spdlog::critical("║              LVGL ASSERTION FAILED                        ║");
    spdlog::critical("╠═══════════════════════════════════════════════════════════╣");
    spdlog::critical("║ File: {}", file);
    spdlog::critical("║ Line: {}", line);
    spdlog::critical("║ Func: {}()", func);
    spdlog::critical("╚═══════════════════════════════════════════════════════════╝");

    // Dump recent log messages that led up to this assertion
    spdlog::critical("=== Recent log messages (backtrace) ===");
    spdlog::dump_backtrace();
}

} // namespace

double monotonic_seconds() {
    const double now = raw_monotonic_seconds();
    const double start = process_start_monotonic();
    // Guard the no-CLOCK_MONOTONIC case: both reads are 0.0, so the offset is
    // 0.0 rather than a nonsense negative.
    return now > start ? now - start : 0.0;
}

std::string format_monotonic(double seconds) {
    // Width 10 = '+' + 5 integer digits + '.' + 3 fractional. Past 99999.999 s
    // (27 h) the integer field grows; alignment degrades gracefully rather than
    // the value being truncated.
    return fmt::format("+{:09.3f}", seconds);
}

namespace detail {

MonotonicReplay& monotonic_replay() {
    thread_local MonotonicReplay state;
    return state;
}

} // namespace detail

bool is_clock_step(double wall_delta_s, double mono_delta_s) {
    // adjtime() slews at most ~500 ppm, so over any interval the honest
    // wall-vs-monotonic disagreement stays under 0.05% of the interval. One
    // full second is orders of magnitude beyond that and cannot be slew.
    constexpr double STEP_THRESHOLD_SECONDS = 1.0;
    return std::fabs(wall_delta_s - mono_delta_s) > STEP_THRESHOLD_SECONDS;
}

std::unique_ptr<spdlog::formatter> make_formatter(SinkKind kind) {
    auto formatter = std::make_unique<spdlog::pattern_formatter>();
    // add_flag must precede set_pattern — the pattern is compiled against the
    // custom-flag table as it is parsed, so registering afterwards leaves the
    // already-compiled '%*' emitting itself literally.
    formatter->add_flag<MonotonicFlagFormatter>('*').set_pattern(pattern_for_sink(kind));
    return formatter;
}

void init_early() {
    // Anchor the monotonic column here rather than on the first log line, so
    // "+00000.000" means process start. init_early() is the first statement in
    // Application::run(); the watchdog anchors in its own init() call below.
    process_start_monotonic();

    // Create minimal console logger at WARN level so early startup code can log
    // without crashing. Attach the crash error-log sink too, so errors during
    // boot (before init()) are still captured for crash diagnostics (#987).
    std::vector<spdlog::sink_ptr> sinks;
    {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_formatter(make_formatter(SinkKind::Console));
        // Pinned explicitly, because the logger floor below now drops to debug
        // for the ring's sake and a sink's own default level is trace. Without
        // this the early console would start echoing debug to stdout — which a
        // daemonized launch redirects straight into the journal or log file.
        console->set_level(EARLY_CONSOLE_LEVEL);
        sinks.push_back(std::move(console));
    }

    // Ring-buffer sink, installed HERE rather than waiting for init(). Startup
    // Phase 2 (Config load, corrupt-settings restore, backup recovery) runs
    // entirely before init(), so without a ring at this point those lines exist
    // in no artifact a user can send us — see g_early_ring_unadopted.
    //
    // resolve_ring_capacity() is safe this early: it reads HELIX_LOG_RING_LINES
    // and /proc/meminfo, neither of which needs Config. Cost is one extra
    // PlatformCapabilities::detect() at startup, not extra memory — init()
    // adopts this sink instead of allocating a second one.
    //
    // Level split mirrors init(): the ring gets debug, the console stays at
    // EARLY_CONSOLE_LEVEL, and the logger floor is the more verbose of the two
    // because spdlog gates at the logger before any sink sees a message. When
    // HELIX_BUNDLE_LOG_DEBUG=0 declines debug capture there is no configured
    // level to fall back to yet, so the ring matches the console — which leaves
    // the early logger behaving exactly as it did before this sink existed.
    const spdlog::level::level_enum ring_level =
        ring_captures_debug() ? spdlog::level::debug : EARLY_CONSOLE_LEVEL;
    g_ring_sink = std::make_shared<MonotonicRingSink>(resolve_ring_capacity());
    g_ring_sink->set_formatter(make_formatter(SinkKind::File));
    g_ring_sink->set_level(ring_level);
    g_early_ring_unadopted = true;
    sinks.push_back(g_ring_sink);

#ifndef HELIX_WATCHDOG
    sinks.push_back(crash_error_log_sink());
#endif
    auto logger = std::make_shared<spdlog::logger>("helix", sinks.begin(), sinks.end());
    logger->set_level(std::min(EARLY_CONSOLE_LEVEL, ring_level));
    spdlog::set_default_logger(logger);
}

StdoutKind classify_stdout() {
    if (isatty(STDOUT_FILENO) != 0)
        return StdoutKind::Tty;

    struct stat st {};
    if (fstat(STDOUT_FILENO, &st) != 0)
        return StdoutKind::Other; // conservative: not forceable

    if (S_ISFIFO(st.st_mode))
        return StdoutKind::Pipe;
    if (S_ISREG(st.st_mode))
        return StdoutKind::File;
    if (S_ISSOCK(st.st_mode))
        return StdoutKind::Socket;
    return StdoutKind::Other;
}

bool should_add_console(LogTarget effective_target, bool enable_console, bool force_console,
                        bool test_mode, StdoutKind stdout_kind) {
    if (!enable_console)
        return false;

    switch (effective_target) {
    case LogTarget::Console:
        // Console is the only sink for this target — always attach.
        return true;
    case LogTarget::Android:
        // stdout is invisible under logcat; android_sink carries the output.
        // Deliberately checked BEFORE test_mode: Android + --test is not a real
        // configuration, and a console sink would be unreadable there anyway, so
        // test mode does not override this.
        return false;
    default:
        // Journal/Syslog/File already capture output structurally. Attach the
        // console for interactive shell runs so dev boxes and `ssh -t` still see
        // colored output, but not for daemonized launches where stdout is
        // redirected into that same journal/file — that double-logs every line
        // (root cause of the Snapmaker U1 tmpfs blowout: /tmp/helixscreen.log
        // grew to 498 MB at trace level).
        //
        // force_console (explicit -v/--log-level) additionally attaches the
        // console for a PIPE — a human watching via `| tee` — but never for a
        // regular file or socket, which are the daemon redirect and the systemd
        // journal. The shipped launcher synthesizes --log-level/-vv from
        // HELIX_LOG_LEVEL / HELIX_DEBUG, so force_console IS reachable under
        // systemd; forcing there would reintroduce the blowout. See the full
        // rationale on should_add_console() in logging_init.h.
        //
        // test_mode attaches the console for ANY stdout kind, closing the gap for
        // `--test -vv > file` (#1105's literal repro, which a reporter may well
        // have redirected rather than piped). Safe because --test never runs in
        // production: no systemd unit, init script, procd shim, or launcher
        // passes it, so test mode cannot reach a daemonized double-log path.
        if (test_mode)
            return true;
        if (stdout_kind == StdoutKind::Tty)
            return true;
        return force_console && stdout_kind == StdoutKind::Pipe;
    }
}

void init(const LogConfig& config) {
    // No-op when init_early() already ran; anchors the watchdog build, which
    // calls init() directly.
    process_start_monotonic();

    std::vector<spdlog::sink_ptr> sinks;

    // Resolve auto-detection first so we can decide about console
    LogTarget effective_target =
        (config.target == LogTarget::Auto) ? detect_best_target() : config.target;

    // Snapshot for effective_destination() — recorded before sink construction
    // so the About panel reflects the same target the sinks are built from.
    g_effective_target = effective_target;
    g_effective_file_path =
        (effective_target == LogTarget::File) ? resolve_log_file_path(config.file_path) : "";

    // Console sink — added when enabled AND the target benefits from it.
    // Decision lives in should_add_console() so it is unit-testable without a TTY;
    // classify_stdout() is the only part that touches the real fd.
    bool add_console =
        should_add_console(effective_target, config.enable_console, config.force_console,
                           config.test_mode, classify_stdout());
    if (add_console) {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_formatter(make_formatter(SinkKind::Console));
        sinks.push_back(std::move(console));
    }

    // The console/system sinks emit only at the user-configured level so
    // persistent logs (/var/log/messages, the journal, the console) keep their
    // normal volume — no spam, no tmpfs blowout. The logger floor is raised
    // below (to debug) so the ring buffer alone gets the extra detail.
    g_effective_log_level = config.level;
    for (auto& s : sinks) {
        s->set_level(config.level);
    }

    // Add system sink (also at the configured level — set immediately after).
    {
        size_t before = sinks.size();
        add_system_sink(sinks, effective_target, config.file_path);
        for (size_t i = before; i < sinks.size(); ++i) {
            sinks[i]->set_level(config.level);
        }
    }

    // In-memory ring-buffer sink — installed on ALL platforms, adopted from
    // init_early() when there is one to adopt (see below). This is the
    // authoritative source for the debug bundle's log_tail: always the live
    // process, always fresh, and (by default) always carrying DEBUG even when
    // the persistent sinks run at WARN. On syslog-target devices (AD5X/AD5M)
    // the file cascade otherwise falls back to a stale leftover file and only
    // WARN-filtered /var/log/messages lines reach the bundle — the runtime
    // debug context needed to diagnose an in-progress incident (e.g. a stuck
    // IFS filament purge) was being lost entirely.
    //
    // Perf tradeoff (MIPS/AD5X): debug-level emission costs a format pass per
    // line into the ring even when persistent sinks drop it. That cost is
    // bounded (memory ring, no I/O) and justified by the bundle's diagnostic
    // value; HELIX_BUNDLE_LOG_DEBUG=0 reverts the ring to the configured level.
    const spdlog::level::level_enum ring_level =
        ring_captures_debug() ? spdlog::level::debug : config.level;
    if (g_early_ring_unadopted) {
        // Carry init_early()'s buffer forward rather than replacing it, so the
        // Phase-2 config trail is still there when a bundle is collected. Same
        // sink instance, so set_runtime_level()'s identity check against
        // g_ring_sink and tail_ring_buffer()'s shared_ptr copy both still hold.
        // Its formatter is already the File one; leaving it in place also keeps
        // the clock-step detector's memory continuous across the handoff.
        g_early_ring_unadopted = false;
    } else {
        g_ring_sink = std::make_shared<MonotonicRingSink>(resolve_ring_capacity());
        g_ring_sink->set_formatter(make_formatter(SinkKind::File));
    }
    g_ring_sink->set_level(ring_level);
    sinks.push_back(g_ring_sink);

#ifndef HELIX_WATCHDOG
    // Always retain recent ERROR-level lines for crash diagnostics, regardless
    // of the output target (#987 last-ditch reason capture). Its own level is
    // left at the sink default (trace) so it never misses an error.
    sinks.push_back(crash_error_log_sink());
#endif

    // Create logger with all sinks. The logger level gates messages BEFORE any
    // sink sees them, so it must be the MORE VERBOSE of {configured level, ring
    // level} — otherwise debug lines are dropped at the logger and never reach
    // the ring. Per-sink levels (set above) then restore each persistent sink's
    // normal volume; only the ring buffer gains the extra detail.
    auto logger = std::make_shared<spdlog::logger>("helix", sinks.begin(), sinks.end());
    logger->set_level(std::min(config.level, ring_level));

    // Flush warnings and worse immediately instead of waiting for spdlog's
    // buffer to fill. The UI is routinely SIGKILLed rather than shut down
    // cleanly — an init script escalating `kill` to `kill -9`, or a firmware
    // helper stopping the GUI to free RAM — and anything still buffered at that
    // moment is lost. The lost lines are exactly the ones describing why the
    // process was in trouble, which is what makes such a failure undiagnosable
    // after the fact. Levels below warn stay buffered so ordinary logging on
    // flash-backed boards keeps its write batching.
    logger->flush_on(spdlog::level::warn);

    // Set as default logger
    spdlog::set_default_logger(logger);

    // Enable backtrace buffer to capture recent log messages before an assertion
    // These get dumped when spdlog::dump_backtrace() is called in the assert handler
    spdlog::enable_backtrace(32);

    // Register C++ callback for LVGL assert handler
    // This provides spdlog integration and LVGL state context
    g_helix_assert_cpp_callback = lvgl_assert_spdlog_callback;

    // NOTE: LVGL log handler is registered separately AFTER lv_init()
    // because lv_init() resets the global state and clears any callbacks.
    // See Application::init_display() which calls register_lvgl_log_handler().

    // Log what we configured (at debug level so it's not noisy)
    spdlog::debug("[Logging] Initialized: target={}, console={}, backtrace=32 messages",
                  log_target_name(effective_target), config.enable_console ? "yes" : "no");
}

size_t ring_capacity_for_ram(size_t total_ram_mb) {
    // total_ram_mb == 0 means detection failed (non-Linux, unreadable
    // /proc/meminfo); fall back to the historical size rather than guessing.
    if (total_ram_mb == 0) {
        return MIN_RING_LINES;
    }
    const size_t scaled = total_ram_mb * RING_LINES_PER_MB;
    return std::clamp(scaled, MIN_RING_LINES, MAX_RING_LINES);
}

LogTarget parse_log_target(const std::string& str) {
    if (str == "journal")
        return LogTarget::Journal;
    if (str == "syslog")
        return LogTarget::Syslog;
    if (str == "file")
        return LogTarget::File;
    if (str == "console")
        return LogTarget::Console;
    if (str == "android")
        return LogTarget::Android;
    return LogTarget::Auto; // Default for "auto" or unrecognized
}

std::string effective_destination() {
    switch (g_effective_target) {
    case LogTarget::Journal:
        return "systemd journal";
    case LogTarget::Syslog:
        return "syslog";
    case LogTarget::File:
        return g_effective_file_path;
    case LogTarget::Console:
        return "console";
    case LogTarget::Android:
        return "Android logcat";
    case LogTarget::Auto:
        return "";
    }
    return "";
}

std::string effective_log_file_path() {
    return (g_effective_target == LogTarget::File) ? g_effective_file_path : std::string{};
}

std::string tail_ring_buffer(int num_lines) {
    auto sink = g_ring_sink; // copy the shared_ptr so a concurrent init() swap is safe
    if (!sink) {
        return {};
    }
    size_t lim = num_lines > 0 ? static_cast<size_t>(num_lines) : 0;
    auto lines = sink->last_formatted(lim); // oldest-first, already formatted
    if (lines.empty()) {
        return {};
    }
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out << '\n';
        }
        // last_formatted() appends the pattern's trailing newline; strip it so
        // join produces one clean newline between entries (not a blank line).
        std::string& line = lines[i];
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        out << line;
    }
    return out.str();
}

size_t ring_buffer_capacity() {
    return g_ring_sink ? resolve_ring_capacity() : 0;
}

spdlog::level::level_enum effective_log_level() {
    return g_effective_log_level;
}

const char* log_target_name(LogTarget target) {
    switch (target) {
    case LogTarget::Auto:
        return "auto";
    case LogTarget::Journal:
        return "journal";
    case LogTarget::Syslog:
        return "syslog";
    case LogTarget::File:
        return "file";
    case LogTarget::Console:
        return "console";
    case LogTarget::Android:
        return "android";
    }
    return "unknown";
}

bool is_valid_log_target(const std::string& str) {
    return str == "auto" || str == "journal" || str == "syslog" || str == "file" ||
           str == "console";
}

const char* log_target_accepted_values() {
    return "auto, journal, syslog, file, console";
}

bool is_valid_log_level(const std::string& str) {
    return str == "trace" || str == "debug" || str == "info" || str == "warn" || str == "error" ||
           str == "critical" || str == "off";
}

const char* log_level_accepted_values() {
    return "trace, debug, info, warn, error, critical, off";
}

std::string log_env_override(const char* var_name, bool (*validate)(const std::string&),
                             const char* accepted_values) {
    const char* raw = std::getenv(var_name);
    if (raw == nullptr || raw[0] == '\0') {
        return {};
    }
    std::string value(raw);
    if (validate != nullptr && !validate(value)) {
        // init_early() has already installed a console logger at warn, so this
        // is visible even though the real sinks are not built yet.
        spdlog::warn("[Logging] Ignoring invalid {}='{}' (accepted: {})", var_name, value,
                     accepted_values != nullptr ? accepted_values : "");
        return {};
    }
    return value;
}

std::string resolve_log_setting(const std::string& cli_value, const std::string& env_value,
                                const std::string& config_value) {
    if (!cli_value.empty()) {
        return cli_value;
    }
    if (!env_value.empty()) {
        return env_value;
    }
    return config_value;
}

spdlog::level::level_enum parse_level(const std::string& str,
                                      spdlog::level::level_enum default_level) {
    if (str.empty()) {
        return default_level;
    }
    if (str == "trace") {
        return spdlog::level::trace;
    }
    if (str == "debug") {
        return spdlog::level::debug;
    }
    if (str == "info") {
        return spdlog::level::info;
    }
    if (str == "warn" || str == "warning") {
        return spdlog::level::warn;
    }
    if (str == "error") {
        return spdlog::level::err;
    }
    if (str == "critical") {
        return spdlog::level::critical;
    }
    if (str == "off") {
        return spdlog::level::off;
    }
    return default_level;
}

spdlog::level::level_enum verbosity_to_level(int verbosity) {
    if (verbosity <= 0) {
        return spdlog::level::warn;
    }
    switch (verbosity) {
    case 1:
        return spdlog::level::info;
    case 2:
        return spdlog::level::debug;
    default:
        return spdlog::level::trace;
    }
}

int to_hv_level(spdlog::level::level_enum level) {
    // libhv levels: VERBOSE(0) < DEBUG(1) < INFO(2) < WARN(3) < ERROR(4) < FATAL(5) < SILENT(6)
    switch (level) {
    case spdlog::level::trace:
    case spdlog::level::debug:
        return 1; // LOG_LEVEL_DEBUG (libhv has no trace, cap at debug)
    case spdlog::level::info:
        return 2; // LOG_LEVEL_INFO
    case spdlog::level::warn:
        return 3; // LOG_LEVEL_WARN
    case spdlog::level::err:
        return 4; // LOG_LEVEL_ERROR
    case spdlog::level::critical:
        return 5; // LOG_LEVEL_FATAL
    case spdlog::level::off:
        return 6; // LOG_LEVEL_SILENT
    default:
        return 3; // LOG_LEVEL_WARN
    }
}

#ifndef HELIX_WATCHDOG
void set_runtime_level(spdlog::level::level_enum level) {
    // Mirror init()'s split: the logger floor must stay at least as verbose as
    // the ring buffer so debug keeps reaching it, while each persistent sink
    // (and libhv) moves to the user-requested level. A plain spdlog::set_level()
    // would set the logger floor to `level` and starve the ring of debug when
    // the user picks WARN — defeating the always-on debug-capture fix.
    const spdlog::level::level_enum ring_level =
        ring_captures_debug() ? spdlog::level::debug : level;
    g_effective_log_level = level;

    // The crash error-log sink must keep capturing ERROR regardless of the
    // user's level choice (#987), so it is excluded from the per-sink retune.
    spdlog::sink_ptr crash_sink;
#ifndef HELIX_WATCHDOG
    crash_sink = crash_error_log_sink();
#endif

    if (auto logger = spdlog::default_logger()) {
        for (auto& s : logger->sinks()) {
            if (s == g_ring_sink) {
                // Leave the ring at its debug floor so bundles keep debug detail.
                s->set_level(ring_level);
            } else if (crash_sink && s.get() == crash_sink.get()) {
                // Crash breadcrumb sink: never raise above error (#987).
                s->set_level(std::min(s->level(), spdlog::level::err));
            } else {
                s->set_level(level);
            }
        }
        logger->set_level(std::min(level, ring_level));
    } else {
        spdlog::set_level(std::min(level, ring_level));
    }

    hlog_set_level(to_hv_level(level));
    spdlog::info("[Logging] Runtime log level changed to {}",
                 spdlog::level::to_string_view(level).data());
}
#endif

spdlog::level::level_enum resolve_log_level(int cli_verbosity, const std::string& config_level_str,
                                            bool test_mode) {
    // Precedence: CLI verbosity > config file > defaults

    // CLI verbosity takes top precedence
    if (cli_verbosity > 0) {
        return verbosity_to_level(cli_verbosity);
    }

    // Config file level (if specified)
    if (!config_level_str.empty()) {
        // Use warn as fallback for invalid config strings
        return parse_level(config_level_str, spdlog::level::warn);
    }

    // Defaults: test mode = debug, production = warn
    return test_mode ? spdlog::level::debug : spdlog::level::warn;
}

} // namespace logging
} // namespace helix
