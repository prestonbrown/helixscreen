// SPDX-License-Identifier: GPL-3.0-or-later
//
// The spdlog call surface the repo headers use, mapped onto esp_log. Copied
// from the Plan 2 native-audit shim so the two firmware trees stay in lockstep;
// formatting routes through the repo's OWN bundled fmt (lib/spdlog/.../bundled,
// header-only, probed clean on Xtensa GCC) so format-string checking has the
// exact semantics of the Linux build.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef HELIX_SHIM_NAIVE_FMT
#define FMT_HEADER_ONLY 1
#include <spdlog/include/spdlog/fmt/bundled/format.h> // repo bundled fmt via -isystem <repo>/lib
#endif

#include "platform_stubs.h" // helix_shim_log()
#include "sdkconfig.h"      // CONFIG_HELIX_LOG_STRIP_DEBUG

namespace spdlog {

// Parity with real spdlog/common.h, which forward-declares the formatter base
// class: logging_init.h names std::unique_ptr<spdlog::formatter> in a
// declaration ESP32 TUs see but never call (logging_init.cpp is excluded).
class formatter;

namespace level {
enum level_enum : int {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    err = 4,
    critical = 5,
    off = 6,
    n_levels
};
}

namespace sinks {
class sink {
  public:
    virtual ~sink() = default;
};
} // namespace sinks
using sink_ptr = std::shared_ptr<sinks::sink>;

#ifndef HELIX_SHIM_NAIVE_FMT

template <typename... Args>
inline void log(level::level_enum lvl, fmt::format_string<Args...> f, Args&&... args) {
    helix_shim_log(static_cast<int>(lvl), fmt::format(f, std::forward<Args>(args)...).c_str());
}
template <typename T> inline void log(level::level_enum lvl, const T& msg) {
    helix_shim_log(static_cast<int>(lvl), fmt::format("{}", msg).c_str());
}

#else // HELIX_SHIM_NAIVE_FMT: compile-only, ignores arguments

template <typename S, typename... Args>
inline void log(level::level_enum lvl, const S& f, Args&&...) {
    helix_shim_log(static_cast<int>(lvl), "[unformatted]");
    (void)f;
}

#endif

#define HELIX_SHIM_LEVEL_FN(name, lvl)                                                             \
    template <typename... Args> inline void name(Args&&... args) {                                 \
        ::spdlog::log(lvl, std::forward<Args>(args)...);                                           \
    }

#if CONFIG_HELIX_LOG_STRIP_DEBUG
// debug/trace never reach the console at the shipped INFO level, so the calls
// are dropped to empty inline templates instead of routing through
// ::spdlog::log. Call sites keep compiling unchanged (same argument list,
// same void return), but with nothing left in the body the compiler proves
// each format-string literal and its fmt::format_string/fmt::format
// instantiation unreachable and drops them, taking the format-string data and
// the per-call-site argument-packing code with them.
template <typename... Args> inline void trace(Args&&...) {}
template <typename... Args> inline void debug(Args&&...) {}
#else
HELIX_SHIM_LEVEL_FN(trace, level::trace)
HELIX_SHIM_LEVEL_FN(debug, level::debug)
#endif
HELIX_SHIM_LEVEL_FN(info, level::info)
HELIX_SHIM_LEVEL_FN(warn, level::warn)
HELIX_SHIM_LEVEL_FN(error, level::err)
HELIX_SHIM_LEVEL_FN(critical, level::critical)
#undef HELIX_SHIM_LEVEL_FN

class logger {
  public:
    void flush() {}
    void set_level(level::level_enum) {}
    level::level_enum level() const {
        return level::info;
    }
    std::vector<sink_ptr>& sinks() {
        static std::vector<sink_ptr> s;
        return s;
    }
};

inline std::shared_ptr<logger> default_logger() {
    static auto l = std::make_shared<logger>();
    return l;
}
inline void set_default_logger(std::shared_ptr<logger>) {}
inline void set_level(level::level_enum) {}
inline level::level_enum get_level() {
    return level::info;
}
inline bool should_log(level::level_enum) {
    return true;
}
inline void enable_backtrace(size_t) {}
inline void dump_backtrace() {}
inline void flush_on(level::level_enum) {}
inline void set_pattern(const std::string&) {}

} // namespace spdlog
