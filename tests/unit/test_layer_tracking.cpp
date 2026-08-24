// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_layer_tracking.cpp
 * @brief Tests for layer tracking: print_stats.info primary path + gcode response fallback
 *
 * Verifies that print_layer_current_ subject is updated from both:
 * 1. Moonraker print_stats.info.current_layer (primary path via update_from_status)
 * 2. Gcode response parsing (fallback for slicers that don't emit SET_PRINT_STATS_INFO)
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_manager.h"
#include "printer_state.h"

#include <regex>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using json = nlohmann::json;

// ============================================================================
// Helper: parse a gcode response line for layer info (mirrors application.cpp logic)
// ============================================================================

namespace {

struct LayerParseResult {
    int layer = -1;
    int total = -1;
};

LayerParseResult parse_layer_from_gcode(const std::string& line) {
    LayerParseResult result;

    // Pattern 1: SET_PRINT_STATS_INFO CURRENT_LAYER=N [TOTAL_LAYER=N]
    if (line.find("SET_PRINT_STATS_INFO") != std::string::npos) {
        auto pos = line.find("CURRENT_LAYER=");
        if (pos != std::string::npos) {
            result.layer = std::atoi(line.c_str() + pos + 14);
        }
        pos = line.find("TOTAL_LAYER=");
        if (pos != std::string::npos) {
            result.total = std::atoi(line.c_str() + pos + 12);
        }
    }

    // Pattern 2: ;LAYER:N
    if (result.layer < 0 && line.size() >= 8 && line[0] == ';' && line[1] == 'L' &&
        line[2] == 'A' && line[3] == 'Y' && line[4] == 'E' && line[5] == 'R' && line[6] == ':') {
        result.layer = std::atoi(line.c_str() + 7);
    }

    return result;
}

} // namespace

// ============================================================================
// Primary path: print_stats.info.current_layer via update_from_status
// ============================================================================

TEST_CASE("Layer tracking: print_stats.info.current_layer updates subject",
          "[layer_tracking][print_stats]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start printing
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    SECTION("current_layer updates from info object") {
        json status = {{"print_stats", {{"info", {{"current_layer", 5}, {"total_layer", 110}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 5);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 110);
    }

    SECTION("null info does not crash or update") {
        // Set initial value
        json status = {{"print_stats", {{"info", {{"current_layer", 3}}}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 3);

        // Send null info - should not change the value
        json null_info = {{"print_stats", {{"info", nullptr}}}};
        state.update_from_status(null_info);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 3);
    }

    SECTION("missing info key does not crash") {
        json status = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(status);
        // Should still be at default (0)
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }
}

// ============================================================================
// Gcode response parsing (unit tests for the parsing logic)
// ============================================================================

TEST_CASE("Layer tracking: gcode response parsing", "[layer_tracking][gcode]") {
    SECTION("SET_PRINT_STATS_INFO CURRENT_LAYER=N parses correctly") {
        auto result = parse_layer_from_gcode("SET_PRINT_STATS_INFO CURRENT_LAYER=5");
        REQUIRE(result.layer == 5);
        REQUIRE(result.total == -1); // no total in this line
    }

    SECTION("SET_PRINT_STATS_INFO with both CURRENT_LAYER and TOTAL_LAYER") {
        auto result =
            parse_layer_from_gcode("SET_PRINT_STATS_INFO CURRENT_LAYER=3 TOTAL_LAYER=110");
        REQUIRE(result.layer == 3);
        REQUIRE(result.total == 110);
    }

    SECTION(";LAYER:N comment format (OrcaSlicer/PrusaSlicer/Cura)") {
        auto result = parse_layer_from_gcode(";LAYER:42");
        REQUIRE(result.layer == 42);
    }

    SECTION(";LAYER:0 parses zero layer") {
        auto result = parse_layer_from_gcode(";LAYER:0");
        REQUIRE(result.layer == 0);
    }

    SECTION("unrelated gcode lines are ignored") {
        auto result = parse_layer_from_gcode("ok");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode("G1 X10 Y20 Z0.3");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode("M104 S200");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode("");
        REQUIRE(result.layer == -1);
    }

    SECTION("short lines don't cause out-of-bounds") {
        auto result = parse_layer_from_gcode(";L");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode(";LAYER");
        REQUIRE(result.layer == -1);
    }
}

// ============================================================================
// set_print_layer_current setter (thread-safe path)
// ============================================================================

TEST_CASE("Layer tracking: set_print_layer_current setter", "[layer_tracking][setter]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("setter updates the subject via async") {
        state.set_print_layer_current(7);
        // Process the async queue so the value actually lands
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 7);
    }

    SECTION("setter and print_stats.info both update same subject") {
        // Simulate gcode fallback setting layer
        state.set_print_layer_current(10);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 10);

        // Then print_stats.info comes in with a different value (takes precedence naturally)
        json status = {{"print_stats", {{"info", {{"current_layer", 12}}}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 12);
    }

    SECTION("setter marks has_real_layer_data true") {
        REQUIRE_FALSE(state.has_real_layer_data());
        state.set_print_layer_current(5);
        // Flag is set inside the async lambda, so drain the queue first
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(state.has_real_layer_data());
    }
}

// ============================================================================
// virtual_sdcard.layer path (K1C and newer Klipper)
// ============================================================================

TEST_CASE("Layer tracking: virtual_sdcard.layer updates subject",
          "[layer_tracking][virtual_sdcard]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start printing. print_duration > 0 marks real printing (past PRINT_START),
    // which the fallback layer tiers now require before estimating/deriving.
    json printing = {{"print_stats", {{"state", "printing"}, {"print_duration", 120}}}};
    state.update_from_status(printing);

    SECTION("layer and layer_count update from virtual_sdcard") {
        json status = {
            {"virtual_sdcard", {{"progress", 0.50}, {"layer", 158}, {"layer_count", 296}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 158);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 296);
        REQUIRE(state.has_real_layer_data());
    }

    SECTION("virtual_sdcard.layer takes precedence over estimation") {
        // Set total for estimation to have something to work with
        state.set_print_layer_total(296);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Send progress with layer data — should use layer, not estimation
        json status = {
            {"virtual_sdcard", {{"progress", 0.66}, {"layer", 158}, {"layer_count", 296}}}};
        state.update_from_status(status);

        // 0.66 * 296 = 195 (estimation), but real layer is 158
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 158);
    }

    SECTION("virtual_sdcard.layer prevents future estimation") {
        json with_layer = {{"virtual_sdcard", {{"progress", 0.50}, {"layer", 100}}}};
        state.update_from_status(with_layer);
        REQUIRE(state.has_real_layer_data());

        // Further progress without layer data should NOT overwrite via estimation
        state.set_print_layer_total(296);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        json without_layer = {{"virtual_sdcard", {{"progress", 0.80}}}};
        state.update_from_status(without_layer);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 100);
    }

    SECTION("missing layer field falls back to estimation") {
        state.set_print_layer_total(200);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        json no_layer = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(no_layer);

        // Should estimate: 50% of 200 = 100
        REQUIRE_FALSE(state.has_real_layer_data());
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 100);
    }
}

// ============================================================================
// Source precedence: print_stats.info > virtual_sdcard
// ============================================================================
//
// When both sources arrive in the same status update, slicer-supplied data
// (print_stats.info.current_layer / total_layer via SET_PRINT_STATS_INFO)
// must win over the Klipper-side virtual_sdcard fallback. Previously the
// virtual_sdcard branch ran second and silently overwrote the info value —
// preventing slicers that emit accurate per-feature layer numbering from
// being trusted on printers that also report sdcard layers.

TEST_CASE("Layer tracking: print_stats.info wins over virtual_sdcard in same update",
          "[layer_tracking][precedence]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    SECTION("info.current_layer beats virtual_sdcard.layer when both present") {
        json combined = {
            {"print_stats", {{"info", {{"current_layer", 42}, {"total_layer", 200}}}}},
            {"virtual_sdcard", {{"progress", 0.21}, {"layer", 999}, {"layer_count", 9999}}}};
        state.update_from_status(combined);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 42);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 200);
    }

    SECTION("virtual_sdcard takes over when info missing in subsequent update") {
        // First update: info-only — sets layer to 10
        json info_only = {
            {"print_stats", {{"info", {{"current_layer", 10}, {"total_layer", 100}}}}}};
        state.update_from_status(info_only);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 10);

        // Second update: virtual_sdcard only — should now drive layer
        json sdcard_only = {
            {"virtual_sdcard", {{"progress", 0.5}, {"layer", 50}, {"layer_count", 100}}}};
        state.update_from_status(sdcard_only);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 50);
    }

    SECTION("partial info — only current_layer set — still prefers info, sdcard fills total") {
        json partial = {
            {"print_stats", {{"info", {{"current_layer", 7}}}}},
            {"virtual_sdcard", {{"progress", 0.0}, {"layer", 99}, {"layer_count", 150}}}};
        state.update_from_status(partial);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 7);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 150);
    }
}

