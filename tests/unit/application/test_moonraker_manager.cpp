// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_manager.cpp
 * @brief Unit tests for MoonrakerManager class
 *
 * Tests Moonraker client/API lifecycle, configuration, and notification queue.
 *
 * Note: MoonrakerManager has heavy dependencies (MoonrakerClient, MoonrakerAPI,
 * EmergencyStopOverlay, etc.) that require full LVGL initialization. These tests
 * focus on the configuration interface. Full initialization tests are done as
 * integration tests.
 */

#include "runtime_config.h"

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../catch_amalgamated.hpp"

// ============================================================================
// RuntimeConfig Tests (MoonrakerManager dependency)
// ============================================================================

TEST_CASE("MoonrakerManager uses RuntimeConfig for mock decisions", "[application][config]") {
    RuntimeConfig config;

    SECTION("Default is not mock mode") {
        REQUIRE_FALSE(config.should_mock_moonraker());
        REQUIRE_FALSE(config.should_use_test_files());
    }

    SECTION("Test mode enables mock Moonraker") {
        config.test_mode = true;
        REQUIRE(config.should_mock_moonraker());
        REQUIRE(config.should_use_test_files());
    }

    SECTION("Real Moonraker flag overrides mock") {
        config.test_mode = true;
        config.use_real_moonraker = true;
        REQUIRE_FALSE(config.should_mock_moonraker());
        // Note: should_use_test_files is controlled by use_real_files, not use_real_moonraker
        REQUIRE(config.should_use_test_files());
    }

    SECTION("Real files flag affects API mock") {
        config.test_mode = true;
        config.use_real_files = true;
        REQUIRE_FALSE(config.should_use_test_files());
        REQUIRE(config.should_mock_moonraker()); // Moonraker mock unaffected
    }
}

TEST_CASE("RuntimeConfig simulation speedup", "[application][config]") {
    RuntimeConfig config;

    REQUIRE(config.sim_speedup == 1.0);

    config.sim_speedup = 10.0;
    REQUIRE(config.sim_speedup == 10.0);

    config.sim_speedup = 0.5;
    REQUIRE(config.sim_speedup == 0.5);
}

TEST_CASE("RuntimeConfig mock_auto_start_print flag", "[application][config]") {
    RuntimeConfig config;

    REQUIRE_FALSE(config.mock_auto_start_print);

    config.mock_auto_start_print = true;
    REQUIRE(config.mock_auto_start_print);
}

TEST_CASE("RuntimeConfig mock_auto_history flag", "[application][config]") {
    RuntimeConfig config;

    REQUIRE_FALSE(config.mock_auto_history);

    config.mock_auto_history = true;
    REQUIRE(config.mock_auto_history);
}

TEST_CASE("RuntimeConfig mock_ams_gate_count", "[application][config]") {
    RuntimeConfig config;

    // Default is 4 gates
    REQUIRE(config.mock_ams_gate_count == 4);

    config.mock_ams_gate_count = 8;
    REQUIRE(config.mock_ams_gate_count == 8);
}

// ============================================================================
// Mid-Print Detection Tests (should_start_print_collector)
// ============================================================================
// Tests the logic that prevents "Preparing Print" from showing when the app
// starts while a print is already in progress.

#include "moonraker_manager.h"
#include "print_collector_arming.h"
#include "printer_state.h"

using namespace helix;

// ============================================================================
// HELIX_MOCK_PRINTER authoritative-over-saved-type contract
// ============================================================================
// PrinterDetector::auto_detect_and_save() short-circuits when a printer type is
// already persisted in config. Under HELIX_MOCK_PRINTER, MoonrakerManager::init()
// clears the saved type (gated strictly on the env var) BEFORE detection runs so
// the mock's reported identity re-resolves every launch. These tests pin that
// clear-vs-preserve behavior against a real Config without spinning up the full
// (LVGL-heavy) MoonrakerManager init path. They mirror exactly the production
// guard in moonraker_manager.cpp::init().

#include "config.h"
#include "wizard_config_paths.h"

namespace {

// Replays the production env-gated clear from MoonrakerManager::init(). Kept in
// lockstep with that block — if the seam moves, update both.
void apply_mock_printer_type_clear(Config& cfg) {
    if (std::getenv("HELIX_MOCK_PRINTER")) {
        const std::string type_path = cfg.df() + helix::wizard::PRINTER_TYPE;
        const std::string prev = cfg.get<std::string>(type_path, "");
        if (!prev.empty()) {
            cfg.set<std::string>(type_path, "");
        }
    }
}

// RAII helper: set HELIX_MOCK_PRINTER for the scope, restore prior value after.
struct ScopedMockPrinterEnv {
    std::string saved;
    bool had = false;
    explicit ScopedMockPrinterEnv(const char* value) {
        if (const char* prev = std::getenv("HELIX_MOCK_PRINTER")) {
            saved = prev;
            had = true;
        }
        if (value) {
            setenv("HELIX_MOCK_PRINTER", value, 1);
        } else {
            unsetenv("HELIX_MOCK_PRINTER");
        }
    }
    ~ScopedMockPrinterEnv() {
        if (had) {
            setenv("HELIX_MOCK_PRINTER", saved.c_str(), 1);
        } else {
            unsetenv("HELIX_MOCK_PRINTER");
        }
    }
};

} // namespace

