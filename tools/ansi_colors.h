// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ansi_colors.h
 * @brief ANSI escape codes for terminal colors and formatting
 *
 * Portable ANSI color codes that work across macOS/Linux terminals.
 * Based on btop++ approach - using raw ANSI escape sequences.
 */

#pragma once

#include <string>
#include <unistd.h>

namespace ansi {

// Check if output is a TTY (for auto-disable when piped)
inline bool is_tty() {
    return isatty(STDOUT_FILENO);
}

// Colors
constexpr const char* RESET = "\033[0m";
constexpr const char* BOLD = "\033[1m";
constexpr const char* DIM = "\033[2m";
constexpr const char* ITALIC = "\033[3m";
constexpr const char* UNDERLINE = "\033[4m";

// Foreground colors
constexpr const char* BLACK = "\033[30m";
constexpr const char* RED = "\033[31m";
constexpr const char* GREEN = "\033[32m";
constexpr const char* YELLOW = "\033[33m";
constexpr const char* BLUE = "\033[34m";
constexpr const char* MAGENTA = "\033[35m";
constexpr const char* CYAN = "\033[36m";
constexpr const char* WHITE = "\033[37m";

// Bright foreground colors
constexpr const char* BRIGHT_BLACK = "\033[90m";
constexpr const char* BRIGHT_RED = "\033[91m";
constexpr const char* BRIGHT_GREEN = "\033[92m";
constexpr const char* BRIGHT_YELLOW = "\033[93m";
constexpr const char* BRIGHT_BLUE = "\033[94m";
constexpr const char* BRIGHT_MAGENTA = "\033[95m";
constexpr const char* BRIGHT_CYAN = "\033[96m";
constexpr const char* BRIGHT_WHITE = "\033[97m";

// Semantic colors (btop-style)
inline std::string success(const std::string& text) {
    return std::string(BRIGHT_GREEN) + text + RESET;
}

inline std::string error(const std::string& text) {
    return std::string(BRIGHT_RED) + text + RESET;
}

inline std::string warning(const std::string& text) {
    return std::string(BRIGHT_YELLOW) + text + RESET;
}

inline std::string info(const std::string& text) {
    return std::string(BRIGHT_CYAN) + text + RESET;
}

inline std::string dim(const std::string& text) {
    return std::string(DIM) + text + RESET;
}

inline std::string header(const std::string& text) {
    return std::string(BOLD) + std::string(BRIGHT_CYAN) + text + RESET;
}

inline std::string section(const std::string& text) {
    return std::string(BOLD) + std::string(CYAN) + text + RESET;
}

inline std::string key(const std::string& text) {
    return std::string(BRIGHT_BLUE) + text + RESET;
}

inline std::string value(const std::string& text) {
    return std::string(WHITE) + text + RESET;
}

} // namespace ansi
