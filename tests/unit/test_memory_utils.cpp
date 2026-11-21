// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_memory_utils.cpp
 * @brief Unit tests for memory utility functions
 *
 * Tests the memory checking functions used to determine if G-code
 * rendering is safe given current system memory and file sizes.
 */

#include "memory_utils.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// is_gcode_2d_streaming_safe_impl Tests
//
// The gate prices what a streaming render allocates: the layer index (one
// 40-byte gcode::StreamingLayerEntry per layer, one layer per 500 bytes of
// G-code), the layer cache at this RAM tier's budget, an ARGB8888 ghost buffer
// the size of the display, a 3MB margin, and — only when the G-code cache
// directory is RAM-backed — the downloaded file itself.
//
// Expected totals below are computed by hand rather than from the gate's own
// constants: a test that recomputes the formula agrees with any formula.
// ============================================================================

namespace {

/// 128MB board (Elegoo Centauri Carbon class): constrained tier, 2MB cache.
constexpr size_t kConstrainedTotalKb = 128 * 1024;
/// 384MB board: normal tier, 16MB cache.
constexpr size_t kNormalTotalKb = 384 * 1024;
/// 1GB board: good tier, 32MB cache.
constexpr size_t kGoodTotalKb = 1024 * 1024;

/// Whether the resolved G-code cache directory is tmpfs/ramfs.
constexpr bool kOnFlash = false;
constexpr bool kOnTmpfs = true;

} // namespace

TEST_CASE("2D streaming safe: small file with plenty of RAM", "[memory][streaming]") {
    // 1MB file, 64MB available, 800x480 display, 1GB board.
    // index 81 + cache 32768 + ghost 1500 + margin 3072 = 37421KB needed.
    size_t file_size = 1 * 1024 * 1024; // 1MB
    size_t available_kb = 64 * 1024;    // 64MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kGoodTotalKb, display_width,
                                            display_height, kOnFlash));
}

TEST_CASE("2D streaming safe: large file with plenty of RAM", "[memory][streaming]") {
    // 50MB file, 128MB available, 800x480 display, 384MB board.
    // index 4095 + cache 16384 + ghost 1500 + margin 3072 = 25051KB needed.
    size_t file_size = 50 * 1024 * 1024; // 50MB
    size_t available_kb = 128 * 1024;    // 128MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kNormalTotalKb, display_width,
                                            display_height, kOnFlash));
}

TEST_CASE("2D streaming safe: AD5M typical scenario", "[memory][streaming]") {
    // 12.8MB file (real print), 38MB available, 800x480, 47MB board caching to
    // /data (real storage).
    // index 1023 + cache 2048 + ghost 1500 + margin 3072 = 7643KB needed.
    size_t file_size = 12800 * 1024; // 12.8MB
    size_t available_kb = 38 * 1024; // 38MB
    size_t total_kb = 47 * 1024;     // 47MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, total_kb, display_width,
                                            display_height, kOnFlash));
}

TEST_CASE("2D streaming unsafe: insufficient RAM for requirements", "[memory][streaming]") {
    // 10MB file, only 4MB available, 800x480 display.
    // index 819 + cache 2048 + ghost 1500 + margin 3072 = 7439KB needed.
    size_t file_size = 10 * 1024 * 1024; // 10MB
    size_t available_kb = 4 * 1024;      // 4MB (very constrained)
    int display_width = 800;
    int display_height = 480;

    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                                  display_width, display_height, kOnFlash));
}

TEST_CASE("2D streaming: larger display increases ghost buffer requirement",
          "[memory][streaming]") {
    // Same file, same RAM, but 1920x1080 display.
    // Ghost buffer: 1920 * 1080 * 4 = 8100KB vs 1500KB for 800x480.
    size_t file_size = 5 * 1024 * 1024; // 5MB
    size_t available_kb = 10 * 1024;    // 10MB
    int small_width = 800;
    int small_height = 480;
    int large_width = 1920;
    int large_height = 1080;

    // Small display fits: index 409 + cache 2048 + ghost 1500 + margin 3072 = 7029KB.
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                            small_width, small_height, kOnFlash));

    // Large display does not: same terms with ghost 8100 = 13629KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                                  large_width, large_height, kOnFlash));
}