TEST_CASE("HELIX_MOCK_PRINTER clears a stale saved printer type",
          "[application][mock_printer][regression]") {
    // No active printer set → df() routes to the "default" section; the test
    // only needs a single consistent path for the set/clear/get round-trip.
    Config cfg;
    const std::string type_path = cfg.df() + helix::wizard::PRINTER_TYPE;

    // Simulate a stale persisted type from a previous (non-mock) run.
    cfg.set<std::string>(type_path, "Voron 2.4");
    REQUIRE(cfg.get<std::string>(type_path, "") == "Voron 2.4");

    SECTION("Env set → saved type is cleared so detection re-resolves") {
        ScopedMockPrinterEnv env("ad5m");
        apply_mock_printer_type_clear(cfg);
        REQUIRE(cfg.get<std::string>(type_path, "") == "");
    }

    SECTION("Env unset → saved type is preserved (zero behavior change)") {
        ScopedMockPrinterEnv env(nullptr);
        apply_mock_printer_type_clear(cfg);
        REQUIRE(cfg.get<std::string>(type_path, "") == "Voron 2.4");
    }
}

TEST_CASE("HELIX_MOCK_PRINTER clear is a no-op when no type is saved",
          "[application][mock_printer][regression]") {
    Config cfg;
    const std::string type_path = cfg.df() + helix::wizard::PRINTER_TYPE;
    REQUIRE(cfg.get<std::string>(type_path, "") == "");

    ScopedMockPrinterEnv env("voron_24");
    apply_mock_printer_type_clear(cfg);
    REQUIRE(cfg.get<std::string>(type_path, "") == "");
}

TEST_CASE("should_start_print_collector - fresh print start", "[application][print_start]") {
    // Transition from STANDBY to PRINTING with 0% progress = fresh print start
    REQUIRE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                           PrintJobState::PRINTING, 0, true));
    // Non-initial transitions always start (user explicitly started a print)
    REQUIRE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                           PrintJobState::PRINTING, 0, false));
}

TEST_CASE("should_start_print_collector - reprint after restart with stale terminal progress",
          "[application][print_start][regression]") {
    // On-device bug: after an app restart with a just-completed print still in
    // print_stats, progress stays pinned at a stale 100% from the terminal
    // Complete state. The first user-started reprint then transitions
    // Complete(3) -> Printing(1) with progress=100%, initial=true. The old
    // unconditional mid-print-join skip fired here → collector skipped → NO
    // pre-print phase tracking on the reprint. A transition INTO printing from a
    // TERMINAL state (Complete/Cancelled/Error) is unambiguously a fresh
    // user-started print, so the skip must NOT apply regardless of stale
    // progress/duration.
    SECTION("Complete -> Printing with stale progress=100, initial=true → STARTS") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::COMPLETE, PrintJobState::PRINTING,
            /*current_progress=*/100, /*is_initial_transition=*/true,
            /*current_print_duration=*/0));
    }
    SECTION("Cancelled -> Printing with stale progress + duration, initial=true → STARTS") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::CANCELLED, PrintJobState::PRINTING,
            /*current_progress=*/57, /*is_initial_transition=*/true,
            /*current_print_duration=*/600));
    }
    SECTION("Error -> Printing with stale progress, initial=true → STARTS") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::ERROR, PrintJobState::PRINTING,
            /*current_progress=*/42, /*is_initial_transition=*/true,
            /*current_print_duration=*/0));
    }
    SECTION("Cancelled -> Printing progress=0 initial=true → STARTS (unchanged)") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::CANCELLED, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/true,
            /*current_print_duration=*/0));
    }
}

TEST_CASE("should_start_print_collector - boot into running print still suppressed",
          "[application][print_start][regression]") {
    // The genuine boot-into-active-print case presents as STANDBY -> PRINTING
    // (printer was idle when we booted) with stale progress/duration from the
    // running print. STANDBY is NOT terminal, so the mid-print-join skip must
    // STILL fire — otherwise we'd show "Preparing Print" partway through a real
    // print we joined at boot.
    SECTION("Standby -> Printing progress>0 initial=true → SUPPRESSED") {
        REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/45, /*is_initial_transition=*/true,
            /*current_print_duration=*/0));
    }
    SECTION("Standby -> Printing print_duration>0 (progress=0) initial=true → SUPPRESSED") {
        REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/true,
            /*current_print_duration=*/10645));
    }
    SECTION("Standby -> Printing progress=0 duration=0 initial=true → STARTS (fresh start)") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/true,
            /*current_print_duration=*/0));
    }
}

TEST_CASE("should_start_print_collector - mid-print detection (app boot only)",
          "[application][print_start]") {
    // App boots, finds print already running (initial transition with progress > 0)
    // This is the ONLY case where mid-print detection should suppress the collector
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                                 PrintJobState::PRINTING, 1, true));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::STANDBY, PrintJobState::PRINTING, 31, true));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::STANDBY, PrintJobState::PRINTING, 99, true));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::STANDBY, PrintJobState::PRINTING, 100, true));
}

TEST_CASE("should_start_print_collector - reprint after cancel with stale progress",
          "[application][print_start]") {
    // Real flow: CANCELLED → STANDBY → PRINTING with stale progress from old print.
    // The prev_state seen is STANDBY (not CANCELLED). Non-initial transition must
    // start the collector regardless of stale progress.
    REQUIRE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                           PrintJobState::PRINTING, 57, false));
    REQUIRE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                           PrintJobState::PRINTING, 100, false));
    REQUIRE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                           PrintJobState::PRINTING, 1, false));
}

TEST_CASE("should_start_print_collector - already printing", "[application][print_start]") {
    // If already printing, no transition → don't start
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::PRINTING, PrintJobState::PRINTING, 0, false));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::PRINTING, PrintJobState::PRINTING, 50, false));
}

TEST_CASE("should_start_print_collector - paused states", "[application][print_start]") {
    // Transition from PAUSED to PRINTING = resume, not fresh start
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::PAUSED, PrintJobState::PRINTING, 0, false));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::PAUSED, PrintJobState::PRINTING, 50, false));
    // Transition to PAUSED (not PRINTING) = don't start
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                                 PrintJobState::PAUSED, 0, false));
}

