// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file application_bootstrap.cpp
 * @brief Application startup phases before the display exists: data-root cwd,
 *        CLI/env argument parsing, config load and logging setup.
 */

#include "app_globals.h"
#include "application.h"
#include "board_identity.h"
#include "cli_args.h"
#include "config.h"
#include "core/data_root_resolver.h"
#include "display_settings_manager.h"
#include "environment_config.h"
#include "hv/hlog.h" // libhv logging - sync level with spdlog
#include "logging_init.h"
#include "runtime_config.h"
#include "system/config_trust.h"
#include "system/update_checker.h"
#include "thermal_rate_model.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace helix;

// External globals for logging (defined in cli_args.cpp, populated by parse_cli_args)
extern std::string g_log_dest_cli;
extern std::string g_log_file_cli;
extern std::string g_log_level_cli;

void Application::ensure_project_root_cwd() {
    using EnvConfig = helix::config::EnvironmentConfig;

    // HELIX_DATA_DIR takes priority - allows standalone deployment
    // Validate BEFORE chdir to avoid corrupting the working directory
    if (auto data_dir = EnvConfig::get_data_dir()) {
        if (helix::is_valid_data_root(data_dir->c_str())) {
            if (chdir(data_dir->c_str()) == 0) {
                spdlog::info("[Application] Using HELIX_DATA_DIR: {}", *data_dir);
                return;
            }
            spdlog::warn("[Application] HELIX_DATA_DIR '{}' valid but chdir failed: {}", *data_dir,
                         strerror(errno));
        } else {
            spdlog::warn("[Application] HELIX_DATA_DIR '{}' has no ui_xml/ directory", *data_dir);
        }
    }

    // Fall back to auto-detection from executable path
    char exe_path[PATH_MAX];

#ifdef __APPLE__
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        spdlog::warn("[Application] Could not get executable path");
        return;
    }
    char resolved[PATH_MAX];
    if (realpath(exe_path, resolved)) {
        strncpy(exe_path, resolved, PATH_MAX - 1);
        exe_path[PATH_MAX - 1] = '\0';
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        spdlog::warn("[Application] Could not read /proc/self/exe");
        return;
    }
    exe_path[len] = '\0';
#else
    return;
#endif

    std::string data_root = helix::board::resolve_install_root(exe_path);
    if (!data_root.empty()) {
        if (chdir(data_root.c_str()) == 0) {
            spdlog::info("[Application] Auto-detected data root: {}", data_root);
            return;
        }
        spdlog::warn("[Application] Found data root '{}' but chdir failed: {}", data_root,
                     strerror(errno));
    }

    // Last resort: check if CWD already has what we need
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) && helix::is_valid_data_root(cwd)) {
        spdlog::debug("[Application] Current working directory is already valid: {}", cwd);
        return;
    }

    spdlog::error("[Application] Could not find HelixScreen data root (ui_xml/ directory). "
                  "Set HELIX_DATA_DIR or run from the install directory.");
}

bool Application::parse_args(int argc, char** argv) {
    // Parse CLI args first
    if (!helix::parse_cli_args(argc, argv, m_args, m_screen_width, m_screen_height)) {
        return false;
    }

    // Apply environment variable overrides using type-safe EnvironmentConfig
    using EnvConfig = helix::config::EnvironmentConfig;

    // HELIX_SCREEN_SIZE: screen size override (alternative to -s flag)
    // Only applies if -s was not passed on the command line
    if (m_screen_width == 0 && m_screen_height == 0) {
        m_screen_size_env = EnvConfig::get_screen_size();
        if (m_screen_size_env) {
            m_screen_size_env_valid = helix::parse_screen_size_string(
                m_screen_size_env->c_str(), m_screen_width, m_screen_height);
        }
    }

    // HELIX_AUTO_QUIT_MS: auto-quit timeout (100ms - 1hr)
    if (m_args.timeout_sec == 0) {
        if (auto timeout = EnvConfig::get_auto_quit_seconds()) {
            m_args.timeout_sec = *timeout;
        }
    }

    // HELIX_AUTO_SCREENSHOT: enable screenshot mode
    if (EnvConfig::get_screenshot_enabled()) {
        m_args.screenshot_enabled = true;
    }

    // HELIX_AMS_GATES: mock AMS gate count (1-16)
    if (auto gates = EnvConfig::get_mock_ams_gates()) {
        get_runtime_config()->mock_ams_gate_count = *gates;
    }

    // HELIX_BENCHMARK: benchmark mode
    if (EnvConfig::get_benchmark_mode()) {
        spdlog::info("[Application] Benchmark mode enabled");
    }

    return true;
}

bool Application::init_config() {
    m_config = Config::get_instance();

    // Use separate config file for test mode to avoid conflicts with real printer settings
    const char* config_path = get_runtime_config()->test_mode ? RuntimeConfig::kTestConfigPath
                                                              : RuntimeConfig::kProdConfigPath;
    spdlog::info("[Application] Using config: {}", config_path);
    // A Moonraker update removes the config links into printer_data, and
    // without a restarting service the watchdog starts this binary before
    // anything puts them back (#2147). Relink before init reads settings.json.
    if (!get_runtime_config()->test_mode) {
        const std::string install_root = app_get_install_root();
        if (UpdateChecker::startup_runs_restore(getenv("HELIX_SERVICE_RESTARTS"), install_root)) {
            UpdateChecker::run_restore_user_config(install_root);
        }
    }
    m_config->init(config_path);

    // Load persisted thermal heating rates so estimates are available immediately
    ThermalRateManager::instance().load_from_config(*m_config);

    return true;
}

