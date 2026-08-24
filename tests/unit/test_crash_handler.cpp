// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_crash_handler.cpp
 * @brief Unit tests for crash handler - signal-safe crash file writing and parsing
 *
 * Tests the crash file parsing, JSON event creation, file lifecycle,
 * and TelemetryManager integration. Does NOT trigger real signals --
 * tests only the file-based parsing and event creation logic.
 *
 * Written TDD-style - tests WILL FAIL if crash_handler is removed.
 */

#include "config.h"
#include "system/crash_error_log_sink.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Helper: create a temporary directory for test isolation
// ============================================================================

class CrashTestFixture {
  public:
    CrashTestFixture() {
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_crash_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);
        crash_path_ = (temp_dir_ / "crash.txt").string();
    }

    ~CrashTestFixture() {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    [[nodiscard]] std::string crash_path() const {
        return crash_path_;
    }
    [[nodiscard]] fs::path temp_dir() const {
        return temp_dir_;
    }

    /// Write a mock crash file with the given content
    void write_crash_file(const std::string& content) const {
        std::ofstream ofs(crash_path_);
        ofs << content;
        ofs.close();
    }

    /// Write a realistic crash file matching the signal handler's output format
    void write_realistic_crash_file() const {
        write_crash_file("signal:11\n"
                         "name:SIGSEGV\n"
                         "version:0.9.6\n"
                         "timestamp:1707350400\n"
                         "uptime:3600\n"
                         "bt:0x0040abcd\n"
                         "bt:0x0040ef01\n"
                         "bt:0x00401234\n");
    }

  private:
    fs::path temp_dir_;
    std::string crash_path_;
};

// ============================================================================
// Crash File Detection [telemetry][crash]
// ============================================================================

TEST_CASE_METHOD(CrashTestFixture, "Crash: has_crash_file returns false when no file exists",
                 "[telemetry][crash]") {
    REQUIRE_FALSE(crash_handler::has_crash_file(crash_path()));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: has_crash_file returns true when file exists",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    REQUIRE(crash_handler::has_crash_file(crash_path()));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: has_crash_file returns false for empty file",
                 "[telemetry][crash]") {
    write_crash_file("");
    REQUIRE_FALSE(crash_handler::has_crash_file(crash_path()));
}

// ============================================================================
// Crash File Format Parsing [telemetry][crash]
// ============================================================================

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts signal number",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("signal"));
    REQUIRE(result["signal"] == 11);
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts signal name",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("signal_name"));
    REQUIRE(result["signal_name"] == "SIGSEGV");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts version",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("app_version"));
    REQUIRE(result["app_version"] == "0.9.6");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file converts timestamp to ISO 8601",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("timestamp"));
    // 1707350400 = 2024-02-08T00:00:00Z
    std::string ts = result["timestamp"];
    REQUIRE(ts.find('T') != std::string::npos);
    REQUIRE(ts.find('Z') != std::string::npos);
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts uptime",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("uptime_sec"));
    REQUIRE(result["uptime_sec"] == 3600);
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts backtrace entries",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("backtrace"));
    REQUIRE(result["backtrace"].is_array());
    REQUIRE(result["backtrace"].size() == 3);
    REQUIRE(result["backtrace"][0] == "0x0040abcd");
    REQUIRE(result["backtrace"][1] == "0x0040ef01");
    REQUIRE(result["backtrace"][2] == "0x00401234");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file with no backtrace omits field",
                 "[telemetry][crash]") {
    write_crash_file("signal:6\nname:SIGABRT\nversion:1.0.0\ntimestamp:1707350400\nuptime:100\n");
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result["signal"] == 6);
    REQUIRE(result["signal_name"] == "SIGABRT");
    // No backtrace entries should mean no backtrace field
    REQUIRE_FALSE(result.contains("backtrace"));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse returns null for missing file",
                 "[telemetry][crash]") {
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE(result.is_null());
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse returns null for file missing required fields",
                 "[telemetry][crash]") {
    // File with only version, no signal info
    write_crash_file("version:1.0.0\nuptime:100\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE(result.is_null());
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse handles all signal types", "[telemetry][crash]") {
    struct SignalCase {
        int number;
        const char* name;
    };

    SignalCase signals[] = {
        {11, "SIGSEGV"},
        {6, "SIGABRT"},
        {7, "SIGBUS"},
        {8, "SIGFPE"},
    };

    for (const auto& sig : signals) {
        std::string content = "signal:" + std::to_string(sig.number) +
                              "\n"
                              "name:" +
                              sig.name +
                              "\n"
                              "version:1.0.0\n"
                              "timestamp:1707350400\n"
                              "uptime:0\n";
        write_crash_file(content);

        auto result = crash_handler::read_crash_file(crash_path());
        REQUIRE_FALSE(result.is_null());
        REQUIRE(result["signal"] == sig.number);
        REQUIRE(result["signal_name"] == sig.name);
    }
}

// ============================================================================
// Crash File Cleanup [telemetry][crash]
// ============================================================================

TEST_CASE_METHOD(CrashTestFixture, "Crash: remove_crash_file deletes the file",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    REQUIRE(crash_handler::has_crash_file(crash_path()));

    crash_handler::remove_crash_file(crash_path());
    REQUIRE_FALSE(crash_handler::has_crash_file(crash_path()));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: remove_crash_file is safe for non-existent file",
                 "[telemetry][crash]") {
    // Should not throw or crash
    crash_handler::remove_crash_file(crash_path());
    REQUIRE_FALSE(crash_handler::has_crash_file(crash_path()));
}

// ============================================================================
// abort_msg capture (glibc-only; field is parsed regardless of host libc)
// ============================================================================

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts abort_msg",
                 "[telemetry][crash]") {
    write_crash_file("signal:6\n"
                     "name:SIGABRT\n"
                     "version:0.99.62\n"
                     "timestamp:1707350400\n"
                     "uptime:100\n"
                     "abort_msg:free(): invalid pointer\n");
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("abort_msg"));
    REQUIRE(result["abort_msg"] == "free(): invalid pointer");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: abort_msg absent when not in file",
                 "[telemetry][crash]") {
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE_FALSE(result.contains("abort_msg"));
}