// ============================================================================
// Progress-based layer estimation fallback
// ============================================================================

TEST_CASE("Layer tracking: progress-based estimation fallback", "[layer_tracking][estimation]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start printing. print_duration > 0 marks real printing (past PRINT_START),
    // which the fallback layer tiers now require before estimating/deriving.
    json printing = {{"print_stats", {{"state", "printing"}, {"print_duration", 120}}}};
    state.update_from_status(printing);

    // Set total layers from metadata (this is how it works in practice)
    state.set_print_layer_total(320);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    SECTION("estimates layer from progress when no real layer data") {
        REQUIRE_FALSE(state.has_real_layer_data());

        // 50% progress → ~160/320
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);
    }

    SECTION("estimates at low progress") {
        json progress = {{"virtual_sdcard", {{"progress", 0.01}}}};
        state.update_from_status(progress);

        // 1% of 320 = 3.2, rounded = 3. But clamped to min 1.
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) >= 1);
    }

    SECTION("estimates at high progress") {
        json progress = {{"virtual_sdcard", {{"progress", 0.99}}}};
        state.update_from_status(progress);

        // 99% of 320 = 316.8 → 317
        int estimated = lv_subject_get_int(state.get_print_layer_current_subject());
        REQUIRE(estimated >= 315);
        REQUIRE(estimated <= 320);
    }

    SECTION("does not estimate when total_layers is 0") {
        state.set_print_layer_total(0);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);

        // Should stay at 0 — no total to estimate from
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }

    SECTION("stops estimating once real data arrives from print_stats.info") {
        // First: estimation active
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);
        REQUIRE_FALSE(state.has_real_layer_data());

        // Real data arrives
        json real_layer = {{"print_stats", {{"info", {{"current_layer", 142}}}}}};
        state.update_from_status(real_layer);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 142);
        REQUIRE(state.has_real_layer_data());

        // Further progress updates should NOT overwrite real data
        json progress2 = {{"virtual_sdcard", {{"progress", 0.55}}}};
        state.update_from_status(progress2);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 142);
    }

    SECTION("stops estimating once real data arrives from gcode fallback") {
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        REQUIRE_FALSE(state.has_real_layer_data());

        // Gcode fallback sets real data
        state.set_print_layer_current(150);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(state.has_real_layer_data());

        // Progress update should NOT overwrite
        json progress2 = {{"virtual_sdcard", {{"progress", 0.55}}}};
        state.update_from_status(progress2);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 150);
    }

    SECTION("does not estimate in terminal state even without real data") {
        // Set total layers and make some progress
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);

        // Print completes
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        // Progress update arrives after completion — should NOT change layer
        json progress2 = {{"virtual_sdcard", {{"progress", 0.99}}}};
        state.update_from_status(progress2);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);
    }

    SECTION("has_real_layer_data resets on new print") {
        // Get real data
        json real_layer = {{"print_stats", {{"info", {{"current_layer", 42}}}}}};
        state.update_from_status(real_layer);
        REQUIRE(state.has_real_layer_data());

        // Simulate new print starting (state goes to standby then printing)
        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        // Reset via the same mechanism as real code
        PrinterStateTestAccess::reset(state);
        state.init_subjects(false);

        json printing2 = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing2);

        REQUIRE_FALSE(state.has_real_layer_data());
    }
}

