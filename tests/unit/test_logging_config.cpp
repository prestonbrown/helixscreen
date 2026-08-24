// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "logging_init.h"

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::logging;

// ============================================================================
// parse_level() tests
// ============================================================================

TEST_CASE("parse_level: valid level strings", "[logging][config]") {
    SECTION("trace") {
        REQUIRE(parse_level("trace") == spdlog::level::trace);
    }

    SECTION("debug") {
        REQUIRE(parse_level("debug") == spdlog::level::debug);
    }

    SECTION("info") {
        REQUIRE(parse_level("info") == spdlog::level::info);
    }

    SECTION("warn") {
        REQUIRE(parse_level("warn") == spdlog::level::warn);
    }

    SECTION("warning (alias)") {
        REQUIRE(parse_level("warning") == spdlog::level::warn);
    }

    SECTION("error") {
        REQUIRE(parse_level("error") == spdlog::level::err);
    }

    SECTION("critical") {
        REQUIRE(parse_level("critical") == spdlog::level::critical);
    }

    SECTION("off") {
        REQUIRE(parse_level("off") == spdlog::level::off);
    }
}

TEST_CASE("parse_level: returns default for invalid input", "[logging][config]") {
    SECTION("empty string") {
        REQUIRE(parse_level("", spdlog::level::warn) == spdlog::level::warn);
        REQUIRE(parse_level("", spdlog::level::debug) == spdlog::level::debug);
    }

    SECTION("unrecognized string") {
        REQUIRE(parse_level("verbose", spdlog::level::warn) == spdlog::level::warn);
        REQUIRE(parse_level("TRACE", spdlog::level::info) == spdlog::level::info); // case sensitive
    }
}

// ============================================================================
// verbosity_to_level() tests
// ============================================================================

TEST_CASE("verbosity_to_level: CLI verbosity flags", "[logging][config]") {
    SECTION("-v (1) = info") {
        REQUIRE(verbosity_to_level(1) == spdlog::level::info);
    }

    SECTION("-vv (2) = debug") {
        REQUIRE(verbosity_to_level(2) == spdlog::level::debug);
    }

    SECTION("-vvv (3+) = trace") {
        REQUIRE(verbosity_to_level(3) == spdlog::level::trace);
        REQUIRE(verbosity_to_level(4) == spdlog::level::trace);
        REQUIRE(verbosity_to_level(10) == spdlog::level::trace);
    }

    SECTION("0 = warn (no verbosity flags)") {
        REQUIRE(verbosity_to_level(0) == spdlog::level::warn);
    }

    SECTION("negative = warn") {
        REQUIRE(verbosity_to_level(-1) == spdlog::level::warn);
    }
}

// ============================================================================
// to_hv_level() tests
// ============================================================================

TEST_CASE("to_hv_level: spdlog to libhv level mapping", "[logging][config]") {
    // libhv levels: VERBOSE(0) < DEBUG(1) < INFO(2) < WARN(3) < ERROR(4) < FATAL(5) < SILENT(6)

    SECTION("trace maps to DEBUG (libhv has no trace)") {
        REQUIRE(to_hv_level(spdlog::level::trace) == 1); // LOG_LEVEL_DEBUG
    }

    SECTION("debug maps to DEBUG") {
        REQUIRE(to_hv_level(spdlog::level::debug) == 1); // LOG_LEVEL_DEBUG
    }

    SECTION("info maps to INFO") {
        REQUIRE(to_hv_level(spdlog::level::info) == 2); // LOG_LEVEL_INFO
    }

    SECTION("warn maps to WARN") {
        REQUIRE(to_hv_level(spdlog::level::warn) == 3); // LOG_LEVEL_WARN
    }

    SECTION("error maps to ERROR") {
        REQUIRE(to_hv_level(spdlog::level::err) == 4); // LOG_LEVEL_ERROR
    }

    SECTION("critical maps to FATAL") {
        REQUIRE(to_hv_level(spdlog::level::critical) == 5); // LOG_LEVEL_FATAL
    }

    SECTION("off maps to SILENT") {
        REQUIRE(to_hv_level(spdlog::level::off) == 6); // LOG_LEVEL_SILENT
    }
}

// ============================================================================
// resolve_log_level() tests
// ============================================================================