// abort_msg_state / terminate_msg (issue #987): a blank glibc abort reason is
// no longer ambiguous, and a re-entrant std::terminate still surfaces its reason.
TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts abort_msg_state",
                 "[telemetry][crash]") {
    write_crash_file("signal:6\n"
                     "name:SIGABRT\n"
                     "version:0.99.72\n"
                     "timestamp:1707350400\n"
                     "uptime:100\n"
                     "abort_msg_state:empty\n");
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("abort_msg_state"));
    REQUIRE(result["abort_msg_state"] == "empty");
    // "empty" means glibc stored no reason — there must be no abort_msg line.
    REQUIRE_FALSE(result.contains("abort_msg"));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts terminate_msg",
                 "[telemetry][crash]") {
    write_crash_file("signal:6\n"
                     "name:SIGABRT\n"
                     "version:0.99.72\n"
                     "timestamp:1707350400\n"
                     "uptime:100\n"
                     "abort_msg_state:empty\n"
                     "terminate_msg:std::bad_alloc\n");
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("terminate_msg"));
    REQUIRE(result["terminate_msg"] == "std::bad_alloc");
}

// recent_error* labeled keys collapse into one ordered array (newest first).
TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file groups recent_error lines",
                 "[telemetry][crash]") {
    write_crash_file("signal:6\n"
                     "name:SIGABRT\n"
                     "version:0.99.72\n"
                     "timestamp:1707350400\n"
                     "uptime:100\n"
                     "abort_msg_state:empty\n"
                     "recent_error:newest failure\n"
                     "recent_error2:older failure\n"
                     "recent_error3:oldest failure\n");
    auto result = crash_handler::read_crash_file(crash_path());

    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("recent_errors"));
    REQUIRE(result["recent_errors"].is_array());
    REQUIRE(result["recent_errors"].size() == 3);
    REQUIRE(result["recent_errors"][0] == "newest failure");
    REQUIRE(result["recent_errors"][2] == "oldest failure");
}

// ============================================================================
// End-to-end: real glibc abort propagates via __abort_msg into the crash file.
// Forks a child, triggers a malloc-corruption abort, parent reads the dump.
// glibc-only — gated below so musl/uclibc/macOS skip cleanly.
// ============================================================================
#if defined(__linux__) && defined(__GLIBC__)
#include <dlfcn.h>
#include <sys/wait.h>

// glibc does NOT store the abort reason as a bare string. `__abort_msg` is a
// pointer to `struct abort_msg_s { unsigned int size; char msg[]; }`, so the
// text starts at offsetof(msg) — not at the pointer itself. `size` is the
// mmap length rounded up to a page, so its low byte is always 0x00 on
// little-endian: a reader that treats the pointer as `char*` sees a NUL first
// byte and reports "no message" for every real glibc abort. That is exactly
// what shipped (bundle VP623KVU, v0.99.100 — a heap-corruption SIGABRT whose
// reason was discarded as `abort_msg_state:empty`), and the original tests
// missed it because they published a bare `char*` that glibc never produces.
//
// These helpers publish the REAL layout, so the tests fail if the handler
// stops honouring the struct offset.
namespace {

struct TestAbortMsg {
    unsigned int size;
    char msg[1024];
};

/// Publish `text` into `__abort_msg` using glibc's real struct layout.
/// Static storage so it stays valid through raise(SIGABRT). Child-process use
/// only. Returns false if the symbol is unresolvable.
bool publish_abort_msg(const char* text) {
    static TestAbortMsg buf;
    // Page-rounded, exactly as glibc's ROUND_UP(sizeof + len + 1, pagesize)
    // produces — this is what makes byte 0 a NUL and hides the message from a
    // char*-style read.
    buf.size = 4096;
    std::snprintf(buf.msg, sizeof(buf.msg), "%s", text);
    void** slot = static_cast<void**>(dlsym(RTLD_DEFAULT, "__abort_msg"));
    if (slot == nullptr) {
        return false;
    }
    *slot = &buf;
    return true;
}

} // namespace

TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: SIGABRT signal handler captures __abort_msg into crash file",
                 "[telemetry][crash][subprocess]") {
    // Sanity: __abort_msg must be resolvable on this host. If not, skip cleanly
    // (musl/uclibc/macOS hit the outer #if guard already, so this should always
    // resolve on the platforms the test compiles for).
    if (dlsym(RTLD_DEFAULT, "__abort_msg") == nullptr) {
        SKIP("__abort_msg not resolvable on this libc");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child. Pre-populate __abort_msg in glibc's real struct layout, the
        // same way __libc_message would for an actual abort (free(): invalid
        // pointer / double free / assertion failure), then raise SIGABRT
        // directly. This exercises the crash_handler capture path without
        // depending on the real heap-corruption code path firing through
        // Catch2's harness (Catch2 v3 installs its own SIGABRT handlers that
        // interfere with letting glibc abort() drive the child's termination).
        // The real-glibc-abort case is covered end-to-end below.
        if (!publish_abort_msg("helixscreen abort_msg capture test (synthetic)")) {
            _exit(98);
        }

        crash_handler::install(crash_path());
        raise(SIGABRT);
        _exit(99); // unreachable
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    INFO("raw status = " << status << ", WIFEXITED=" << WIFEXITED(status) << ", WEXITSTATUS="
                         << WEXITSTATUS(status) << ", WIFSIGNALED=" << WIFSIGNALED(status)
                         << ", WTERMSIG=" << WTERMSIG(status));
    // SIGABRT is auto-blocked during sa_sigaction handlers (sigaction default
    // unless SA_NODEFER is set), so the handler's tail `raise(SIGABRT)` stays
    // pending and `_exit(128 + sig)` runs instead. Production watchdog sees
    // exit code 134 for SIGABRT and respawns — same path as a real crash.
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    REQUIRE(crash_handler::has_crash_file(crash_path()));
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result["signal"] == 6);
    REQUIRE(result.contains("abort_msg"));
    std::string msg = result["abort_msg"];
    INFO("abort_msg = " << msg);
    REQUIRE_FALSE(msg.empty());
    REQUIRE(msg.find("helixscreen abort_msg capture test") != std::string::npos);
}

TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: SIGABRT handler normalizes newlines in __abort_msg to spaces",
                 "[telemetry][crash][subprocess]") {
    if (dlsym(RTLD_DEFAULT, "__abort_msg") == nullptr) {
        SKIP("__abort_msg not resolvable on this libc");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Embedded \n and \r must flatten to spaces — the crash file format
        // is "key:value\n", and a raw newline inside the value would split
        // the line at the parser and break the single-field invariant.
        if (!publish_abort_msg("line one\nline two\rline three")) {
            _exit(98);
        }

        crash_handler::install(crash_path());
        raise(SIGABRT);
        _exit(99);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("abort_msg"));
    std::string msg = result["abort_msg"];
    REQUIRE(msg == "line one line two line three");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: SIGABRT handler truncates oversized __abort_msg",
                 "[telemetry][crash][subprocess]") {
    if (dlsym(RTLD_DEFAULT, "__abort_msg") == nullptr) {
        SKIP("__abort_msg not resolvable on this libc");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // 500 'a's followed by a tail marker the bounded copy MUST NOT
        // capture — handler buffer is 256 bytes, max content 255 chars.
        char long_msg[600] = {};
        for (size_t i = 0; i < 500; ++i) {
            long_msg[i] = 'a';
        }
        const char tail[] = "TAILMARKER_SHOULD_BE_TRUNCATED";
        for (size_t j = 0; j < sizeof(tail); ++j) {
            long_msg[500 + j] = tail[j];
        }
        if (!publish_abort_msg(long_msg)) {
            _exit(98);
        }

        crash_handler::install(crash_path());
        raise(SIGABRT);
        _exit(99);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("abort_msg"));
    std::string msg = result["abort_msg"];
    REQUIRE(msg.size() == 255);
    REQUIRE(msg.find("TAILMARKER") == std::string::npos);
    REQUIRE(msg.find_first_not_of('a') == std::string::npos);
}

// Issue #987: when glibc stored no abort reason (bare abort()/raise — NOT a
// malloc/assert/fortify abort), the handler must emit abort_msg_state:empty and
// omit abort_msg, so a blank reason is diagnosable rather than ambiguous.
TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: SIGABRT with empty __abort_msg records abort_msg_state:empty",
                 "[telemetry][crash][subprocess]") {
    if (dlsym(RTLD_DEFAULT, "__abort_msg") == nullptr) {
        SKIP("__abort_msg not resolvable on this libc");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        void** abort_msg_slot = static_cast<void**>(dlsym(RTLD_DEFAULT, "__abort_msg"));
        if (abort_msg_slot == nullptr) {
            _exit(98);
        }
        // Force the "glibc stored no message" condition a bare abort()/raise
        // produces (the exact #987 signature).
        *abort_msg_slot = nullptr;

        crash_handler::install(crash_path());
        raise(SIGABRT);
        _exit(99);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result["signal"] == 6);
    REQUIRE(result.contains("abort_msg_state"));
    REQUIRE(result["abort_msg_state"] == "empty");
    REQUIRE_FALSE(result.contains("abort_msg"));
}

// A non-null abort_msg_s whose message is the empty string is still "no reason"
// — distinct from the nullptr case above, and the branch that used to swallow
// every real message. Pins that `empty` is decided on the message text, not on
// the first byte of the struct's `size` field.
TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: SIGABRT with zero-length abort_msg_s message records empty",
                 "[telemetry][crash][subprocess]") {
    if (dlsym(RTLD_DEFAULT, "__abort_msg") == nullptr) {
        SKIP("__abort_msg not resolvable on this libc");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        if (!publish_abort_msg("")) {
            _exit(98);
        }
        crash_handler::install(crash_path());
        raise(SIGABRT);
        _exit(99);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result["abort_msg_state"] == "empty");
    REQUIRE_FALSE(result.contains("abort_msg"));
}

// The one that matters: let REAL glibc populate __abort_msg via its own
// __libc_message path, rather than modelling the layout ourselves. Every
// synthetic test above shares our understanding of the struct, so all of them
// would keep passing if that understanding were wrong — which is exactly how
// the char*-read shipped. This test has no such blind spot.
//
// Skipped under ASan/TSan/Valgrind: those intercept the double free and
// terminate the child themselves, so glibc never runs and our handler never
// sees the signal.
//
// Both spellings of each guard are required. __SANITIZE_ADDRESS__ and
// __SANITIZE_THREAD__ are GCC's; clang defines neither and answers only
// __has_feature. The sanitizer CI jobs build with clang, so the thread half
// went unguarded and this test ran under TSan, where the child exits 99 instead
// of aborting — a red nightly for a test that was supposed to be compiled out.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) &&                             \
    !(defined(__has_feature) && __has_feature(address_sanitizer)) &&                               \
    !(defined(__has_feature) && __has_feature(thread_sanitizer))
TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: real glibc heap-corruption abort reason reaches the crash file",
                 "[telemetry][crash][subprocess][960]") {
    if (dlsym(RTLD_DEFAULT, "__abort_msg") == nullptr) {
        SKIP("__abort_msg not resolvable on this libc");
    }
    if (getenv("VALGRIND") != nullptr || getenv("LD_PRELOAD") != nullptr) {
        SKIP("allocator interposed — glibc's own corruption check would not fire");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        crash_handler::install(crash_path());
        // Double free: glibc's malloc_printerr -> __libc_message writes the
        // reason into __abort_msg and calls abort(). Reason text varies by
        // glibc version ("free(): double free detected in tcache 2",
        // "double free or corruption (!prev)"), so the assertion below only
        // pins the substring every variant shares.
        //
        // The pointer MUST be volatile. At -O2 (this build's setting) GCC
        // recognises the redundant free/free pair as UB and deletes the second
        // call outright, so the child sails past and _exit(99)s — verified,
        // that is exactly how this test first failed. volatile forces both
        // calls to reach glibc.
        // Announce the corruption before causing it. glibc's message lands on
        // stderr in the middle of an otherwise-passing suite log, where it reads
        // as genuine heap corruption in the test binary — it has already sent one
        // reader off investigating a non-bug. This marker is the only thing that
        // distinguishes "the test worked" from "something is badly wrong".
        //
        // write(2), not fprintf: this is a forked child of a multithreaded
        // process, so stdio locks may be held by threads that do not exist here,
        // and glibc writes its own message straight to fd 2 — sharing the raw fd
        // is what keeps the two lines in order.
        static const char kMarker[] =
            "[test] DELIBERATE double free follows (test_crash_handler.cpp) - "
            "glibc's abort message below is the expected result, not a defect\n";
        ssize_t marker_rc = write(STDERR_FILENO, kMarker, sizeof(kMarker) - 1);
        (void)marker_rc;

        void* volatile p = malloc(64);
        free(const_cast<void*>(p));
        free(const_cast<void*>(p)); // NOLINT — deliberate corruption, this is the test
        _exit(99);                  // glibc failed to detect it — parent's exit-code check fails
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    INFO("raw status = " << status << ", WIFEXITED=" << WIFEXITED(status) << ", WEXITSTATUS="
                         << WEXITSTATUS(status) << ", WIFSIGNALED=" << WIFSIGNALED(status));
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result["signal"] == 6);
    REQUIRE(result["abort_msg_state"] == "present");
    REQUIRE(result.contains("abort_msg"));
    std::string msg = result["abort_msg"];
    INFO("abort_msg = " << msg);
    REQUIRE(msg.find("free") != std::string::npos);
}
#endif // sanitizers
#endif // __linux__ && __GLIBC__

// ============================================================================
// Wrap-safe monotonic durations (uptime, heap-snapshot age)
// ============================================================================

TEST_CASE("Crash: elapsed_ms computes wrap-safe monotonic deltas", "[telemetry][crash]") {
    using crash_handler::detail::elapsed_ms;

    SECTION("ordinary forward delta") {
        REQUIRE(elapsed_ms(1000, 6000) == 5000);
    }
    SECTION("zero delta") {
        REQUIRE(elapsed_ms(12345, 12345) == 0);
    }
    SECTION("crosses the 32-bit wrap at ~49.7 days") {
        // start near the top of the counter, now just past the fold
        REQUIRE(elapsed_ms(0xFFFFF000u, 0x00001000u) == 0x2000u);
    }
    SECTION("full-range wrap stays exact") {
        REQUIRE(elapsed_ms(0xFFFFFFFFu, 0u) == 1u);
    }
}

// heap_snapshot_age_ms must be an AGE. It previously emitted the snapshot's raw
// monotonic timestamp under an "age" label, so a snapshot taken 17 ms before the
// crash was reported as "snapshot age 144712ms" in the auto-filed issue (bundle
// VP623KVU) — reading as a badly stale snapshot when it was in fact fresh.
#if defined(__linux__)
TEST_CASE_METHOD(CrashTestFixture, "Crash: heap_snapshot_age_ms is an age, not a timestamp",
                 "[telemetry][crash][subprocess]") {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t boot_ms = static_cast<uint64_t>(ts.tv_sec) * 1000;
    if (boot_ms < 10000) {
        SKIP("host booted <10s ago — cannot distinguish an age from a timestamp");
    }

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        crash_handler::install(crash_path());
        crash_handler::refresh_heap_snapshot();
        raise(SIGABRT);
        _exit(99);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("heap_snapshot_age_ms"));
    long age = result["heap_snapshot_age_ms"];
    INFO("age = " << age << ", monotonic-since-boot = " << boot_ms);
    // The snapshot was taken microseconds before the raise, so the age is tiny.
    // Emitting the timestamp instead would yield >= boot_ms (>= 10000).
    REQUIRE(age >= 0);
    REQUIRE(age < 1000);
}

// The age alone is undiagnosable when it comes out wrong (bundle ED2YC336
// reported an age exceeding the process uptime, which is impossible). Both raw
// stamps now ship with it, and they must agree with the age exactly — they are
// derived from a single clock read, so any drift means the handler took two.
TEST_CASE_METHOD(CrashTestFixture, "Crash: heap snapshot emits self-consistent raw clock stamps",
                 "[telemetry][crash][subprocess]") {
    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        crash_handler::install(crash_path());
        crash_handler::refresh_heap_snapshot();
        raise(SIGABRT);
        _exit(99);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("heap_snapshot_age_ms"));
    REQUIRE(result.contains("heap_snapshot_ts_ms"));
    REQUIRE(result.contains("heap_mono_ms_now"));

    const long age = result["heap_snapshot_age_ms"];
    const long ts = result["heap_snapshot_ts_ms"];
    const long now = result["heap_mono_ms_now"];
    INFO("age=" << age << " ts=" << ts << " now=" << now);

    // Both stamps are the low 32 bits of CLOCK_MONOTONIC, so the age is their
    // unsigned difference. Reproduce that arithmetic rather than asserting a
    // constant we also hardcoded.
    const uint32_t expected =
        static_cast<uint32_t>(now) - static_cast<uint32_t>(ts); // wraps by design
    REQUIRE(age == static_cast<long>(expected));

    // And the snapshot is not in the future relative to the crash clock here:
    // this process has been up for milliseconds, nowhere near the 49.7-day fold.
    REQUIRE(ts <= now);
}
#endif // __linux__