TEST_CASE("2D streaming: layer index scales with file size", "[memory][streaming]") {
    // Verify that larger files require more memory due to layer index
    size_t available_kb = 8 * 1024; // 8MB available
    int display_width = 800;
    int display_height = 480;

    // 1MB file: index 81 + cache 2048 + ghost 1500 + margin 3072 = 6701KB.
    size_t small_file = 1 * 1024 * 1024;
    REQUIRE(is_gcode_2d_streaming_safe_impl(small_file, available_kb, kConstrainedTotalKb,
                                            display_width, display_height, kOnFlash));

    // 100MB file: index 8191 + cache 2048 + ghost 1500 + margin 3072 = 14811KB.
    size_t large_file = 100 * 1024 * 1024;
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(large_file, available_kb, kConstrainedTotalKb,
                                                  display_width, display_height, kOnFlash));
}

TEST_CASE("2D streaming: the layer index is priced at the real entry size",
          "[memory][streaming][edge]") {
    // 100MB of G-code is 209715 layers by the 1-per-500-bytes estimate, so the
    // index alone is 209715 * 40 / 1024 = 8191KB. Total on a constrained board:
    // 8191 + cache 2048 + ghost 1500 + margin 3072 = 14811KB.
    size_t file_size = 100 * 1024 * 1024;
    int display_width = 800;
    int display_height = 480;

    // Exactly at the boundary is rejected (the gate uses > not >=).
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 14811, kConstrainedTotalKb,
                                                  display_width, display_height, kOnFlash));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 14812, kConstrainedTotalKb, display_width,
                                            display_height, kOnFlash));

    // A per-entry price below sizeof(StreamingLayerEntry) authorizes this file
    // at 12000KB available; the real index does not fit there.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 12000, kConstrainedTotalKb,
                                                  display_width, display_height, kOnFlash));
}

TEST_CASE("2D streaming: the layer cache is priced at this device's budget tier",
          "[memory][streaming][edge]") {
    // Zero-size file: no index, so the cache budget is the only term that moves
    // between tiers. Ghost 1500 + margin 3072 = 4572KB is common to all three.
    size_t file_size = 0;
    int display_width = 800;
    int display_height = 480;

    // Constrained (2MB cache): 4572 + 2048 = 6620KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 6620, kConstrainedTotalKb,
                                                  display_width, display_height, kOnFlash));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 6621, kConstrainedTotalKb, display_width,
                                            display_height, kOnFlash));

    // Normal (16MB cache): 4572 + 16384 = 20956KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 20956, kNormalTotalKb, display_width,
                                                  display_height, kOnFlash));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 20957, kNormalTotalKb, display_width,
                                            display_height, kOnFlash));

    // Good (32MB cache): 4572 + 32768 = 37340KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 37340, kGoodTotalKb, display_width,
                                                  display_height, kOnFlash));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 37341, kGoodTotalKb, display_width,
                                            display_height, kOnFlash));
}

TEST_CASE("2D streaming: a RAM-backed cache directory charges the whole file",
          "[memory][streaming][edge]") {
    // 10MB file on a constrained board, 800x480:
    // index 819 + cache 2048 + ghost 1500 + margin 3072 = 7439KB on storage.
    // On tmpfs the 10240KB file is memory too: 17679KB.
    size_t file_size = 10 * 1024 * 1024;
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 7440, kConstrainedTotalKb, display_width,
                                            display_height, kOnFlash));
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 7440, kConstrainedTotalKb,
                                                  display_width, display_height, kOnTmpfs));

    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 17679, kConstrainedTotalKb,
                                                  display_width, display_height, kOnTmpfs));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 17680, kConstrainedTotalKb, display_width,
                                            display_height, kOnTmpfs));
}