TEST_CASE("resolve_log_level: precedence rules", "[logging][config]") {
    SECTION("CLI verbosity takes precedence over config") {
        // CLI says -vv (debug), config says "error"
        auto level = resolve_log_level(2, "error", false);
        REQUIRE(level == spdlog::level::debug);
    }

    SECTION("config file used when no CLI verbosity") {
        auto level = resolve_log_level(0, "trace", false);
        REQUIRE(level == spdlog::level::trace);
    }

    SECTION("test_mode defaults to debug when no CLI or config") {
        auto level = resolve_log_level(0, "", true);
        REQUIRE(level == spdlog::level::debug);
    }

    SECTION("production defaults to warn when no CLI or config") {
        auto level = resolve_log_level(0, "", false);
        REQUIRE(level == spdlog::level::warn);
    }

    SECTION("CLI verbosity beats test_mode default") {
        // CLI says -v (info), test mode would default to debug
        auto level = resolve_log_level(1, "", true);
        REQUIRE(level == spdlog::level::info);
    }

    SECTION("config beats test_mode default") {
        // Config says warn, test mode would default to debug
        auto level = resolve_log_level(0, "warn", true);
        REQUIRE(level == spdlog::level::warn);
    }
}

// ============================================================================
// Existing parse_log_target() tests (ensure we didn't break it)
// ============================================================================

TEST_CASE("parse_log_target: valid targets", "[logging][config]") {
    REQUIRE(parse_log_target("auto") == LogTarget::Auto);
    REQUIRE(parse_log_target("journal") == LogTarget::Journal);
    REQUIRE(parse_log_target("syslog") == LogTarget::Syslog);
    REQUIRE(parse_log_target("file") == LogTarget::File);
    REQUIRE(parse_log_target("console") == LogTarget::Console);
}

TEST_CASE("parse_log_target: defaults to Auto for unknown", "[logging][config]") {
    REQUIRE(parse_log_target("unknown") == LogTarget::Auto);
    REQUIRE(parse_log_target("") == LogTarget::Auto);
    REQUIRE(parse_log_target("CONSOLE") == LogTarget::Auto); // case sensitive
}

TEST_CASE("log_target_name: round-trip", "[logging][config]") {
    REQUIRE(std::string(log_target_name(LogTarget::Auto)) == "auto");
    REQUIRE(std::string(log_target_name(LogTarget::Journal)) == "journal");
    REQUIRE(std::string(log_target_name(LogTarget::Syslog)) == "syslog");
    REQUIRE(std::string(log_target_name(LogTarget::File)) == "file");
    REQUIRE(std::string(log_target_name(LogTarget::Console)) == "console");
}

// ============================================================================
// should_add_console() tests — the console sink gate (#1105)
//
// Regression cover for: `helix-screen --test -vv | tee log.txt` produced ZERO
// spdlog output.  detect_best_target() returns Journal on any box with
// /run/systemd/journal/socket, and the Journal branch attached the console sink
// only when isatty(stdout) — so piping (the documented way to capture a
// session) silently discarded every line at every -v level.
//
// The force must stay narrow: the shipped launcher synthesizes --log-level/-vv
// from HELIX_LOG_LEVEL / HELIX_DEBUG (scripts/helix-launcher.sh), and
// serve-local-update.sh writes HELIX_LOG_LEVEL=debug onto live devices — so
// force_console IS reachable under systemd.  Forcing a console sink there would
// double-log every line into the journal / launcher.log, which is exactly the
// Snapmaker U1 tmpfs blowout (498 MB /tmp/helixscreen.log) the TTY gate exists
// to prevent.  Hence: force on TTY/pipe, never on regular file or socket.
// ============================================================================

TEST_CASE("should_add_console: Console target always attaches console", "[logging][config]") {
    // Console is the ONLY sink for this target — dropping it means no output at all.
    for (bool force : {false, true}) {
        REQUIRE(should_add_console(LogTarget::Console, true, force, false, StdoutKind::Tty));
        REQUIRE(should_add_console(LogTarget::Console, true, force, false, StdoutKind::Pipe));
        REQUIRE(should_add_console(LogTarget::Console, true, force, false, StdoutKind::File));
        REQUIRE(should_add_console(LogTarget::Console, true, force, false, StdoutKind::Socket));
        REQUIRE(should_add_console(LogTarget::Console, true, force, false, StdoutKind::Other));
    }
}