// End-to-end (#987 last-ditch capture): ERROR log lines flow through
// CrashErrorLogSink into the crash file as recent_error: entries, newest-first,
// and sub-ERROR lines are excluded. Linux-only (signal-handler crash-file
// write path); no glibc dependency since this uses our own ring, not
// __abort_msg.
#if defined(__linux__)
#include <sys/wait.h>
TEST_CASE_METHOD(CrashTestFixture, "Crash: recent ERROR log lines captured into crash file",
                 "[telemetry][crash][subprocess]") {
    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        crash_handler::install(crash_path());
        // Route logging through the crash error-log sink (its ctor registers
        // the ring with the crash handler).
        auto sink =
            spdlog::sink_ptr(&helix::CrashErrorLogSink::instance(), [](spdlog::sinks::sink*) {});
        auto logger = std::make_shared<spdlog::logger>("crashtest", sink);
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);

        spdlog::error("disk on fire 17");
        spdlog::warn("ignored warning"); // below ERROR — must NOT be captured
        spdlog::error("kaboom 42");
        raise(SIGABRT);
        _exit(99); // unreachable
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGABRT);

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("recent_errors"));
    auto& errs = result["recent_errors"];
    REQUIRE(errs.size() == 2);
    REQUIRE(errs[0] == "kaboom 42"); // newest first
    REQUIRE(errs[1] == "disk on fire 17");
    for (const auto& e : errs) {
        REQUIRE(e.get<std::string>().find("ignored warning") == std::string::npos);
    }
}
#endif // __linux__

// ============================================================================
// Install / Uninstall (no real signals) [telemetry][crash]
// ============================================================================

TEST_CASE_METHOD(CrashTestFixture, "Crash: install and uninstall do not crash",
                 "[telemetry][crash]") {
    // Just verify install/uninstall is safe (no actual signal triggering)
    crash_handler::install(crash_path());
    crash_handler::uninstall();

    // Double uninstall should be safe
    crash_handler::uninstall();
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: install with long path does not crash",
                 "[telemetry][crash]") {
    // Test with a path longer than typical but within buffer limits
    std::string long_path = temp_dir().string() + "/" + std::string(200, 'a') + "/crash.txt";
    crash_handler::install(long_path);
    crash_handler::uninstall();
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: install with very long path truncates safely",
                 "[telemetry][crash]") {
    // Path longer than MAX_PATH_LEN (512) -- should truncate, not crash
    std::string very_long_path = "/" + std::string(600, 'x') + "/crash.txt";
    crash_handler::install(very_long_path);
    crash_handler::uninstall();
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: double install is idempotent", "[telemetry][crash]") {
    crash_handler::install(crash_path());
    crash_handler::install(crash_path()); // Should be safe
    crash_handler::uninstall();
}

// ============================================================================
// write_exception_record [telemetry][crash]
// ============================================================================
//
// AD5M v0.99.38-0.99.41 showed write_exception_crash_file in its own crash
// stack — the exception path in main.cpp was heap-using (std::string for path,
// dprintf, backtrace, dl_iterate_phdr) and re-crashed under memory pressure.
// write_exception_record replaces it with async-signal-safe primitives and a
// re-entrance guard.