// ============================================================================
// Regression: a layer-reporting printer must NEVER fabricate a layer from
// progress during pre-print.
//
// Root cause (Snapmaker U1 premature print-start completion): the progress
// estimate was gated on the PER-PRINT has_real_layer_data_ flag.
// reset_for_new_print() clears that flag at the start of every print, and
// Moonraker's DELTA status updates omit unchanged fields — so while
// info.current_layer sits at 0 through the entire ~4 min pre-print
// (homing / bed detect / auto-feed / clean / mesh / prime), the omitted-but-
// unchanged 0 is never re-observed and has_real_layer_data_ stays false.
// File progress, however, climbs to ~2% during the prime line, so the estimate
// fabricated current_layer = max(1, round(0.02 * total)) = 1. That fake "layer
// 1" tripped MoonrakerManager::should_complete_preprint()'s 0->1 edge ~24 s in,
// ending "Preparing..." long before the real first model layer.
//
// Fix: gate the estimate on the STICKY printer_reports_layers_ capability flag
// instead. The U1 sends total_layer in print_stats.info at print start, so the
// sticky flag latches immediately and the estimate is suppressed for the whole
// session — current_layer holds the authoritative info value (0 through
// pre-print) and only a genuine info.current_layer = 1 advances it.
// ============================================================================

