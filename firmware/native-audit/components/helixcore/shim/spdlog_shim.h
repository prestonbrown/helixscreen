// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audit shim: the spdlog call surface the app uses, mapped onto esp_log.
//
// Formatting uses the repo's OWN bundled fmt (lib/spdlog/include/spdlog/fmt/bundled,
// header-only) — probed clean on Xtensa GCC 14. Using the exact same fmt the Linux
// build links means format-string/argument type checking has identical semantics:
// a file that fails here would fail against real spdlog too, and vice versa. The
// naive fall-back (stringify-nothing variadic templates) is kept behind
// HELIX_SHIM_NAIVE_FMT in case fmt ever regresses on a new toolchain, but it is
// NOT the default because it under-reports formatter errors.
//
// Surface covered (measured over src/ + include/, 2026-07-13):
//   spdlog::trace/debug/info/warn/error/critical/log   (~11.7k call sites)
//   spdlog::level::level_enum, set_level, get_level, should_log
//   spdlog::default_logger()->flush(), set_default_logger, logger (minimal)
//   spdlog::sink_ptr / sinks::sink (opaque; passed around by crash-reporter headers)
//   enable_backtrace / dump_backtrace / flush_on / set_pattern (no-ops)
// Deliberately NOT covered: sinks/*.h concrete sinks, spdlog::details — those live
// only in the log-backend setup files, which the audit wants to surface as bucket C
// (an ESP32 port replaces that backend wholesale with esp_log).

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
#if __has_include("sdkconfig.h") // CONFIG_HELIX_LOG_STRIP_DEBUG (helixscreen-esp32 only; unset here —
#include "sdkconfig.h"           // guarded so this frozen tree still compiles with no generated config)
#endif

namespace spdlog {

namespace level {
enum level_enum : int { trace = 0, debug = 1, info = 2, warn = 3, err = 4, critical = 5, off = 6, n_levels };
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
// Single-argument form: real spdlog also routes this through fmt's "{}" formatter,
// so requiring fmt::formatter<T> here matches upstream semantics exactly.
template <typename T>
inline void log(level::level_enum lvl, const T& msg) {
    helix_shim_log(static_cast<int>(lvl), fmt::format("{}", msg).c_str());
}

#else // HELIX_SHIM_NAIVE_FMT: compile-only, ignores arguments

template <typename S, typename... Args>
inline void log(level::level_enum lvl, const S& f, Args&&...) {
    helix_shim_log(static_cast<int>(lvl), "[unformatted]");
    (void)f;
}

#endif

#define HELIX_SHIM_LEVEL_FN(name, lvl)                                                            \
    template <typename... Args>                                                                    \
    inline void name(Args&&... args) {                                                             \
        ::spdlog::log(lvl, std::forward<Args>(args)...);                                           \
    }

#if CONFIG_HELIX_LOG_STRIP_DEBUG
// debug/trace never reach the console at the shipped INFO level, so the calls
// are dropped to empty inline templates instead of routing through
// ::spdlog::log. Call sites keep compiling unchanged (same argument list,
// same void return), but with nothing left in the body the compiler proves
// each format-string literal and its fmt::format_string/fmt::format
// instantiation unreachable and drops them, taking the format-string data and
// the per-call-site argument-packing code with them. The option lives in the
// helixscreen-esp32 tree's Kconfig.projbuild only, so it is always unset here
// and this branch never triggers in this throwaway audit tree.
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
    level::level_enum level() const { return level::info; }
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
inline level::level_enum get_level() { return level::info; }
inline bool should_log(level::level_enum) { return true; }
inline void enable_backtrace(size_t) {}
inline void dump_backtrace() {}
inline void flush_on(level::level_enum) {}
inline void set_pattern(const std::string&) {}

} // namespace spdlog
