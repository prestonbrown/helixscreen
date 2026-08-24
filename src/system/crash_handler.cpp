// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/crash_handler.h"

#include "helix_version.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <string>
#include <unistd.h>

// mallinfo2 (glibc >= 2.33). Fallback to mallinfo (deprecated, 32-bit) for older
// systems like the AD5M (bullseye, glibc 2.31). Both are in <malloc.h>.
#if __has_include(<malloc.h>) && !defined(__APPLE__) && !defined(__ANDROID__)
#include <malloc.h>
#define HAVE_GLIBC_MALLINFO 1
#endif

// LVGL heap monitor
#include "core/lv_obj_tree.h" // lv_obj_get_name() for delete crumbs
#include "lv_init.h"          // lv_is_initialized() — guards the lv_mem_monitor read
#include "misc/lv_types.h"
#include "stdlib/lv_mem.h"

// AddressSanitizer opt-out for the raw stack readers further down. Both the
// frame-pointer walk and the linear stack scan deliberately read memory they
// do not own, which ASan cannot distinguish from a genuine overflow. GCC and
// Clang both spell the attribute no_sanitize_address.
#if defined(__has_attribute)
#if __has_attribute(no_sanitize_address)
#define HELIX_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address, noinline))
#endif
#endif
#ifndef HELIX_NO_SANITIZE_ADDRESS
#define HELIX_NO_SANITIZE_ADDRESS
#endif

// ucontext_t is needed for register state capture in the signal handler.
// On macOS, <sys/ucontext.h> is available without _XOPEN_SOURCE.
// On Linux, <ucontext.h> or <signal.h> provides it.
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

// backtrace() is available on glibc Linux and macOS. Missing on Android NDK
// (bionic) and musl libc (Creality K1/K2 MIPS).
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#elif defined(__ANDROID__)
#include <android/log.h>
#define HAVE_ANDROID_LOG 1
#endif

// dl_iterate_phdr() for discovering ELF load base (ASLR offset).
// glibc Linux only — macOS SDK doesn't ship link.h, Android NDK and musl
// libc don't provide dl_iterate_phdr.
#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
#include <link.h>
#define HAVE_DL_ITERATE_PHDR 1
#endif

// dlsym() for resolving glibc's private __abort_msg symbol at install time
// (used to capture the actual abort reason — "free(): invalid pointer", etc.
// — on SIGABRT). dlfcn.h is POSIX; -ldl is already linked on every platform.
#if !defined(_WIN32)
#include <dlfcn.h>
#define HAVE_DLSYM 1
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

/// One-shot guard: whichever path (write_exception_record OR the signal
/// handler) reaches the crash file first wins. The other skips. Without this,
/// a terminate_handler that writes the EXCEPTION record and then re-enters via
/// abort() races with the SIGABRT signal handler — both open the same path
/// with O_TRUNC and interleave, producing a garbled crash file (bundle
/// SQABQX95 / issue #950 had "name:EXCEPTI" + "veversion:" and parsed as
/// vunknown). Use sig_atomic_t so the SIGABRT-path read is async-signal-safe;
/// __atomic_compare_exchange_n is lock-free on every target we ship.
static volatile sig_atomic_t s_crash_file_written = 0;

/// Application start time, low 32 bits of CLOCK_MONOTONIC ms, captured at
/// install(). MUST NOT be wall-clock: RTC-less devices (every Pi, the AD5M,
/// the CC1) boot with a stale fake-hwclock time and NTP steps the clock
/// forward minutes later, so a wall-clock delta reports whatever the step was.
/// Bundle VP623KVU claimed `uptime:5035` for a process that had been alive
/// ~128 seconds — an 82-minute NTP correction, not an 84-minute session. That
/// sends triage looking for a slow leak when the real crash was 2 minutes in.
static uint32_t s_start_mono_ms = 0;

/// Async-signal-safe monotonic milliseconds (low 32 bits, wraps ~49.7 days).
/// clock_gettime(CLOCK_MONOTONIC) is on POSIX's async-signal-safe list, so this
/// is callable from the signal handler. Returns 0 only if the clock read fails.
static uint32_t monotonic_ms_now() noexcept {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    uint64_t ms =
        static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
    return static_cast<uint32_t>(ms);
}

/// Seconds since install(), from the monotonic clock. Wrap-safe.
static long uptime_seconds() noexcept {
    if (s_start_mono_ms == 0)
        return 0;
    return static_cast<long>(
        crash_handler::detail::elapsed_ms(s_start_mono_ms, monotonic_ms_now()) / 1000u);
}

/// ELF load base address (ASLR offset), discovered at install time
static uintptr_t s_load_base = 0;

/// Whether load_base detection has run (distinguishes "detected 0" from "not yet detected")
static bool s_load_base_detected = false;

/// Text segment bounds (for stack-scanned synthetic backtrace)
static uintptr_t s_text_start = 0;
static uintptr_t s_text_end = 0;

/// Pointer to the UpdateQueue's current callback tag (registered at init)
static volatile const char* const* s_callback_tag_ptr = nullptr;

/// UpdateQueue's ring of recently-completed callback tags (registered at init).
/// Slot `(*s_previous_tag_next - 1) % s_previous_tag_capacity` is newest;
/// walk backwards to emit queue_prev, queue_prev2, ...
/// `s_previous_tag_count_ring[i]` is the number of consecutive identical-tag
/// callbacks coalesced into slot `i` (1 = single, N>1 = repeats). May be null
/// when the producer didn't register counts — emitter falls back to no-suffix.
static volatile const char* const* s_previous_tag_ring = nullptr;
static volatile const uint32_t* s_previous_tag_count_ring = nullptr;
static unsigned int s_previous_tag_capacity = 0;
static volatile const unsigned int* s_previous_tag_next = nullptr;

/// Ring of recent ERROR-level log lines (registered by the logging sink). Each
/// entry is crash_handler::ERROR_LOG_ENTRY_LEN bytes, NUL-terminated; newest slot
/// is (*s_error_log_next - 1) % s_error_log_capacity. Storage is process-
/// lifetime (a singleton sink), so these pointers never dangle.
static const char* s_error_log_ring = nullptr;
static unsigned int s_error_log_capacity = 0;
static volatile const unsigned int* s_error_log_next = nullptr;

// =============================================================================
// Heap snapshot cache
// =============================================================================
//
// Refreshed from the main loop (non-signal-safe calls: open, read, mallinfo2,
// lv_mem_monitor). Signal handler reads these fields directly — pure memory
// load from static storage, async-signal-safe.

struct HeapSnapshot {
    uint32_t ts_ms;            // monotonic ms when captured (0 = never)
    size_t rss_kb;             // /proc/self/statm resident pages * pagesize
    size_t vsz_kb;             // virtual size
    size_t arena_kb;           // glibc: total heap arena
    size_t uordblks_kb;        // glibc: in-use bytes
    size_t fordblks_kb;        // glibc: free bytes in arena
    size_t hblkhd_kb;          // glibc: mmap'd bytes
    uint8_t lv_used_pct;       // LVGL: internal heap usage %
    uint8_t lv_frag_pct;       // LVGL: internal fragmentation %
    size_t lv_total_kb;        // LVGL: total heap size
    size_t lv_free_biggest_kb; // LVGL: largest free block
};
static HeapSnapshot s_heap_snapshot = {};

/// Innermost LVGL event under dispatch, updated from a hook inside
/// lv_obj_event.c::event_send_core. Fields are raw volatile for
/// signal-handler read without synchronization. LVGL is single-threaded so
/// writes are race-free on the producer side, but a signal could observe a
/// mismatched target/original_target/code triple (e.g. target from write N,
/// code from write N-1). In practice events come in bursts of the same code
/// so this is rarely meaningful, and we'd rather avoid synchronization
/// overhead on every LVGL dispatch than eliminate a rare noise case.
static volatile uintptr_t s_current_event_target = 0;
static volatile uintptr_t s_current_event_original_target = 0;
static volatile unsigned int s_current_event_code = 0;

