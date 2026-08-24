// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace helix {
namespace logging {

/**
 * @brief Identifies which spdlog sink a pattern is being chosen for
 *
 * Each sink gets its own format: console/file carry an ms-precision timestamp
 * because nothing else stamps them, while journald/syslog/android rely on the
 * system clock and would double-stamp if we added our own time token. Every
 * sink includes the thread id (%t) — the single highest-value field for
 * diagnosing the main-thread-vs-background-thread confusion behind the
 * async-delete crash family.
 */
enum class SinkKind {
    Console,        ///< stdout color sink — ms timestamp + colored level + thread id
    File,           ///< rotating file sink — ms timestamp + level + thread id
    Journald,       ///< systemd journal — level + thread id (journal stamps time)
    Syslog,         ///< syslog — level + thread id (syslog stamps time)
    Android,        ///< Android logcat — thread id only (logcat adds metadata)
    CrashBreadcrumb ///< crash error-log ring — ms timestamp + level + thread id
};

/**
 * @brief Return the spdlog pattern string for a given sink kind
 *
 * Pure function (no spdlog/sink dependency) so the per-sink format decision is
 * unit-testable without constructing real sinks. Called once per sink right
 * after construction in init()/init_early(), via sink->set_pattern().
 *
 * Invariants enforced by tests/unit/test_log_pattern.cpp:
 *   - every pattern contains %t (thread id)
 *   - every pattern contains %* (the monotonic column — see make_formatter())
 *   - Console and File contain a time token; the system sinks do not
 *   - File carries a full date, so a session crossing midnight is unambiguous
 *   - Console keeps the colored-level tokens %^ / %$
 *
 * Patterns using %* must be installed with make_formatter(), NOT set_pattern():
 * a plain set_pattern() has no handler for the custom flag and emits it
 * literally.
 *
 * @param kind Which sink the pattern is for
 * @return A static pattern string (valid for process lifetime)
 */
const char* pattern_for_sink(SinkKind kind);

/**
 * @brief Seconds elapsed since process start, from CLOCK_MONOTONIC
 *
 * Immune to clock steps and timezone changes, which the wall-clock timestamp is
 * not. Anchored on first call; init_early() forces that to happen at the top of
 * main() so the offset really is since-start. Returns 0.0 where CLOCK_MONOTONIC
 * is unavailable.
 */
double monotonic_seconds();

/**
 * @brief Render a monotonic offset as the fixed-width log column
 *
 * `+00057.398` — zero-padded to 5 integer digits (27 hours) so the column stays
 * aligned and a real gap is visible by eye, growing rather than truncating past
 * that. Pure; unit-tested.
 */
std::string format_monotonic(double seconds);

/**
 * @brief Did the wall clock step between two log lines?
 *
 * Compares how far CLOCK_REALTIME moved against how far CLOCK_MONOTONIC moved
 * over the same interval. NTP slew is bounded at ~500 ppm by adjtime(), so a
 * divergence of a full second cannot be slew — it is a step (or a suspend).
 * Catches backward steps too, which are worse: they interleave lines out of
 * chronological order with nothing to show for it.
 *
 * Pure; unit-tested. See issue #1218 — bundle XRK8KPTF showed a fabricated
 * 14m37s "reconnect stall" that was a +874 s clock step on an RTC-less printer.
 */
bool is_clock_step(double wall_delta_s, double mono_delta_s);

namespace detail {

/**
 * @brief Thread-local replay state for formatting stored log entries
 *
 * `%*` normally reads the clock as it formats, which is right for a sink that
 * formats on the logging thread. A ring buffer does not: it stores raw messages
 * and formats them later, so every line in a dump would receive the dump
 * instant. MonotonicRingSink records the real offset per entry and replays it
 * here while formatting.
 *
 * Thread-local, so a live sink formatting concurrently on another thread is
 * unaffected and keeps reading the clock. `reset_sequence` clears the flag
 * formatter's step-detection memory at the start of a dump, so a dump is
 * self-contained: the first line is never diffed against whatever that
 * formatter last saw (debug_bundle_collector probes the ring with a 1-line
 * tail before log_collector takes the real dump, which would otherwise open
 * every bundle with a fabricated CLOCK_STEP).
 */
struct MonotonicReplay {
    bool active = false;
    double value = 0.0;
    bool reset_sequence = false;
};

/// Accessor for the calling thread's replay state.
MonotonicReplay& monotonic_replay();

} // namespace detail

/**
 * @brief Build the formatter for a sink kind
 *
 * pattern_for_sink() plus the `%*` custom flag that renders the monotonic
 * column and annotates any line across which the wall clock stepped
 * (`CLOCK_STEP+874.000s`). Each call returns an independent formatter with its
 * own step-detection state, which is what sinks need: they format the same
 * message independently, each under its own mutex.
 *
 * Use this everywhere a log sink is configured. sink->set_pattern() is not
 * equivalent — it cannot resolve the custom flag.
 */
std::unique_ptr<spdlog::formatter> make_formatter(SinkKind kind);

/**
 * @brief Log destination targets
 *
 * `Auto` is resolved by detect_best_target():
 * - Android build → Android (logcat)
 * - Linux with HELIX_HAS_SYSTEMD and /run/systemd/journal/socket → Journal
 * - Any other Linux → Syslog
 * - macOS/other → Console
 *
 * Auto NEVER resolves to File, on any platform. The file sink is only reached
 * by asking for it — `--log-dest=file`, `HELIX_LOG_DEST=file`, or `/log_dest`
 * in settings.json — which is what every embedded platform hook does, since
 * "syslog" on a BusyBox box is an in-memory ring that dies with the reboot you
 * are trying to diagnose.
 */
enum class LogTarget {
    Auto,    ///< Detect best available (default)
    Journal, ///< systemd journal (Linux only)
    Syslog,  ///< Traditional syslog (Linux only)
    File,    ///< Rotating file log
    Console, ///< Console only (disable system logging)
    Android  ///< Android logcat via __android_log_print
};

/**
 * @brief Logging configuration
 */
struct LogConfig {
    spdlog::level::level_enum level = spdlog::level::warn;
    bool enable_console = true;         ///< Master switch: false vetoes the console sink outright.
                                        ///< true only makes it eligible — should_add_console()
                                        ///< decides, and for a Journal/Syslog/File target that
                                        ///< still needs a TTY, force_console + a pipe, or
                                        ///< test_mode.
    LogTarget target = LogTarget::Auto; ///< System log destination
    std::string file_path;              ///< Override file path (empty = auto)
    bool force_console = false;         ///< Widen the console gate to a PIPE as well as a TTY
                                        ///< (set when the user explicitly passed -v/--log-level,
                                        ///< or HELIX_LOG_LEVEL). Deliberately does NOT cover a
                                        ///< regular file or socket — those are the daemon
                                        ///< redirect, where a console sink double-logs into the
                                        ///< very file the system sink is writing.
    bool test_mode = false;             ///< Running under --test: always attach the console sink.
                                        ///< Sourced from RuntimeConfig by the CALLER, not read
                                        ///< here: logging_init.o is linked into the watchdog
                                        ///< build, which deliberately does not link
                                        ///< runtime_config.o (mk/watchdog.mk
                                        ///< WATCHDOG_EXTRA_OBJS). Reading it here would be an
                                        ///< undefined symbol at watchdog link time.
};

/**
 * @brief What kind of file descriptor stdout currently is
 *
 * Distinguishes "a human is watching" from "this is a daemon redirect", which
 * the console gate cannot tell from isatty() alone. See should_add_console().
 */
enum class StdoutKind {
    Tty,    ///< Interactive terminal — a human is definitely watching
    Pipe,   ///< FIFO/pipe — a human is watching through `| tee` / `| grep`
    File,   ///< Regular file — daemon redirect (`>> launcher.log`)
    Socket, ///< Socket — systemd's StandardOutput=journal
    Other   ///< Unknown, or fstat() failed — treated conservatively as not forceable
};

/**
 * @brief Classify the process's real stdout file descriptor
 *
 * isatty() first, then fstat() to tell a pipe from a file/socket. A failed
 * fstat() yields StdoutKind::Other, which should_add_console() treats as
 * non-forceable — i.e. it falls back to the plain isatty-only behavior.
 */
StdoutKind classify_stdout();

/**
 * @brief Decide whether the console sink should be attached
 *
 * Pure function (no spdlog/isatty/fstat dependency) so the gate is unit-testable
 * without a real TTY. Called by init() with the already-resolved target and the
 * stdout kind from classify_stdout().
 *
 * Rules:
 *   - Console target: console is the ONLY sink, always add.
 *   - Android target: stdout is invisible (logcat handles output), never add.
 *   - Journal/Syslog/File: a structured destination already captures output.
 *     Add the console when stdout is a TTY (interactive run from a shell), so
 *     dev workstations and `ssh -t` sessions still see colored output.
 *
 *     `force_console` (an explicit -v/--log-level) additionally attaches the
 *     console when stdout is a PIPE, but deliberately NOT when it is a regular
 *     file or a socket:
 *
 *       - A pipe means a human is watching through `tee`/`grep` — the documented
 *         way to capture a session — so the output must not be discarded (#1105).
 *       - A regular file is the daemon redirect (`>> $LOGFILE 2>&1` in the
 *         U1/K1/K2/CC1/AD5M init scripts) and a socket is systemd's
 *         StandardOutput=journal. In both cases stdout already lands in the same
 *         place the structured sink writes, so a console sink double-logs every
 *         line. That is not hypothetical: it caused the Snapmaker U1 tmpfs
 *         blowout where /tmp/helixscreen.log grew to 498 MB at trace level, and
 *         the shipped launcher DOES synthesize --log-level/-vv from the
 *         HELIX_LOG_LEVEL / HELIX_DEBUG env vars (scripts/helix-launcher.sh),
 *         which serve-local-update.sh writes onto live devices — so force_console
 *         is reachable under systemd and must stay off for file/socket.
 *
 *   - test_mode (--test) attaches the console for ANY stdout kind, including a
 *     regular file or socket. This is safe precisely because --test never runs
 *     in production: no systemd unit, init script, procd shim, or launcher
 *     passes it, so test mode cannot reach a daemonized double-log path. It
 *     closes the gap left by the pipe-only force — issue #1105's literal repro
 *     is `helix-screen --test -vv`, and a reporter redirecting with `> file`
 *     rather than `| tee` would otherwise still see nothing.
 *
 *     Deliberate precedence: test_mode does NOT override the Android rule.
 *     Android + test mode is not a real configuration, and stdout is invisible
 *     under logcat either way, so Android keeps returning false.
 *
 *     Accepted tradeoff (production only): `helix-screen -vv > out.log` stays
 *     silent, because a plain redirect is indistinguishable from the daemon
 *     redirect. Unchanged from previous behavior, so not a regression; use
 *     `| tee out.log` instead.
 *   - enable_console is the master switch and vetoes every case.
 *
 * @param effective_target Target after Auto detection has been resolved
 * @param enable_console   Master switch from LogConfig::enable_console
 * @param force_console    User explicitly requested console output (-v/--log-level)
 * @param test_mode        Running under --test (see LogConfig::test_mode)
 * @param stdout_kind      What stdout is (see classify_stdout())
 * @return true if a stdout console sink should be attached
 */
bool should_add_console(LogTarget effective_target, bool enable_console, bool force_console,
                        bool test_mode, StdoutKind stdout_kind);

/**
 * @brief Initialize minimal logging for early startup
 *
 * Sets up a console logger at WARN level plus the debug-bundle ring sink. Call
 * this FIRST in main() before any log calls. The full init() can reconfigure
 * later with user preferences from CLI args and config files.
 *
 * The ring is installed here, not in init(), because config load runs between
 * the two: its diagnostics (corrupt settings, restore-from-backup, parse
 * failures) are logged while only this logger exists, and a bundle collected
 * later has to be able to show them. init() takes over this same sink instance
 * rather than building a new one, so nothing logged here is discarded.
 */
void init_early();

/**
 * @brief Initialize logging subsystem
 *
 * Call once at startup before any log calls. Creates a multi-sink logger
 * that writes to both console (if enabled) and the selected system target.
 *
 * @param config Logging configuration
 */
void init(const LogConfig& config);

/**
 * @brief Ring-buffer capacity for a device with `total_ram_mb` of RAM
 *
 * The debug ring is the only place a bundle can recover live DEBUG context on a
 * device whose persistent sinks run at WARN, so its useful size is "how far back
 * can we see" — and that should scale with the machine rather than being one
 * number chosen for the smallest board. Roughly 0.24% of RAM (~16 lines/MB at
 * ~150 bytes a line), floored at the historical 2000 so no device regresses and
 * capped so a desktop does not hoard megabytes it will never read.
 *
 * Deliberately keyed on TOTAL ram, not available: MemAvailable at logging-init
 * time depends on boot ordering, which would give the same printer a different
 * ring every boot and shrink it hardest under memory pressure — precisely when
 * the diagnostics matter most.
 *
 * @param total_ram_mb Total system RAM in MB (0 = detection failed, use floor)
 * @return Ring capacity in lines
 */
size_t ring_capacity_for_ram(size_t total_ram_mb);

/**
 * @brief Parse log target from string
 *
 * @param str One of: "auto", "journal", "syslog", "file", "console"
 * @return Corresponding LogTarget enum value (Auto if unrecognized)
 */
LogTarget parse_log_target(const std::string& str);

/**
 * @brief Get string name for log target
 *
 * @param target LogTarget enum value
 * @return Human-readable name (e.g., "journal", "syslog")
 */
const char* log_target_name(LogTarget target);

/**
 * @brief Is `str` an accepted log-destination spelling?
 *
 * parse_log_target() cannot answer this — it maps anything unrecognized onto
 * Auto, which is a legal value, so a typo is indistinguishable from a
 * deliberate "auto". Callers that must REJECT a bad value (the `--log-dest`
 * parser, the `HELIX_LOG_DEST` reader) need this predicate instead.
 *
 * Deliberately excludes "android": it is a build-target detail chosen by
 * detect_best_target(), never something a user selects.
 */
bool is_valid_log_target(const std::string& str);

/// Comma-separated accepted values for is_valid_log_target(), for error text.
const char* log_target_accepted_values();

/**
 * @brief Is `str` an accepted log-level spelling?
 *
 * Same rationale as is_valid_log_target(): parse_level() falls back to a
 * default rather than reporting failure. Narrower than parse_level(), which
 * also tolerates the "warning" alias — the user-facing surfaces accept only
 * the canonical spellings so the two lists cannot drift apart silently.
 */
bool is_valid_log_level(const std::string& str);

/// Comma-separated accepted values for is_valid_log_level(), for error text.
const char* log_level_accepted_values();

/**
 * @brief Read a HELIX_LOG_* environment variable, validating it
 *
 * The launcher translates HELIX_LOG_DEST / HELIX_LOG_FILE / HELIX_LOG_LEVEL
 * into --log-dest / --log-file / --log-level, but it is not always in the
 * picture — systemd units, a direct exec, and forked init scripts start the
 * binary themselves. Application::init_logging() reads the variables through
 * this so they work either way (#1249).
 *
 * A value the validator rejects is DROPPED with a warning, never fatal: unlike
 * a CLI typo (which the user sees immediately and can retry), a typo in
 * helixscreen.env would otherwise turn every boot of an appliance into a
 * crash-loop. The caller falls through to the next precedence level.
 *
 * @param var_name        Environment variable to read
 * @param validate        Predicate, or nullptr to accept any non-empty value
 * @param accepted_values Text listed in the warning when validate rejects
 * @return The value, or "" when unset, empty, or rejected
 */
std::string log_env_override(const char* var_name, bool (*validate)(const std::string&),
                             const char* accepted_values);

/**
 * @brief First non-empty of CLI flag, env var, config value
 *
 * The one place the CLI > env > config order for logging settings is written
 * down. Pure, so the precedence itself is unit-testable without a process
 * environment or a Config instance.
 */
std::string resolve_log_setting(const std::string& cli_value, const std::string& env_value,
                                const std::string& config_value);

/**
 * @brief Human-readable description of the currently-active log destination
 *
 * Resolved during init() — reflects the effective target after Auto detection,
 * and for the File target returns the resolved file path. Suitable for display
 * in the About panel.
 *
 * Returns an empty string before init() has been called.
 */
std::string effective_destination();

/**
 * @brief The resolved file path the active file-sink writes to
 *
 * Single source of truth for "which file is the app logging to right now."
 * Returns the resolved path when the effective target is File, or an empty
 * string for every other target (journal, syslog, console, Android) and before
 * init() has run. Unlike effective_destination(), this never returns a
 * human-readable label — it is meant to be read back as an actual path. The
 * crash reporter and debug-bundle collector use it instead of re-deriving
 * candidate paths, so the two never diverge.
 */
std::string effective_log_file_path();

/**
 * @brief Parse log level from string
 *
 * @param str One of: "trace", "debug", "info", "warn", "warning", "error", "critical", "off"
 * @param default_level Level to return if string is empty or unrecognized
 * @return Corresponding spdlog level enum
 */
spdlog::level::level_enum
parse_level(const std::string& str, spdlog::level::level_enum default_level = spdlog::level::warn);

/**
 * @brief Convert CLI verbosity count to log level
 *
 * Maps: 0 -> warn, 1 -> info, 2 -> debug, 3+ -> trace
 *
 * @param verbosity Number of -v flags (0 = none)
 * @return Corresponding spdlog level
 */
spdlog::level::level_enum verbosity_to_level(int verbosity);

/**
 * @brief Convert spdlog level to libhv level
 *
 * libhv levels: VERBOSE(0) < DEBUG(1) < INFO(2) < WARN(3) < ERROR(4) < FATAL(5) < SILENT(6)
 *
 * @param level spdlog log level
 * @return libhv log level integer
 */
int to_hv_level(spdlog::level::level_enum level);

/**
 * @brief Change log level at runtime (no restart needed)
 *
 * Updates both spdlog and libhv log levels immediately.
 * Call from the main thread when the user changes the log level setting.
 *
 * Not available in the watchdog build — the watchdog intentionally does not
 * link libhv, so runtime level changes for libhv's logger are not supported
 * there. The watchdog has its own static log level set at init.
 *
 * @param level New spdlog log level
 */
#ifndef HELIX_WATCHDOG
void set_runtime_level(spdlog::level::level_enum level);
#endif

/**
 * @brief Resolve log level with precedence: CLI > config > defaults
 *
 * @param cli_verbosity CLI -v flag count (0 = none)
 * @param config_level_str Log level from config file (empty = not set)
 * @param test_mode True if running in test mode (affects default)
 * @return Resolved log level
 */
spdlog::level::level_enum resolve_log_level(int cli_verbosity, const std::string& config_level_str,
                                            bool test_mode);

/**
 * @brief Tail of the in-memory ring-buffer log sink (newest-last, joined by \n)
 *
 * The ring buffer is installed on ALL platforms by init() and captures DEBUG
 * regardless of the user-configured level the file/syslog/console sinks run at.
 * It is the authoritative source for the debug bundle's log_tail because it is
 * always the live process and always fresh — unlike the file cascade, which on
 * syslog-target devices (AD5X/AD5M) falls back to stale leftover files and only
 * carries WARN-filtered /var/log/messages lines.
 *
 * Returns at most `num_lines` of the most-recent formatted log lines, oldest
 * first. Empty before init() has installed the sink (e.g. the watchdog build,
 * which does not call init() with a ring sink) or if nothing has been logged.
 *
 * @param num_lines Max lines to return (0 = all retained)
 * @return Newline-joined recent log lines, or empty string
 */
std::string tail_ring_buffer(int num_lines);

/// Number of messages the ring buffer currently retains (capacity), for the
/// bundle's log_meta diagnostic key. 0 before init() installs the sink.
size_t ring_buffer_capacity();

/// The effective spdlog level the persistent (file/syslog/console) sinks run
/// at — i.e. the user-configured level, NOT the ring buffer's debug floor.
/// Lets a bundle reader know whether debug was reaching persistent logs.
spdlog::level::level_enum effective_log_level();

} // namespace logging
} // namespace helix
