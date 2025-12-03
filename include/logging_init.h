// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace helix {
namespace logging {

/**
 * @brief Log destination targets
 *
 * On Linux, the system will auto-detect the best available target:
 * - Journal (systemd) if /run/systemd/journal/socket exists
 * - Syslog as fallback
 * - File as final fallback
 *
 * On macOS, only Console and File are available.
 */
enum class LogTarget {
    Auto,    ///< Detect best available (default)
    Journal, ///< systemd journal (Linux only)
    Syslog,  ///< Traditional syslog (Linux only)
    File,    ///< Rotating file log
    Console  ///< Console only (disable system logging)
};

/**
 * @brief Logging configuration
 */
struct LogConfig {
    spdlog::level::level_enum level = spdlog::level::warn;
    bool enable_console = true;         ///< Always show console output
    LogTarget target = LogTarget::Auto; ///< System log destination
    std::string file_path;              ///< Override file path (empty = auto)
};

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

} // namespace logging
} // namespace helix