/// Identity of the in-flight event target (class_p->name + spec_attr->name).
/// Populated by the LVGL event_send_core hook via
/// helix_crash_note_event_target_id(). Class name is a const char* into the
/// LVGL widget class table (.rodata, stable). Obj name is heap-allocated by
/// LVGL, so we copy bytes into a fixed buffer at hook time — the original
/// memory may be freed by crash time. Empty buffer means "no name".
///
/// Race: same as event target/code — a signal could observe a half-updated
/// triple. Acceptable for diagnostic purposes; a stale identity is still a
/// big lift over the bare hex pointer we have today.
static volatile const char* s_current_event_target_class = nullptr;
static char s_current_event_target_name[32] = {0};

// =============================================================================
// Breadcrumb ring buffer (activity context for crash diagnosis)
// =============================================================================

/// Single breadcrumb slot. Size = 72 bytes.
struct BreadcrumbSlot {
    /// Low 32 bits of ms-since-boot (CLOCK_MONOTONIC). 0 = empty/uninitialized.
    /// Stored last with release semantics so readers see a complete slot.
    uint32_t ts_ms;
    char category[8]; // null-terminated, truncated
    char subject[60]; // null-terminated, truncated
};

/// Ring size: 256 slots × 72 bytes = 18432 bytes static.
/// Doubled from 128 to absorb the per-step pstat_act / pstat_thm crumbs
/// added for cluster:pstat-async-delete (#906) without displacing the
/// async_d / sync_d crumbs from earlier in the same trace. Production
/// bundles since 2026-04-29 captured ≤80 crumbs each, but the new
/// instrumentation adds ~6 per Print Status reactivation, and tick
/// crumbs are still cheap to absorb at this size.
static constexpr size_t BREADCRUMB_RING_SIZE = 256;
static BreadcrumbSlot s_breadcrumb_ring[BREADCRUMB_RING_SIZE] = {};

/// Monotonically-incrementing write index. Modulo ring size gives slot.
/// Wraps at 2^32 — at 100 crumbs/sec that's ~500 days, acceptable.
static std::atomic<uint32_t> s_breadcrumb_idx{0};

/// Copy a C string into a fixed-size buffer, always null-terminating.
/// Returns number of bytes written (excluding terminator).
static size_t copy_truncated(char* dst, size_t dst_size, const char* src) noexcept {
    if (dst_size == 0)
        return 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    return i;
}

/// Saved previous signal actions for restoration
static struct sigaction s_old_sigsegv = {};
static struct sigaction s_old_sigabrt = {};
static struct sigaction s_old_sigbus = {};
static struct sigaction s_old_sigfpe = {};

/// Mirror of glibc's private `struct abort_msg_s`. `__libc_message()` mmaps one
/// of these, writes the abort reason ("free(): invalid pointer", "double free
/// or corruption", ...) into `msg`, and stores the STRUCT POINTER in
/// `__abort_msg` before calling `raise(SIGABRT)`.
///
/// The text therefore starts at `offsetof(msg)`, not at the pointer itself.
/// Reading `__abort_msg` as a plain `char*` lands on `size` — which is the mmap
/// length rounded up to a page, so its low byte is always 0x00 on little-endian
/// — and every real abort reason reads back as an empty string. That shipped
/// from v0.99.71 through v0.99.104: bundle VP623KVU (v0.99.100) was a glibc
/// heap-corruption SIGABRT whose reason existed in memory and was reported as
/// `abort_msg_state:empty`, leaving issue #960 with the exact diagnostic it had
/// been closed waiting for.
struct GlibcAbortMsg {
    unsigned int size; ///< mmap length, page-rounded (NOT a string length)
    char msg[1];       ///< NUL-terminated reason; real allocation is `size` bytes
};

/// Cached pointer to glibc's private `__abort_msg` symbol (i.e. a pointer to
/// the pointer). Resolved once at install() via dlsym (not async-signal-safe);
/// stays nullptr on non-glibc libcs — the field is then simply absent from the
/// crash file.
static GlibcAbortMsg** s_abort_msg_ptr = nullptr;

/// Reason text stashed by the C++ std::terminate handler (main.cpp) before it
/// does any fault-prone work (rethrow / exception::what() / writing the crash
/// file). If that handler re-faults and falls through to its bare `abort()`
/// re-entrance guard, glibc leaves `__abort_msg` empty — so the SIGABRT handler
/// emits this instead as `terminate_msg:`. Without it, a re-entrant terminate
/// produces a blank SIGABRT record with no reason at all (issue #987).
/// Newlines are stripped at set time so it stays a single crash-file line.
static char s_terminate_msg[256] = {};

void crash_handler::set_terminate_context(const char* what) noexcept {
    if (what == nullptr) {
        return;
    }
    // Bounded, allocation-free copy with newline strip — safe to call from a
    // std::terminate handler (which may run after the heap is corrupt).
    size_t n = 0;
    for (; n + 1 < sizeof(s_terminate_msg) && what[n] != '\0'; ++n) {
        char c = what[n];
        s_terminate_msg[n] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    s_terminate_msg[n] = '\0';
}

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
extern char _etext[];
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

/// Async-signal-safe: write a "key:<decimal>\n" line using the caller-provided
/// scratch buffer. Consolidates the common `safe_write` + `int_to_str` dance
/// used by the heap snapshot and event sections.
static void write_kv_long(int fd, const char* key, long value, char* num_buf, size_t num_buf_size) {
    safe_write(fd, key);
    safe_write(fd, int_to_str(num_buf, num_buf_size, value));
    safe_write(fd, "\n");
}

/// Async-signal-safe frame-pointer chain walker.
///
/// Fallback for the case where libgcc's DWARF-based `backtrace()` bails after
/// 0-2 frames on SIGABRT (signal trampoline has no CFI; glibc's `backtrace()`
/// can also itself crash when the heap is corrupted — we've seen this on Pi
/// v0.99.36). Every supported arch uses the same 2-word frame layout when
/// compiled with -fno-omit-frame-pointer:
///   [fp]            = saved_fp   (caller's frame pointer)
///   [fp + wordsize] = saved_lr   (return address into caller)
///
/// This matches AArch64/x86_64 unconditionally and modern GCC codegen for
/// ARM32 (both ARM and Thumb-2) with -fno-omit-frame-pointer. On ARM32 the
/// caller must pass r7 (Thumb) or r11 (ARM) in initial_fp — see the CPSR.T
/// check at the call site.
///
/// Bounds checks: fp must be word-aligned, monotonically increasing up the
/// stack, and inside [sp, sp + MAX_STACK_SIZE). On ARM32 we additionally
/// require saved_lr to fall inside the binary .text range — function
/// prologues without a standard FP (leaf libc routines, inline asm) can
/// plant a saved_fp value that walks into unrelated memory, and the .text
/// filter catches that. If any check fails we stop walking rather than
/// wander into bad memory.
/// Reads one machine word of raw stack memory at `base + index * word_size`.
///
/// Both stack walkers below read memory that belongs to other frames on
/// purpose: recovering return addresses when the DWARF unwinder cannot get
/// past the signal frame is the entire point of them. Those reads cross the
/// redzones AddressSanitizer inserts between stack frames, so an instrumented
/// build reports every one as a stack-buffer-underflow — ASan's own output
/// tags them with its "custom stack unwind mechanism" false-positive hint.
/// Eight such reports per run were what let the nightly ASan gate be written
/// off as noise.
///
/// Routing both readers through one opted-out accessor keeps ASan live over the
/// rest of the handler instead of blanket-disabling the file. noinline stops the
/// attribute from being lost when the helper is folded into an instrumented
/// caller.
///
/// Bounds are the caller's job and the two callers differ: fp_walk_backtrace
/// validates every address against [sp, sp + MAX_STACK_SIZE) before walking,
/// while the linear scan below simply reads a fixed 256 words up from SP and
/// can run off the end of the mapping. That is pre-existing behaviour and is
/// survivable where it runs — we are already inside a fatal signal handler —
/// but it is a real limit of the scan, not something this accessor fixes.
HELIX_NO_SANITIZE_ADDRESS static uintptr_t read_stack_word(uintptr_t base, size_t index,
                                                           uintptr_t word_size) {
    if (word_size == 8) {
        return static_cast<uintptr_t>(*(reinterpret_cast<const volatile uint64_t*>(base) + index));
    }
    return static_cast<uintptr_t>(*(reinterpret_cast<const volatile uint32_t*>(base) + index));
}

static int fp_walk_backtrace(int fd, uintptr_t initial_fp, uintptr_t sp, uintptr_t word_size) {
#if defined(__aarch64__) || defined(__x86_64__) || defined(__arm__)
    constexpr uintptr_t MAX_STACK_SIZE = 16 * 1024 * 1024;
    constexpr int MAX_FRAMES = 48;

    if (initial_fp == 0 || sp == 0 || word_size == 0)
        return 0;

    char hex_buf[32];
    int frames_emitted = 0;
    // Clamp to prevent 32-bit overflow: on ARM32 Linux, user stacks sit near
    // 0xff000000, so sp + 16MB silently wraps to ~0x00Cxxxxx and every
    // `fp + 8 > stack_hi` check spuriously trips, killing the walk on the
    // first iteration.
    uintptr_t stack_hi = sp + MAX_STACK_SIZE;
    if (stack_hi < sp) {
        stack_hi = ~static_cast<uintptr_t>(0);
    }
    uintptr_t fp = initial_fp;
    uintptr_t prev_fp = 0;

    for (int i = 0; i < MAX_FRAMES; ++i) {
        if ((fp & (word_size - 1)) != 0)
            break;
        if (fp < sp || fp + 2 * word_size > stack_hi)
            break;
        if (fp <= prev_fp && prev_fp != 0)
            break;

        uintptr_t saved_fp = read_stack_word(fp, 0, word_size);
        uintptr_t saved_lr = read_stack_word(fp, 1, word_size);

#if defined(__arm__)
        // ARM32 LRs from Thumb callers have bit 0 set (the Thumb-interwork
        // flag). Clear it for symbolization. The crashing function may be
        // "leaf-ish" — if it only pushed {r7} and not {r7, lr} (common for
        // [[noreturn]] trap functions) then [fp+4] contains stack garbage
        // rather than a saved_lr. Filter by .text range and skip (don't
        // break), so the saved_fp chain can still walk into the caller
        // whose frame does have a proper {r7, lr} layout.
        uintptr_t lr_addr = saved_lr & ~static_cast<uintptr_t>(1);
        bool lr_in_text = (s_text_start != 0 && s_text_end > s_text_start &&
                           lr_addr >= s_text_start && lr_addr < s_text_end);
        if (lr_in_text && lr_addr != 0) {
            safe_write(fd, "bt:");
            safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), lr_addr));
            safe_write(fd, "\n");
            ++frames_emitted;
        }
