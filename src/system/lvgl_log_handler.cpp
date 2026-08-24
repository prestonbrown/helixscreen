// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_log_handler.h"

#include "helix_lvgl_anomaly.h"
#include "runtime_config.h"
#include "subject_debug_registry.h"
#include "system/telemetry_manager.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <lvgl.h>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

// Stack trace support (macOS and Linux with glibc)
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
#define HAVE_STACK_TRACE 1
#include <cxxabi.h>   // For __cxa_demangle (symbol demangling)
#include <execinfo.h> // For backtrace() and backtrace_symbols()
#endif

namespace {

// When true, translation warnings are suppressed to trace level during init
static bool s_suppress_translation_warnings = false;

// First-occurrence cache for high-frequency LVGL warnings/errors (e.g. missing
// image assets). LVGL retries fs_open + image_decoder_get_info on every render
// frame that touches a broken image, which in field logs accounted for >90% of
// the volume. Dedupe by exact message text after stripping LVGL's leading
// "(timestamp, +delta)\t" prefix; first occurrence logs at the original level,
// subsequent identical messages drop to debug.
constexpr size_t LVGL_DEDUPE_MAX = 256;
static std::mutex s_dedupe_mutex;
static std::unordered_set<std::string> s_seen_messages;

/**
 * @brief Strip LVGL's "(timestamp, +delta)\t" prefix from a log message.
 *
 * LVGL prepends a timing header like "(33165.838, +7629213)\t" to every
 * log line. The numbers vary per call, so we strip them to compare bodies.
 */
std::string_view strip_lvgl_timestamp(std::string_view msg) {
    if (msg.empty() || msg.front() != '(')
        return msg;
    size_t close = msg.find(')');
    if (close == std::string_view::npos || close + 1 >= msg.size())
        return msg;
    size_t body = close + 1;
    while (body < msg.size() && (msg[body] == '\t' || msg[body] == ' '))
        ++body;
    return msg.substr(body);
}

/**
 * @brief Check if a message matches one of the per-frame retry patterns
 *        whose repeats should be silenced.
 */
bool is_high_frequency_retry(std::string_view msg) {
    return msg.find("fs_open: Could not open file:") != std::string_view::npos ||
           msg.find("image_decoder_get_info: File open failed") != std::string_view::npos ||
           msg.find("lv_draw_image: Couldn't get info about the image") != std::string_view::npos ||
           msg.find("lv_image_set_src: failed to get image info") != std::string_view::npos;
}

/**
 * @brief Record a message body and report whether it has been seen before.
 *
 * @return true if the message is a repeat (caller should drop to debug),
 *         false if it is the first occurrence (caller logs at original level).
 */
bool seen_before(const std::string& msg) {
    std::string key(strip_lvgl_timestamp(msg));
    std::lock_guard<std::mutex> lock(s_dedupe_mutex);
    if (s_seen_messages.size() >= LVGL_DEDUPE_MAX) {
        // Best-effort cap: clear and start over. 256 unique broken-asset
        // messages in one session would be extraordinary.
        s_seen_messages.clear();
    }
    auto [_, inserted] = s_seen_messages.insert(std::move(key));
    return !inserted;
}

#ifdef HAVE_STACK_TRACE
/**
 * @brief Demangle a C++ symbol name
 * @param mangled The mangled symbol name
 * @return Demangled name, or original if demangling fails
 */
std::string demangle_symbol(const char* mangled) {
    // Extract the mangled name from the symbol string
    // Format on macOS: "N  path  address  symbol + offset"
    // Format on Linux: "path(symbol+offset) [address]" or "path [address]"

    std::string result(mangled);

#if defined(__APPLE__)
    // macOS format: "0   helix-screen  0x...  _ZN5helix... + 123"
    // Find the mangled symbol (starts with _ and contains letters)
    const char* last_space = nullptr;

    // Find the last occurrence of a space followed by an underscore
    for (const char* p = mangled; *p != '\0'; ++p) {
        if (*p == ' ' && *(p + 1) == '_') {
            last_space = p + 1;
        }
    }

    if (last_space != nullptr) {
        // Find the end of the symbol (space before " + ")
        const char* sym_end = last_space;
        while (*sym_end != '\0' && *sym_end != ' ') {
            ++sym_end;
        }

        std::string symbol(last_space, sym_end);

        int status = 0;
        char* demangled = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
        if (status == 0 && demangled != nullptr) {
            // Replace the mangled symbol with demangled in the output
            size_t sym_pos = result.find(symbol);
            if (sym_pos != std::string::npos) {
                result.replace(sym_pos, symbol.length(), demangled);
            }
            free(demangled);
        }
    }
#elif defined(__linux__)
    // Linux format: "path(mangled+0x123) [0x...]"
    size_t paren_start = result.find('(');
    size_t plus_pos = result.find('+', paren_start);
    if (paren_start != std::string::npos && plus_pos != std::string::npos) {
        std::string symbol = result.substr(paren_start + 1, plus_pos - paren_start - 1);
        if (!symbol.empty()) {
            int status = 0;
            char* demangled = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
            if (status == 0 && demangled != nullptr) {
                result.replace(paren_start + 1, symbol.length(), demangled);
                free(demangled);
            }
        }
    }
#endif

    return result;
}

/**
 * @brief Print a stack trace to help debug subject type mismatches
 *
 * Captures the current call stack and logs it using spdlog.
 * Attempts to demangle C++ symbol names for readability.
 */
void print_stack_trace() {
    constexpr int MAX_FRAMES = 32;
    void* callstack[MAX_FRAMES];

    int frames = backtrace(callstack, MAX_FRAMES);
    char** symbols = backtrace_symbols(callstack, frames);

    if (symbols == nullptr) {
        spdlog::warn("  Stack trace: (unable to capture)");
        return;
    }

    spdlog::warn("  Stack trace:");
    // Skip first 3 frames: print_stack_trace, lvgl_log_callback, and LVGL internal
    for (int i = 3; i < frames; ++i) {
        std::string demangled = demangle_symbol(symbols[i]);
        spdlog::warn("    #{} {}", i - 3, demangled);
    }

    free(symbols);
}
#endif // HAVE_STACK_TRACE

/**
 * @brief Parse a pointer from a hex string like "0x7f8a1234"
 * @param hex_str Hex string starting with "0x"
 * @return Parsed pointer value, or nullptr on failure
 */
void* parse_pointer(const char* hex_str) {
    if (hex_str == nullptr || hex_str[0] != '0' || (hex_str[1] != 'x' && hex_str[1] != 'X')) {
        return nullptr;
    }
    char* end = nullptr;
    uintptr_t addr = std::strtoull(hex_str, &end, 16);
    if (end == hex_str) {
        return nullptr;
    }
    return reinterpret_cast<void*>(addr);
}

/**
 * @brief Check if the log message is a subject type mismatch warning
 *
 * LVGL warns with: "Subject type is not X"
 * After patching LVGL, we may also see: "(ptr=0x..., type=N)"
 *
 * @param buf The log message buffer
 * @return true if this is a subject type mismatch warning
 */
bool is_subject_type_mismatch(const char* buf) {
    return std::strstr(buf, "Subject type is not") != nullptr;
}

/**
 * @brief Try to extract pointer from enhanced LVGL log message
 *
 * Looks for pattern: "(ptr=0x...," or "(ptr=0x...)"
 *
 * @param buf The log message buffer
 * @return Pointer if found, nullptr otherwise
 */
lv_subject_t* extract_subject_pointer(const char* buf) {
    // Look for "(ptr=0x"
    const char* ptr_start = std::strstr(buf, "(ptr=0x");
    if (ptr_start == nullptr) {
        return nullptr;
    }

    // Move past "(ptr="
    ptr_start += 5; // Skip "(ptr="

    return static_cast<lv_subject_t*>(parse_pointer(ptr_start));
}

/**
 * @brief Extract expected type from "Subject type is not X" message
 * @param buf The log message buffer
 * @return Expected type name or empty string
 */
std::string extract_expected_type(const char* buf) {
    const char* pattern = "Subject type is not ";
    const char* start = std::strstr(buf, pattern);
    if (start == nullptr) {
        return "";
    }

    start += std::strlen(pattern);

    // Find end of type name (next space, newline, or end)
    const char* end = start;
    while (*end != '\0' && *end != ' ' && *end != '\n' && *end != '\r' && *end != '(') {
        ++end;
    }

    return std::string(start, end);
}

/**
 * @brief Log enhanced subject debug info when available
 * @param ptr Subject pointer from the warning
 */
void log_subject_debug_info(lv_subject_t* ptr) {
    if (ptr == nullptr) {
        return;
    }

    auto* info = SubjectDebugRegistry::instance().lookup(ptr);
    if (info != nullptr) {
        spdlog::warn("  -> Subject: \"{}\" ({}) registered at {}:{}", info->name,
                     SubjectDebugRegistry::type_name(info->type), info->file, info->line);
    } else {
        spdlog::warn("  -> Subject at {} not found in debug registry", static_cast<void*>(ptr));
    }
}

/**
 * @brief LVGL log callback that routes to spdlog
 *
 * Called by LVGL for all log messages. Routes to appropriate spdlog level
 * and provides enhanced debugging for subject type mismatch warnings.
 *
 * @param level LVGL log level
 * @param buf Log message buffer (null-terminated)
 */
void lvgl_log_callback(lv_log_level_t level, const char* buf) {
    // Strip trailing newline if present (spdlog adds its own)
    std::string msg(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
        msg.pop_back();
    }

    // Downgrade noisy scroll coordinate warnings to debug level
    // These fire when touch/scroll goes past screen edge - not actionable
    bool is_scroll_boundary_warning =
        (level == LV_LOG_LEVEL_WARN &&
         (msg.find("which is greater than") != std::string::npos ||
          msg.find("which is less than") != std::string::npos) &&
         (msg.find("ver. res") != std::string::npos || msg.find("hor. res") != std::string::npos));

    // Downgrade translation warnings to debug level
    // Expected when languages have incomplete translations - not actionable at runtime
    // "language is not found" is the no-pack-registered fallback, which English
    // takes on every boot by design (our tags ARE English — see
    // helix::ui::ensure_translation_loaded). LVGL emits it once per language.
    bool is_translation_warning = (level == LV_LOG_LEVEL_WARN &&
                                   (msg.find("language is missing from tag") != std::string::npos ||
                                    msg.find("tag is not found") != std::string::npos ||
                                    msg.find("language is not found") != std::string::npos));

    // Dedupe high-frequency retry messages (broken image assets, etc.) — first
    // occurrence at original level, repeats at debug. Decided up-front so the
    // WARN-path anomaly detection below still runs on the first hit.
    bool is_repeat_retry = (level == LV_LOG_LEVEL_WARN || level == LV_LOG_LEVEL_ERROR) &&
                           is_high_frequency_retry(msg) && seen_before(msg);

    // Route to appropriate spdlog level
    switch (level) {
    case LV_LOG_LEVEL_TRACE:
        spdlog::trace("[LVGL] {}", msg);
        break;
    case LV_LOG_LEVEL_INFO:
        spdlog::info("[LVGL] {}", msg);
        break;
    case LV_LOG_LEVEL_WARN:
        if (is_translation_warning && s_suppress_translation_warnings) {
            spdlog::trace("[LVGL] {}", msg);
        } else if (is_scroll_boundary_warning || is_translation_warning || is_repeat_retry) {
            spdlog::debug("[LVGL] {}", msg);
        } else {
            spdlog::warn("[LVGL] {}", msg);
            // Detect NULL guard hits from our safety patches. These indicate a
            // near-crash that was prevented — report via helix_lvgl_anomaly so
            // the event includes a backtrace (frame PCs). Distinct codes let us
            // tell failure modes apart in telemetry queries.
            const char* code = nullptr;
            if (msg.find("NULL dest_buf") != std::string::npos ||
                msg.find("draw_buf is NULL") != std::string::npos ||
                msg.find("NULL draw descriptor") != std::string::npos) {
                code = "null_dest_buf";
            } else if (msg.find("NULL font") != std::string::npos) {
                code = "null_font";
            } else if (msg.find("goto_xy returned NULL") != std::string::npos ||
                       msg.find("Skipping glyph draw") != std::string::npos) {
                code = "glyph_draw_skip";
            } else if (msg.find("subject is NULL") != std::string::npos) {
                code = "null_subject";
            } else if (msg.find("Subject type is not") != std::string::npos) {
                // XML binding wired wrong subject type — real bug in panel XML.
                code = "subject_type_mismatch";
            } else if (msg.find("coordinates out of range") != std::string::npos) {
                code = "coord_out_of_range";
            } else if (msg.find("duplicate schedule") != std::string::npos) {
                // Redundant with helix_lvgl_anomaly from lv_obj_delete_async,
                // but kept so the log + telemetry paths stay in sync if either
                // is ever missed.
                code = "obj_delete_async_dup";
            } else if (msg.find("skipping invalid obj") != std::string::npos) {
                code = "obj_delete_async_cb_invalid";
            } else if (msg.find("observer->subject is NULL") != std::string::npos) {
                // XML component scope tore down subject before the observing
                // widget's delete event fired — real ordering bug.
                code = "observer_subject_freed";
            } else if (msg.find("arc_value_changed_event_cb: subject is NULL") !=
                       std::string::npos) {
                // Arc event still wired to a subject that was freed (stale
                // user_data binding).
                code = "arc_event_null_subject";
            }
            if (code) {
                helix_lvgl_anomaly(code, msg.c_str());
            }
        }
        break;
    case LV_LOG_LEVEL_ERROR:
        if (is_repeat_retry) {
            spdlog::debug("[LVGL] {}", msg);
        } else {
            spdlog::error("[LVGL] {}", msg);
            // Detect heap corruption reports from lv_xml_get_font() and fire telemetry
            if (msg.find("HEAP_CORRUPTION") != std::string::npos) {
                TelemetryManager::instance().record_error("memory", "heap_corruption", msg);
            }
            // Detect render buffer failures from our safety patches
            if (msg.find("reshape failed") != std::string::npos) {
                TelemetryManager::instance().record_error("display", "render_failure", msg);
            }
        }
        break;
    case LV_LOG_LEVEL_USER:
        spdlog::info("[LVGL:USER] {}", msg);
        break;
    case LV_LOG_LEVEL_NONE:
    default:
        // Should not reach here, but log anyway
        spdlog::debug("[LVGL] {}", msg);
        break;
    }

    // Enhanced subject debugging for type mismatch warnings
    if (is_subject_type_mismatch(buf)) {
        std::string expected_type = extract_expected_type(buf);
        if (!expected_type.empty()) {
            spdlog::warn("  -> Expected type: {}", expected_type);
        }

        // Try to extract and lookup subject pointer
        lv_subject_t* ptr = extract_subject_pointer(buf);
        log_subject_debug_info(ptr);

#ifdef HAVE_STACK_TRACE
        // Print stack trace to help identify the code path that triggered the mismatch
        if (RuntimeConfig::debug_subjects()) {
            print_stack_trace();
        }
#endif
    }
}

} // namespace

namespace helix {
namespace logging {

void register_lvgl_log_handler() {
    lv_log_register_print_cb(lvgl_log_callback);
    spdlog::trace("[Logging] Registered custom LVGL log handler");
}

void set_suppress_translation_warnings(bool suppress) {
    s_suppress_translation_warnings = suppress;
}

} // namespace logging
} // namespace helix
