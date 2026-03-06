// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/crash_handler.h"

#include "helix_version.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <string>
#include <unistd.h>

// ucontext_t is needed for register state capture in the signal handler.
// On macOS, <sys/ucontext.h> is available without _XOPEN_SOURCE.
// On Linux, <ucontext.h> or <signal.h> provides it.
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

// backtrace() is available on glibc (Linux) and macOS
#if (defined(__GLIBC__) || defined(__APPLE__)) && !defined(__ANDROID__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#elif defined(__ANDROID__)
#include <android/log.h>
#define HAVE_ANDROID_LOG 1
#endif

// dl_iterate_phdr() for discovering ELF load base (ASLR offset)
#if defined(__linux__)
#include <link.h>
#define HAVE_DL_ITERATE_PHDR 1
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

// =============================================================================
// Static buffers for async-signal-safe crash handler
// All data must be pre-allocated -- NO heap in the signal handler.
// =============================================================================

/// Maximum path length for the crash file
static constexpr size_t MAX_PATH_LEN = 512;

/// Pre-allocated buffer for the crash file path (copied at install time)
static char s_crash_path[MAX_PATH_LEN] = {};

/// Whether the crash handler is installed
static volatile sig_atomic_t s_installed = 0;

/// Application start time (for uptime calculation)
static time_t s_start_time = 0;

/// ELF load base address (ASLR offset), discovered at install time
static uintptr_t s_load_base = 0;

/// Whether load_base detection has run (distinguishes "detected 0" from "not yet detected")
static bool s_load_base_detected = false;

/// Saved previous signal actions for restoration
static struct sigaction s_old_sigsegv = {};
static struct sigaction s_old_sigabrt = {};
static struct sigaction s_old_sigbus = {};
static struct sigaction s_old_sigfpe = {};

// =============================================================================
// Async-signal-safe helpers
// These use ONLY functions from the POSIX async-signal-safe list.
// =============================================================================

// =============================================================================
// ELF load base discovery (called at install time, NOT in signal handler)
// =============================================================================

// __executable_start is a linker-defined symbol at the beginning of the ELF image.
// On static-PIE, its runtime address reflects the ASLR offset.
// On non-PIE, it's typically 0x10000 (ARM) or 0x400000 (x86_64).
#if defined(__linux__)
extern "C" {
extern char __executable_start[];
}
#endif

#ifdef HAVE_DL_ITERATE_PHDR
/// Callback for dl_iterate_phdr: find the main executable's load base.
/// The main executable has an empty dlpi_name.
static int find_load_base_cb(struct dl_phdr_info* info, size_t /*size*/, void* /*data*/) {
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
        s_load_base = static_cast<uintptr_t>(info->dlpi_addr);
        return 1; // stop iteration
    }
    return 0;
}
#endif

namespace {

/// Async-signal-safe: write a C string to a file descriptor
static void safe_write(int fd, const char* str) {
    if (!str)
        return;
    size_t len = 0;
    while (str[len] != '\0')
        ++len;
    // Ignore write() return; best effort in signal handler
    (void)write(fd, str, len);
}

/// Async-signal-safe: convert an integer to decimal string in-place
/// Returns pointer to the start of the number within buf
static char* int_to_str(char* buf, size_t buf_size, long value) {
    if (buf_size == 0)
        return buf;

    // Handle negative
    bool negative = (value < 0);
    unsigned long uval =
        negative ? static_cast<unsigned long>(-value) : static_cast<unsigned long>(value);

    // Build digits from the end
    char* end = buf + buf_size - 1;
    *end = '\0';
    char* p = end;

    if (uval == 0) {
        --p;
        *p = '0';
    } else {
        while (uval > 0 && p > buf) {
            --p;
            *p = '0' + static_cast<char>(uval % 10);
            uval /= 10;
        }
    }

    if (negative && p > buf) {
        --p;
        *p = '-';
    }

    return p;
}

/// Async-signal-safe: convert a pointer to hex string
/// Returns pointer to start of hex string within buf
static char* ptr_to_hex(char* buf, size_t buf_size, uintptr_t value) {
    if (buf_size < 3)
        return buf;

    static const char hex_chars[] = "0123456789abcdef";

    char* end = buf + buf_size - 1;
    *end = '\0';
    char* p = end;

    if (value == 0) {
        --p;
        *p = '0';
    } else {
        while (value > 0 && p > buf + 2) {
            --p;
            *p = hex_chars[value & 0xF];
            value >>= 4;
        }
    }

    // Prefix with 0x
    --p;
    *p = 'x';
    --p;
    *p = '0';

    return p;
}

/// Async-signal-safe: get the signal name string
static const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "UNKNOWN";
    }
}