#else
        if (saved_lr != 0) {
            safe_write(fd, "bt:");
            safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), saved_lr));
            safe_write(fd, "\n");
            ++frames_emitted;
        }
#endif

        if (saved_fp == 0)
            break;
        prev_fp = fp;
        fp = saved_fp;
    }

    return frames_emitted;
#else
    (void)fd;
    (void)initial_fp;
    (void)sp;
    (void)word_size;
    return 0;
#endif
}

/// The signal handler itself -- async-signal-safe ONLY
static void crash_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    // If write_exception_record already laid down a crash file (e.g. the
    // C++ terminate_handler ran before falling through to abort()), don't
    // trample it — the EXCEPTION record is strictly more informative than
    // a bare SIGABRT signature. The CAS is async-signal-safe (lock-free).
    sig_atomic_t expected = 0;
    if (!__atomic_compare_exchange_n(&s_crash_file_written, &expected, 1, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        // Already written by the exception path. Skip the dump but still
        // re-raise with the default handler so the process exits with the
        // correct status — never leave the signal unhandled.
        struct sigaction sa_dfl = {};
        sa_dfl.sa_handler = SIG_DFL;
        sigaction(sig, &sa_dfl, nullptr);
        raise(sig);
        _exit(128 + sig);
    }

    // Open crash file (O_CREAT | O_WRONLY | O_TRUNC)
    // These are all async-signal-safe
    int fd = open(s_crash_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        // Primary path unwritable (chroot/overlay/PrivateTmp/missing parent
        // dir). Fall back to stderr — fd=2 is always open and gets captured
        // by both journald (systemd) and the watchdog's stderr pipe, so the
        // diagnostic still reaches a log even if no file lands on disk.
        // Marker prefix lets the watchdog/journal scan recognize it.
        safe_write(STDERR_FILENO, "\n=== HELIX_CRASH_DUMP ===\n");
        fd = STDERR_FILENO;
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

    // Write uptime (monotonic — immune to the NTP step, see s_start_mono_ms)
    long uptime = uptime_seconds();
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

    // SIGABRT-only: glibc abort reason from __abort_msg (resolved at install).
    // Distinguishes "free(): invalid pointer" / "double free or corruption" /
    // "munmap_chunk(): invalid pointer" / assertion failures — without this
    // every SIGABRT looked identical from the issue body (see issue #960).
    //
    // Always emit `abort_msg_state` for SIGABRT (issue #987) so a missing reason
    // is never ambiguous. A blank `abort_msg` previously had three indistinct
    // causes; the state tells them apart:
    //   unresolved — dlsym couldn't find __abort_msg (non-glibc, or stripped).
    //   empty      — glibc stored no message: a bare abort()/raise(SIGABRT) or a
    //                std::terminate fall-through, NOT a malloc/assert/fortify
    //                abort (those all populate __abort_msg). Look at terminate_msg.
    //   present    — a real glibc diagnostic follows on the `abort_msg` line.
    if (sig == SIGABRT) {
        if (s_abort_msg_ptr == nullptr) {
            safe_write(fd, "abort_msg_state:unresolved\n");
        } else {
            const GlibcAbortMsg* am = *s_abort_msg_ptr;
            // The reason lives at `am->msg`, NOT at `am` — see GlibcAbortMsg.
            // Bound the read by the struct's own allocation so a corrupt or
            // garbage `size` can't walk us off the mapping while we are already
            // inside a signal handler.
            const size_t msg_cap = (am != nullptr && am->size > sizeof(unsigned int))
                                       ? am->size - sizeof(unsigned int)
                                       : 0;
            const char* msg = (am == nullptr) ? nullptr : am->msg;
            if (msg == nullptr || msg_cap == 0 || msg[0] == '\0') {
                safe_write(fd, "abort_msg_state:empty\n");
            } else {
                // Bounded copy + newline strip so the value stays a single
                // line "key:value\n" in the crash file format.
                char abort_msg_buf[256];
                size_t n = 0;
                for (; n + 1 < sizeof(abort_msg_buf) && n < msg_cap && msg[n] != '\0'; ++n) {
                    char c = msg[n];
                    abort_msg_buf[n] = (c == '\n' || c == '\r') ? ' ' : c;
                }
                abort_msg_buf[n] = '\0';
                safe_write(fd, "abort_msg_state:present\n");
                safe_write(fd, "abort_msg:");
                safe_write(fd, abort_msg_buf);
                safe_write(fd, "\n");
            }
        }
    }

    // std::terminate reason stashed before main.cpp's terminate handler did any
    // fault-prone work. Only non-empty when terminate executed, so emit it for
    // any signal — it's the reason behind an otherwise-blank SIGABRT (#987).
    if (s_terminate_msg[0] != '\0') {
        safe_write(fd, "terminate_msg:");
        safe_write(fd, s_terminate_msg);
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
        // CPSR is needed downstream to tell which mode the crashing function
        // was running in (Thumb-2 vs ARM). The FP walker already consumes it
        // for r7-vs-r11 selection, but exposing it here lets the worker /
        // resolve-backtrace script confirm the choice.
        safe_write(fd, "reg_cpsr:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), uctx->uc_mcontext.arm_cpsr));
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
#elif defined(__mips__)
        safe_write(fd, "reg_pc:");
        safe_write(
            fd, ptr_to_hex(hex_buf, sizeof(hex_buf), static_cast<uintptr_t>(uctx->uc_mcontext.pc)));
        safe_write(fd, "\n");
        safe_write(fd, "reg_sp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[29])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_ra:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[31])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_fp:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[30])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_at:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[1])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_v0:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[2])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_v1:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[3])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_a0:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[4])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_a1:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[5])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_a2:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[6])));
        safe_write(fd, "\n");
        safe_write(fd, "reg_a3:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[7])));
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

    if (s_text_start != 0) {
        safe_write(fd, "text_start:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), s_text_start));
        safe_write(fd, "\n");
        safe_write(fd, "text_end:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), s_text_end));
        safe_write(fd, "\n");
    }

    // Write UpdateQueue callback tag if a queued callback was executing.
    // The volatile qualifier ensures the signal handler reads the current value,
    // not a cached one. We cast away volatile for safe_write — the pointer
    // target (a string literal) is in read-only memory and won't change.
    if (s_callback_tag_ptr) {
        const char* tag = const_cast<const char*>(*s_callback_tag_ptr);
        if (tag) {
            safe_write(fd, "queue_callback:");
            safe_write(fd, tag);
            safe_write(fd, "\n");
        }
    }
    // Walk the previous-tag ring newest→oldest. Emits queue_prev, queue_prev2,
    // ... for each non-null slot. Stops at the first null (ring not yet wrapped).
    if (s_previous_tag_ring && s_previous_tag_capacity > 0 && s_previous_tag_next) {
        unsigned int next = *s_previous_tag_next;
        // Labels for up to 8 slots; caller typically registers 4.
        static const char* const LABELS[] = {
            "queue_prev:",  "queue_prev2:", "queue_prev3:", "queue_prev4:",
            "queue_prev5:", "queue_prev6:", "queue_prev7:", "queue_prev8:",
        };
        const unsigned int label_count = sizeof(LABELS) / sizeof(LABELS[0]);
        const unsigned int limit =
            s_previous_tag_capacity < label_count ? s_previous_tag_capacity : label_count;
        for (unsigned int i = 0; i < limit; ++i) {
            // Reading from (next - 1 - i) handles wraparound via unsigned arithmetic.
            unsigned int idx = (next + s_previous_tag_capacity - 1 - i) % s_previous_tag_capacity;
            const char* prev = const_cast<const char*>(s_previous_tag_ring[idx]);
            if (!prev)
                break; // Unfilled slot — rest of the ring is empty.
            safe_write(fd, LABELS[i]);
            safe_write(fd, prev);
            // Append " (xN)" when the producer coalesced N>1 consecutive
            // identical tags into this slot. Skip the suffix for N<=1 to keep
            // single-shot tags clean. Reading the count is racy with the
            // producer mid-increment; worst case we report off-by-one, which
            // is acceptable for diagnostics.
            if (s_previous_tag_count_ring) {
                uint32_t count = s_previous_tag_count_ring[idx];
                if (count > 1) {
                    safe_write(fd, " (x");
                    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), static_cast<long>(count)));
                    safe_write(fd, ")");
                }
            }
            safe_write(fd, "\n");
        }
    }

    // Walk the recent-ERROR-log ring newest→oldest. Surfaces the last error
    // messages before the crash — the only reason clue when an abort set no
    // __abort_msg and skipped the std::terminate handler (issue #987). Entries
    // are pre-formatted + NUL-terminated by the sink, so the handler only reads.
    if (s_error_log_ring && s_error_log_capacity > 0 && s_error_log_next) {
        unsigned int next = *s_error_log_next;
        static const char* const ERR_LABELS[] = {
            "recent_error:",  "recent_error2:", "recent_error3:", "recent_error4:",
            "recent_error5:", "recent_error6:", "recent_error7:", "recent_error8:",
        };
        const unsigned int label_count = sizeof(ERR_LABELS) / sizeof(ERR_LABELS[0]);
        const unsigned int limit =
            s_error_log_capacity < label_count ? s_error_log_capacity : label_count;
        for (unsigned int i = 0; i < limit; ++i) {
            unsigned int idx = (next + s_error_log_capacity - 1 - i) % s_error_log_capacity;
            const char* entry =
                s_error_log_ring + static_cast<size_t>(idx) * crash_handler::ERROR_LOG_ENTRY_LEN;
            if (entry[0] == '\0')
                break; // Unfilled slot — rest of the ring is empty.
            safe_write(fd, ERR_LABELS[i]);
            safe_write(fd, entry);
            safe_write(fd, "\n");
        }
    }

    // Cached heap snapshot (refreshed from main loop; reads here are signal-safe).
    // Acquire pairs with the release-store of ts_ms in refresh_heap_snapshot().
    if (__atomic_load_n(&s_heap_snapshot.ts_ms, __ATOMIC_ACQUIRE) != 0) {
        const HeapSnapshot& h = s_heap_snapshot;
        // AGE, not the raw capture timestamp. Emitting ts_ms here meant a
        // snapshot taken 17ms before the crash was reported as "snapshot age
        // 144712ms" (bundle VP623KVU) — it was the monotonic-since-boot stamp,
        // and every consumer (crash_reporter's `age_ms`, the issue body, the
        // text summary) renders it as an age.
        //
        // Both raw stamps go out alongside the derived age, because the age
        // alone cannot be diagnosed when it comes out wrong: bundle ED2YC336
        // reported 120036989ms (33h) against a 9.8h uptime, which is impossible
        // — the snapshot cannot predate the process. Two causes produce that,
        // and only the raw pair distinguishes them: ts > now means the 32-bit
        // monotonic counter folded (49.7 days of uptime), ts << now means the
        // 10s refresh from the main loop genuinely stalled.
        //
        // One clock read, shared by the age and the emitted value, so the three
        // numbers are always mutually consistent.
        const uint32_t mono_now = monotonic_ms_now();
        write_kv_long(fd, "heap_snapshot_age_ms:",
                      static_cast<long>(crash_handler::detail::elapsed_ms(h.ts_ms, mono_now)),
                      num_buf, sizeof(num_buf));
        write_kv_long(fd, "heap_snapshot_ts_ms:", static_cast<long>(h.ts_ms), num_buf,
                      sizeof(num_buf));
        write_kv_long(fd, "heap_mono_ms_now:", static_cast<long>(mono_now), num_buf,
                      sizeof(num_buf));
        write_kv_long(fd, "heap_rss_kb:", static_cast<long>(h.rss_kb), num_buf, sizeof(num_buf));
        write_kv_long(fd, "heap_vsz_kb:", static_cast<long>(h.vsz_kb), num_buf, sizeof(num_buf));
        if (h.arena_kb != 0) {
            write_kv_long(fd, "heap_arena_kb:", static_cast<long>(h.arena_kb), num_buf,
                          sizeof(num_buf));
            write_kv_long(fd, "heap_used_kb:", static_cast<long>(h.uordblks_kb), num_buf,
                          sizeof(num_buf));
            write_kv_long(fd, "heap_free_kb:", static_cast<long>(h.fordblks_kb), num_buf,
                          sizeof(num_buf));
            write_kv_long(fd, "heap_mmap_kb:", static_cast<long>(h.hblkhd_kb), num_buf,
                          sizeof(num_buf));
        }
        if (h.lv_total_kb != 0) {
            write_kv_long(fd, "lv_heap_total_kb:", static_cast<long>(h.lv_total_kb), num_buf,
                          sizeof(num_buf));
            write_kv_long(fd, "lv_heap_used_pct:", static_cast<long>(h.lv_used_pct), num_buf,
                          sizeof(num_buf));
            write_kv_long(fd, "lv_heap_frag_pct:", static_cast<long>(h.lv_frag_pct), num_buf,
                          sizeof(num_buf));
            write_kv_long(fd, "lv_heap_free_biggest_kb:", static_cast<long>(h.lv_free_biggest_kb),
                          num_buf, sizeof(num_buf));
        }
    }

    // Current LVGL event under dispatch (updated by event_send_core hook)
    {
        uintptr_t tgt = s_current_event_target;
        uintptr_t orig = s_current_event_original_target;
        if (tgt != 0) {
            safe_write(fd, "event_target:");
            safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), tgt));
            safe_write(fd, "\n");
            if (orig != 0 && orig != tgt) {
                safe_write(fd, "event_original_target:");
                safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), orig));
                safe_write(fd, "\n");
            }
            write_kv_long(fd, "event_code:", static_cast<long>(s_current_event_code), num_buf,
                          sizeof(num_buf));
            // Widget identity (paired hook from patched event_send_core).
            // Bundle 3XNZQB2R: bare hex pointer wasn't enough to ID the click;
            // these two lines turn "0x23042e0" into "lv_button (recovery_dismiss_btn)".
            const char* cls = const_cast<const char*>(s_current_event_target_class);
            if (cls != nullptr) {
                safe_write(fd, "event_target_class:");
                safe_write(fd, cls);
                safe_write(fd, "\n");
            }
            if (s_current_event_target_name[0] != '\0') {
                safe_write(fd, "event_target_name:");
                safe_write(fd, s_current_event_target_name);
                safe_write(fd, "\n");
            }
        }
    }

    // Dump breadcrumb ring (oldest → newest). Torn writes are tolerated:
    // slot.ts_ms is stored last with release semantics, so a zero ts means the
    // slot is either empty or mid-write. In both cases we skip. Shared with
    // test code via the public dump_to_fd() entry point.
    crash_handler::breadcrumb::dump_to_fd(fd);

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
#elif defined(__mips__)
        safe_write(fd, "bt:");
        safe_write(
            fd, ptr_to_hex(hex_buf, sizeof(hex_buf), static_cast<uintptr_t>(uctx->uc_mcontext.pc)));
        safe_write(fd, "\n");
        safe_write(fd, "bt:");
        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf),
                                  static_cast<uintptr_t>(uctx->uc_mcontext.gregs[31])));
        safe_write(fd, "\n");