TEST_CASE("2D streaming: 6MB file on a 128MB board with a tmpfs cache directory",
          "[memory][streaming][edge]") {
    // 480x272 panel, 11976KB available, cache directory on tmpfs:
    // index 487 + cache 2048 + ghost 510 + margin 3072 + spill 6095 = 12212KB,
    // which does not fit. The same file cached on real storage costs 6117KB and
    // streams fine — flash devices must keep the preview.
    size_t file_size = 6241553;
    size_t available_kb = 11976;
    int display_width = 480;
    int display_height = 272;

    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                                  display_width, display_height, kOnTmpfs));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                            display_width, display_height, kOnFlash));

    // Boundaries either way, so every term is pinned.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 12212, kConstrainedTotalKb,
                                                  display_width, display_height, kOnTmpfs));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 12213, kConstrainedTotalKb, display_width,
                                            display_height, kOnTmpfs));
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 6117, kConstrainedTotalKb,
                                                  display_width, display_height, kOnFlash));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 6118, kConstrainedTotalKb, display_width,
                                            display_height, kOnFlash));
}

TEST_CASE("2D streaming: zero file size", "[memory][streaming][edge]") {
    // No layer index and no spill for an empty file: cache 2048 + ghost 1500 +
    // margin 3072 = 6620KB, tmpfs or not.
    size_t file_size = 0;
    size_t available_kb = 7 * 1024; // 7MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                            display_width, display_height, kOnFlash));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, kConstrainedTotalKb,
                                            display_width, display_height, kOnTmpfs));
}

// ============================================================================
// MemoryInfo Tests
// ============================================================================

TEST_CASE("MemoryInfo: is_constrained threshold", "[memory]") {
    MemoryInfo info;

    // Below 64MB available = low memory
    info.available_kb = 63 * 1024;
    REQUIRE(info.is_low_memory());

    // At 64MB available = not low memory
    info.available_kb = 64 * 1024;
    REQUIRE_FALSE(info.is_low_memory());

    // Above 64MB available = not low memory
    info.available_kb = 128 * 1024;
    REQUIRE_FALSE(info.is_low_memory());
}

TEST_CASE("MemoryInfo: available_mb conversion", "[memory]") {
    MemoryInfo info;

    info.available_kb = 64 * 1024; // 64MB
    REQUIRE(info.available_mb() == 64);

    info.available_kb = 38 * 1024; // 38MB (AD5M typical)
    REQUIRE(info.available_mb() == 38);

    info.available_kb = 1024; // 1MB
    REQUIRE(info.available_mb() == 1);
}

// ============================================================================
// oom_score_adj Tests
//
// helix-launcher.sh exports HELIX_OOM_SCORE_ADJ only when Klipper is co-hosted;
// helix-screen applies it to itself so that under memory pressure the kernel
// kills the UI (which helix-watchdog restarts) instead of Klipper (which
// cannot be restarted mid-print).
// ============================================================================

TEST_CASE("parse_oom_score_adj: absent or empty value is not applicable", "[memory][oom]") {
    int adj = -12345;
    REQUIRE_FALSE(parse_oom_score_adj(nullptr, adj));
    REQUIRE_FALSE(parse_oom_score_adj("", adj));
    REQUIRE(adj == -12345); // untouched
}

TEST_CASE("parse_oom_score_adj: zero is the documented disable spelling", "[memory][oom]") {
    int adj = -12345;
    REQUIRE_FALSE(parse_oom_score_adj("0", adj));
    REQUIRE(adj == -12345);
}

TEST_CASE("parse_oom_score_adj: accepts the launcher default", "[memory][oom]") {
    int adj = 0;
    REQUIRE(parse_oom_score_adj("300", adj));
    REQUIRE(adj == 300);
}

TEST_CASE("parse_oom_score_adj: accepts a negative adjustment", "[memory][oom]") {
    int adj = 0;
    REQUIRE(parse_oom_score_adj("-500", adj));
    REQUIRE(adj == -500);
}