/// Async-signal-safe: map signal + code to a human-readable name
static const char* get_fault_code_name(int sig, int code) {
    if (sig == SIGSEGV) {
        switch (code) {
        case SEGV_MAPERR:
            return "SEGV_MAPERR";
        case SEGV_ACCERR:
            return "SEGV_ACCERR";
        default:
            return "UNKNOWN";
        }
    } else if (sig == SIGBUS) {
        switch (code) {
        case BUS_ADRALN:
            return "BUS_ADRALN";
        case BUS_ADRERR:
            return "BUS_ADRERR";
        default:
            return "UNKNOWN";
        }
    } else if (sig == SIGFPE) {
        switch (code) {
        case FPE_INTDIV:
            return "FPE_INTDIV";
        case FPE_FLTDIV:
            return "FPE_FLTDIV";
        case FPE_FLTOVF:
            return "FPE_FLTOVF";
        default:
            return "UNKNOWN";
        }
    }
    return "UNKNOWN";
}

/// The signal handler itself -- async-signal-safe ONLY
static void crash_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    // Open crash file (O_CREAT | O_WRONLY | O_TRUNC)
    // These are all async-signal-safe
    int fd = open(s_crash_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        // Cannot write crash file; just exit
        _exit(128 + sig);
    }

    char num_buf[32];

    // Write signal number
    safe_write(fd, "signal:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), sig));
    safe_write(fd, "\n");

    // Write signal name
    safe_write(fd, "name:");
    safe_write(fd, signal_name(sig));
    safe_write(fd, "\n");

    // Write version
    safe_write(fd, "version:");
    safe_write(fd, HELIX_VERSION);
    safe_write(fd, "\n");

    // Write timestamp (time() is async-signal-safe per POSIX)
    time_t now = time(nullptr);
    safe_write(fd, "timestamp:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), static_cast<long>(now)));
    safe_write(fd, "\n");

    // Write uptime
    long uptime = 0;
    if (s_start_time > 0 && now >= s_start_time) {
        uptime = static_cast<long>(now - s_start_time);
    }
    safe_write(fd, "uptime:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), uptime));
    safe_write(fd, "\n");

    char hex_buf[32];

    // Write fault address (from siginfo_t)
    if (info) {
        safe_write(fd, "fault_addr:");
        safe_write(
            fd, ptr_to_hex(hex_buf, sizeof(hex_buf), reinterpret_cast<uintptr_t>(info->si_addr)));
        safe_write(fd, "\n");

        safe_write(fd, "fault_code:");
        safe_write(fd, int_to_str(num_buf, sizeof(num_buf), info->si_code));
        safe_write(fd, "\n");

        safe_write(fd, "fault_code_name:");
        safe_write(fd, get_fault_code_name(sig, info->si_code));
        safe_write(fd, "\n");
    }

    // Write register state from ucontext
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
#if defined(__APPLE__) && defined(__aarch64__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__pc));
        safe_write(fd, "\n");
        safe_write(fd, "reg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__sp));
        safe_write(fd, "\n");
        safe_write(fd, "reg_lr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__lr));
        safe_write(fd, "\n");
#elif defined(__APPLE__) && defined(__x86_64__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__rip));
        safe_write(fd, "\n");
        safe_write(fd, "reg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__rsp));
        safe_write(fd, "\n");
        safe_write(fd, "reg_bp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__rbp));
        safe_write(fd, "\n");
#elif defined(__arm__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_pc));
        safe_write(fd, "\n");
        safe_write(fd, "reg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_sp));
        safe_write(fd, "\n");
        safe_write(fd, "reg_lr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_lr));
        safe_write(fd, "\n");
        // ARM32 general-purpose registers for crash analysis
        safe_write(fd, "reg_r0:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r0));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r1:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r1));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r2:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r2));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r3:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r3));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r4:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r4));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r5:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r5));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r6:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r6));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r7:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r7));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r8:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r8));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r9:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r9));
        safe_write(fd, "\n");
        safe_write(fd, "reg_r10:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_r10));
        safe_write(fd, "\n");
        safe_write(fd, "reg_fp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_fp));
        safe_write(fd, "\n");
        safe_write(fd, "reg_ip:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_ip));
        safe_write(fd, "\n");
#elif defined(__aarch64__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.pc));
        safe_write(fd, "\n");
        safe_write(fd, "reg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.sp));
        safe_write(fd, "\n");
        safe_write(fd, "reg_lr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.regs[30]));
        safe_write(fd, "\n");
#elif defined(__x86_64__)
        safe_write(fd, "reg_pc:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RIP]));
        safe_write(fd, "\n");
        safe_write(fd, "reg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RSP]));
        safe_write(fd, "\n");
        safe_write(fd, "reg_bp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RBP]));
        safe_write(fd, "\n");
#endif
    }

    // Write ELF load base (for ASLR address resolution)
    // Always write when detection ran so resolvers can distinguish
    // "load_base is 0" (non-PIE / static-PIE with dl_iterate_phdr) from
    // "load_base not detected" (detection never ran)
    if (s_load_base_detected) {
        safe_write(fd, "load_base:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), s_load_base));
        safe_write(fd, "\n");
    }

    // Inject ucontext PC and LR as the first backtrace entries.
    // On ARM32 (static binary), backtrace() cannot unwind past the signal
    // frame — it only returns crash_handler + signal_restorer (useless).
    // The ucontext registers contain the actual crash location.
    // Writing them as bt: entries ensures downstream resolvers (resolve-backtrace.sh,
    // telemetry-crashes.py) symbolize them with load_base adjustment.
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
#if defined(__arm__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_pc));
        safe_write(fd, "\n");
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_lr));
        safe_write(fd, "\n");
#elif defined(__APPLE__) && defined(__aarch64__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__pc));
        safe_write(fd, "\n");
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__lr));
        safe_write(fd, "\n");
#elif defined(__aarch64__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.pc));
        safe_write(fd, "\n");
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.regs[30]));
        safe_write(fd, "\n");
#elif defined(__APPLE__) && defined(__x86_64__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext->__ss.__rip));
        safe_write(fd, "\n");
#elif defined(__x86_64__)
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.gregs[REG_RIP]));
        safe_write(fd, "\n");
#endif
    }

    // Write backtrace from unwinder (may duplicate ucontext frames above,
    // but provides additional frames when unwinding succeeds)
#ifdef HAVE_BACKTRACE
    void* frames[64];
    int frame_count = backtrace(frames, 64);
    for (int i = 0; i < frame_count; ++i) {
        safe_write(fd, "bt:");
        safe_write(fd,
                   ptr_to_hex(hex_buf, sizeof(hex_buf), reinterpret_cast<uintptr_t>(frames[i])));
        safe_write(fd, "\n");
    }
#elif defined(HAVE_ANDROID_LOG)
    __android_log_print(ANDROID_LOG_FATAL, "HelixScreen", "CRASH: signal %d", sig);
#endif

    // Dump stack memory around SP for ARM32 crash analysis.
    // On ARM32 static binaries, backtrace() is useless (can't unwind past signal frame).
    // Stack scanning lets us find return addresses back into the binary text segment.
#if defined(__arm__) && defined(__linux__)
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
        auto sp = uctx->uc_mcontext.arm_sp;
        // Dump 128 words (512 bytes) from SP upward — covers several stack frames
        safe_write(fd, "stack_base:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), sp));
        safe_write(fd, "\n");
        const auto* stack_ptr = reinterpret_cast<const uint32_t*>(sp);
        for (int i = 0; i < 128; ++i) {
            safe_write(fd, "stk:");
            safe_write(
                fd, ptr_to_hex(hex_buf, sizeof(hex_buf), static_cast<uintptr_t>(stack_ptr[i])));
            safe_write(fd, "\n");
        }
    }
#endif

    // Dump /proc/self/maps so we can distinguish binary vs shared library frames.
    // Not formally async-signal-safe, but /proc is a kernel pseudo-filesystem that
    // doesn't involve userspace state. Widely used in crash handlers (Chromium, Firefox).
#ifdef __linux__
    {
        int maps_fd = open("/proc/self/maps", O_RDONLY);
        if (maps_fd >= 0) {
            // Read in chunks and write executable mappings as "map:" lines.
            // /proc/self/maps is typically 2-8 KB. We use a static buffer to
            // avoid any heap allocation in the signal handler.
            static char maps_buf[8192];
            ssize_t n;
            while ((n = read(maps_fd, maps_buf, sizeof(maps_buf) - 1)) > 0) {
                maps_buf[n] = '\0';
                // Write each line prefixed with "map:"
                char* line_start = maps_buf;
                for (ssize_t i = 0; i < n; ++i) {
                    if (maps_buf[i] == '\n') {
                        maps_buf[i] = '\0';
                        safe_write(fd, "map:");
                        safe_write(fd, line_start);
                        safe_write(fd, "\n");
                        line_start = maps_buf + i + 1;
                    }
                }
                // Handle trailing partial line
                if (line_start < maps_buf + n) {
                    safe_write(fd, "map:");
                    safe_write(fd, line_start);
                    safe_write(fd, "\n");
                }
            }
            close(maps_fd);
        }
    }
#endif

    close(fd);

    // Re-raise with default handler so the process exits with the correct status
    // and generates a core dump if configured
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);

    // Fallback if raise() somehow returns
    _exit(128 + sig);
}

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

void crash_handler::install(const std::string& crash_file_path) {
    if (s_installed) {
        spdlog::debug("[CrashHandler] Already installed, skipping");
        return;
    }

    // Copy path into static buffer (no heap in signal handler)
    if (crash_file_path.size() >= MAX_PATH_LEN) {
        spdlog::error("[CrashHandler] Path too long ({} >= {}), truncating", crash_file_path.size(),
                      MAX_PATH_LEN);
    }
    size_t copy_len = std::min(crash_file_path.size(), MAX_PATH_LEN - 1);
    std::memcpy(s_crash_path, crash_file_path.c_str(), copy_len);
    s_crash_path[copy_len] = '\0';

    // Record start time for uptime calculation
    s_start_time = time(nullptr);

    // Discover ELF load base for ASLR address resolution
    // Must be done before signal handler runs (dl_iterate_phdr is NOT async-signal-safe)
#ifdef HAVE_DL_ITERATE_PHDR
    s_load_base = 0;
    dl_iterate_phdr(find_load_base_cb, nullptr);
#endif

#if defined(__linux__)
    // Fallback for static-PIE: dl_iterate_phdr returns 0 because there's no
    // dynamic linker. Use __executable_start (linker-defined symbol at the start
    // of the ELF image) to compute the actual ASLR load base.
    // On static-PIE ARM32, __executable_start's runtime address IS the load base
    // (file offset of __executable_start is 0x0 in the ELF).
    if (s_load_base == 0) {
        auto exec_start = reinterpret_cast<uintptr_t>(__executable_start);
        // Heuristic: if the runtime address is above 0x100000, ASLR is active.
        // Non-PIE ARM32 has __executable_start at 0x10000; non-PIE x86_64 at 0x400000.
        // ASLR'd static-PIE addresses are typically 0x5xxxxxxx+ (ARM32) or 0x5xxxxx+ (x86_64).
        if (exec_start > 0x100000) {
            s_load_base = exec_start;
        }
    }
#endif

    s_load_base_detected = true;
    if (s_load_base != 0) {
        spdlog::debug("[CrashHandler] ELF load base: 0x{:x} (ASLR active)", s_load_base);
    } else {
        spdlog::debug("[CrashHandler] ELF load base: 0 (non-PIE or static without ASLR)");
    }

    // Install signal handlers via sigaction (not signal())
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND: restore default after first signal (prevents recursive crash handler)
    // SA_SIGINFO: pass siginfo_t and ucontext to handler for fault/register capture
    sa.sa_flags = SA_RESETHAND | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, &s_old_sigsegv);
    sigaction(SIGABRT, &sa, &s_old_sigabrt);
    sigaction(SIGBUS, &sa, &s_old_sigbus);
    sigaction(SIGFPE, &sa, &s_old_sigfpe);

    s_installed = 1;
    spdlog::info("[CrashHandler] Installed signal handlers (crash file: {})", s_crash_path);
}

void crash_handler::uninstall() {
    if (!s_installed) {
        return;
    }

    // Restore previous handlers
    sigaction(SIGSEGV, &s_old_sigsegv, nullptr);
    sigaction(SIGABRT, &s_old_sigabrt, nullptr);
    sigaction(SIGBUS, &s_old_sigbus, nullptr);
    sigaction(SIGFPE, &s_old_sigfpe, nullptr);

    s_installed = 0;
    s_crash_path[0] = '\0';
    spdlog::debug("[CrashHandler] Uninstalled signal handlers");
}

bool crash_handler::has_crash_file(const std::string& crash_file_path) {
    std::error_code ec;
    return fs::exists(crash_file_path, ec) && fs::file_size(crash_file_path, ec) > 0;
}

nlohmann::json crash_handler::read_crash_file(const std::string& crash_file_path) {
    try {
        std::ifstream file(crash_file_path);
        if (!file.good()) {
            spdlog::warn("[CrashHandler] Cannot open crash file: {}", crash_file_path);
            return nullptr;
        }

        json result;
        json backtrace_arr = json::array();

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }

            auto colon_pos = line.find(':');
            if (colon_pos == std::string::npos || colon_pos == 0) {
                continue;
            }

            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            if (key == "signal") {
                try {
                    result["signal"] = std::stoi(value);
                } catch (...) {
                    result["signal"] = 0;
                }
            } else if (key == "name") {
                result["signal_name"] = value;
            } else if (key == "version") {
                result["app_version"] = value;
            } else if (key == "timestamp") {
                try {
                    // Convert unix timestamp to ISO 8601
                    time_t ts = static_cast<time_t>(std::stol(value));
                    struct tm utc_tm;
#ifdef _WIN32
                    gmtime_s(&utc_tm, &ts);
#else
                    gmtime_r(&ts, &utc_tm);
#endif
                    char buf[32];
                    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
                    result["timestamp"] = std::string(buf);
                } catch (...) {
                    // Use raw value as fallback
                    result["timestamp"] = value;
                }
            } else if (key == "uptime") {
                try {
                    result["uptime_sec"] = std::stol(value);
                } catch (...) {
                    result["uptime_sec"] = 0;
                }
            } else if (key == "fault_addr") {
                result["fault_addr"] = value;
            } else if (key == "fault_code") {
                try {
                    result["fault_code"] = std::stoi(value);
                } catch (...) {
                    result["fault_code"] = 0;
                }
            } else if (key == "fault_code_name") {
                result["fault_code_name"] = value;
            } else if (key == "reg_pc") {
                result["reg_pc"] = value;
            } else if (key == "reg_sp") {
                result["reg_sp"] = value;
            } else if (key == "reg_lr") {
                result["reg_lr"] = value;
            } else if (key == "reg_bp") {
                result["reg_bp"] = value;
            } else if (key == "load_base") {
                result["load_base"] = value;
            } else if (key == "exception") {
                result["exception"] = value;
            } else if (key == "bt") {
                backtrace_arr.push_back(value);
            } else if (key == "map") {
                // Memory map lines from /proc/self/maps
                if (!result.contains("memory_map")) {
                    result["memory_map"] = json::array();
                }
                result["memory_map"].push_back(value);
            } else if (key == "stack_base") {
                result["stack_base"] = value;
            } else if (key == "stk") {
                // Stack memory dump words
                if (!result.contains("stack_dump")) {
                    result["stack_dump"] = json::array();
                }
                result["stack_dump"].push_back(value);
            } else if (key.rfind("reg_", 0) == 0) {
                // Generic register capture (reg_r0, reg_fp, etc.)
                result[key] = value;
            }
        }

        if (!backtrace_arr.empty()) {
            result["backtrace"] = backtrace_arr;
        }

        // Validate minimum required fields
        if (!result.contains("signal") || !result.contains("signal_name")) {
            spdlog::warn("[CrashHandler] Crash file missing required fields");
            return nullptr;
        }

        spdlog::info("[CrashHandler] Read crash file: signal={} ({})", result.value("signal", 0),
                     result.value("signal_name", "unknown"));

        return result;
    } catch (const std::exception& e) {
        spdlog::error("[CrashHandler] Failed to parse crash file: {}", e.what());
        return nullptr;
    }
}

void crash_handler::remove_crash_file(const std::string& crash_file_path) {
    std::error_code ec;
    if (fs::remove(crash_file_path, ec)) {
        spdlog::debug("[CrashHandler] Removed crash file: {}", crash_file_path);
    } else if (ec) {
        spdlog::warn("[CrashHandler] Failed to remove crash file: {}", ec.message());
    }
}

void crash_handler::write_mock_crash_file(const std::string& crash_file_path) {
    std::ofstream ofs(crash_file_path);
    if (!ofs.good()) {
        spdlog::error("[CrashHandler] Cannot write mock crash file: {}", crash_file_path);
        return;
    }

    time_t now = time(nullptr);

    ofs << "signal:11\n";
    ofs << "name:SIGSEGV\n";
    ofs << "version:" << HELIX_VERSION << "\n";
    ofs << "timestamp:" << now << "\n";
    ofs << "uptime:1234\n";
    ofs << "fault_addr:0x00000000\n";
    ofs << "fault_code:1\n";
    ofs << "fault_code_name:SEGV_MAPERR\n";
    ofs << "reg_pc:0x00400abc\n";
    ofs << "reg_sp:0x7ffd12345678\n";
    ofs << "load_base:0x00400000\n";
    ofs << "bt:0x00400abc\n";
    ofs << "bt:0x00400def\n";
    ofs << "bt:0x00401234\n";
    ofs << "bt:0x00405678\n";
    ofs << "bt:0x00409abc\n";

    spdlog::info("[CrashHandler] Wrote mock crash file: {}", crash_file_path);
}