#endif
    }

    // Frame-pointer chain walk. Runs BEFORE backtrace() and the stack dump
    // because it's the most reliable fallback when libgcc's DWARF unwinder
    // bails at the signal trampoline. Bounds-checked; if the chain is
    // corrupt it stops early rather than wandering into bad memory.
    //
    // Arch-specific register selection:
    //   AArch64 / x86_64: fp = x29 / rbp, 8-byte words
    //   ARM32: Thumb-2 code uses r7; ARM-mode code uses r11 (fp). CPSR.T
    //          tells us which mode the crashing function ran in. Debian
    //          armhf userspace is predominantly Thumb-2 (Pi/SonicPad).
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
        uintptr_t fp_reg = 0;
        uintptr_t sp_reg = 0;
        uintptr_t word_size = 0;
#if defined(__APPLE__) && defined(__aarch64__)
        fp_reg = static_cast<uintptr_t>(uctx->uc_mcontext->__ss.__fp);
        sp_reg = static_cast<uintptr_t>(uctx->uc_mcontext->__ss.__sp);
        word_size = 8;
#elif defined(__APPLE__) && defined(__x86_64__)
        fp_reg = static_cast<uintptr_t>(uctx->uc_mcontext->__ss.__rbp);
        sp_reg = static_cast<uintptr_t>(uctx->uc_mcontext->__ss.__rsp);
        word_size = 8;
