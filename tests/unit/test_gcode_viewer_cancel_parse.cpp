// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The viewer's background load parses the whole file in a `while (getline)`
// loop. That loop used to run to completion regardless of cancellation — the
// only check happened AFTER the parse (and after the 3D geometry build).
//
// cancel_build() joins that thread, and it is called from start_build(), which
// runs on the MAIN thread when the user opens a different file. So switching
// files blocked the LVGL loop until the previous file had finished parsing:
// seconds for a multi-megabyte print on a 2-core board, on top of the
// layer-cache stall from the same report (bundle C2CP6ZAW).
//
// The contract pinned here: starting a new load while a large parse is in
// flight must return promptly, NOT after the in-flight parse completes.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

/// Locates one of the repo's large test prints, falling back to a generated
/// file. The shipped assets are the faithful input, but the test must not
/// silently pass just because it was run from a different working directory —
/// so a miss synthesises an equivalently long file rather than skipping.
struct LargeGCode {
    fs::path path;
    bool generated = false;

    LargeGCode() {
        // Largest first, and deliberately so. The parser runs at roughly 80MB/s
        // here (4.5MB measured at 56ms), so anything under ~25MB finishes too
        // fast to tell an interrupted parse from one that simply completed —
        // the full_ms guard below rejects such a file rather than passing
        // vacuously. On a desktop should_use_gcode_streaming() is false (its
        // threshold scales with available RAM), so these take the full-load
        // path, which is the one with the cancellable getline loop.
        static const char* CANDIDATES[] = {
            "assets/test_gcodes/Night Spirit_v1_2_og.gcode",
            "assets/test_gcodes/vaso_voronoi_V2_abajo.gcode",
            "assets/test_gcodes/Low poly vase v1.1 flat top.gcode",
            "assets/test_gcodes/Benchbin_MK4_MMU3.gcode",
        };
        for (const char* c : CANDIDATES) {
            std::error_code ec;
            if (fs::exists(c, ec)) {
                path = c;
                return;
            }
        }

        // Synthesised fallback: ~1.5M movement lines. Line count, not byte
        // count, is what the test depends on — enough that the parse is long
        // enough to interrupt measurably (see the full_ms guard).
        generated = true;
        path = fs::temp_directory_path() / "helix_cancel_parse_big.gcode";
        std::ofstream o(path);
        o << ";FLAVOR:Marlin\n;Generated for cancel-parse test\n";
        for (int layer = 0; layer < 1500; ++layer) {
            o << ";LAYER:" << layer << "\nG1 Z" << (0.2 * layer) << "\n";
            for (int i = 0; i < 1000; ++i) {
                o << "G1 X" << (i % 200) << " Y" << (i % 150) << " E" << (i * 0.01) << "\n";
            }
        }
    }

    ~LargeGCode() {
        if (generated) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

/// A trivially small file, used as the *second* load so that what we time is
/// the cancel-and-join of the first, not the cost of the new parse.
struct TinyGCode {
    fs::path path = fs::temp_directory_path() / "helix_cancel_parse_tiny.gcode";

    TinyGCode() {
        std::ofstream o(path);
        o << ";LAYER:0\nG1 Z0.2\nG1 X10 Y10 E1\nG1 X20 Y20 E2\n";
    }
    ~TinyGCode() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

std::atomic<int> g_loads_finished{0};

void on_load_done(lv_obj_t*, void*, bool) {
    g_loads_finished.fetch_add(1);
}

} // namespace

/// Pumps LVGL until `done` or the real-time budget expires, returning true if
/// `done` became satisfied.
///
/// The real sleep is load-bearing. process_lvgl() advances LVGL's tick with
/// lv_tick_inc() — VIRTUAL time — and returns immediately, so a bare
/// `while (!done) process_lvgl(20)` spins through a nominal two-minute budget in
/// milliseconds and never yields to the parse thread it is waiting on.
template <typename Fixture, typename Pred>
static bool pump_until(Fixture& fx, Pred done, milliseconds budget) {
    const auto deadline = steady_clock::now() + budget;
    while (steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        fx.process_lvgl(5);
        std::this_thread::sleep_for(milliseconds(5));
    }
    return done();
}

TEST_CASE_METHOD(LVGLTestFixture, "gcode viewer cancels an in-flight parse promptly",
                 "[gcode][viewer][threading][slow]") {
    LargeGCode big;
    TinyGCode tiny;

    lv_obj_t* viewer = ui_gcode_viewer_create(lv_screen_active());
    REQUIRE(viewer != nullptr);

    g_loads_finished.store(0);
    ui_gcode_viewer_set_load_callback(viewer, on_load_done, nullptr);

    // --- Calibrate: how long does a FULL load of this file take? -------------
    // Derived, not hardcoded: the threshold below is a fraction of the measured
    // parse so the test stays meaningful on a fast desktop and a slow ARM box.
    const auto full_start = steady_clock::now();
    ui_gcode_viewer_load_file(viewer, big.path.string().c_str());

    // The completion callback is delivered on the main thread, so pump until it
    // lands. Bounded so a hang fails the test instead of wedging the suite.
    const bool loaded =
        pump_until(*this, [] { return g_loads_finished.load() > 0; }, milliseconds(60000));
    const auto full_ms = duration_cast<milliseconds>(steady_clock::now() - full_start).count();
    REQUIRE(loaded);

    REQUIRE(g_loads_finished.load() >= 1);

    // If the parse is too quick, a cancel cannot be distinguished from waiting
    // it out, and the test would pass against the bug. Fail loudly instead.
    INFO("full parse took " << full_ms << "ms (file: " << big.path.string() << ")");
    REQUIRE(full_ms > 300);

    // --- Now start that same large parse and interrupt it -------------------
    ui_gcode_viewer_clear(viewer);
    g_loads_finished.store(0);

    ui_gcode_viewer_load_file(viewer, big.path.string().c_str());

    // Let the worker get properly into the getline loop. Deliberately short
    // relative to full_ms so the parse is certainly still running.
    std::this_thread::sleep_for(milliseconds(std::min<long long>(150, full_ms / 4)));

    // This is the measurement: load_file -> start_build -> cancel_build -> join.
    // With no in-loop cancellation check the join waits out the rest of the
    // large parse; with it, the worker notices within CANCEL_POLL_LINES.
    const auto cancel_start = steady_clock::now();
    ui_gcode_viewer_load_file(viewer, tiny.path.string().c_str());
    const auto cancel_ms = duration_cast<milliseconds>(steady_clock::now() - cancel_start).count();

    INFO("cancel took " << cancel_ms << "ms vs full parse " << full_ms << "ms");

    // Generous margin: the point is orders-of-magnitude, not a tight bound. The
    // bug produces ~the remainder of full_ms; the fix produces near-zero.
    CHECK(cancel_ms < full_ms / 3);

    // And the viewer is left usable, holding the file we actually asked for.
    CHECK(pump_until(*this, [] { return g_loads_finished.load() > 0; }, milliseconds(20000)));
    CHECK(ui_gcode_viewer_has_content(viewer));

    ui_gcode_viewer_clear(viewer);
    lv_obj_delete(viewer);
    process_lvgl(50);
}