TEST_CASE_METHOD(CrashTestFixture, "Crash: write_exception_record writes parseable record",
                 "[telemetry][crash]") {
    crash_handler::install(crash_path());
    crash_handler::write_exception_record("std::runtime_error: test");
    crash_handler::uninstall();

    REQUIRE(crash_handler::has_crash_file(crash_path()));
    json parsed = crash_handler::read_crash_file(crash_path());
    REQUIRE(parsed != nullptr);
    REQUIRE(parsed["signal"] == 0);
    REQUIRE(parsed["signal_name"] == "EXCEPTION");
    REQUIRE(parsed.contains("app_version"));
    REQUIRE(parsed.contains("timestamp"));
    REQUIRE(parsed.contains("uptime_sec"));
    REQUIRE(parsed["exception"] == "std::runtime_error: test");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: write_exception_record handles null what",
                 "[telemetry][crash]") {
    crash_handler::install(crash_path());
    crash_handler::write_exception_record(nullptr);
    crash_handler::uninstall();

    REQUIRE(crash_handler::has_crash_file(crash_path()));
    json parsed = crash_handler::read_crash_file(crash_path());
    REQUIRE(parsed != nullptr);
    REQUIRE(parsed["signal"] == 0);
    REQUIRE(parsed["signal_name"] == "EXCEPTION");
    // exception: key should be absent since what was nullptr
    REQUIRE_FALSE(parsed.contains("exception"));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: write_exception_record is no-op without install",
                 "[telemetry][crash]") {
    // Ensure we start in uninstalled state — prior tests may have left state;
    // uninstall is idempotent.
    crash_handler::uninstall();

    // No install() — s_crash_path is empty, so this must do nothing
    // (it absolutely must not segfault or open("") ).
    crash_handler::write_exception_record("should not write anywhere");

    REQUIRE_FALSE(crash_handler::has_crash_file(crash_path()));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: TelemetryManager enqueues crash event from file",
                 "[telemetry][crash]") {
    // Write a crash file in the temp directory that TelemetryManager will use
    std::string tm_crash_path = (temp_dir() / "crash.txt").string();
    {
        std::ofstream ofs(tm_crash_path);
        ofs << "signal:11\nname:SIGSEGV\nversion:0.9.6\ntimestamp:1707350400\nuptime:3600\n"
            << "bt:0x0040abcd\nbt:0x0040ef01\n";
    }

    // Enable telemetry via Config singleton (single source of truth since
    // config_version 14; previously telemetry_config.json was a sibling).
    {
        helix::Config* cfg = helix::Config::get_instance();
        cfg->set<bool>("/telemetry_enabled", true);
    }

    // Initialize TelemetryManager with the temp dir (init calls check_previous_crash)
    auto& tm = TelemetryManager::instance();
    tm.shutdown();
    tm.init(temp_dir().string());

    // Should have enqueued the crash event
    REQUIRE(tm.queue_size() >= 1);

    auto snapshot = tm.get_queue_snapshot();
    // Find the crash event
    bool found_crash = false;
    for (const auto& event : snapshot) {
        if (event.contains("event") && event["event"] == "crash") {
            found_crash = true;

            // Verify crash event schema
            REQUIRE(event.contains("schema_version"));
            REQUIRE(event["schema_version"] == TelemetryManager::SCHEMA_VERSION);
            REQUIRE(event.contains("device_id"));
            REQUIRE(event["device_id"].is_string());
            REQUIRE(event.contains("timestamp"));
            REQUIRE(event.contains("signal"));
            REQUIRE(event["signal"] == 11);
            REQUIRE(event.contains("signal_name"));
            REQUIRE(event["signal_name"] == "SIGSEGV");
            REQUIRE(event.contains("app_version"));
            REQUIRE(event["app_version"] == "0.9.6");
            REQUIRE(event.contains("uptime_sec"));
            REQUIRE(event["uptime_sec"] == 3600);
            REQUIRE(event.contains("backtrace"));
            REQUIRE(event["backtrace"].size() == 2);
            break;
        }
    }
    REQUIRE(found_crash);

    // Crash file is intentionally NOT deleted by TelemetryManager —
    // CrashReporter owns the lifecycle and removes it after user interaction
    REQUIRE(crash_handler::has_crash_file(tm_crash_path));

    // Clean up for other tests
    fs::remove(tm_crash_path);
    tm.shutdown();
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: TelemetryManager ignores absent crash file",
                 "[telemetry][crash]") {
    // Initialize without any crash file
    auto& tm = TelemetryManager::instance();
    tm.shutdown();
    tm.init(temp_dir().string());

    // No crash events should be enqueued
    auto snapshot = tm.get_queue_snapshot();
    for (const auto& event : snapshot) {
        REQUIRE_FALSE(event["event"] == "crash");
    }

    tm.shutdown();
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: when disabled, crash event is not enqueued",
                 "[telemetry][crash]") {
    // Write a crash file
    {
        std::ofstream ofs((temp_dir() / "crash.txt").string());
        ofs << "signal:11\nname:SIGSEGV\nversion:0.9.6\ntimestamp:1707350400\nuptime:3600\n";
    }

    // Explicitly disable via Config — Config is a singleton, prior tests in
    // this binary may have enabled it.
    {
        helix::Config* cfg = helix::Config::get_instance();
        cfg->set<bool>("/telemetry_enabled", false);
    }
    auto& tm = TelemetryManager::instance();
    tm.shutdown();
    tm.init(temp_dir().string());

    // No crash events should be enqueued
    auto snapshot = tm.get_queue_snapshot();
    for (const auto& event : snapshot) {
        REQUIRE_FALSE(event["event"] == "crash");
    }

    // Crash file is intentionally NOT deleted by TelemetryManager —
    // CrashReporter owns the lifecycle and removes it after user interaction
    REQUIRE(crash_handler::has_crash_file((temp_dir() / "crash.txt").string()));

    // Clean up for other tests
    fs::remove(temp_dir() / "crash.txt");
    tm.shutdown();
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: crash event has correct device_id format",
                 "[telemetry][crash]") {
    // Write crash file
    {
        std::ofstream ofs((temp_dir() / "crash.txt").string());
        ofs << "signal:6\nname:SIGABRT\nversion:1.0.0\ntimestamp:1707350400\nuptime:0\n";
    }

    // Enable telemetry so crash event is enqueued
    {
        helix::Config* cfg = helix::Config::get_instance();
        cfg->set<bool>("/telemetry_enabled", true);
    }

    auto& tm = TelemetryManager::instance();
    tm.shutdown();
    tm.init(temp_dir().string());

    auto snapshot = tm.get_queue_snapshot();
    bool found = false;
    for (const auto& event : snapshot) {
        if (event.contains("event") && event["event"] == "crash") {
            found = true;
            std::string device_id = event["device_id"];
            // Device ID should be a 64-character hex hash (SHA-256)
            REQUIRE(device_id.size() == 64);
            for (char c : device_id) {
                bool valid_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                REQUIRE(valid_hex);
            }
            break;
        }
    }
    REQUIRE(found);

    tm.shutdown();
}

// ============================================================================
// Phase 2: Fault Info & Register State Parsing [telemetry][crash]
// ============================================================================

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts fault_addr",
                 "[telemetry][crash]") {
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
                     "fault_addr:0x00000000\nfault_code:1\nfault_code_name:SEGV_MAPERR\n"
                     "reg_pc:0x00920bac\nreg_sp:0xbe8ff420\nreg_lr:0x0091a3c0\n"
                     "bt:0x920bac\nbt:0xf7101290\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("fault_addr"));
    REQUIRE(result["fault_addr"] == "0x00000000");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts fault_code and name",
                 "[telemetry][crash]") {
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
                     "fault_addr:0x00000000\nfault_code:1\nfault_code_name:SEGV_MAPERR\n"
                     "bt:0x920bac\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("fault_code"));
    REQUIRE(result["fault_code"] == 1);
    REQUIRE(result.contains("fault_code_name"));
    REQUIRE(result["fault_code_name"] == "SEGV_MAPERR");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts register state",
                 "[telemetry][crash]") {
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
                     "fault_addr:0xdeadbeef\nfault_code:2\nfault_code_name:SEGV_ACCERR\n"
                     "reg_pc:0x00920bac\nreg_sp:0xbe8ff420\nreg_lr:0x0091a3c0\n"
                     "bt:0x920bac\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("reg_pc"));
    REQUIRE(result["reg_pc"] == "0x00920bac");
    REQUIRE(result.contains("reg_sp"));
    REQUIRE(result["reg_sp"] == "0xbe8ff420");
    REQUIRE(result.contains("reg_lr"));
    REQUIRE(result["reg_lr"] == "0x0091a3c0");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts reg_bp for x86_64",
                 "[telemetry][crash]") {
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
                     "fault_addr:0x00000000\nfault_code:1\nfault_code_name:SEGV_MAPERR\n"
                     "reg_pc:0x00400abc\nreg_sp:0x7ffd12345678\nreg_bp:0x7ffd12345690\n"
                     "bt:0x400abc\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("reg_bp"));
    REQUIRE(result["reg_bp"] == "0x7ffd12345690");
    // Should NOT have reg_lr when reg_bp is present
    REQUIRE_FALSE(result.contains("reg_lr"));
}

TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: parse old format crash file without fault/register fields",
                 "[telemetry][crash]") {
    // This is the existing format - should continue working
    write_realistic_crash_file();
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result["signal"] == 11);
    REQUIRE(result["signal_name"] == "SIGSEGV");
    // New fields should be absent, not error
    REQUIRE_FALSE(result.contains("fault_addr"));
    REQUIRE_FALSE(result.contains("fault_code"));
    REQUIRE_FALSE(result.contains("fault_code_name"));
    REQUIRE_FALSE(result.contains("reg_pc"));
    REQUIRE_FALSE(result.contains("reg_sp"));
    REQUIRE_FALSE(result.contains("reg_lr"));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file with partial fault fields",
                 "[telemetry][crash]") {
    // Only fault_addr, no fault_code or registers
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:100\n"
                     "fault_addr:0x00000000\n"
                     "bt:0x920bac\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("fault_addr"));
    REQUIRE(result["fault_addr"] == "0x00000000");
    REQUIRE_FALSE(result.contains("fault_code"));
    REQUIRE_FALSE(result.contains("reg_pc"));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file extracts queue_callback tag",
                 "[telemetry][crash]") {
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
                     "fault_addr:0x00000000\nfault_code:1\nfault_code_name:SEGV_MAPERR\n"
                     "reg_pc:0x00920bac\nreg_sp:0xbe8ff420\nreg_lr:0x0091a3c0\n"
                     "queue_callback:ToastManager::dismiss\n"
                     "bt:0x920bac\nbt:0xf7101290\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE(result.contains("queue_callback"));
    REQUIRE(result["queue_callback"] == "ToastManager::dismiss");
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: parse crash file without queue_callback is fine",
                 "[telemetry][crash]") {
    write_crash_file("signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
                     "bt:0x920bac\n");
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    REQUIRE_FALSE(result.contains("queue_callback"));
}

TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: write_mock_crash_file includes fault and register fields",
                 "[telemetry][crash]") {
    crash_handler::write_mock_crash_file(crash_path());
    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());
    // Mock file should include fault info
    REQUIRE(result.contains("fault_addr"));
    REQUIRE(result.contains("fault_code"));
    REQUIRE(result.contains("fault_code_name"));
    // Mock file should include at least PC and SP registers
    REQUIRE(result.contains("reg_pc"));
    REQUIRE(result.contains("reg_sp"));
}

TEST_CASE_METHOD(CrashTestFixture, "Crash: TelemetryManager crash event includes fault fields",
                 "[telemetry][crash]") {
    std::string tm_crash_path = (temp_dir() / "crash.txt").string();
    {
        std::ofstream ofs(tm_crash_path);
        ofs << "signal:11\nname:SIGSEGV\nversion:0.9.18\ntimestamp:1707350400\nuptime:3174\n"
            << "fault_addr:0x00000000\nfault_code:1\nfault_code_name:SEGV_MAPERR\n"
            << "reg_pc:0x00920bac\nreg_sp:0xbe8ff420\nreg_lr:0x0091a3c0\n"
            << "bt:0x920bac\nbt:0xf7101290\n";
    }
    {
        helix::Config* cfg = helix::Config::get_instance();
        cfg->set<bool>("/telemetry_enabled", true);
    }

    auto& tm = TelemetryManager::instance();
    tm.shutdown();
    tm.init(temp_dir().string());

    auto snapshot = tm.get_queue_snapshot();
    bool found = false;
    for (const auto& event : snapshot) {
        if (event.contains("event") && event["event"] == "crash") {
            found = true;
            REQUIRE(event.contains("fault_addr"));
            REQUIRE(event["fault_addr"] == "0x00000000");
            REQUIRE(event.contains("fault_code"));
            REQUIRE(event["fault_code"] == 1);
            REQUIRE(event.contains("fault_code_name"));
            REQUIRE(event["fault_code_name"] == "SEGV_MAPERR");
            break;
        }
    }
    REQUIRE(found);

    fs::remove(tm_crash_path);
    tm.shutdown();
}

// ============================================================================
// Breadcrumb ring + extended parser coverage [telemetry][crash]
// ============================================================================
//
// Protects the signal-handler-adjacent code paths that a future refactor
// could silently break: ring wraparound math, string truncation in
// copy_truncated, and parser coverage for the post-v0.99.31 fields
// (breadcrumbs, event_target / event_original_target / event_code, and the
// heap_* / lv_heap_* snapshot fields).

namespace {

/// Call breadcrumb::dump_to_fd into a pipe and return every line emitted.
/// The dump uses write() directly (signal-safe), so a pipe is the simplest
/// readable sink from a normal test context.
std::vector<std::string> capture_breadcrumb_dump() {
    int pipe_fds[2];
    REQUIRE(::pipe(pipe_fds) == 0);
    crash_handler::breadcrumb::dump_to_fd(pipe_fds[1]);
    ::close(pipe_fds[1]);

    std::string buf;
    char chunk[256];
    ssize_t n;
    while ((n = ::read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
        buf.append(chunk, static_cast<size_t>(n));
    }
    ::close(pipe_fds[0]);

    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] == '\n') {
            lines.emplace_back(buf.substr(start, i - start));
            start = i + 1;
        }
    }
    return lines;
}

} // namespace