#elif defined(__aarch64__)
        fp_reg = static_cast<uintptr_t>(uctx->uc_mcontext.regs[29]);
        sp_reg = static_cast<uintptr_t>(uctx->uc_mcontext.sp);
        word_size = 8;
#elif defined(__x86_64__)
        fp_reg = static_cast<uintptr_t>(uctx->uc_mcontext.gregs[REG_RBP]);
        sp_reg = static_cast<uintptr_t>(uctx->uc_mcontext.gregs[REG_RSP]);
        word_size = 8;
#elif defined(__arm__)
        // CPSR bit 5 (T) = 1 when the crashing function was running in
        // Thumb state. GCC's -fno-omit-frame-pointer uses r7 as FP in
        // Thumb and r11 (arm_fp) in ARM mode. Both with 2-word frame layout.
        constexpr unsigned long CPSR_THUMB_BIT = 0x20;
        if (uctx->uc_mcontext.arm_cpsr & CPSR_THUMB_BIT) {
            fp_reg = uctx->uc_mcontext.arm_r7;
        } else {
            fp_reg = uctx->uc_mcontext.arm_fp;
        }
        sp_reg = uctx->uc_mcontext.arm_sp;
        word_size = 4;
#endif
        if (fp_reg != 0 && sp_reg != 0 && word_size != 0) {
            fp_walk_backtrace(fd, fp_reg, sp_reg, word_size);
        }
    }

    // Dump stack memory around SP and emit any words that fall within our .text
    // as synthetic backtrace entries. Essential fallback when backtrace() can't
    // unwind past the signal frame (common on static binaries and when libgcc's
    // DWARF unwinder bails at the signal tramp). Runs on every arch — backtrace()
    // may succeed, but the stack scan catches cases where it fails or returns
    // only 1-2 libc frames.
#ifdef __linux__
    if (ucontext) {
        const auto* uctx = static_cast<const ucontext_t*>(ucontext);
        uintptr_t sp = 0;
#if defined(__arm__)
        sp = uctx->uc_mcontext.arm_sp;
        constexpr int WORD_SIZE = 4;
#elif defined(__aarch64__)
        sp = static_cast<uintptr_t>(uctx->uc_mcontext.sp);
        constexpr int WORD_SIZE = 8;
#elif defined(__x86_64__)
        sp = static_cast<uintptr_t>(uctx->uc_mcontext.gregs[REG_RSP]);
        constexpr int WORD_SIZE = 8;
#elif defined(__mips__)
        sp = static_cast<uintptr_t>(uctx->uc_mcontext.gregs[29]);
        constexpr int WORD_SIZE = 4;
#else
        constexpr int WORD_SIZE = 0;
#endif
        if (sp != 0 && WORD_SIZE != 0) {
            safe_write(fd, "stack_base:");
            safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), sp));
            safe_write(fd, "\n");

            // Scan up to 256 words (1-2 KB of stack) for better coverage on
            // 64-bit archs with larger frames. Raw stk: dump kept to 128 words
            // to bound output size.
            constexpr int DUMP_WORDS = 128;
            constexpr int SCAN_WORDS = 256;

            auto read_word = [&](int i) -> uintptr_t {
                return read_stack_word(sp, static_cast<size_t>(i), WORD_SIZE);
            };

            for (int i = 0; i < DUMP_WORDS; ++i) {
                safe_write(fd, "stk:");
                safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), read_word(i)));
                safe_write(fd, "\n");
            }

            // Stack-scanned synthetic backtrace: emit any stack word that falls
            // within the binary .text segment as a bt: entry. Downstream resolver
            // symbolizes these — at least one frame is usually a real return addr.
            if (s_text_start != 0 && s_text_end > s_text_start) {
                safe_write(fd, "bt_source:stack_scan\n");
                for (int i = 0; i < SCAN_WORDS; ++i) {
                    uintptr_t word = read_word(i);
                    if (word >= s_text_start && word < s_text_end) {
                        safe_write(fd, "bt:");
                        safe_write(fd, ptr_to_hex(hex_buf, sizeof(hex_buf), word));
                        safe_write(fd, "\n");
                    }
                }
            }
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

    // backtrace() runs LAST because libgcc's DWARF unwinder can itself crash
    // when the heap is corrupted (we've seen signal handlers truncated at this
    // point on Pi v0.99.36 — bundle 7FYYQLVM / #827). Stack dump, maps, and
    // FP-walk frames above are written first so a nested crash here doesn't
    // lose them. Frames here may duplicate the FP-walk entries, which is fine —
    // downstream symbolization deduplicates.