TEST_CASE("should_start_print_collector - non-printing transitions", "[application][print_start]") {
    // Transitions that don't involve PRINTING = don't start
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::STANDBY, PrintJobState::COMPLETE, 0, false));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::PRINTING, PrintJobState::COMPLETE, 100, false));
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        PrintJobState::PRINTING, PrintJobState::CANCELLED, 50, false));
}

TEST_CASE("should_start_print_collector - mid-print attach via print_duration",
          "[application][print_start]") {
    // The state-change observer fires synchronously while print_progress is still
    // 0 (virtual_sdcard / display_status haven't updated in the same tick).
    // print_duration is the load-bearing mid-print signal: 0 at fresh start,
    // >0 when joining a print already in progress. This case caused
    // "Starting Print..." to stick mid-print and dropped print_elapsed updates.
    SECTION("Initial transition with progress=0 but print_duration>0 → suppress") {
        REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/true,
            /*current_print_duration=*/10645));
    }
    SECTION("Initial transition with both signals zero → fresh start") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/true,
            /*current_print_duration=*/0));
    }
    SECTION("Initial transition with print_duration=1s → still mid-print") {
        REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/true,
            /*current_print_duration=*/1));
    }
    SECTION("Non-initial transition ignores print_duration (stale from prior print)") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/false,
            /*current_print_duration=*/9999));
    }
    SECTION("Default duration arg preserves backward compat") {
        // Existing 4-arg call sites must still compile and behave identically
        // when print_duration was never plumbed through.
        REQUIRE(MoonrakerManager::should_start_print_collector(PrintJobState::STANDBY,
                                                               PrintJobState::PRINTING, 0, true));
    }
}

TEST_CASE("should_start_print_collector - mid-print error recovery does not restart collector",
          "[application][print_start]") {
    // AFC error recovery on a Voron drives PRINTING → ERROR → PRINTING without ever
    // resetting the print. The recovery transition is non-initial, so the app-boot
    // mid-print guard does not apply; print_duration carries the real elapsed time of
    // the underway print. Restarting the collector here wipes phase state and leaves
    // "Starting Print..." stuck on the Print Status panel (issue #1042).
    SECTION("ERROR → PRINTING with print_duration>0 → suppress (the #1042 repro)") {
        REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
            PrintJobState::ERROR, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/false,
            /*current_print_duration=*/10645));
    }
    SECTION("ERROR → PRINTING with high progress and print_duration>0 → suppress") {
        REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
            PrintJobState::ERROR, PrintJobState::PRINTING,
            /*current_progress=*/63, /*is_initial_transition=*/false,
            /*current_print_duration=*/10645));
    }
    SECTION("ERROR → PRINTING with print_duration=0 → fresh start (error before extrusion)") {
        // An error that occurred before any extrusion has no elapsed print time, so the
        // following PRINTING transition is a genuine fresh start and must run the collector.
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::ERROR, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/false,
            /*current_print_duration=*/0));
    }
    SECTION("Fresh STANDBY → PRINTING still starts (guard is not always-false)") {
        REQUIRE(MoonrakerManager::should_start_print_collector(
            PrintJobState::STANDBY, PrintJobState::PRINTING,
            /*current_progress=*/0, /*is_initial_transition=*/false,
            /*current_print_duration=*/0));
    }
}

// ============================================================================
// Pre-print Completion Gate (should_complete_preprint)
// ============================================================================
// FIX C: the pre-print → printing hand-off must be gated on the REAL first
// layer (print_stats.info.current_layer >= 1), NOT raw print_duration. On the
// Snapmaker U1 (and any firmware whose PRINT_START purges/auto-feeds during
// the print_stats.state=printing window) print_duration goes >0 while the
// nozzle is still heating/homing, so the old "first extrusion" shortcut ended
// the Preparing phase minutes early. Printers that never report real layer
// data keep the old print_duration fallback so they still complete.

TEST_CASE("should_complete_preprint - layer-reporting printer waits for layer 1",
          "[application][print_start]") {
    // U1 case: printer reports layers (sticky true), pre-print extrusion drives
    // print_duration > 0 while current_layer is still 0. Must NOT complete.
    // seen_layer_zero is true here (the fresh 0 for this print was observed).
    SECTION("Pre-print extrusion (print_duration>0, current_layer==0) does NOT complete") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/0, /*print_duration=*/42,
            /*seen_layer_zero=*/true));
    }
    SECTION("Long pre-print purge still does NOT complete while layer==0") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/0, /*print_duration=*/600,
            /*seen_layer_zero=*/true));
    }
    SECTION("First real layer (current_layer==1) DOES complete once zero was seen") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/1, /*print_duration=*/42,
            /*seen_layer_zero=*/true));
    }
    SECTION("Any layer >= 1 completes once zero was seen (later observation)") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/5, /*print_duration=*/100,
            /*seen_layer_zero=*/true));
    }
    SECTION("Layer 1 with print_duration still 0 completes (layer is authoritative)") {
        // current_layer can lead print_duration on some firmwares — the real
        // first layer is the signal, not extrusion timing.
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/1, /*print_duration=*/0,
            /*seen_layer_zero=*/true));
    }
}

