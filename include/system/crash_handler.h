// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file crash_handler.h
 * @brief Async-signal-safe crash handler for telemetry
 *
 * Installs signal handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE.
 * On crash, writes a minimal crash file to disk using only
 * async-signal-safe functions (open, write, close, _exit).
 * NO heap allocation, NO mutex, NO spdlog in the signal handler.
 *
 * On next startup, TelemetryManager reads the crash file and
 * enqueues it as a telemetry event.
 *
 * Crash file format (line-oriented text, easy to parse):
 * @code
 * signal:11
 * name:SIGSEGV
 * version:0.9.6
 * timestamp:1707350400
 * uptime:3600
 * bt:0x0040abcd
 * bt:0x0040ef01
 * @endcode
 */

#include <cstdint>
#include <string>

#include "hv/json.hpp"

namespace crash_handler {

/// Fixed width (bytes, incl. NUL) of one captured recent-error-log entry.
inline constexpr unsigned int ERROR_LOG_ENTRY_LEN = 256;
/// Number of recent ERROR-level log lines retained for crash diagnostics.
inline constexpr unsigned int ERROR_LOG_RING_CAPACITY = 4;

/**
 * @brief Install crash signal handlers
 *
 * Registers handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE via sigaction().
 * The path is copied into a static buffer so the signal handler can use it
 * without heap allocation.
 *
 * @param crash_file_path Path where crash data will be written on crash
 */
void install(const std::string& crash_file_path);

/**
 * @brief Uninstall crash signal handlers (restore defaults)
 *
 * Restores the default signal disposition for all handled signals.
 */
void uninstall();

/**
 * @brief Check if a crash file exists from a previous crash
 * @param crash_file_path Path to check
 * @return true if a crash file was found
 */
bool has_crash_file(const std::string& crash_file_path);

/**
 * @brief Read and parse a crash file into structured data
 *
 * Parses the line-oriented crash file and returns a JSON object
 * suitable for TelemetryManager's event queue. Returns null JSON
 * on parse failure.
 *
 * @param crash_file_path Path to the crash file
 * @return JSON object with crash event data, or null on failure
 */
nlohmann::json read_crash_file(const std::string& crash_file_path);

/**
 * @brief Delete the crash file after it has been processed
 * @param crash_file_path Path to the crash file to remove
 */
void remove_crash_file(const std::string& crash_file_path);

/**
 * @brief Write a synthetic crash file for testing the crash reporter UI
 *
 * Creates a realistic-looking crash.txt at the given path with a fake
 * SIGSEGV, current version, and sample backtrace addresses.
 *
 * @param crash_file_path Path where the mock crash file will be written
 */
void write_mock_crash_file(const std::string& crash_file_path);

/**
 * @brief Write a minimal crash record for an uncaught C++ exception
 *
 * Async-signal-safe by construction — uses only the static crash path buffer
 * and primitives shared with the signal handler (no heap allocation, no
 * dprintf, no backtrace, no dl_iterate_phdr). Intended to be called from
 * `std::terminate` handlers and top-level catch blocks where the program
 * is already in an unrecoverable state and any heap touch could re-crash
 * (this was observed on AD5M v0.99.38-0.99.41 — the prior heap-using
 * implementation in main.cpp showed up in its own crash backtrace).
 *
 * Requires `crash_handler::install()` to have been called previously so the
 * destination path is known. If not installed, the call is a no-op.
 *
 * @param what Optional exception message (e.what()), may be nullptr
 */
void write_exception_record(const char* what) noexcept;

/**
 * @brief Stash the std::terminate reason so a re-entrant terminate still
 *        surfaces it.
 *
 * Call from a `std::terminate` handler BEFORE any fault-prone work (rethrow,
 * `exception::what()`, writing the crash file). If that handler re-faults and
 * falls through to a bare `abort()`, glibc leaves `__abort_msg` empty and the
 * SIGABRT signal handler would otherwise have no reason to report — it instead
 * emits this text as `terminate_msg:` in the crash record (issue #987).
 *
 * Bounded copy into a static buffer, newline-stripped, no allocation — safe to
 * call when the heap may already be corrupt. nullptr is a no-op.
 *
 * @param what Exception message (e.what()) or a placeholder; may be nullptr.
 */
void set_terminate_context(const char* what) noexcept;

/**
 * @brief Intentionally SIGSEGV through a deep call chain, for verifying the
 *        signal handler's unwind path on real hardware.
 *
 * Call chain (distinct function names, `__attribute__((noinline))`):
 *   trigger_test_crash() -> crash_level_1() -> crash_level_2() ->
 *   crash_level_3() -> crash_level_4() -> *(volatile int*)0 = 1;
 *
 * Use via `HELIX_CRASH_TEST=1` env var. The generated crash.txt should
 * resolve a live FP-walk chain that includes all five symbols. Absence of
 * any of them means the walker broke somewhere. Only intended for
 * signal-handler development — never wire this into production paths.
 */
[[noreturn]] void trigger_test_crash();

/**
 * @brief Register a pointer to the current callback tag
 *
 * The UpdateQueue stores the tag of the currently executing callback in a
 * volatile pointer. Registering it here lets the crash signal handler read
 * and write it to crash.txt without depending on ui_update_queue.h.
 *
 * @param tag_ptr Pointer to the volatile const char* that holds the current tag
 */
void register_callback_tag_ptr(volatile const char* const* tag_ptr);

/**
 * @brief Register a ring of recently-completed callback tags
 *
 * Lets the signal handler emit `queue_prev:` / `queue_prev2:` / ... lines
 * naming the most recently completed callbacks when the crash happens
 * AFTER a queued callback finished. Useful for pinning heap corruption
 * that detonates on the next main-thread malloc, or cascades a few
 * callbacks later.
 *
 * The ring is walked newest→oldest starting at `(*next - 1) % capacity`.
 * Producer writes slot `*next % capacity`, then stores `*next + 1` back.
 * All four parameters are read by the signal handler, so their storage
 * must outlive any possible crash (typically static).
 *
 * `count_ring[i]` records how many consecutive identical-tag callbacks
 * share slot `i`. The producer coalesces repeats by bumping the count
 * instead of advancing — high-frequency callbacks (per-WebSocket-tick
 * temperature updates, etc.) collapse into a single slot tagged
 * `tag (xN)`, preserving runway for distinct prior callbacks. Pass
 * `nullptr` for `count_ring` to disable counts (legacy behavior).
 *
 * @param ring       Pointer to the tag-pointer array (string literals)
 * @param count_ring Parallel array of repeat counts (or nullptr to disable)
 * @param capacity   Number of slots in the ring (power-of-two recommended)
 * @param next       Pointer to the write-index counter (monotonically increasing)
 */
void register_previous_tag_ring(volatile const char* const* ring,
                                volatile const uint32_t* count_ring, unsigned int capacity,
                                volatile const unsigned int* next);

/**
 * @brief Register a ring of the most recent ERROR-level log messages
 *
 * The signal handler emits these as `recent_error:` / `recent_error2:` / ...
 * (newest→oldest) on ANY crash. This is the last-ditch reason when an abort
 * sets no `__abort_msg` and doesn't pass through the std::terminate handler —
 * e.g. a bare `abort()`/`raise(SIGABRT)` from a dependency (issue #987).
 *
 * `ring` is `capacity * ERROR_LOG_ENTRY_LEN` contiguous bytes; each entry is a
 * NUL-terminated string. The producer fills slot `*next % capacity`, NUL-
 * terminates it, then advances `*next`. All storage must outlive any possible
 * crash (process-lifetime / static). Pass `nullptr` ring to disable.
 *
 * @param ring     Base of the entry buffer (capacity × ERROR_LOG_ENTRY_LEN bytes)
 * @param capacity Number of entry slots in the ring
 * @param next     Pointer to the monotonically-increasing write counter
 */
void register_error_log_ring(const char* ring, unsigned int capacity,
                             volatile const unsigned int* next) noexcept;

/**
 * @brief Record the LVGL event currently being dispatched
 *
 * Maintains a volatile record of the innermost lv_obj_t under dispatch.
 * Patched into LVGL's event_send_core() via patches/lvgl_event_crash_hook
 * so every event dispatch (including internal ones like REFR_EXT_DRAW_SIZE
 * and LAYOUT_CHANGED) updates this slot. On crash, the signal handler
 * dumps the last-seen target as event_target and event_code lines.
 *
 * This gives crashes in LVGL layout/draw code the pointer + event code of
 * the widget that was being processed — enough to cross-reference with
 * breadcrumbs (e.g. last XML component instantiated) and name the widget.
 *
 * Signal-safe: two volatile writes, no locks, no allocations.
 */
void set_current_event(const void* target, const void* original_target, unsigned int code) noexcept;

/**
 * @brief Refresh the cached heap snapshot
 *
 * Call from the main loop every ~10s. Captures /proc/self/statm RSS,
 * glibc mallinfo (when available), and lv_mem_monitor() into a static
 * buffer. On crash, the signal handler dumps the most recent snapshot
 * without calling any of these non-async-signal-safe functions.
 *
 * Cheap enough (~1 µs) to call every frame if desired, but 10s is plenty.
 */
void refresh_heap_snapshot() noexcept;

namespace detail {

/**
 * @brief Wrap-safe elapsed milliseconds between two 32-bit monotonic samples.
 *
 * All crash-record durations (process uptime, heap-snapshot age) are derived
 * from the low 32 bits of CLOCK_MONOTONIC, which wraps every ~49.7 days.
 * Unsigned subtraction gives the correct delta across the wrap; a signed or
 * clamped subtraction would report 0 (or a ~49-day value) on any device up
 * that long. Exposed for test only — callers inside the handler use it via
 * the monotonic helpers.
 *
 * @param start_ms Earlier monotonic sample.
 * @param now_ms   Later monotonic sample.
 * @return now_ms - start_ms, modulo 2^32.
 */
uint32_t elapsed_ms(uint32_t start_ms, uint32_t now_ms) noexcept;

} // namespace detail

/**
 * @brief Breadcrumb ring buffer for crash-time activity context
 *
 * Records short, structured events into a fixed-size ring. On crash, the last
 * ~64 entries are dumped to crash.txt as `crumb:<ms> <category> <subject>`
 * lines. Producers can be called from any thread and from signal handlers
 * (though not intended to be the latter).
 *
 * Storage is entirely static (no heap). Writers are lock-free; readers
 * tolerate torn writes by requiring a complete timestamp.
 *
 * Use for high-signal transitions: panel navigation, modal show/hide, XML
 * component instantiation, style add/remove. Avoid for hot-path events.
 */
namespace breadcrumb {

/**
 * @brief Record an activity breadcrumb
 *
 * @param category Short tag (<= 7 chars): "nav", "modal", "xml", "style",
 *                 "frame", "evt". Truncated if longer.
 * @param subject  Object/panel/component name (<= 59 chars). Truncated.
 *
 * Both pointers may be nullptr (treated as empty). Lock-free, no heap, no
 * syscalls.
 *
 * THREADING CONTRACT: single-producer (LVGL/UI thread only). Do NOT call
 * from signal handlers or from background threads. Two concurrent producers
 * that hash to the same slot (possible on ring wrap) would interleave
 * byte-level writes into `category`/`subject`; readers would see a
 * consistent `ts_ms` but garbled text. All real producers today run on
 * the LVGL thread (navigation, modals, frame tick, boot).
 */
void note(const char* category, const char* subject) noexcept;

/**
 * @brief Record an activity breadcrumb with a numeric detail
 *
 * Appends " <detail>" to the subject. Same single-producer / LVGL-thread
 * contract as the two-arg overload.
 */
void note(const char* category, const char* subject, long detail) noexcept;

/**
 * @brief Dump the ring buffer to a file descriptor as crumb: lines
 *
 * Writes the oldest-to-newest breadcrumbs as `crumb:<ms> <cat> <subj>\n`
 * lines using only async-signal-safe primitives. Used by the crash signal
 * handler; exposed here so tests can verify ring state by dumping to a
 * pipe and parsing the output.
 */
void dump_to_fd(int fd) noexcept;

} // namespace breadcrumb

} // namespace crash_handler