#ifdef HAVE_BACKTRACE
    {
        void* frames[64];
        int frame_count = backtrace(frames, 64);
        for (int i = 0; i < frame_count; ++i) {
            safe_write(fd, "bt:");
            safe_write(
                fd, ptr_to_hex(hex_buf, sizeof(hex_buf), reinterpret_cast<uintptr_t>(frames[i])));
            safe_write(fd, "\n");
        }
    }
#elif defined(HAVE_ANDROID_LOG)
    __android_log_print(ANDROID_LOG_FATAL, "HelixScreen", "CRASH: signal %d", sig);
#endif

    // Don't close STDERR_FILENO — that's the stderr fallback path and other
    // signal handling / kernel default action still needs it open.
    if (fd != STDERR_FILENO) {
        close(fd);
    }

    // Re-raise with default handler so the process exits with the correct status
    // and generates a core dump if configured
    struct sigaction sa = {}; // Aggregate init (async-signal-safe, no memset)
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

void crash_handler::register_callback_tag_ptr(volatile const char* const* tag_ptr) {
    s_callback_tag_ptr = tag_ptr;
}

void crash_handler::register_previous_tag_ring(volatile const char* const* ring,
                                               volatile const uint32_t* count_ring,
                                               unsigned int capacity,
                                               volatile const unsigned int* next) {
    s_previous_tag_ring = ring;
    s_previous_tag_count_ring = count_ring;
    s_previous_tag_capacity = capacity;
    s_previous_tag_next = next;
}

void crash_handler::register_error_log_ring(const char* ring, unsigned int capacity,
                                            volatile const unsigned int* next) noexcept {
    s_error_log_ring = ring;
    s_error_log_capacity = capacity;
    s_error_log_next = next;
}

void crash_handler::set_current_event(const void* target, const void* original_target,
                                      unsigned int code) noexcept {
    s_current_event_target = reinterpret_cast<uintptr_t>(target);
    s_current_event_original_target = reinterpret_cast<uintptr_t>(original_target);
    s_current_event_code = code;
    // Identity is updated separately via helix_crash_note_event_target_id()
    // — patched LVGL calls both bridges per dispatch. Clear identity here on
    // the dispatch-end NULL clear so we don't carry stale strings.
    if (target == nullptr) {
        s_current_event_target_class = nullptr;
        s_current_event_target_name[0] = '\0';
    }
}

// C-ABI bridge for LVGL — see include/system/crash_handler.h
extern "C" void helix_crash_note_event(const void* target, const void* original_target,
                                       unsigned int code) {
    crash_handler::set_current_event(target, original_target, code);
}

// C-ABI bridge for the in-flight event target's identity. Called by patched
// LVGL event_send_core right after helix_crash_note_event(). Class name is
// stored by reference (LVGL class names live in static .rodata). Obj name is
// copied to a fixed buffer because LVGL frees the heap-allocated name when
// the widget is destroyed. NULL args clear the slots.
extern "C" void helix_crash_note_event_target_id(const char* class_name, const char* obj_name) {
    s_current_event_target_class = class_name;
    if (obj_name == nullptr) {
        s_current_event_target_name[0] = '\0';
        return;
    }
    // Manual copy — strncpy could read past the source if shorter than buf;
    // a hand-rolled loop bounds both source and destination.
    size_t i = 0;
    constexpr size_t BUF_SIZE = sizeof(s_current_event_target_name);
    while (i < BUF_SIZE - 1) {
        char c = obj_name[i];
        if (c == '\0')
            break;
        s_current_event_target_name[i] = c;
        ++i;
    }
    s_current_event_target_name[i] = '\0';
}

// C-ABI bridge: report the binary's text segment bounds, captured by
// crash_handler::install() via __executable_start / _etext. Returns 1 if
// bounds are valid (init has run), 0 otherwise. Used by the patched LVGL
// dispatch-cb-bounds gate.
extern "C" int helix_get_text_bounds(unsigned long* lo, unsigned long* hi) {
    if (s_text_start == 0 || s_text_end <= s_text_start) {
        return 0;
    }
    if (lo)
        *lo = static_cast<unsigned long>(s_text_start);
    if (hi)
        *hi = static_cast<unsigned long>(s_text_end);
    return 1;
}

// C-ABI bridges for LVGL — see include/system/crash_handler.h.
//
// Subject is "<class>:<name>" when the widget has an XML name, else just the
// class; detail is the pointer. The bare class name was not enough to act on:
// bundle 6F3QJLFG's final crumb before a heap abort was `async_d lv_obj
// 57241184`, and "some plain container was being deleted" does not identify a
// site — nearly every teardown crumb in that trace read the same. The name is
// what the XML author wrote, so it points straight at a file
// (prestonbrown/helixscreen#960).
//
// Read here rather than passed in from the patch: four patches already touch
// lv_obj_tree.c, so widening that hook means the pristine-file regeneration
// dance in patches/README.md. lv_obj_get_name() is a plain read of
// spec_attr->name with no allocation, and this file already links LVGL for
// lv_mem_monitor(). The name may be heap-owned by LVGL, so it is copied into
// the fixed slot immediately — which breadcrumb::note() does.
static void note_delete_crumb(const char* category, const void* obj,
                              const char* class_name) noexcept {
    const char* cls = class_name ? class_name : "?";

    // Guard the LVGL call: teardown can reach here after lv_deinit(), where
    // spec_attr is no longer safe to walk.
    const char* name = nullptr;
    if (obj && lv_is_initialized()) {
        name = lv_obj_get_name(static_cast<const lv_obj_t*>(obj));
    }

    if (!name || !name[0]) {
        crash_handler::breadcrumb::note(category, cls, reinterpret_cast<long>(obj));
        return;
    }

    // "<class>:<name>" — breadcrumb::note truncates to the slot's 60 bytes.
    char subject[60];
    size_t n = copy_truncated(subject, sizeof(subject), cls);
    if (n + 1 < sizeof(subject)) {
        subject[n++] = ':';
        copy_truncated(subject + n, sizeof(subject) - n, name);
    }
    crash_handler::breadcrumb::note(category, subject, reinterpret_cast<long>(obj));
}

extern "C" void helix_crash_note_async_del(const void* obj, const char* class_name) {
    note_delete_crumb("async_d", obj, class_name);
}

extern "C" void helix_crash_note_sync_del(const void* obj, const char* class_name) {
    note_delete_crumb("sync_d", obj, class_name);
}

uint32_t crash_handler::detail::elapsed_ms(uint32_t start_ms, uint32_t now_ms) noexcept {
    // Unsigned wraparound is well-defined and yields the correct delta across
    // the ~49.7-day fold of the low 32 bits of CLOCK_MONOTONIC. Do NOT "fix"
    // this with a now >= start guard — that reports 0 for any device up longer
    // than the fold, which is exactly the class of long-uptime device whose
    // crash durations matter most.
    return now_ms - start_ms;
}

void crash_handler::refresh_heap_snapshot() noexcept {
    // Timestamp first so a racing signal-handler reader either sees a stale
    // complete snapshot or the prior one — never a mix of old/new fields.
    HeapSnapshot snap = {};

    // /proc/self/statm: "size resident shared text lib data dt" in pages
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd >= 0) {
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            unsigned long size_pages = 0, rss_pages = 0;
            if (sscanf(buf, "%lu %lu", &size_pages, &rss_pages) == 2) {
                long page_kb = sysconf(_SC_PAGESIZE) / 1024;
                snap.vsz_kb = size_pages * static_cast<size_t>(page_kb);
                snap.rss_kb = rss_pages * static_cast<size_t>(page_kb);
            }
        }
    }