TEST_CASE("Layer tracking: layer-reporting printer never estimates during preprint",
          "[layer_tracking][estimation][snapmaker]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // --- Print starts. The U1 emits SET_PRINT_STATS_INFO TOTAL_LAYER=10 /
    //     CURRENT_LAYER=0 in the slicer header, so info carries both fields up
    //     front. This latches the sticky printer_reports_layers_ capability and
    //     seeds current_layer at the authoritative 0. ---
    json start = {{"print_stats",
                   {{"state", "printing"}, {"info", {{"total_layer", 10}, {"current_layer", 0}}}}}};
    state.update_from_status(start);

    REQUIRE(state.printer_reports_layers());
    REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 10);

    // --- reset_for_new_print() runs (async, after the collector starts). It
    //     clears the PER-PRINT has_real_layer_data_ flag but NOT the sticky
    //     capability flag. This is the exact window that used to break. ---
    state.reset_for_new_print();
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(state.has_real_layer_data());
    REQUIRE(state.printer_reports_layers()); // sticky — survives reset
    // Re-seed total (reset cleared current to 0; total survives in the subject
    // but re-send it the way Moonraker would on the next delta that carries it).
    json total_only = {{"print_stats", {{"info", {{"total_layer", 10}}}}}};
    state.update_from_status(total_only);

    SECTION("progress climbing during preprint does NOT fabricate layer 1") {
        // Pre-print: file progress creeps up (prime line, ~2%) while
        // info.current_layer is omitted by Moonraker (unchanged 0 → delta drops
        // it). Before the fix this estimated max(1, round(0.02*10)) = 1.
        json progress2pct = {{"virtual_sdcard", {{"progress", 0.02}}}};
        state.update_from_status(progress2pct);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);

        // Progress keeps climbing through the rest of the silent pre-print —
        // still no estimate, layer stays pinned at the authoritative 0.
        json progress15pct = {{"virtual_sdcard", {{"progress", 0.15}}}};
        state.update_from_status(progress15pct);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }

    SECTION("only the real info.current_layer=1 advances the layer") {
        // Pre-print progress — no estimate.
        json progress = {{"virtual_sdcard", {{"progress", 0.05}}}};
        state.update_from_status(progress);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);

        // The genuine first model layer: slicer emits CURRENT_LAYER=1 → info.
        json real_layer1 = {{"print_stats", {{"info", {{"current_layer", 1}}}}}};
        state.update_from_status(real_layer1);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 1);
        REQUIRE(state.has_real_layer_data());
    }
}

// ============================================================================
// Sticky printer_reports_layers — survives per-print reset (FIX C, L093)
// ============================================================================
// Device-grounded regression: on the Snapmaker U1 the printer reports layers
// (print_stats.info.total_layer is present from print start), but
// reset_for_new_print() — which runs when a new print transitions IDLE ->
// preparing — clears the PER-PRINT has_real_layer_data_ flag. The U1 does not
// continuously re-emit info.current_layer during pre-print, so has_real_layer_data
// stays FALSE through the whole purge. The earlier pre-print completion gate
// discriminated on that racy per-print flag and therefore took the print_duration
// fallback mid-purge, completing Preparing minutes early. The STICKY
// printer_reports_layers flag must survive reset_for_new_print() so the gate keeps
// taking the real-first-layer path. This test drives the ACTUAL print_stats parse
// path (not the pure helper with hand-picked args), replicating the device sequence.