// C-ABI bridge for LVGL (C source) to record the current event target. Calls
// crash_handler::set_current_event() — same semantics, usable from C.
extern "C" void helix_crash_note_event(const void* target, const void* original_target,
                                       unsigned int code);

// C-ABI bridge for the in-flight event target's identity (class name + obj
// name). Patched LVGL event_send_core extracts these from e->current_target
// and hands them to us; we save copies (signal-safe, fixed buffers) so the
// crash handler can dump them as event_target_class / event_target_name.
// Bundle 3XNZQB2R: bare event_target=0x23042e0 wasn't enough to ID the click;
// next bundle in this signature should name the widget. Both args may be NULL.
extern "C" void helix_crash_note_event_target_id(const char* class_name, const char* obj_name);

// C-ABI bridge: text segment bounds for the patched LVGL cb-bounds gate.
// Returns 1 if bounds are valid, 0 if not yet captured (early init). The
// dispatch gate falls back to "no check" when bounds aren't ready, which is
// fine — the corruption surface we're guarding against (a heap-resident cb
// pointer in a per-widget event handler list) is a runtime mutation, not an
// init-time issue.
extern "C" int helix_get_text_bounds(unsigned long* lo, unsigned long* hi);

// C-ABI bridges for LVGL (C source) to breadcrumb the root of a destruction
// subtree. Called from lv_obj_delete() and lv_obj_delete_async_cb at the
// moment tear-down begins — names class + pointer so #840/L081-class SIGBUS
// crashes inside lv_event_mark_deleted carry the doomed root in the crumb
// ring. Low-frequency: one crumb per delete root, not per descendant.
extern "C" void helix_crash_note_async_del(const void* obj, const char* class_name);
extern "C" void helix_crash_note_sync_del(const void* obj, const char* class_name);