TEST_CASE("parse_oom_score_adj: clamps to the kernel range", "[memory][oom]") {
    int adj = 0;
    REQUIRE(parse_oom_score_adj("2000", adj));
    REQUIRE(adj == 1000);

    REQUIRE(parse_oom_score_adj("-2000", adj));
    REQUIRE(adj == -1000);
}

TEST_CASE("parse_oom_score_adj: rejects non-numeric and trailing garbage", "[memory][oom]") {
    int adj = -12345;
    REQUIRE_FALSE(parse_oom_score_adj("abc", adj));
    REQUIRE_FALSE(parse_oom_score_adj("300x", adj));
    REQUIRE_FALSE(parse_oom_score_adj("+", adj));
    REQUIRE(adj == -12345);
}

TEST_CASE("write_oom_score_adj: writes the value verbatim", "[memory][oom]") {
    auto path = std::filesystem::temp_directory_path() /
                ("helix_oom_score_adj_" + std::to_string(::getpid()));

    REQUIRE(write_oom_score_adj(300, path.string().c_str()));

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string contents;
    std::getline(in, contents);
    in.close();
    std::filesystem::remove(path);

    REQUIRE(contents == "300");
}

TEST_CASE("write_oom_score_adj: reports failure on an unwritable path", "[memory][oom]") {
    // A path under a file (not a directory) can never be opened for writing.
    auto base =
        std::filesystem::temp_directory_path() / ("helix_oom_notdir_" + std::to_string(::getpid()));
    std::ofstream(base) << "x";

    auto bogus = base / "oom_score_adj";
    REQUIRE_FALSE(write_oom_score_adj(300, bogus.string().c_str()));

    std::filesystem::remove(base);
}

TEST_CASE("resonance memory check refuses on headroom, warns on total", "[memory][resonance]") {
    auto mb = [](size_t total, size_t avail, size_t swap) {
        helix::MemoryInfo m;
        m.total_kb = total * 1024;
        m.available_kb = avail * 1024;
        m.swap_free_kb = swap * 1024;
        return m;
    };
    using helix::ResonanceMemory;
    // CC1 as measured: 114 MB, no swap, 25 MB available -> wedged klippy.
    CHECK(helix::resonance_memory_check(mb(114, 25, 0)) == ResonanceMemory::REFUSE);
    // AD5M as measured: less RAM, but swap brings headroom to ~74 MB -> survived.
    CHECK(helix::resonance_memory_check(mb(107, 10, 64)) == ResonanceMemory::WARN);
    // Swap counts; the floor is exclusive at 64.
    CHECK(helix::resonance_memory_check(mb(150, 30, 33)) == ResonanceMemory::REFUSE);
    CHECK(helix::resonance_memory_check(mb(150, 30, 34)) == ResonanceMemory::WARN);
    // A big host with plenty free starts without asking.
    CHECK(helix::resonance_memory_check(mb(1024, 600, 0)) == ResonanceMemory::OK);
    // A big host that is nearly out is still refused: the floor is about headroom.
    CHECK(helix::resonance_memory_check(mb(1024, 40, 0)) == ResonanceMemory::REFUSE);
    // Unreadable memory info never blocks.
    CHECK(helix::resonance_memory_check(helix::MemoryInfo{}) == ResonanceMemory::OK);
}

// ============================================================================
// Heap trim rate limit
// ============================================================================

TEST_CASE("Heap trim: the first trim is always due, later ones hold a 30 s floor",
          "[memory][heap-trim]") {
    struct Case {
        uint32_t now;
        std::optional<uint32_t> last;
        bool want;
    };
    const Case cases[] = {
        {0, std::nullopt, true},
        {5000, std::nullopt, true},
        {10000, 10000, false},
        {39999, 10000, false},
        {40000, 10000, true},
        {100000, 10000, true},
        // The tick wraps at 2^32 ms; elapsed time is measured across it.
        {0x00001000u, 0xFFFFF000u, false},
        {0x00005000u, 0xFFFFE000u, false},
        {0x00007530u, 0x00000000u, true},
        {0x00007000u, 0xFFFF9000u, true},
        {0x00001000u, 0xFFFF0000u, true},
        {0xFFFFF100u, 0xFFFFF000u, false},
    };
    for (const auto& c : cases) {
        INFO("now=" << c.now << " last=" << (c.last ? std::to_string(*c.last) : "none"));
        CHECK(heap_trim_due(c.now, c.last) == c.want);
    }
}