TEST_CASE("should_add_console: Android target never attaches console", "[logging][config]") {
    // stdout is invisible under logcat; android_sink carries the output.
    for (bool force : {false, true}) {
        REQUIRE_FALSE(should_add_console(LogTarget::Android, true, force, false, StdoutKind::Tty));
        REQUIRE_FALSE(should_add_console(LogTarget::Android, true, force, false, StdoutKind::Pipe));
        REQUIRE_FALSE(should_add_console(LogTarget::Android, true, force, false, StdoutKind::File));
        REQUIRE_FALSE(
            should_add_console(LogTarget::Android, true, force, false, StdoutKind::Socket));
        REQUIRE_FALSE(
            should_add_console(LogTarget::Android, true, force, false, StdoutKind::Other));
    }
}

TEST_CASE("should_add_console: structured targets always attach on a TTY", "[logging][config]") {
    // Interactive shell run -> user sees colored output alongside the journal.
    // Unchanged behavior, with or without an explicit -v.
    for (bool force : {false, true}) {
        for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
            REQUIRE(should_add_console(target, true, force, false, StdoutKind::Tty));
        }
    }
}

TEST_CASE("should_add_console: structured targets stay quiet without an explicit -v",
          "[logging][config]") {
    // No flag -> only a TTY gets a console. Preserves the daemon behavior.
    for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
        REQUIRE_FALSE(should_add_console(target, true, false, false, StdoutKind::Pipe));
        REQUIRE_FALSE(should_add_console(target, true, false, false, StdoutKind::File));
        REQUIRE_FALSE(should_add_console(target, true, false, false, StdoutKind::Socket));
        REQUIRE_FALSE(should_add_console(target, true, false, false, StdoutKind::Other));
    }
}

TEST_CASE("should_add_console: explicit -v forces console for a PIPE", "[logging][config]") {
    // The #1105 fix: `--test -vv | tee run.log` on a journald box. A pipe means a
    // human is watching, so honor the flag even though stdout is not a TTY.
    for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
        REQUIRE(should_add_console(target, true, true, false, StdoutKind::Pipe));
    }
}

TEST_CASE("should_add_console: explicit -v does NOT force console for a regular file",
          "[logging][config]") {
    // The daemon redirect (`>> $LOGFILE 2>&1` in the U1/K1/K2/CC1/AD5M init
    // scripts). stdout already lands in the file the structured sink writes, so a
    // console sink here double-logs every line -> the U1 498 MB tmpfs blowout.
    // Reachable because the launcher synthesizes --log-level from HELIX_LOG_LEVEL.
    for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
        REQUIRE_FALSE(should_add_console(target, true, true, false, StdoutKind::File));
    }
}

TEST_CASE("should_add_console: explicit -v does NOT force console for a socket",
          "[logging][config]") {
    // systemd StandardOutput=journal hands the process a socket. Forcing a console
    // sink writes every line into the journal a second time.
    for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
        REQUIRE_FALSE(should_add_console(target, true, true, false, StdoutKind::Socket));
    }
}

TEST_CASE("should_add_console: unknown stdout kind is not forceable", "[logging][config]") {
    // fstat() failed or an fd type we do not classify — fall back to the plain
    // isatty-only behavior rather than risk double-logging a daemon.
    for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
        REQUIRE_FALSE(should_add_console(target, true, true, false, StdoutKind::Other));
    }
}

TEST_CASE("should_add_console: enable_console=false vetoes every combination",
          "[logging][config]") {
    // Master switch — must beat force_console, a live TTY, and every target.
    for (bool force : {false, true}) {
        for (auto kind : {StdoutKind::Tty, StdoutKind::Pipe, StdoutKind::File, StdoutKind::Socket,
                          StdoutKind::Other}) {
            for (auto target : {LogTarget::Console, LogTarget::Journal, LogTarget::Syslog,
                                LogTarget::File, LogTarget::Android}) {
                REQUIRE_FALSE(should_add_console(target, false, force, false, kind));
            }
        }
    }
}