TEST_CASE("should_complete_preprint - layer-reporting printer NEVER uses print_duration fallback",
          "[application][print_start][regression]") {
    // L093 device-realism regression: this replicates the EXACT on-device U1
    // pre-print state, where the prior unit tests fed a convenient value and
    // missed the bug. On the U1 the printer reports layers (sticky true, because
    // print_stats.info.total_layer is present from print start), the slicer
    // auto-feed/purge drives print_duration > 0 while current_layer is still 0
    // and the nozzle is still heating. The earlier fix discriminated on the racy
    // per-print has_real_layer_data, which reset_for_new_print() had transiently
    // cleared to false → the print_duration fallback fired mid-purge ("first
    // extrusion (fallback)") and dropped Preparing minutes early. With the sticky
    // discriminator a layer-reporting printer must NEVER take the print_duration
    // fallback — only the real 0->>=1 edge completes it.
    SECTION("Sticky-true + current_layer=0 + print_duration>0 → does NOT complete (the bug)") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/0, /*print_duration=*/120,
            /*seen_layer_zero=*/true));
    }
    SECTION("Sticky-true, big purge duration, layer still 0 → does NOT complete") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/0, /*print_duration=*/900,
            /*seen_layer_zero=*/true));
    }
    SECTION("Sticky-true, the real first layer edge 0->1 → completes") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/1, /*print_duration=*/120,
            /*seen_layer_zero=*/true));
    }
}

TEST_CASE("should_complete_preprint - stale layer from previous print does NOT complete",
          "[application][print_start][regression]") {
    // Back-to-back prints: the previous print ended at e.g. current_layer=250
    // with the printer reporting layers (sticky true). reset_for_new_print()
    // (which zeroes the layer subject) is dispatched async AFTER
    // collector->start(), so there is a window where the collector is active but
    // current_layer still reads 250 and we have NOT yet observed a fresh 0
    // (seen_layer_zero=false). A pure level read would complete the new print's
    // pre-print phase instantly — the exact regression. The seen_layer_zero edge
    // guard must reject it.
    SECTION("Stale layer 250, zero not yet observed → does NOT complete") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/250, /*print_duration=*/0,
            /*seen_layer_zero=*/false));
    }
    SECTION("Stale layer 250 WITH pre-print extrusion (duration>0) → still does NOT complete") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/250, /*print_duration=*/30,
            /*seen_layer_zero=*/false));
    }
    SECTION("Even current_layer==1 does NOT complete before a fresh 0 was observed") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/1, /*print_duration=*/0,
            /*seen_layer_zero=*/false));
    }
    SECTION("After reset zeroes the layer (seen_layer_zero=true), layer 1 completes normally") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/1, /*print_duration=*/0,
            /*seen_layer_zero=*/true));
    }
}

TEST_CASE("should_complete_preprint - non-layer-reporting printer falls back to print_duration",
          "[application][print_start]") {
    // A printer that has NEVER emitted SET_PRINT_STATS_INFO / virtual_sdcard.layer
    // this session (printer_reports_layers sticky-false) must keep the OLD
    // behavior: complete on first extrusion. Otherwise it would never leave
    // Preparing (its current_layer is only a progress-derived estimate, not a
    // real signal). seen_layer_zero is irrelevant on this fallback path.
    SECTION("print_duration>0 completes when printer never reported layers") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/false, /*current_layer=*/0, /*print_duration=*/3,
            /*seen_layer_zero=*/false));
    }
    SECTION("print_duration==0 does NOT complete when printer never reported layers") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/false, /*current_layer=*/0, /*print_duration=*/0,
            /*seen_layer_zero=*/true));
    }
    SECTION("Estimated layer >= 1 alone does NOT complete (estimate is not a real signal)") {
        // For a non-reporting printer current_layer is progress-derived and can
        // read >=1 during pre-print; only print_duration is trusted here.
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/false, /*current_layer=*/2, /*print_duration=*/0,
            /*seen_layer_zero=*/true));
    }
}

// ============================================================================
// Shutdown Observer Release Contract (generalized; original: issue #888)
// ============================================================================
// `Application::shutdown()` deinit's all PrinterState subjects via
// StaticSubjectRegistry::deinit_all() BEFORE destroying singletons that the
// Application owns directly (m_moonraker, etc.). Subject memory is freed at
// that point, so any ObserverGuard member whose dtor still calls
// lv_observer_remove() — i.e. any member NOT released() in that class's
// own shutdown() — segfaults during teardown.
//
// Originally this test covered just MoonrakerManager (issue #888 / bundle
// T7M2ZYPY). Generalized via the SHUTDOWN_OBSERVER_CLASSES table below: any
// class in the table is structurally checked for the same contract.
//
// **When to add a class to the table:** when the class is owned by
// Application as a unique_ptr that's destroyed AFTER
// StaticSubjectRegistry::deinit_all() in Application::shutdown() (search for
// the deinit_all() call to find the boundary), AND the class observes static
// PrinterState/AmsState subjects via ObserverGuard members. New singletons
// reset()'d *before* deinit_all() don't need this — their observers are
// removed cleanly while subjects are still alive.
//
// The test parses both header and impl. It catches the structural mistake
// of adding a new ObserverGuard member without a matching release() call,
// without needing to spin up the class's full dependency graph.