#ifdef HAVE_GLIBC_MALLINFO
    // Prefer mallinfo2 (glibc >= 2.33, 64-bit fields). Fall back to mallinfo on
    // older systems (AD5M runs bullseye with glibc 2.31).
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    snap.arena_kb = mi.arena / 1024;
    snap.uordblks_kb = mi.uordblks / 1024;
    snap.fordblks_kb = mi.fordblks / 1024;
    snap.hblkhd_kb = mi.hblkhd / 1024;
#elif defined(__GLIBC__)
    // mallinfo() uses int fields — truncates past 2 GiB, fine for our targets
    struct mallinfo mi = mallinfo();
    snap.arena_kb = static_cast<unsigned>(mi.arena) / 1024;
    snap.uordblks_kb = static_cast<unsigned>(mi.uordblks) / 1024;
    snap.fordblks_kb = static_cast<unsigned>(mi.fordblks) / 1024;
    snap.hblkhd_kb = static_cast<unsigned>(mi.hblkhd) / 1024;
#endif
#endif

    // Guard the LVGL read: this is noexcept diagnostic code that must never be
    // the thing that crashes, and lv_mem_monitor() walks allocator state that
    // does not exist before lv_init(). Production always has LVGL up by the
    // time the main loop calls this; tests and early-boot callers may not.
    if (lv_is_initialized()) {
        lv_mem_monitor_t mon = {};
        lv_mem_monitor(&mon);
        snap.lv_total_kb = mon.total_size / 1024;
        snap.lv_used_pct = mon.used_pct;
        snap.lv_frag_pct = mon.frag_pct;
        snap.lv_free_biggest_kb = mon.free_biggest_size / 1024;
    }

    uint32_t publish_ts = monotonic_ms_now();
    if (publish_ts == 0)
        publish_ts = 1;

    // Publish in two phases. The signal handler gates on ts_ms != 0, and
    // ts_ms is the leading field of HeapSnapshot — a naive `s_heap_snapshot =
    // snap` would copy the new ts_ms first, letting a signal interleaved
    // mid-copy read a new timestamp alongside stale trailing fields. Store
    // the body with ts_ms=0 first, then release-store the real ts_ms so
    // readers either see "no snapshot" or a fully-populated one.
    snap.ts_ms = 0;
    s_heap_snapshot = snap;
    __atomic_store_n(&s_heap_snapshot.ts_ms, publish_ts, __ATOMIC_RELEASE);
}

// -----------------------------------------------------------------------------
// Breadcrumb producers (single-producer: LVGL/UI thread only)
//
// note() is lock-free and signal-reader-safe (the signal handler dumping the
// ring observes consistent per-slot state via the ts_ms release-store), but
// it is NOT multi-producer safe: two concurrent writers that hash to the
// same slot (possible on ring wrap) would interleave byte-level writes into
// category/subject. All real producers today run on the LVGL thread —
// navigation, modals, the per-frame tick, and boot initialization — so the
// single-producer contract holds. Do not call from background threads or
// signal handlers.
// -----------------------------------------------------------------------------

namespace {

/// Breadcrumb timestamp: low 32 bits of ms-since-boot (CLOCK_MONOTONIC), never
/// zero because zero is reserved for "empty slot". These are absolute stamps on
/// the same clock as s_start_mono_ms — NOT offsets from process start — so
/// crumb times are comparable to `/proc/uptime` on the reporter's device but
/// are not directly comparable to the `uptime:` field. Wraps every ~49 days;
/// deltas within a single crash bundle stay meaningful either way.
static uint32_t breadcrumb_now_ms() noexcept {
    uint32_t v = monotonic_ms_now();
    return v == 0 ? 1 : v;
}

} // namespace

void crash_handler::breadcrumb::note(const char* category, const char* subject) noexcept {
    uint32_t i = s_breadcrumb_idx.fetch_add(1, std::memory_order_relaxed);
    BreadcrumbSlot& slot = s_breadcrumb_ring[i % BREADCRUMB_RING_SIZE];

    // Invalidate the slot while we overwrite it, so a concurrent reader sees
    // "empty" rather than a mix of old and new strings.
    __atomic_store_n(&slot.ts_ms, 0u, __ATOMIC_RELAXED);

    copy_truncated(slot.category, sizeof(slot.category), category);
    copy_truncated(slot.subject, sizeof(slot.subject), subject);

    // Publish with release so readers (signal handler) see a complete slot.
    __atomic_store_n(&slot.ts_ms, breadcrumb_now_ms(), __ATOMIC_RELEASE);
}

void crash_handler::breadcrumb::dump_to_fd(int fd) noexcept {
    char num_buf[32];
    uint32_t end = s_breadcrumb_idx.load(std::memory_order_acquire);
    uint32_t start = (end > BREADCRUMB_RING_SIZE) ? end - BREADCRUMB_RING_SIZE : 0;
    for (uint32_t i = start; i < end; ++i) {
        const BreadcrumbSlot& slot = s_breadcrumb_ring[i % BREADCRUMB_RING_SIZE];
        uint32_t ts = __atomic_load_n(&slot.ts_ms, __ATOMIC_ACQUIRE);
        if (ts == 0)
            continue;
        safe_write(fd, "crumb:");
        safe_write(fd, int_to_str(num_buf, sizeof(num_buf), static_cast<long>(ts)));
        safe_write(fd, " ");
        // category and subject are always null-terminated by copy_truncated.
        // An empty category is still written as an empty field between spaces
        // so the parser can split on whitespace deterministically.
        safe_write(fd, slot.category[0] ? slot.category : "-");
        safe_write(fd, " ");
        safe_write(fd, slot.subject[0] ? slot.subject : "-");
        safe_write(fd, "\n");
    }
}

void crash_handler::breadcrumb::note(const char* category, const char* subject,
                                     long detail) noexcept {
    uint32_t i = s_breadcrumb_idx.fetch_add(1, std::memory_order_relaxed);
    BreadcrumbSlot& slot = s_breadcrumb_ring[i % BREADCRUMB_RING_SIZE];

    __atomic_store_n(&slot.ts_ms, 0u, __ATOMIC_RELAXED);

    copy_truncated(slot.category, sizeof(slot.category), category);
    size_t n = copy_truncated(slot.subject, sizeof(slot.subject), subject);

    // Append " <detail>" if there's room. int_to_str writes right-aligned into
    // a small scratch buffer and returns a pointer into it.
    if (n + 2 < sizeof(slot.subject)) {
        slot.subject[n++] = ' ';
        char numbuf[24];
        char* num = int_to_str(numbuf, sizeof(numbuf), detail);
        while (*num && n + 1 < sizeof(slot.subject)) {
            slot.subject[n++] = *num++;
        }
        slot.subject[n] = '\0';
    }

    __atomic_store_n(&slot.ts_ms, breadcrumb_now_ms(), __ATOMIC_RELEASE);
}

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

    // Record start time for uptime calculation (monotonic, not wall-clock)
    s_start_mono_ms = monotonic_ms_now();

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

    // Compute text segment bounds for stack-scanned synthetic backtrace.
    // In the signal handler, stack words within [text_start, text_end) are
    // likely return addresses into the binary.
#if defined(__linux__)
    s_text_start = reinterpret_cast<uintptr_t>(__executable_start);
    s_text_end = reinterpret_cast<uintptr_t>(_etext);
#endif

    if (s_load_base != 0) {
        spdlog::debug("[CrashHandler] ELF load base: 0x{:x} (ASLR active)", s_load_base);
    } else {
        spdlog::debug("[CrashHandler] ELF load base: 0 (non-PIE or static without ASLR)");
    }
    spdlog::debug("[CrashHandler] Text segment: 0x{:x} - 0x{:x}", s_text_start, s_text_end);

    // Resolve glibc's __abort_msg for SIGABRT diagnostic capture. dlsym is NOT
    // async-signal-safe, so this runs once at install time and the handler just
    // reads the cached pointer. Symbol is GLIBC_PRIVATE-versioned, but dlsym
    // resolves it by name at runtime (the technique apport/coredumpctl/sentry
    // use). Stays null on musl/uclibc/Apple — field is then omitted.