TEST_CASE("Layer tracking: printer_reports_layers is sticky across reset_for_new_print",
          "[layer_tracking][print_stats][regression]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Sentinel: a fresh session has never seen layer data.
    REQUIRE_FALSE(state.printer_reports_layers());
    REQUIRE_FALSE(state.has_real_layer_data());

    // --- Print A: U1 reports total_layer at print start (current_layer arrives later). ---
    json print_a_start = {
        {"print_stats",
         {{"state", "printing"}, {"info", {{"total_layer", 10}, {"current_layer", 0}}}}}};
    state.update_from_status(print_a_start);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(state.printer_reports_layers()); // latched immediately from total_layer
    REQUIRE(state.has_real_layer_data());

    // Print A advances and finishes.
    state.update_from_status(
        {{"print_stats",
          {{"state", "printing"}, {"info", {{"total_layer", 10}, {"current_layer", 10}}}}}});
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    state.update_from_status({{"print_stats", {{"state", "complete"}}}});
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    // Force phase back to IDLE so the next set_print_start_state triggers the
    // IDLE -> preparing new-print path (which calls reset_for_new_print()).
    state.reset_print_start_state();
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // --- Print B begins: pre-print phase opens. This is the IDLE -> INITIALIZING
    //     transition that fires reset_for_new_print() in set_print_start_state(). ---
    lv_subject_set_int(state.get_print_active_subject(), 1);
    state.set_print_start_state(PrintStartPhase::INITIALIZING, "Preparing Print...", 0);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    SECTION("reset_for_new_print clears the per-print flag but NOT the sticky one") {
        // Per-print flag cleared (the U1 hasn't re-emitted current_layer yet)...
        REQUIRE_FALSE(state.has_real_layer_data());
        // ...but the sticky printer-capability flag survives.
        REQUIRE(state.printer_reports_layers());
    }

    SECTION("Pre-print purge does NOT complete despite has_real_layer_data being false") {
        // EXACT device state mid-purge: per-print flag false, current_layer 0,
        // print_duration ticking up from auto-feed/purge. Discriminating on the
        // sticky flag keeps us on the real-first-layer path → no completion.
        REQUIRE_FALSE(state.has_real_layer_data());
        REQUIRE(state.printer_reports_layers());
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/state.printer_reports_layers(),
            /*current_layer=*/lv_subject_get_int(state.get_print_layer_current_subject()),
            /*print_duration=*/120, /*seen_layer_zero=*/true));
    }

    SECTION("Real first layer 0->1 DOES complete once the U1 re-emits current_layer") {
        // U1 emits current_layer=1 at the real first layer.
        state.update_from_status(
            {{"print_stats", {{"state", "printing"}, {"info", {{"current_layer", 1}}}}}});
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 1);
        REQUIRE(state.printer_reports_layers());
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/state.printer_reports_layers(),
            /*current_layer=*/lv_subject_get_int(state.get_print_layer_current_subject()),
            /*print_duration=*/120, /*seen_layer_zero=*/true));
    }
}

TEST_CASE("Layer tracking: never-reporting printer keeps sticky false (fallback preserved)",
          "[layer_tracking][print_stats][regression]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // A genuine non-reporter: state updates arrive with NO layer field anywhere.
    state.update_from_status({{"print_stats", {{"state", "printing"}}}});
    state.update_from_status({{"virtual_sdcard", {{"progress", 0.10}}}});
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE_FALSE(state.printer_reports_layers());

    // With the sticky flag false, the print_duration fallback is preserved so
    // the printer still leaves Preparing on first extrusion.
    REQUIRE(MoonrakerManager::should_complete_preprint(
        /*printer_reports_layers=*/state.printer_reports_layers(),
        /*current_layer=*/0, /*print_duration=*/5, /*seen_layer_zero=*/false));
}

// ============================================================================
// Z-height current-layer derivation (issue kostake#4542)
//
// For printers whose slicer never reports a layer number (no
// print_stats.info.current_layer, no virtual_sdcard.layer — printer_reports_layers
// stays false), HelixScreen used to fall back straight to a progress-fraction
// estimate round(progress% * total). Progress is byte/time based and drifts high
// early in a print: at 12% of 75 layers it yields ~9 while the true layer is 5.
//
// The fix inserts a Z-height derivation tier between the real-layer sources and
// the progress estimate. When slice geometry (layer_height) is known and a
// commanded Z is available, the layer is derived as
//   round((z - first_layer_height) / layer_height) + 1, clamped [1, total].
// This matches Mainsail/Fluidd. It remains an ESTIMATE: has_real_layer_data and
// printer_reports_layers stay false (the UI "~" prefix honestly signals derived).
// ============================================================================