namespace {

std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return {};
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::set<std::string> extract_observer_guard_members(const std::string& header) {
    std::set<std::string> members;
    // Match: optional whitespace, "ObserverGuard" (with optional helix:: qual),
    // whitespace, identifier, ";". Captures m_-prefixed names and trailing-
    // underscore names so the contract works for both naming conventions.
    std::regex re(R"((?:helix::)?ObserverGuard\s+([A-Za-z][A-Za-z0-9_]*)\s*;)");
    auto begin = std::sregex_iterator(header.begin(), header.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        members.insert((*it)[1].str());
    }
    return members;
}

// Body of <ClassName>::<methodName>(), from the signature's opening brace to its
// match via a brace-balance state machine. Empty when the signature is not found.
// Nested lambdas are included, which is what callers parsing registration sites
// need — those live inside the discovery lambdas, not at statement level.
std::string extract_method_body(const std::string& impl, const std::string& class_name,
                                const std::string& method_name) {
    std::regex sig_re("(?:void|bool|int)\\s+" + class_name + "::" + method_name +
                      R"(\s*\([^)]*\)\s*(?:noexcept)?\s*\{)");
    std::smatch m;
    if (!std::regex_search(impl, m, sig_re)) {
        return {};
    }
    size_t start = m.position(0) + m.length(0);
    int depth = 1;
    size_t pos = start;
    while (pos < impl.size() && depth > 0) {
        if (impl[pos] == '{')
            ++depth;
        else if (impl[pos] == '}')
            --depth;
        ++pos;
    }
    return impl.substr(start, pos - start - 1);
}

std::set<std::string> extract_released_members_in_method(const std::string& impl,
                                                         const std::string& class_name,
                                                         const std::string& method_name) {
    // Extract `member.release();` calls from the method body.
    std::string body = extract_method_body(impl, class_name, method_name);
    if (body.empty()) {
        return {};
    }

    std::set<std::string> released;
    std::regex rel_re(R"(([A-Za-z][A-Za-z0-9_]*)\.release\s*\(\s*\)\s*;)");
    auto begin = std::sregex_iterator(body.begin(), body.end(), rel_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        released.insert((*it)[1].str());
    }
    return released;
}

struct ShutdownObserverContract {
    std::string header_path;
    std::string impl_path;
    std::string class_name;
    std::string shutdown_method;
    int min_expected_members; // Sanity guard for the regex parser.
};

// Classes whose destructor runs AFTER StaticSubjectRegistry::deinit_all() in
// Application::shutdown(). Add new entries when the same risk pattern
// appears — see comment block above for criteria.
const std::vector<ShutdownObserverContract>& shutdown_observer_classes() {
    static const std::vector<ShutdownObserverContract> contracts = {
        {"include/moonraker_manager.h", "src/application/moonraker_manager.cpp", "MoonrakerManager",
         "shutdown",
         /*min_expected_members=*/6},
    };
    return contracts;
}

// A (method, handler) pair as written at a register/unregister_method_callback site.
using MethodHandler = std::pair<std::string, std::string>;

std::set<MethodHandler> extract_method_callback_pairs(const std::string& body, bool unregister) {
    // The \b keeps the register scan from also matching unregister_method_callback:
    // the 'n' preceding "register" there is a word character, so no boundary exists.
    std::regex re(std::string(unregister ? R"(\bunregister)" : R"(\bregister)") +
                  R"RX(_method_callback\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)")RX");
    std::set<MethodHandler> pairs;
    auto begin = std::sregex_iterator(body.begin(), body.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        pairs.insert({(*it)[1].str(), (*it)[2].str()});
    }
    return pairs;
}

// Method callbacks registered by name inside application.cpp whose body reaches a panel, a
// subject, or a manager-owned pointer that teardown destroys before it releases the
// MoonrakerClient. The shared PrinterSession::teardown_printer_scope() must drop these.
// Registration is not confined to setup_discovery_callbacks - layer_tracker and
// action_prompt_manager install from GcodeResponseRouting::attach - so the registration scan below
// covers both files.
//
// Subscriptions owned by a feature's attach()/detach() pair are not listed: their
// registration lives in the feature, and tests/unit/test_discovery_attach_detach.cpp proves
// detach() leaves nothing behind. detach_calls_required_in_teardown() below pins that the
// teardown calls each one.
const std::vector<MethodHandler>& handlers_requiring_teardown() {
    static const std::vector<MethodHandler> handlers = {
        {"notify_gcode_response", "layer_tracker"},
        {"notify_gcode_response", "action_prompt_manager"},
    };
    return handlers;
}

// The detach half of every attach()/detach() pair discovery installs.
const std::vector<std::string>& detach_calls_required_in_teardown() {
    static const std::vector<std::string> calls = {
        "TimelapseState::instance().detach(",
        "UpdateChecker::instance().detach(",
        "detach_print_hours(",
        "spoolman_sync::detach(",
    };
    return calls;
}

} // namespace

TEST_CASE("Shutdown observer release contract — every ObserverGuard member is released",
          "[application][shutdown][regression]") {
    for (const auto& c : shutdown_observer_classes()) {
        DYNAMIC_SECTION(c.class_name << "::" << c.shutdown_method) {
            auto header = read_file(c.header_path);
            auto impl = read_file(c.impl_path);
            REQUIRE_FALSE(header.empty());
            REQUIRE_FALSE(impl.empty());

            auto members = extract_observer_guard_members(header);
            INFO("Header: " << c.header_path);
            REQUIRE(static_cast<int>(members.size()) >= c.min_expected_members);

            auto released =
                extract_released_members_in_method(impl, c.class_name, c.shutdown_method);
            INFO("Impl: " << c.impl_path << " method: " << c.shutdown_method);
            REQUIRE_FALSE(released.empty());

            for (const auto& member : members) {
                INFO("ObserverGuard `" << member << "` declared in " << c.header_path
                                       << " must call release() in " << c.class_name
                                       << "::" << c.shutdown_method
                                       << "(). Subjects are deinit'd before this class's "
                                          "destructor runs, so the implicit ObserverGuard "
                                          "dtor calling lv_observer_remove() on freed memory "
                                          "crashes (issue #888 family).");
                REQUIRE(released.count(member) == 1);
            }
        }
    }
}