#ifdef HAVE_DLSYM
    s_abort_msg_ptr = static_cast<GlibcAbortMsg**>(dlsym(RTLD_DEFAULT, "__abort_msg"));
    if (s_abort_msg_ptr != nullptr) {
        spdlog::debug("[CrashHandler] __abort_msg resolved — glibc abort reasons "
                      "will be captured in crash file");
    }
#endif

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

    // Seed the ring with a boot marker so crashes very early in startup still
    // show at least one breadcrumb — absence of any crumb then indicates the
    // ring wasn't populated rather than a pre-install crash.
    crash_handler::breadcrumb::note("boot", HELIX_VERSION);
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
    // Clear the first-writer-wins guard so a subsequent install()+write cycle
    // (test fixtures, soft-restart sequences) can lay down a fresh crash file.
    // In production this matters only for the soft-restart path; in tests it
    // prevents back-to-back write_exception_record cases from no-op'ing.
    __atomic_store_n(&s_crash_file_written, 0, __ATOMIC_SEQ_CST);
    spdlog::debug("[CrashHandler] Uninstalled signal handlers");
}

void crash_handler::write_exception_record(const char* what) noexcept {
    // Without install() we have no destination path. The pre-install C++ catch
    // blocks in main() are uninteresting in practice (Application's constructor
    // doesn't throw before install completes), so dropping is acceptable.
    if (s_crash_path[0] == '\0') {
        return;
    }

    // First-writer-wins. If a signal already laid down a crash file, don't
    // overwrite — the original signal carries fault_addr/registers we'd lose.
    sig_atomic_t expected = 0;
    if (!__atomic_compare_exchange_n(&s_crash_file_written, &expected, 1, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        return;
    }

    int fd = open(s_crash_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }

    char num_buf[32];

    // Schema-compatible with the SIGSEGV handler's output. signal:0 + name:EXCEPTION
    // tells the parser this is the exception path (no register dump, no fault_addr).
    safe_write(fd, "signal:0\n");
    safe_write(fd, "name:EXCEPTION\n");

    safe_write(fd, "version:");
    safe_write(fd, HELIX_VERSION);
    safe_write(fd, "\n");

    // time(nullptr) is async-signal-safe per POSIX.
    time_t now = time(nullptr);
    safe_write(fd, "timestamp:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), static_cast<long>(now)));
    safe_write(fd, "\n");

    long uptime = uptime_seconds();
    safe_write(fd, "uptime:");
    safe_write(fd, int_to_str(num_buf, sizeof(num_buf), uptime));
    safe_write(fd, "\n");

    if (what) {
        safe_write(fd, "exception:");
        safe_write(fd, what);
        safe_write(fd, "\n");
    }

    // Dump breadcrumbs — same context the signal handler emits, lets the
    // resolver localize the throw site even though backtrace() would be useless
    // here (the C++ unwinder has already collapsed the original throw frame).
    crash_handler::breadcrumb::dump_to_fd(fd);

    close(fd);
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
            } else if (key == "queue_callback") {
                result["queue_callback"] = value;
            } else if (key == "event_target") {
                result["event_target"] = value;
            } else if (key == "event_original_target") {
                result["event_original_target"] = value;
            } else if (key == "event_code") {
                try {
                    result["event_code"] = std::stoi(value);
                } catch (...) {
                    result["event_code"] = 0;
                }
            } else if (key == "event_target_class") {
                result["event_target_class"] = value;
            } else if (key == "event_target_name") {
                result["event_target_name"] = value;
            } else if (key.rfind("heap_", 0) == 0 || key.rfind("lv_heap_", 0) == 0) {
                // All heap snapshot fields are numeric kilobyte / percent values
                try {
                    result[key] = std::stol(value);
                } catch (...) {
                    result[key] = 0;
                }
            } else if (key == "text_start") {
                result["text_start"] = value;
            } else if (key == "text_end") {
                result["text_end"] = value;
            } else if (key == "bt_source") {
                result["bt_source"] = value;
            } else if (key == "exception") {
                result["exception"] = value;
            } else if (key == "abort_msg") {
                result["abort_msg"] = value;
            } else if (key == "abort_msg_state") {
                result["abort_msg_state"] = value;
            } else if (key == "terminate_msg") {
                result["terminate_msg"] = value;
            } else if (key.rfind("recent_error", 0) == 0) {
                // recent_error / recent_error2 / ... — distinct labeled keys
                // (newest first), collected into one ordered array.
                if (!result.contains("recent_errors")) {
                    result["recent_errors"] = json::array();
                }
                result["recent_errors"].push_back(value);
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
            } else if (key == "crumb") {
                // Breadcrumb line: "<ms> <category> <subject>"
                // Preserve as a raw string — downstream (crash_reporter, telemetry)
                // is free to split and pretty-print.
                if (!result.contains("breadcrumbs")) {
                    result["breadcrumbs"] = json::array();
                }
                result["breadcrumbs"].push_back(value);
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
    ofs << "crumb:1000 boot v0.0.0\n";
    ofs << "crumb:5200 nav home\n";
    ofs << "crumb:5210 xml home_card\n";
    ofs << "crumb:8300 modal confirm_print\n";
    ofs << "crumb:9100 nav status\n";
    ofs << "event_target:0x7fc0d2a8\n";
    ofs << "event_original_target:0x7fc0d300\n";
    ofs << "event_code:29\n"; /* LV_EVENT_REFR_EXT_DRAW_SIZE */
    ofs << "heap_snapshot_age_ms:8217\n";
    ofs << "heap_rss_kb:38400\n";
    ofs << "heap_vsz_kb:102400\n";
    ofs << "heap_arena_kb:40960\n";
    ofs << "heap_used_kb:38912\n";
    ofs << "heap_free_kb:2048\n";
    ofs << "heap_mmap_kb:512\n";
    ofs << "lv_heap_total_kb:512\n";
    ofs << "lv_heap_used_pct:88\n";
    ofs << "lv_heap_frag_pct:31\n";
    ofs << "lv_heap_free_biggest_kb:14\n";

    spdlog::info("[CrashHandler] Wrote mock crash file: {}", crash_file_path);
}

// =============================================================================
// trigger_test_crash() — deterministic SIGSEGV for verifying the unwind path.
// Each level is noinline + externally visible so the five stack frames appear
// in crash.txt. The top-level symbols are clustered in the static namespace
// below; they're only meant to be called via trigger_test_crash() and should
// not be reachable from normal code paths.
// =============================================================================

namespace {

// NOTE: these return int (not [[noreturn]]) so each function emits a full
// prologue + epilogue; without the epilogue padding they compress to 9-byte
// shims whose return addresses collide with the next level's entry point,
// making symbolization pick the wrong name. The return value is visibly used
// by the caller so the compiler can't tail-call-optimize the chain away.

[[gnu::noinline]] static int crash_level_4() {
    volatile int* p = nullptr;
    *p = 1; // SIGSEGV here — SEGV_MAPERR at 0x0
    return 4;
}

[[gnu::noinline]] static int crash_level_3() {
    int r = crash_level_4();
    return r + 3;
}

[[gnu::noinline]] static int crash_level_2() {
    int r = crash_level_3();
    return r + 2;
}

[[gnu::noinline]] static int crash_level_1() {
    int r = crash_level_2();
    return r + 1;
}

} // namespace

[[noreturn]] void crash_handler::trigger_test_crash() {
    spdlog::warn("[CrashHandler] HELIX_CRASH_TEST active — triggering SIGSEGV at crash_level_4()");
    // Read the result so the compiler can't elide the chain.
    volatile int sink = crash_level_1();
    (void)sink;
    // Unreachable; if control somehow returns, terminate to keep [[noreturn]] honest.
    _exit(99);
}