TEST_CASE("Heap trim: release_free_heap trims exactly where the libc can", "[memory][heap-trim]") {
#if defined(__GLIBC__) && !defined(__UCLIBC__)
    CHECK(heap_trim_supported());
#else
    CHECK_FALSE(heap_trim_supported());
#endif
    CHECK(release_free_heap() == heap_trim_supported());
}

// ============================================================================
// Per-process memory for telemetry [memory][proc_stat]
// ============================================================================

TEST_CASE("parse_proc_stat reads comm, major faults and resident pages", "[memory][proc_stat]") {
    ProcStat st;
    // A comm may hold spaces and parentheses; it ends at the LAST ')'.
    REQUIRE(parse_proc_stat("812 (my (odd) name) S 1 812 812 0 -1 4194560 2000 0 37 0 50 20 0 0 20 "
                            "0 3 0 1500 123456789 9000 18446744073709551615",
                            st));
    CHECK(st.comm == "my (odd) name");
    CHECK(st.major_faults == 37);
    CHECK(st.rss_pages == 9000);

    CHECK_FALSE(parse_proc_stat("", st));
    CHECK_FALSE(parse_proc_stat("812 (truncated) S 1 812", st));
    CHECK_FALSE(parse_proc_stat(
        "812 no-parens S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21", st));
}

TEST_CASE("telemetry_process_name passes only known program names", "[memory][proc_stat]") {
    struct Row {
        const char* comm;
        const char* want;
    };
    const Row rows[] = {
        {"helix-screen", "helix-screen"},
        {"moonraker", "moonraker"},
        {"ustreamer", "ustreamer"},
        {"KlipperScreen", "KlipperScreen"},
        {"lmd", "lmd"},
        // The kernel keeps 15 characters of a comm; a longer known name still matches.
        {"systemd-journal", "systemd-journald"},
        {"python3", "python3"},
        {"python3.11", "python3.11"},
        // Free text a binary's builder chose: never sent as is.
        {"jsmith-sync", "other"},
        {"printer3-cam", "other"},
        {"python_jsmith", "other"},
        {"helix-screen2", "other"},
        {"kworker/0:1H", "other"},
        {"", "other"},
    };
    for (const Row& r : rows) {
        INFO("comm: " << r.comm);
        CHECK(telemetry_process_name(r.comm) == r.want);
    }
}

TEST_CASE("telemetry_process_name names interpreter services from a fixed allowlist",
          "[memory][proc_stat]") {
    using namespace std::string_literals;
    struct Row {
        const char* comm;
        std::string cmdline;
        const char* want;
    };
    const Row rows[] = {
        {"python3",
         "/home/pi/klippy-env/bin/python\0/home/pi/klipper/klippy/klippy.py\0"
         "/home/pi/printer_data/config/printer.cfg\0-l\0/tmp/klippy.log\0"s,
         "klipper"},
        {"python",
         "/home/pi/moonraker-env/bin/python\0/home/pi/moonraker/moonraker/moonraker.py\0"
         "-d\0/home/pi/printer_data\0"s,
         "moonraker"},
        {"python3", "python3\0-m\0moonraker\0"s, "moonraker"},
        {"python3",
         "/home/pi/moonraker-obico-env/bin/python3\0-m\0moonraker_obico.app\0"
         "-c\0/home/pi/printer_data/config/moonraker-obico.cfg\0"s,
         "obico"},
        {"python3", "/usr/bin/python3\0/home/pi/KlipperScreen/screen.py\0"s, "klipperscreen"},
        {"bash", "/bin/bash\0/usr/local/bin/crowsnest\0-c\0/home/pi/crowsnest.conf\0"s,
         "crowsnest"},
        {"python3", "python3\0-m\0octoapp\0"s, "octoapp"},
        {"python3.11", "/opt/spoolman/.venv/bin/python\0-m\0uvicorn\0spoolman.main:app\0"s,
         "spoolman"},
        // Not on the allowlist: the bare interpreter, never an argument.
        {"python3", "python3\0/home/pi/secret_project/run.py\0--token\0abc\0"s, "python3"},
        // argv[0] alone names no service, even a venv named after one.
        {"python3", "/home/pi/klippy-env/bin/python3\0"s, "python3"},
        {"node", "node\0/srv/app/server.js\0"s, "node"},
        {"sh", ""s, "sh"},
        // Not an interpreter: its comm, whatever the arguments say.
        {"ustreamer", "/usr/bin/ustreamer\0--host\0moonraker\0"s, "ustreamer"},
        {"helix-screen", "/opt/helix/helix-screen\0klippy.py\0"s, "helix-screen"},
        {"jsmith-sync", "/home/jsmith/bin/jsmith-sync\0moonraker\0"s, "other"},
    };
    for (const Row& r : rows) {
        INFO("comm: " << r.comm);
        CHECK(telemetry_process_name(r.comm, r.cmdline) == r.want);
    }
}