TEST_CASE("Method callbacks are unregistered by the shared teardown",
          "[application][shutdown][regression]") {
    // The unit is the (method, handler) pair, not the method. notify_history_changed
    // carries AboutOverlay_print_hours alongside PrintHistoryManager's own handler,
    // released by that manager's destructor, so the method appearing in an unregister
    // call is no evidence that a particular handler is released. notify_gcode_response
    // is the same shape: action_prompt_manager and layer_tracker are independent.
    //
    // Registration re-runs on every reconnect, and on the printer-switch path on a
    // fresh client. MoonrakerClient::register_method_callback inserts into a std::map
    // keyed by handler name, and std::map::insert does not overwrite, so a repeat is a
    // no-op rather than a second live handler.
    //
    // Neither teardown path can be driven at runtime (see application_test_access.h),
    // so this is a source-level contract in the same shape as the ObserverGuard release
    // contract above.
    const std::string impl = read_file("src/application/application.cpp");
    REQUIRE_FALSE(impl.empty());

    const std::string session_impl = read_file("src/application/printer_session.cpp");
    REQUIRE_FALSE(session_impl.empty());
    const std::string tear_down =
        extract_method_body(session_impl, "PrinterSession", "tear_down_printer_state");
    const std::string shutdown = extract_method_body(impl, "Application", "shutdown");
    const std::string scope =
        extract_method_body(session_impl, "PrinterSession", "teardown_printer_scope");
    REQUIRE_FALSE(tear_down.empty());
    REQUIRE_FALSE(shutdown.empty());
    REQUIRE_FALSE(scope.empty());

    // Both paths run the one ordered teardown, so one body holds every unregister.
    CHECK(tear_down.find("teardown_printer_scope(TeardownScope::PrinterSwitch)") !=
          std::string::npos);
    CHECK(shutdown.find("TeardownScope::ProcessExit") != std::string::npos);

    // The G-code response handlers register and unregister inside GcodeResponseRouting;
    // the teardown must call both of its detach halves.
    const std::string routing = read_file("src/application/gcode_response_routing.cpp");
    REQUIRE_FALSE(routing.empty());
    const std::string detach_handlers =
        extract_method_body(routing, "GcodeResponseRouting", "detach_handlers");
    REQUIRE_FALSE(detach_handlers.empty());
    CHECK(scope.find("m_routing.detach_handlers(") != std::string::npos);
    CHECK(scope.find("m_routing.release_routers()") != std::string::npos);

    // Registration sites are spread across setup_discovery_callbacks and
    // GcodeResponseRouting::attach, so scan whole translation units rather than one body.
    auto registered = extract_method_callback_pairs(session_impl, /*unregister=*/false);
    registered.merge(extract_method_callback_pairs(routing, /*unregister=*/false));
    auto dropped = extract_method_callback_pairs(scope, /*unregister=*/true);
    dropped.merge(extract_method_callback_pairs(detach_handlers, /*unregister=*/true));

    // Guard the parser: a regex matching nothing would satisfy every check below.
    REQUIRE(registered.size() >= handlers_requiring_teardown().size());
    REQUIRE_FALSE(dropped.empty());

    for (const auto& call : detach_calls_required_in_teardown()) {
        DYNAMIC_SECTION("teardown calls " << call) {
            CHECK(scope.find(call) != std::string::npos);
        }
    }

    for (const auto& handler : handlers_requiring_teardown()) {
        DYNAMIC_SECTION(handler.first << " / " << handler.second) {
            {
                INFO(
                    "Not registered in application.cpp or gcode_response_routing.cpp. The table in "
                    "handlers_requiring_teardown() is stale.");
                CHECK(registered.count(handler) == 1);
            }
            {
                INFO("teardown_printer_scope must unregister this handler. "
                     "StaticPanelRegistry::destroy_all() and StaticSubjectRegistry::"
                     "deinit_all() both run before the client is released.");
                CHECK(dropped.count(handler) == 1);
            }
        }
    }
}

TEST_CASE("A printer switch freezes the UpdateQueue before it disconnects the client",
          "[application][shutdown][regression]") {
    // Work the WebSocket thread enqueues after the disconnect must be buffered and then
    // discarded by update_queue_shutdown(), not run after the objects it touches are gone.
    // Teardown cannot be driven at runtime (see application_test_access.h), so this is a
    // source-level contract.
    const std::string impl = read_file("src/application/printer_session.cpp");
    const std::string scope = extract_method_body(impl, "PrinterSession", "teardown_printer_scope");
    REQUIRE_FALSE(scope.empty());
    const auto freeze = scope.find("queue_freeze.emplace(");
    const auto disconnect = scope.find("client()->disconnect()");
    const auto drain = scope.find("helix::ui::update_queue_shutdown();");
    REQUIRE(freeze != std::string::npos);
    REQUIRE(disconnect != std::string::npos);
    REQUIRE(drain != std::string::npos);
    CHECK(freeze < disconnect);
    CHECK(disconnect < drain);
}

TEST_CASE("should_complete_preprint - completes when the layer-zero sample is never delivered",
          "[application][print_start][regression]") {
    // notify_status_update is coalesced, so the 0 sample is not guaranteed to be
    // observed: a fast first layer, or a reconnect part-way in, can make the
    // first value the app ever sees >= 1. Requiring the 0 -> >=1 edge alone left
    // the collector active forever, so PrintLifecycleState never left Preparing
    // and the pre-print overlay covered the entire print. Reproduced with the
    // mock at --sim-speed 15, where current_layer arrives as 3, 8, 13, ...
    SECTION("Zero never seen, but the counter advanced — completes") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/8, /*print_duration=*/42,
            /*seen_layer_zero=*/false, /*layer_advanced=*/true));
    }
    SECTION("Zero never seen and no advance yet — still waits") {
        // A single positive sample proves nothing: it could be a stale value
        // carried over from the previous print.
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/8, /*print_duration=*/42,
            /*seen_layer_zero=*/false, /*layer_advanced=*/false));
    }
    SECTION("Advance still cannot complete while the layer is below 1") {
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/0, /*print_duration=*/600,
            /*seen_layer_zero=*/false, /*layer_advanced=*/true));
    }
    SECTION("Original zero-edge route is unchanged") {
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/true, /*current_layer=*/1, /*print_duration=*/0,
            /*seen_layer_zero=*/true, /*layer_advanced=*/false));
    }
}