TEST_CASE("Crash: breadcrumb ring keeps the newest 256 entries on wraparound",
          "[telemetry][crash]") {
    // Drain the ring first — static state leaks across test cases. Writing
    // ring-size drain entries pushes any residual application breadcrumbs out.
    // Ring size: src/system/crash_handler.cpp BREADCRUMB_RING_SIZE = 256.
    for (int i = 0; i < 256; ++i) {
        crash_handler::breadcrumb::note("drain", "drain");
    }

    // Write 262 distinguishable entries; the ring holds 256, so entries 0..5
    // should be overwritten. Encode the index as the detail suffix so we
    // can extract it from the dumped subject string.
    for (int i = 0; i < 262; ++i) {
        crash_handler::breadcrumb::note("t", "entry", i);
    }

    auto lines = capture_breadcrumb_dump();
    REQUIRE(lines.size() == 256);

    auto extract_index = [](const std::string& line) -> int {
        auto pos = line.rfind(' ');
        if (pos == std::string::npos)
            return -1;
        return std::stoi(line.substr(pos + 1));
    };

    REQUIRE(extract_index(lines.front()) == 6);
    REQUIRE(extract_index(lines.back()) == 261);

    // Indices must be strictly monotonic — order preservation matters for
    // correlating with the crash location further down the log.
    for (size_t i = 1; i < lines.size(); ++i) {
        REQUIRE(extract_index(lines[i]) == extract_index(lines[i - 1]) + 1);
    }
}

TEST_CASE("Crash: breadcrumb category/subject are truncated with null terminator",
          "[telemetry][crash]") {
    // Ring size: src/system/crash_handler.cpp BREADCRUMB_RING_SIZE = 256.
    for (int i = 0; i < 256; ++i) {
        crash_handler::breadcrumb::note("drain", "drain");
    }

    // category longer than 7 chars, subject longer than 59 chars. If
    // copy_truncated failed to null-terminate, the dump would walk into
    // neighboring slot bytes and produce visible garbage.
    const std::string long_category(32, 'C');
    const std::string long_subject(200, 'S');
    crash_handler::breadcrumb::note(long_category.c_str(), long_subject.c_str());

    auto lines = capture_breadcrumb_dump();
    REQUIRE_FALSE(lines.empty());
    const std::string& last = lines.back();

    // Format: "crumb:<ms> <cat> <subj>"
    auto subj_start = last.rfind(' ');
    REQUIRE(subj_start != std::string::npos);
    auto cat_start = last.rfind(' ', subj_start - 1);
    REQUIRE(cat_start != std::string::npos);

    std::string category = last.substr(cat_start + 1, subj_start - cat_start - 1);
    std::string subject = last.substr(subj_start + 1);

    REQUIRE(category.size() <= 7);
    REQUIRE(category == std::string(category.size(), 'C'));
    REQUIRE(subject.size() <= 59);
    REQUIRE(subject == std::string(subject.size(), 'S'));
}

TEST_CASE_METHOD(CrashTestFixture,
                 "Crash: parser surfaces breadcrumbs, event target, and heap snapshot",
                 "[telemetry][crash]") {
    // Synthesize a crash file exercising every post-v0.99.31 field. This is
    // the contract that telemetry ingestion, crash_reporter rendering, and
    // offline analysis all depend on — a silent parser regression shows up
    // as missing fields on the dashboard, not a build break.
    write_crash_file("signal:11\n"
                     "name:SIGSEGV\n"
                     "version:0.99.99\n"
                     "timestamp:1707350400\n"
                     "uptime:42\n"
                     "bt:0x400abc\n"
                     "event_target:0x7fc0d2a8\n"
                     "event_original_target:0x7fc0d300\n"
                     "event_code:29\n"
                     "heap_snapshot_age_ms:8217\n"
                     "heap_rss_kb:38400\n"
                     "heap_vsz_kb:102400\n"
                     "heap_arena_kb:40960\n"
                     "heap_used_kb:38912\n"
                     "heap_free_kb:2048\n"
                     "heap_mmap_kb:512\n"
                     "lv_heap_total_kb:512\n"
                     "lv_heap_used_pct:88\n"
                     "lv_heap_frag_pct:31\n"
                     "lv_heap_free_biggest_kb:14\n"
                     "crumb:1000 boot v0.99.99\n"
                     "crumb:5200 nav home\n"
                     "crumb:5210 xml home_card\n");

    auto result = crash_handler::read_crash_file(crash_path());
    REQUIRE_FALSE(result.is_null());

    REQUIRE(result["event_target"] == "0x7fc0d2a8");
    REQUIRE(result["event_original_target"] == "0x7fc0d300");
    REQUIRE(result["event_code"] == 29);

    REQUIRE(result["heap_snapshot_age_ms"] == 8217);
    REQUIRE(result["heap_rss_kb"] == 38400);
    REQUIRE(result["heap_arena_kb"] == 40960);
    REQUIRE(result["heap_used_kb"] == 38912);
    REQUIRE(result["lv_heap_used_pct"] == 88);
    REQUIRE(result["lv_heap_frag_pct"] == 31);
    REQUIRE(result["lv_heap_free_biggest_kb"] == 14);

    REQUIRE(result.contains("breadcrumbs"));
    REQUIRE(result["breadcrumbs"].is_array());
    REQUIRE(result["breadcrumbs"].size() == 3);
    REQUIRE(result["breadcrumbs"][0] == "1000 boot v0.99.99");
    REQUIRE(result["breadcrumbs"][1] == "5200 nav home");
    REQUIRE(result["breadcrumbs"][2] == "5210 xml home_card");
}