// ============================================================================
// test_mode dimension (#1105 gap closer)
//
// #1105's literal repro is `./build/bin/helix-screen --test -vv`. On a TTY that
// always worked, so the reporter must have redirected stdout — and if they used
// `> file` rather than `| tee`, the pipe-only force still leaves them with
// nothing.  --test never runs in production (no systemd unit, init script, procd
// shim, or launcher passes it), so attaching the console unconditionally in test
// mode cannot reach a daemonized double-log path and carries no blowout risk.
// ============================================================================

TEST_CASE("should_add_console: test mode attaches console for ANY stdout kind",
          "[logging][config]") {
    // The gap closer: `--test -vv > file` and `--test -vv` under any redirect
    // must produce output. No -v needed either — test mode alone is enough.
    for (bool force : {false, true}) {
        for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
            REQUIRE(should_add_console(target, true, force, true, StdoutKind::Tty));
            REQUIRE(should_add_console(target, true, force, true, StdoutKind::Pipe));
            REQUIRE(should_add_console(target, true, force, true, StdoutKind::File));
            REQUIRE(should_add_console(target, true, force, true, StdoutKind::Socket));
            REQUIRE(should_add_console(target, true, force, true, StdoutKind::Other));
        }
    }
}

TEST_CASE("should_add_console: test mode does NOT override the Android rule", "[logging][config]") {
    // Deliberate precedence call: Android + test mode is not a real configuration,
    // and stdout is invisible under logcat regardless, so Android stays false.
    for (bool force : {false, true}) {
        for (auto kind : {StdoutKind::Tty, StdoutKind::Pipe, StdoutKind::File, StdoutKind::Socket,
                          StdoutKind::Other}) {
            REQUIRE_FALSE(should_add_console(LogTarget::Android, true, force, true, kind));
        }
    }
}

TEST_CASE("should_add_console: test mode is still vetoed by enable_console=false",
          "[logging][config]") {
    // Master switch outranks test mode too.
    for (auto kind : {StdoutKind::Tty, StdoutKind::Pipe, StdoutKind::File, StdoutKind::Socket,
                      StdoutKind::Other}) {
        REQUIRE_FALSE(should_add_console(LogTarget::Journal, false, true, true, kind));
    }
}

TEST_CASE("should_add_console: production guard intact when test mode is off",
          "[logging][config]") {
    // Regression fence for the blowout guard: with test_mode=false, an explicit
    // -v must still NOT force a console onto a daemon's file/socket stdout.
    for (auto target : {LogTarget::Journal, LogTarget::Syslog, LogTarget::File}) {
        REQUIRE_FALSE(should_add_console(target, true, true, false, StdoutKind::File));
        REQUIRE_FALSE(should_add_console(target, true, true, false, StdoutKind::Socket));
        REQUIRE_FALSE(should_add_console(target, true, true, false, StdoutKind::Other));
        // ...but a pipe still works (the #1105 fix for non-test runs).
        REQUIRE(should_add_console(target, true, true, false, StdoutKind::Pipe));
    }
}

// ============================================================================
// Ring capacity scales with device RAM
// ============================================================================

TEST_CASE("ring_capacity_for_ram scales with the machine", "[logging][config][ring]") {
    using helix::logging::ring_capacity_for_ram;

    SECTION("small boards keep the historical floor, never regress") {
        REQUIRE(ring_capacity_for_ram(107) == 2000); // AD5M
        REQUIRE(ring_capacity_for_ram(128) == 2048); // CC1, just over the floor
    }

    SECTION("mid-range boards get proportionally more") {
        // AD5X: 473 MB -> 7568 lines, ~4x the old fixed 2000.
        REQUIRE(ring_capacity_for_ram(473) == 7568);
        REQUIRE(ring_capacity_for_ram(473) > ring_capacity_for_ram(128));
    }

    SECTION("large machines are capped — more lines stop paying for themselves") {
        REQUIRE(ring_capacity_for_ram(2048) == 20000);
        REQUIRE(ring_capacity_for_ram(8192) == 20000);
    }

    SECTION("failed detection falls back to the floor rather than 0") {
        REQUIRE(ring_capacity_for_ram(0) == 2000);
    }

    SECTION("monotonic in RAM") {
        size_t prev = 0;
        for (size_t mb : {0u, 64u, 107u, 128u, 256u, 473u, 512u, 1024u, 2048u, 4096u}) {
            size_t cap = ring_capacity_for_ram(mb);
            if (mb > 0) {
                REQUIRE(cap >= prev);
            }
            REQUIRE(cap >= 2000);
            REQUIRE(cap <= 20000);
            prev = cap;
        }
    }
}