// ============================================================================
// PrintCollectorArming - boot-join arming state
// ============================================================================

TEST_CASE("PrintCollectorArming re-arms on reset so a printer switch cannot join mid-print",
          "[application][print_start][regression]") {
    // init_print_start_collector() re-runs on every printer switch
    // (application.cpp, inside connect_moonraker()). The arming state must
    // re-arm with it. When this lived in a function-local static, the
    // initializer ran once per process and only prev_state was reassigned, so
    // after the first print the mid-print-join suppression was permanently off:
    // switching to a printer already partway through a job drew a full
    // "Preparing..." overlay over a running print.
    helix::PrintCollectorArming arming;

    SECTION("a fresh instance is armed") {
        REQUIRE(arming.is_initial_transition());
    }

    SECTION("consuming the first transition disarms it") {
        arming.consume_initial_transition();
        REQUIRE_FALSE(arming.is_initial_transition());
    }

    SECTION("reset re-arms after the first transition was consumed") {
        arming.consume_initial_transition();
        REQUIRE_FALSE(arming.is_initial_transition());

        arming.reset(); // printer switch
        REQUIRE(arming.is_initial_transition());
    }

    SECTION("reset also clears the remembered previous state") {
        arming.note_transition(PrintJobState::PRINTING);
        REQUIRE(arming.prev_state() == PrintJobState::PRINTING);

        arming.reset();
        REQUIRE(arming.prev_state() == PrintJobState::STANDBY);
    }
}

TEST_CASE("PrintCollectorArming drives the boot-join suppression across a printer switch",
          "[application][print_start][regression]") {
    // The end-to-end shape of the bug: connect to printer A, run a print to
    // completion, then switch to printer B which is already 60% through a job.
    // After the switch the collector must be suppressed, exactly as it would be
    // on a cold boot into that same running print.
    helix::PrintCollectorArming arming;

    // Printer A: a normal print start consumes the initial transition.
    REQUIRE(MoonrakerManager::should_start_print_collector(
        arming.prev_state(), PrintJobState::PRINTING,
        /*current_progress=*/0, arming.is_initial_transition(),
        /*current_print_duration=*/0));
    arming.consume_initial_transition();
    arming.note_transition(PrintJobState::PRINTING);
    arming.note_transition(PrintJobState::COMPLETE);

    // Printer switch: init_print_start_collector() runs again.
    arming.reset();

    // Printer B is already 60% in. This presents as STANDBY -> PRINTING with
    // stale progress, which is the genuine boot-into-running-print case and
    // must be suppressed.
    REQUIRE_FALSE(MoonrakerManager::should_start_print_collector(
        arming.prev_state(), PrintJobState::PRINTING,
        /*current_progress=*/60, arming.is_initial_transition(),
        /*current_print_duration=*/4200));
}

// ============================================================================
// Collector teardown vs a live preparing job
// ============================================================================

TEST_CASE("should_stop_print_collector spares a collector armed at commit",
          "[application][print_start][preparing]") {
    // A print we initiated ourselves reaches PRINTING via a transient hop:
    // Klipper leaves the previous job's terminal state, passes through STANDBY,
    // and only then reports PRINTING. The observer's teardown branch fires on
    // any non-printing state, so without this the collector armed at commit is
    // stopped on the way INTO the print it was armed for.
    SECTION("standby hop does not stop a collector while a job is being prepared") {
        REQUIRE_FALSE(MoonrakerManager::should_stop_print_collector(PrintJobState::STANDBY,
                                                                    /*has_preparing_job=*/true));
        REQUIRE_FALSE(MoonrakerManager::should_stop_print_collector(PrintJobState::COMPLETE,
                                                                    /*has_preparing_job=*/true));
    }

    SECTION("with no preparing job the teardown still fires") {
        REQUIRE(MoonrakerManager::should_stop_print_collector(PrintJobState::STANDBY,
                                                              /*has_preparing_job=*/false));
        REQUIRE(MoonrakerManager::should_stop_print_collector(PrintJobState::CANCELLED,
                                                              /*has_preparing_job=*/false));
    }

    SECTION("printing and paused never tear the collector down") {
        // PRINT_START runs INSIDE the job, so the collector must survive the
        // handoff and complete on its own phase detection.
        REQUIRE_FALSE(
            MoonrakerManager::should_stop_print_collector(PrintJobState::PRINTING, false));
        REQUIRE_FALSE(MoonrakerManager::should_stop_print_collector(PrintJobState::PAUSED, false));
        REQUIRE_FALSE(MoonrakerManager::should_stop_print_collector(PrintJobState::PRINTING, true));
    }
}

// ============================================================================
// Retiring a preparing job that never became a print
//
// The collector is armed at COMMIT, so every exit from the preparing window has
// to answer whether it stops. The epoch observer used to punt this to the
// print-state observer, which cannot see it: that one fires only when
// print_state_enum CHANGES, and a job that dies before the printer accepts it
// never moves the wire off standby.
//
// Left armed, the collector keeps parsing gcode responses, so the next command
// the user runs by hand - homing from the Motion panel after cancelling a start
// - is read as a pre-print phase and re-raises the "Preparing Print" overlay
// over the controls they are using.
//
// NOTE: this covers the DECISION. The dispatch that calls it lives in an
// ObserverGuard inside MoonrakerManager, which has heavy dependencies and is not
// constructible here (see the header note at the top of this file).
// ============================================================================

