// SPDX-License-Identifier: GPL-3.0-or-later

// The crash handler's breadcrumb ring takes crumbs from any thread. The slot
// protocol that makes that safe is src/system/breadcrumb_slot.h; the ring is
// driven through its public API.

#include "../test_helpers/breadcrumb_capture.h"
#include "src/system/breadcrumb_slot.h"
#include "system/crash_handler.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using crash_handler::breadcrumb::note;
namespace slot_protocol = helix::crumbs;

namespace {

/// A crumb's text with each half copied in and terminated.
slot_protocol::CrumbText crumb_text(const char* category, const char* subject) {
    slot_protocol::CrumbText text{};
    char* const chars = slot_protocol::crumb_chars(text);
    std::snprintf(chars, slot_protocol::kCrumbCategoryLen, "%s", category);
    std::snprintf(chars + slot_protocol::kCrumbCategoryLen, slot_protocol::kCrumbSubjectLen, "%s",
                  subject);
    return text;
}

std::vector<std::string> split_lines(const std::string& all) {
    std::vector<std::string> lines;
    size_t pos = 0;
    size_t nl = 0;
    while ((nl = all.find('\n', pos)) != std::string::npos) {
        lines.push_back(all.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

/// Parses a crumb line, `crumb:<ms> t<writer> s<writer> <n>` or the same without
/// the `crumb:` prefix, which is how the crash record keeps it. False for any
/// other shape, which is what a line that mixed two crumbs or cut one short
/// would have.
bool parse_tagged_crumb(std::string line, long& writer, long& number) {
    if (line.compare(0, 6, "crumb:") == 0) {
        line.erase(0, 6);
    }
    size_t p = 0;
    const auto digits = [&](long& out) {
        const size_t from = p;
        out = 0;
        while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
            out = out * 10 + (line[p] - '0');
            ++p;
        }
        return p > from;
    };
    const auto lit = [&](char c) {
        if (p < line.size() && line[p] == c) {
            ++p;
            return true;
        }
        return false;
    };
    long stamp = 0;
    long subject_writer = 0;
    if (!digits(stamp) || !lit(' ')) {
        return false;
    }
    if (!lit('t') || !digits(writer) || !lit(' ') || !lit('s') || !digits(subject_writer) ||
        !lit(' ')) {
        return false;
    }
    if (!digits(number) || p != line.size()) {
        return false;
    }
    return writer == subject_writer;
}

/// A ring dump read without Catch2, so it is safe off the main thread.
std::string dump_ring() {
    std::FILE* f = std::tmpfile();
    if (!f) {
        return {};
    }
    const int fd = ::fileno(f);
    crash_handler::breadcrumb::dump_to_fd(fd);
    ::lseek(fd, 0, SEEK_SET);
    std::string all;
    char chunk[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
        all.append(chunk, static_cast<size_t>(n));
    }
    std::fclose(f);
    return all;
}

/// A temporary crash file, removed with its directory afterwards.
class CrashFile {
  public:
    CrashFile()
        : dir_(std::filesystem::temp_directory_path() /
               ("helix_crumb_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(dir_);
    }
    ~CrashFile() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    CrashFile(const CrashFile&) = delete;
    CrashFile& operator=(const CrashFile&) = delete;
    [[nodiscard]] std::string path() const {
        return (dir_ / "crash.txt").string();
    }

  private:
    std::filesystem::path dir_;
};

/// The crumbs of a crash record, each checked to be a whole tagged crumb.
/// Returns how many came from writers other than @p other_than.
int check_record_crumbs(const nlohmann::json& result, long other_than) {
    REQUIRE(result.contains("breadcrumbs"));
    const auto& crumbs = result["breadcrumbs"];
    REQUIRE(crumbs.is_array());
    int others = 0;
    for (const auto& crumb : crumbs) {
        long writer = 0;
        long number = 0;
        const std::string line = crumb.get<std::string>();
        INFO("crumb: " << line);
        CHECK(parse_tagged_crumb(line, writer, number));
        others += writer != other_than;
    }
    return others;
}

} // namespace

// --- The slot protocol, on slots of the test's own.

TEST_CASE("A crumb is whole only once the writer that took its slot has sealed it",
          "[crash][breadcrumb][slot]") {
    slot_protocol::CrumbSlot slot{};
    const slot_protocol::CrumbText text = crumb_text("nav", "home");
    slot_protocol::CrumbText read{};
    uint32_t ts = 0;

    REQUIRE(slot_protocol::crumb_enter(slot, 0, 4));
    // A reader in here, as the crash handler would be, has nothing to print.
    CHECK_FALSE(slot_protocol::crumb_read(slot, read, ts));

    slot_protocol::crumb_store(slot, text, 500);
    CHECK_FALSE(slot_protocol::crumb_read(slot, read, ts));

    slot_protocol::crumb_seal(slot, 0);
    REQUIRE(slot_protocol::crumb_read(slot, read, ts));
    CHECK(ts == 500);
    CHECK(std::string(slot_protocol::crumb_category(read)) == "nav");
    CHECK(std::string(slot_protocol::crumb_subject(read)) == "home");
}

TEST_CASE("A slot stays with its writer until the crumb is whole, then passes to the next lap",
          "[crash][breadcrumb][slot]") {
    constexpr uint32_t kRingSize = 4;
    slot_protocol::CrumbSlot slot{};
    const slot_protocol::CrumbText first = crumb_text("nav", "first");
    const slot_protocol::CrumbText next = crumb_text("nav", "next");
    slot_protocol::CrumbText read{};
    uint32_t ts = 0;

    REQUIRE(slot_protocol::crumb_enter(slot, 0, kRingSize));
    // The crumb one lap on lands in the same slot, and cannot take it yet.
    CHECK_FALSE(slot_protocol::crumb_enter(slot, kRingSize, kRingSize));

    slot_protocol::crumb_store(slot, first, 500);
    slot_protocol::crumb_seal(slot, 0);
    REQUIRE(slot_protocol::crumb_read(slot, read, ts));
    CHECK(std::string(slot_protocol::crumb_subject(read)) == "first");

    REQUIRE(slot_protocol::crumb_enter(slot, kRingSize, kRingSize));
    // While the next crumb is being written a reader has nothing from the
    // slot: it is its writer's until the crumb is whole.
    CHECK_FALSE(slot_protocol::crumb_read(slot, read, ts));

    slot_protocol::crumb_store(slot, next, 600);
    slot_protocol::crumb_seal(slot, kRingSize);
    // Then it gives that crumb alone.
    REQUIRE(slot_protocol::crumb_read(slot, read, ts));
    CHECK(ts == 600);
    CHECK(std::string(slot_protocol::crumb_subject(read)) == "next");
}

TEST_CASE(
    "On the ring's first lap each slot takes its own crumb, and the marks wrap with the index",
    "[crash][breadcrumb][slot]") {
    constexpr uint32_t kRingSize = 4;
    std::vector<slot_protocol::CrumbSlot> ring(kRingSize);
    const slot_protocol::CrumbText text = crumb_text("t", "x");
    slot_protocol::CrumbText read{};
    uint32_t ts = 0;

    for (uint32_t index = 0; index < 3 * kRingSize; ++index) {
        INFO("index " << index);
        slot_protocol::CrumbSlot& slot = ring[index % kRingSize];
        REQUIRE(slot_protocol::crumb_enter(slot, index, kRingSize));
        slot_protocol::crumb_store(slot, text, index);
        slot_protocol::crumb_seal(slot, index);
    }
    // Each slot answers to the last crumb that took it.
    for (uint32_t s = 0; s < kRingSize; ++s) {
        INFO("slot " << s);
        REQUIRE(slot_protocol::crumb_read(ring[s], read, ts));
        CHECK(ts == s + 2 * kRingSize);
    }

    // The crumb index wraps at 2^32. The marks wrap with it, so the next crumb
    // of a slot is admitted behind the one a lap back even across the wrap.
    slot_protocol::CrumbSlot slot{};
    const uint32_t before_wrap = 0xFFFFFFFEu;
    const uint32_t after_wrap = before_wrap + kRingSize; // 2
    slot.seq = 2u * (before_wrap - kRingSize) + 2u;
    REQUIRE(slot_protocol::crumb_enter(slot, before_wrap, kRingSize));
    slot_protocol::crumb_store(slot, text, 7);
    slot_protocol::crumb_seal(slot, before_wrap);
    REQUIRE(slot_protocol::crumb_read(slot, read, ts));
    CHECK(ts == 7);

    REQUIRE(slot_protocol::crumb_enter(slot, after_wrap, kRingSize));
    CHECK_FALSE(slot_protocol::crumb_read(slot, read, ts));
    slot_protocol::crumb_store(slot, text, 9);
    slot_protocol::crumb_seal(slot, after_wrap);
    REQUIRE(slot_protocol::crumb_read(slot, read, ts));
    CHECK(ts == 9);
}

TEST_CASE("A slot left on an older crumb takes the next one; one holding a newer crumb does not",
          "[crash][breadcrumb][slot]") {
    constexpr uint32_t kRingSize = 4;
    slot_protocol::CrumbSlot slot{};
    const slot_protocol::CrumbText text = crumb_text("nav", "late");
    slot_protocol::CrumbText read{};
    uint32_t ts = 0;

    REQUIRE(slot_protocol::crumb_enter(slot, 0, kRingSize));
    slot_protocol::crumb_store(slot, text, 500);
    slot_protocol::crumb_seal(slot, 0);
    // Two laps on, the lap between never having reached this slot.
    REQUIRE(slot_protocol::crumb_enter(slot, 2 * kRingSize, kRingSize));
    slot_protocol::crumb_store(slot, text, 700);
    slot_protocol::crumb_seal(slot, 2 * kRingSize);

    // An index behind what the slot holds takes nothing, and the slot still
    // gives the crumb it holds.
    CHECK_FALSE(slot_protocol::crumb_enter(slot, kRingSize, kRingSize));
    CHECK_FALSE(slot_protocol::crumb_enter(slot, 2 * kRingSize + 1u, kRingSize));
    REQUIRE(slot_protocol::crumb_read(slot, read, ts));
    CHECK(ts == 700);
    // A lap past what it holds, the slot is free again.
    REQUIRE(slot_protocol::crumb_enter(slot, 3 * kRingSize, kRingSize));
}

// --- The ring through its public API.

TEST_CASE("Crumbs noted from several threads at once are each dumped whole",
          "[crash][breadcrumb][threading]") {
    constexpr int kWriters = 4;
    constexpr int kCrumbsPerWriter = 3000;
    // Whatever earlier cases left in the ring is pushed out first, so every
    // line a dump holds from here on is this case's own.
    for (int i = 0; i < 300; ++i) {
        note("t9", "s9", i);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> writers_done{false};
    std::atomic<int> torn_lines{0};
    std::atomic<long> dumps{0};
    std::mutex first_bad_mutex;
    std::string first_bad;

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([w, &start] {
            const std::string category = "t" + std::to_string(w);
            const std::string subject = "s" + std::to_string(w);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int n = 0; n < kCrumbsPerWriter; ++n) {
                note(category.c_str(), subject.c_str(), n);
            }
        });
    }
    std::thread reader([&] {
        while (!writers_done.load(std::memory_order_acquire)) {
            const std::string all = dump_ring();
            dumps.fetch_add(1);
            for (const std::string& line : split_lines(all)) {
                long writer = 0;
                long number = 0;
                if (parse_tagged_crumb(line, writer, number)) {
                    continue;
                }
                if (torn_lines.fetch_add(1) == 0) {
                    std::lock_guard<std::mutex> lock(first_bad_mutex);
                    first_bad = line;
                }
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (std::thread& writer : writers) {
        writer.join();
    }
    writers_done.store(true, std::memory_order_release);
    reader.join();

    // The reader read while the writers wrote, and found every line whole.
    REQUIRE(dumps.load() > 0);
    {
        INFO("first line that did not parse: " << first_bad);
        CHECK(torn_lines.load() == 0);
    }

    // With the writers joined, the ring holds the newest 256 crumbs, each once.
    std::set<std::pair<long, long>> seen;
    long listed = 0;
    for (const std::string& line : helix::capture_breadcrumb_lines()) {
        long writer = 0;
        long number = 0;
        INFO("line: " << line);
        REQUIRE(parse_tagged_crumb(line, writer, number));
        ++listed;
        CHECK(seen.emplace(writer, number).second);
    }
    CHECK(listed == 256);
}

// --- The crash report, taken while crumbs are being written.

TEST_CASE("Crash: a fault inside a crumb's own write still leaves a whole report",
          "[telemetry][crash][breadcrumb][subprocess]") {
    CrashFile crash_file;
    // A subject whose last character is the last of its page: reading past it
    // faults inside note(), after the slot is taken and before the crumb is
    // sealed.
    const long page_size = ::sysconf(_SC_PAGESIZE);
    void* const mapping = ::mmap(nullptr, static_cast<size_t>(2 * page_size),
                                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(mapping != MAP_FAILED);
    auto* const pages = static_cast<char*>(mapping);
    pages[page_size - 1] = 'x';
    REQUIRE(::mprotect(pages + page_size, static_cast<size_t>(page_size), PROT_NONE) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        crash_handler::install(crash_file.path());
        for (int i = 0; i < 300; ++i) {
            note("t9", "s9", i);
        }
        note("nav", pages + page_size - 1);
        _exit(99);
    }
    ::munmap(pages, static_cast<size_t>(2 * page_size));
    REQUIRE(pid > 0);

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 128 + SIGSEGV);

    const auto result = crash_handler::read_crash_file(crash_file.path());
    REQUIRE_FALSE(result.is_null());
    CHECK(result["signal"] == SIGSEGV);
    // The ring was full and every crumb in the report is whole. The crumb that
    // faulted took the oldest slot and was never sealed, so the report holds
    // the other 255 and skips that slot.
    CHECK(check_record_crumbs(result, 9) == 0);
    CHECK(result["breadcrumbs"].size() == 255);
}

TEST_CASE("Crash: a report is written while other threads are still noting crumbs",
          "[telemetry][crash][breadcrumb][subprocess]") {
    CrashFile crash_file;
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
    // ThreadSanitizer stops a forked child of a threaded process from starting
    // threads of its own.
    SKIP("ThreadSanitizer cannot start threads in a forked child");
#else
    pid_t pid = fork();
    if (pid == 0) {
        crash_handler::install(crash_file.path());
        for (int i = 0; i < 300; ++i) {
            note("t9", "s9", i);
        }
        std::atomic<bool> run{true};
        std::atomic<long> noted[2] = {{0}, {0}};
        const auto hammer = [&run, &noted](int w) {
            const std::string category = "t" + std::to_string(w);
            const std::string subject = "s" + std::to_string(w);
            for (long n = 0; run.load(std::memory_order_relaxed); ++n) {
                note(category.c_str(), subject.c_str(), n % 1000000);
                noted[w].fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread first(hammer, 0);
        std::thread second(hammer, 1);
        // Each has crumbs in the ring before the report is taken, and each goes
        // on noting while it is written.
        while (noted[0].load(std::memory_order_relaxed) < 300 ||
               noted[1].load(std::memory_order_relaxed) < 300) {
            std::this_thread::yield();
        }
        raise(SIGSEGV);
        run.store(false);
        first.join();
        second.join();
        _exit(99);
    }
    REQUIRE(pid > 0);

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 128 + SIGSEGV);

    const auto result = crash_handler::read_crash_file(crash_file.path());
    REQUIRE_FALSE(result.is_null());
    CHECK(result["signal"] == SIGSEGV);
    // The report holds the ring's whole crumbs, the writers' among them. A
    // slot a writer is inside while it is read is left out, and so is one a
    // writer laps before the dump reaches it, so a few may be missing.
    CHECK(check_record_crumbs(result, 9) > 0);
    CHECK(result["breadcrumbs"].size() >= 200);
#endif
}