TEST_CASE("Layer tracking: Z-height derivation for non-reporting slicer",
          "[layer_tracking][zheight]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Non-reporting printer: state=printing, no layer field anywhere.
    // print_duration > 0 marks real printing (past PRINT_START) so the Z-height
    // derivation tier is allowed to run.
    state.update_from_status({{"print_stats", {{"state", "printing"}, {"print_duration", 120}}}});
    state.set_print_layer_total(75);
    state.set_print_layer_heights(0.2, 0.2);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    SECTION("kostake#4542: derives 5 from Z, not 9 from progress fraction") {
        // 12% progress of 75 layers = 9 via progress estimate; Z=1.0mm with
        // 0.2mm layers = round((1.0-0.2)/0.2)+1 = round(4)+1 = 5.
        json status = {{"virtual_sdcard", {{"progress", 0.12}}},
                       {"gcode_move", {{"gcode_position", {10.0, 10.0, 1.0, 0.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 5);
        // Not a slicer-reported field (pre-print gate must stay unaffected)...
        REQUIRE_FALSE(state.has_real_layer_data());
        REQUIRE_FALSE(state.printer_reports_layers());
        // ...but display-accurate (Mainsail parity) — the label drops the "~".
        REQUIRE(state.layer_is_accurate());
    }

    SECTION("first layer: Z at first_layer_height derives layer 1") {
        json status = {{"virtual_sdcard", {{"progress", 0.05}}},
                       {"gcode_move", {{"gcode_position", {10.0, 10.0, 0.2, 0.0}}}}};
        state.update_from_status(status);

        // round((0.2-0.2)/0.2)+1 = 1.
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 1);
    }

    SECTION("clamps to total when Z runs past the model") {
        json status = {{"virtual_sdcard", {{"progress", 0.99}}},
                       {"gcode_move", {{"gcode_position", {10.0, 10.0, 100.0, 0.0}}}}};
        state.update_from_status(status);

        // round((100-0.2)/0.2)+1 = 500, clamped to total 75.
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 75);
    }
}

TEST_CASE("Layer tracking: real info.current_layer still wins over Z derivation",
          "[layer_tracking][zheight]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.update_from_status({{"print_stats", {{"state", "printing"}}}});
    state.set_print_layer_total(75);
    state.set_print_layer_heights(0.2, 0.2);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // Real layer 5 AND a gcode Z (2.8mm) that would derive 15. Real value wins,
    // and printer_reports_layers latches true so Z-derivation is suppressed.
    json status = {{"print_stats", {{"info", {{"current_layer", 5}}}}},
                   {"gcode_move", {{"gcode_position", {10.0, 10.0, 2.8, 0.0}}}}};
    state.update_from_status(status);

    REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 5);
    REQUIRE(state.has_real_layer_data());
    REQUIRE(state.printer_reports_layers());
    REQUIRE(state.layer_is_accurate());
}

TEST_CASE("Layer tracking: progress estimate preserved when slice geometry unknown",
          "[layer_tracking][zheight]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Non-reporting printer with NO heights set (e.g. non-sliced job) — tier 4
    // (progress fraction) must still apply. print_duration > 0 marks real
    // printing (past PRINT_START) so the progress-fraction tier is allowed to run.
    state.update_from_status({{"print_stats", {{"state", "printing"}, {"print_duration", 120}}}});
    state.set_print_layer_total(75);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    json status = {{"virtual_sdcard", {{"progress", 0.12}}}};
    state.update_from_status(status);

    // round(0.12 * 75) = 9 — the historical progress-fraction behavior.
    REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 9);
    REQUIRE_FALSE(state.has_real_layer_data());
    // Progress-fraction guess is NOT accurate — the label keeps the "~".
    REQUIRE_FALSE(state.layer_is_accurate());
}

// ============================================================================
// PRINT_START gate: fallback layer tiers must NOT fabricate a layer before
// real printing begins (user Discord report).
//
// During the print-start gcode (bed mesh / purge line / Z-hop) Klipper reports
// state="printing" but has NOT yet advanced print_duration (it only ticks once
// real extrusion moves execute). Meanwhile gcode_move.gcode_position[2] is
// driven high by bed probing / Z-hop (e.g. Z=2mm) and file-position progress
// creeps up as the START_PRINT macro streams the file header. Without a gate the
// Z-height derivation computed round((2.0-0.2)/0.2)+1 = 10 and — because it sets
// layer_z_derived_ — presented that garbage as an ACCURATE layer (no "~"),
// "freaking out" at 10/15 until real printing self-corrected it. The fallback
// tiers now require print_duration > 0.
// ============================================================================