TEST_CASE("should_stop_collector_on_retirement stops unless the printer took the job",
          "[application][print_start_collector]") {
    SECTION("Confirmed - the printer is printing, PRINT_START runs inside the job") {
        REQUIRE_FALSE(
            MoonrakerManager::should_stop_collector_on_retirement(PrintJobState::PRINTING));
        REQUIRE_FALSE(MoonrakerManager::should_stop_collector_on_retirement(PrintJobState::PAUSED));
    }

    SECTION("every other exit means no print is coming") {
        // Cancelled / Failed / TimedOut / Superseded all leave the wire here.
        REQUIRE(MoonrakerManager::should_stop_collector_on_retirement(PrintJobState::STANDBY));
        REQUIRE(MoonrakerManager::should_stop_collector_on_retirement(PrintJobState::COMPLETE));
        REQUIRE(MoonrakerManager::should_stop_collector_on_retirement(PrintJobState::CANCELLED));
        REQUIRE(MoonrakerManager::should_stop_collector_on_retirement(PrintJobState::ERROR));
    }
}

// ============================================================================
// Klippy leaving READY ends the pre-print window
//
// A verify_heater shutdown mid-PRINT_START leaves print_stats reporting PAUSED
// on some firmware (Snapmaker U1), and should_stop_print_collector() spares
// every state that has a job, so the print-state observer alone keeps the
// collector posting ETA updates for a print that can no longer run.
// ============================================================================

TEST_CASE("should_stop_collector_on_klippy_state stops on shutdown and error",
          "[application][print_start_collector]") {
    SECTION("a dead Klipper ends the pre-print window whatever print_stats says") {
        REQUIRE(MoonrakerManager::should_stop_collector_on_klippy_state(KlippyState::SHUTDOWN));
        REQUIRE(MoonrakerManager::should_stop_collector_on_klippy_state(KlippyState::ERROR));
        // The print-state axis alone would keep it running in exactly this case.
        REQUIRE_FALSE(MoonrakerManager::should_stop_print_collector(PrintJobState::PAUSED, false));
    }

    SECTION("ready and startup leave the collector alone") {
        REQUIRE_FALSE(MoonrakerManager::should_stop_collector_on_klippy_state(KlippyState::READY));
        REQUIRE_FALSE(
            MoonrakerManager::should_stop_collector_on_klippy_state(KlippyState::STARTUP));
    }
}

// ============================================================================
// Printer-edge collector arming — the observer path, not the pure predicate
// ============================================================================
// Every case above pins should_start_print_collector() as a function; none
// drives the print-state observer that wires it to a live collector. That
// observer is the path a print started by ANOTHER client (Mainsail) takes,
// where print_stats moving to PRINTING is the only signal, and it is the
// load-bearing assumption behind the print-start arm of the discretionary-gcode
// busy gate: a jog during a START macro is refused because is_in_print_start()
// is true, and it is true because this observer drove the phase subject off
// IDLE. Drives the real manager with its mock client.

#include "../../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "application_test_fixture.h"
#include "filament_sensor_manager.h"
#include "print_start_collector.h"
#include "print_start_phase.h"
#include "spoolman_manager.h"

namespace {

class ManagerCollectorArmingFixture : public ApplicationTestFixture {
  public:
    ManagerCollectorArmingFixture() {
        // The global PrinterState the observer registers on; park it in
        // STANDBY so the write below is a real no-job -> PRINTING edge.
        auto& ps = get_printer_state();
        ps.init_subjects(false);
        lv_subject_set_int(ps.get_print_state_enum_subject(),
                           static_cast<int>(PrintJobState::STANDBY));

        REQUIRE(mgr.init(config(), nullptr));
        mgr.init_print_start_collector();
    }

    ~ManagerCollectorArmingFixture() override {
        mgr.shutdown();
        // create_api() handed this manager's API to app_globals and three
        // singletons; null them so nothing later in the binary dereferences
        // the freed instance.
        set_moonraker_api(nullptr);
        AmsState::instance().set_moonraker_api(nullptr);
        FilamentSensorManager::instance().set_moonraker_api(nullptr);
        SpoolmanManager::instance().set_api(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    MoonrakerManager mgr;
};

} // namespace

TEST_CASE_METHOD(ManagerCollectorArmingFixture, "shutdown clears the global client it installed",
                 "[application][moonraker_manager]") {
    REQUIRE(get_moonraker_client() != nullptr);
    mgr.shutdown();
    CHECK(get_moonraker_client() == nullptr);
}

TEST_CASE_METHOD(ManagerCollectorArmingFixture,
                 "print-state observer arms the collector off IDLE on a printer-edge start",
                 "[application][print_start]") {
    auto& ps = get_printer_state();
    auto* phase_subject = ps.get_print_start_phase_subject();
    REQUIRE(lv_subject_get_int(phase_subject) == static_cast<int>(PrintStartPhase::IDLE));
    REQUIRE_FALSE(ps.is_in_print_start());

    // A print started from another frontend: only print_stats moves.
    lv_subject_set_int(ps.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // The collector started and published its first phase (deferred through
    // the UpdateQueue, hence the drain above): the phase left IDLE, which is
    // what is_in_print_start() reads.
    CHECK(mgr.print_start_collector()->is_active());
    CHECK(lv_subject_get_int(phase_subject) != static_cast<int>(PrintStartPhase::IDLE));
    CHECK(ps.is_in_print_start());
}