// ============================================================================
// HELIX_LOG_* environment precedence (issue #1249)
//
// The launcher translating HELIX_LOG_* into --log-* flags is not enough: a
// systemd unit's Environment=, a direct exec, and third-party init scripts all
// start helix-screen without it. Application::init_logging() therefore reads
// the variables itself, through log_env_override() + resolve_log_setting().
// ============================================================================

namespace {

/// RAII setenv/unsetenv so a failing REQUIRE cannot leak a variable into the
/// next test in the same process.
class ScopedEnv {
  public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* prev = std::getenv(name)) {
            had_previous_ = true;
            previous_ = prev;
        }
        if (value != nullptr) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (had_previous_) {
            ::setenv(name_, previous_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    const char* name_;
    bool had_previous_ = false;
    std::string previous_;
};

constexpr const char* ENV_DEST = "HELIX_LOG_DEST";
constexpr const char* ENV_LEVEL = "HELIX_LOG_LEVEL";
constexpr const char* ENV_FILE = "HELIX_LOG_FILE";

} // namespace

TEST_CASE("is_valid_log_target matches the --log-dest accepted set", "[logging][config][1249]") {
    for (const char* ok : {"auto", "journal", "syslog", "file", "console"}) {
        INFO(ok);
        REQUIRE(is_valid_log_target(ok));
    }
    // "android" is chosen by detect_best_target(), never selected by a user.
    REQUIRE_FALSE(is_valid_log_target("android"));
    REQUIRE_FALSE(is_valid_log_target(""));
    REQUIRE_FALSE(is_valid_log_target("File"));
    REQUIRE_FALSE(is_valid_log_target("filee"));
    REQUIRE_FALSE(is_valid_log_target("/var/log/helix.log"));

    // Everything the predicate accepts must round-trip through parse_log_target
    // to something other than an accidental Auto.
    REQUIRE(parse_log_target("file") == LogTarget::File);
    REQUIRE(parse_log_target("console") == LogTarget::Console);
}

TEST_CASE("is_valid_log_level matches the --log-level accepted set", "[logging][config][1249]") {
    for (const char* ok : {"trace", "debug", "info", "warn", "error", "critical", "off"}) {
        INFO(ok);
        REQUIRE(is_valid_log_level(ok));
    }
    // parse_level() tolerates the "warning" alias; the user-facing surfaces do
    // not, and the CLI parser has always rejected it.
    REQUIRE_FALSE(is_valid_log_level("warning"));
    REQUIRE_FALSE(is_valid_log_level(""));
    REQUIRE_FALSE(is_valid_log_level("DEBUG"));
    REQUIRE_FALSE(is_valid_log_level("verbose"));
}

TEST_CASE("log_env_override reads a valid variable", "[logging][config][1249]") {
    SECTION("destination") {
        ScopedEnv env(ENV_DEST, "file");
        REQUIRE(log_env_override(ENV_DEST, &is_valid_log_target, log_target_accepted_values()) ==
                "file");
    }
    SECTION("level") {
        ScopedEnv env(ENV_LEVEL, "debug");
        REQUIRE(log_env_override(ENV_LEVEL, &is_valid_log_level, log_level_accepted_values()) ==
                "debug");
    }
    SECTION("a path takes no validator — any non-empty string is accepted") {
        ScopedEnv env(ENV_FILE, "/opt/config/mod_data/log/helix.log");
        REQUIRE(log_env_override(ENV_FILE, nullptr, nullptr) ==
                "/opt/config/mod_data/log/helix.log");
    }
}

TEST_CASE("log_env_override yields empty when unset or blank", "[logging][config][1249]") {
    SECTION("unset") {
        ScopedEnv env(ENV_DEST, nullptr);
        REQUIRE(
            log_env_override(ENV_DEST, &is_valid_log_target, log_target_accepted_values()).empty());
    }
    SECTION("set to the empty string — helixscreen.env ships `#HELIX_LOG_FILE=` commented "
            "out, but a user can uncomment it with no value") {
        ScopedEnv env(ENV_FILE, "");
        REQUIRE(log_env_override(ENV_FILE, nullptr, nullptr).empty());
    }
}

TEST_CASE("log_env_override drops an invalid value instead of aborting",
          "[logging][config][1249]") {
    // A typo in helixscreen.env must not crash-loop an appliance: unlike a CLI
    // typo, nobody is at a prompt to fix it. The value is dropped (with a
    // warning) so the caller falls through to the next precedence level.
    SECTION("destination") {
        ScopedEnv env(ENV_DEST, "sysloge");
        std::string got;
        REQUIRE_NOTHROW(
            got = log_env_override(ENV_DEST, &is_valid_log_target, log_target_accepted_values()));
        REQUIRE(got.empty());
    }
    SECTION("level") {
        ScopedEnv env(ENV_LEVEL, "louder");
        std::string got;
        REQUIRE_NOTHROW(
            got = log_env_override(ENV_LEVEL, &is_valid_log_level, log_level_accepted_values()));
        REQUIRE(got.empty());
    }
    SECTION("a rejected value must not leak into parse_log_target as Auto-by-accident") {
        ScopedEnv env(ENV_DEST, "nonsense");
        const std::string from_env =
            log_env_override(ENV_DEST, &is_valid_log_target, log_target_accepted_values());
        // Falls through to the config tier, which here says "file".
        REQUIRE(resolve_log_setting("", from_env, "file") == "file");
        REQUIRE(parse_log_target(resolve_log_setting("", from_env, "file")) == LogTarget::File);
    }
}

TEST_CASE("resolve_log_setting: CLI > env > config", "[logging][config][1249]") {
    SECTION("CLI wins over both") {
        REQUIRE(resolve_log_setting("console", "file", "syslog") == "console");
    }
    SECTION("env wins over config when there is no CLI flag") {
        REQUIRE(resolve_log_setting("", "file", "syslog") == "file");
    }
    SECTION("config is used when neither CLI nor env is set") {
        REQUIRE(resolve_log_setting("", "", "syslog") == "syslog");
    }
    SECTION("all empty stays empty — the caller supplies its own default") {
        REQUIRE(resolve_log_setting("", "", "").empty());
    }
    SECTION("a file path resolves the same way") {
        REQUIRE(resolve_log_setting("/cli.log", "/env.log", "/cfg.log") == "/cli.log");
        REQUIRE(resolve_log_setting("", "/env.log", "/cfg.log") == "/env.log");
        REQUIRE(resolve_log_setting("", "", "/cfg.log") == "/cfg.log");
    }
}

TEST_CASE("end-to-end precedence for the ZMOD hook configuration", "[logging][config][1249]") {
    // hooks-ad5m-zmod.sh exports HELIX_LOG_DEST=file plus a path under
    // /opt/config, which is where ZMOD's TAR_CONFIG archiver actually looks.
    ScopedEnv dest(ENV_DEST, "file");
    ScopedEnv file(ENV_FILE, "/opt/config/mod_data/log/helix.log");

    const std::string env_dest =
        log_env_override(ENV_DEST, &is_valid_log_target, log_target_accepted_values());
    const std::string env_file = log_env_override(ENV_FILE, nullptr, nullptr);

    SECTION("with no CLI flags and no config, the hook's values are what apply") {
        REQUIRE(parse_log_target(resolve_log_setting("", env_dest, "auto")) == LogTarget::File);
        REQUIRE(resolve_log_setting("", env_file, "") == "/opt/config/mod_data/log/helix.log");
    }

    SECTION("an explicit CLI flag still overrides the hook") {
        REQUIRE(parse_log_target(resolve_log_setting("console", env_dest, "auto")) ==
                LogTarget::Console);
        REQUIRE(resolve_log_setting("/tmp/manual.log", env_file, "") == "/tmp/manual.log");
    }

    SECTION("the env beats a settings.json value") {
        REQUIRE(parse_log_target(resolve_log_setting("", env_dest, "syslog")) == LogTarget::File);
        REQUIRE(resolve_log_setting("", env_file, "/var/log/from-settings.log") ==
                "/opt/config/mod_data/log/helix.log");
    }
}