namespace {

/// A fake /proc with one stat file per pid.
struct FakeProc {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("helix_fake_proc_" + std::to_string(::getpid()));
    FakeProc() {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "self");
        std::filesystem::create_directories(root / "sys");
    }
    ~FakeProc() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void add(const std::string& pid, const std::string& comm, long rss_pages, long majflt = 0) {
        std::filesystem::create_directories(root / pid);
        std::ofstream(root / pid / "stat")
            << pid << " (" << comm << ") S 1 1 1 0 -1 0 10 0 " << majflt
            << " 0 5 5 0 0 20 0 1 0 100 1000000 " << rss_pages << " 18446744073709551615\n";
    }
};

} // namespace

TEST_CASE("top_processes_by_rss lists the largest resident processes, largest first",
          "[memory][proc_stat]") {
    FakeProc proc;
    proc.add("101", "klippy", 51200);                       // 200 MB at 4 KB pages
    proc.add("102", "moonraker", 25600);                    // 100 MB
    proc.add("103", "helix-screen", 12800);                 // 50 MB
    proc.add("104", "jsmith-cam", 2560);                    // 10 MB
    proc.add("2", "kthreadd", 0);                           // kernel threads hold no RSS
    std::filesystem::create_directories(proc.root / "105"); // exited between readdir and open
    proc.add("106", "python3", 1280);                       // 5 MB
    std::ofstream(proc.root / "106" / "cmdline") << std::string("python3\0-m\0moonraker\0", 21);

    const auto top = top_processes_by_rss(3, proc.root.c_str(), 4);
    REQUIRE(top.size() == 3);
    CHECK(top[0].name == "klippy");
    CHECK(top[0].rss_mb == 200);
    CHECK(top[1].name == "moonraker");
    CHECK(top[1].rss_mb == 100);
    CHECK(top[2].name == "helix-screen");
    CHECK(top[2].rss_mb == 50);

    // Fewer processes than asked for: every one with resident memory.
    const auto all = top_processes_by_rss(10, proc.root.c_str(), 4);
    REQUIRE(all.size() == 5);
    CHECK(all[3].name == "other");     // not a known program
    CHECK(all[4].name == "moonraker"); // named from its cmdline, not "python3"
    CHECK(top_processes_by_rss(3, "/nonexistent-proc-root", 4).empty());
}

TEST_CASE("read_self_major_faults reads the stat file's majflt", "[memory][proc_stat]") {
    FakeProc proc;
    proc.add("self", "helix-screen", 100, 4242);
    CHECK(read_self_major_faults((proc.root / "self" / "stat").c_str()) ==
          std::optional<uint64_t>(4242));
    CHECK_FALSE(read_self_major_faults("/nonexistent-proc-root/self/stat").has_value());
#ifdef __linux__
    CHECK(read_self_major_faults().has_value());
#endif
}