bool Application::init_logging() {
    using namespace helix::logging;

    // Apply the configured timezone BEFORE the first timestamped line. It is
    // applied again later by DisplaySettingsManager::init_subjects() (which owns
    // the setting and its subject) — idempotent, and by then the value already
    // matches. Without this the log's wall clock jumps mid-startup on any device
    // whose configured zone differs from the host's, which reads as a stall in a
    // debug bundle (#1218).
    DisplaySettingsManager::apply_configured_timezone();

    LogConfig log_config;

    // HELIX_LOG_* is read HERE, not only translated by scripts/helix-launcher.sh
    // into --log-dest/--log-file/--log-level. The launcher is not always in the
    // picture: a systemd unit with Environment=, a hand-run binary over SSH, and
    // third-party init scripts (ZMOD ships its own fork of ours) all start
    // helix-screen directly, and until now every HELIX_LOG_* they exported was
    // silently ignored — the variables looked configurable and were not (#1249).
    // The launcher's flags still win, because a CLI flag outranks the env below.
    const std::string env_log_dest =
        log_env_override("HELIX_LOG_DEST", &is_valid_log_target, log_target_accepted_values());
    const std::string env_log_level =
        log_env_override("HELIX_LOG_LEVEL", &is_valid_log_level, log_level_accepted_values());
    // No validity predicate for a path: any string is a candidate, and an
    // unopenable one degrades to the platform's normal sink inside init().
    const std::string env_log_file = log_env_override("HELIX_LOG_FILE", nullptr, nullptr);

    // Resolve log level: --log-level > HELIX_LOG_LEVEL > -v flags > config file > defaults
    std::string config_level = m_config->get<std::string>("/log_level", "");
    if (!g_log_level_cli.empty()) {
        log_config.level = parse_level(g_log_level_cli, spdlog::level::warn);
    } else if (!env_log_level.empty()) {
        log_config.level = parse_level(env_log_level, spdlog::level::warn);
    } else {
        log_config.level =
            resolve_log_level(m_args.verbosity, config_level, get_runtime_config()->test_mode);
    }

    // An explicit -v/--log-level means the user asked to watch the logs, so attach
    // the console sink even when stdout is a pipe. Without this, `helix-screen -vv |
    // tee run.log` on any box with a systemd journal socket produces no output at
    // all — auto-detection picks the Journal target, whose console gate is
    // isatty(stdout). A bare run with no flag keeps the journal-only behavior.
    //
    // HELIX_LOG_LEVEL counts as explicit for the same reason the flag does: the
    // launcher already turns that variable into --log-level=, so it has ALWAYS
    // set force_console on a launcher-started device. Treating the direct-env
    // path differently would make the same helixscreen.env behave one way under
    // the launcher and another under systemd/a forked init script, which is the
    // exact inconsistency this block exists to remove. The blast radius is
    // bounded: force_console only adds a sink for a PIPE — should_add_console()
    // still refuses a regular file or socket, which is where the daemon
    // double-log (the Snapmaker U1 tmpfs blowout) came from.
    log_config.force_console =
        m_args.verbosity > 0 || !g_log_level_cli.empty() || !env_log_level.empty();

    // --test always gets a console sink, whatever stdout is (pipe, file, socket).
    // Read here rather than inside logging_init.cpp because that TU is linked into
    // the watchdog build, which does not link runtime_config.o.
    log_config.test_mode = get_runtime_config()->test_mode;

    // Resolve log destination: CLI > HELIX_LOG_DEST > config > auto
    log_config.target = parse_log_target(resolve_log_setting(
        g_log_dest_cli, env_log_dest, m_config->get<std::string>("/log_dest", "auto")));

    // Resolve log file path: CLI > HELIX_LOG_FILE > config. A config-sourced
    // path must pass the same confinement the launcher applies to
    // HELIX_LOG_FILE (see helix::config_trust::log_path_allowed): settings.json
    // is web-writable and must not aim a root-written log at arbitrary files.
    // A refused path falls through to CLI/env, then the default location.
    std::string config_log_path = m_config->get<std::string>("/log_path", "");
    if (!config_log_path.empty() && !helix::config_trust::log_path_allowed(config_log_path)) {
        spdlog::warn("[Application] /log_path '{}' refused (must be a *.log file under /tmp, "
                     "/var/log or the install dir, no .. , not a symlink) - using the default",
                     config_log_path);
        config_log_path.clear();
    }
    log_config.file_path = resolve_log_setting(g_log_file_cli, env_log_file, config_log_path);

    init(log_config);

    // Set libhv log level from config (CLI -v flags don't affect libhv)
    spdlog::level::level_enum hv_spdlog_level = parse_level(config_level, spdlog::level::warn);
    hlog_set_level(libhv_level_for(hv_spdlog_level));

    return true;
}