TEST_CASE("Layer tracking: fallback tiers do NOT fabricate a layer during PRINT_START",
          "[layer_tracking][zheight][print_start]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Non-reporting slicer, print-start phase: state=printing but print_duration
    // is still 0 (bed mesh / purge / Z-hop, no real extrusion time yet). Slice
    // geometry + total layers are known from file metadata.
    state.update_from_status({{"print_stats", {{"state", "printing"}, {"print_duration", 0}}}});
    state.set_print_layer_total(75);
    state.set_print_layer_heights(0.2, 0.2);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    SECTION("Z-derivation is suppressed while print_duration==0 (no bogus layer 10)") {
        // Z=2.0mm during a probe / Z-hop would derive round((2.0-0.2)/0.2)+1 = 10.
        json status = {{"virtual_sdcard", {{"progress", 0.03}}},
                       {"gcode_move", {{"gcode_position", {10.0, 10.0, 2.0, 0.0}}}}};
        state.update_from_status(status);

        // Holds at the pre-print default (0) rather than fabricating a layer...
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
        // ...and must NOT claim display-accuracy (would drop the "~" prefix).
        REQUIRE_FALSE(state.layer_is_accurate());
    }

    SECTION("progress-fraction tier is suppressed while print_duration==0") {
        // Unknown slice geometry routes to the progress-fraction tier instead.
        state.set_print_layer_heights(0.0, 0.0);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // 12% of 75 = 9 would be fabricated without the gate.
        json status = {{"virtual_sdcard", {{"progress", 0.12}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }

    SECTION("derivation resumes once print_duration>0 (feature not disabled)") {
        // Same Z, but now real printing has started (print_duration > 0). The
        // Z-height derivation must produce the correct layer, proving the gate
        // only defers — it does not disable the tier.
        json status = {{"print_stats", {{"print_duration", 30}}},
                       {"virtual_sdcard", {{"progress", 0.12}}},
                       {"gcode_move", {{"gcode_position", {1.0, 1.0, 1.0, 0.0}}}}};
        state.update_from_status(status);

        // Z=1.0mm, 0.2mm layers => round((1.0-0.2)/0.2)+1 = 5.
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 5);
        REQUIRE(state.layer_is_accurate());
    }
}

// ============================================================================
// PRINT_START ETA gate: the remaining-time smoother must not seed from
// progress-extrapolation while print_duration==0.
//
// The EMA smoother (smoothed_remaining_) seeds on the first extrapolation and
// converges slowly (alpha ~0.06 at low progress). If it seeds on PRINT_START
// noise — high file progress while print_duration is still 0 — it latches onto a
// bogus estimate and "gets stuck". The extrapolation branch already requires
// print_duration > 0; while print_duration==0 the slicer-estimate fallback is
// used instead, so the ETA is a sane full-print estimate rather than a latched
// noise value. This locks that invariant (mutation: dropping print_duration > 0
// from the extrapolation branch seeds the smoother and this REQUIRE fails).
// ============================================================================

TEST_CASE("ETA: smoother not seeded from extrapolation while print_duration==0",
          "[layer_tracking][eta][print_start]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Slicer estimate known from metadata; real printing not started yet.
    state.set_estimated_print_time(3600); // 60 min
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    state.update_from_status({{"print_stats", {{"state", "printing"}}}});

    // PRINT_START noise: file-position progress already at 10% (macro streamed the
    // header) while print_duration is still 0. Establish progress in its own
    // update — the remaining-time calc reads print_progress_ from the PRIOR value
    // (virtual_sdcard is parsed AFTER the print_stats block within one update).
    state.update_from_status({{"virtual_sdcard", {{"progress", 0.10}}}});

    // total_duration present so the remaining-time block runs; print_duration 0.
    state.update_from_status({{"print_stats", {{"print_duration", 0}, {"total_duration", 45}}}});

    // With print_duration==0 the noisy extrapolation is NOT used and the smoother
    // is NOT seeded. The slicer-estimate fallback applies: 3600 * (100-10)/100.
    REQUIRE(lv_subject_get_int(state.get_print_time_left_subject()) == 3240);
}
