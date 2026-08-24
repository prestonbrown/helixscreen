// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preprint_predictor.cpp
 * @brief Unit tests for PreprintPredictor weighted average and remaining time
 *
 * Tests pure prediction logic without LVGL or Config dependencies.
 */

#include "preprint_predictor.h"
#include "printer_state.h"

#include <set>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::PreprintEntry;
using helix::PreprintPredictor;

// ============================================================================
// Empty State
// ============================================================================

TEST_CASE("PreprintPredictor: defaults without history", "[print][predictor]") {
    PreprintPredictor predictor;

    // Empty predictor has no predictions (entries-based)
    REQUIRE_FALSE(predictor.has_predictions());
    // But default phase durations are still available via predicted_phases()
    auto defaults = predictor.predicted_phases();
    REQUIRE_FALSE(defaults.empty());
    REQUIRE(predictor.predicted_total() > 0);
    // remaining_seconds returns 0 with no history (collector uses thermal model instead)
    REQUIRE(predictor.remaining_seconds({}, 0, 0) == 0);
}

TEST_CASE("PreprintPredictor: default_phase_durations returns expected phases",
          "[print][predictor]") {
    auto defaults = PreprintPredictor::default_phase_durations();
    REQUIRE(defaults.size() == 6);
    REQUIRE(defaults[static_cast<int>(PrintStartPhase::HOMING)] == 30);
    REQUIRE(defaults[static_cast<int>(PrintStartPhase::BED_MESH)] == 90);
    REQUIRE(defaults[static_cast<int>(PrintStartPhase::QGL)] == 60);
    REQUIRE(defaults[static_cast<int>(PrintStartPhase::Z_TILT)] == 45);
    REQUIRE(defaults[static_cast<int>(PrintStartPhase::CLEANING)] == 20);
    REQUIRE(defaults[static_cast<int>(PrintStartPhase::PURGING)] == 15);
}

// ============================================================================
// Single Entry
// ============================================================================

TEST_CASE("PreprintPredictor: single entry uses 100% weight", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{185, 1700000000, {{2, 25}, {3, 90}, {7, 30}, {9, 20}}}});

    REQUIRE(predictor.has_predictions());
    // predicted_total() returns wall-clock total_seconds (185), not phase sum.
    // The 20s gap vs phase sum (165) represents unmapped/heating time.
    REQUIRE(predictor.predicted_total() == 185);

    auto phases = predictor.predicted_phases();
    REQUIRE(phases[2] == 25);
    REQUIRE(phases[3] == 90);
    REQUIRE(phases[7] == 30);
    REQUIRE(phases[9] == 20);
}

// ============================================================================
// Two Entries (60/40 weighting)
// ============================================================================

TEST_CASE("PreprintPredictor: two entries favor newer entry", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({
        {100, 1700000000, {{2, 20}}}, // older: lower weight
        {100, 1700000001, {{2, 30}}}, // newer: higher weight
    });

    auto phases = predictor.predicted_phases();
    // Exponential weighting: newer entry dominates
    // Prediction should be between 20 and 30, closer to 30
    REQUIRE(phases[2] > 24);
    REQUIRE(phases[2] <= 30);
}

// ============================================================================
// Three Entries (50/30/20 weighting)
// ============================================================================

TEST_CASE("PreprintPredictor: three entries favor newest", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({
        {100, 1700000000, {{2, 10}}}, // oldest
        {100, 1700000001, {{2, 20}}}, // middle
        {100, 1700000002, {{2, 30}}}, // newest
    });

    auto phases = predictor.predicted_phases();
    // Exponential weighting: prediction closer to 30 than to 10
    REQUIRE(phases[2] > 18);
    REQUIRE(phases[2] <= 30);
}

// ============================================================================
// FIFO Trimming
// ============================================================================

TEST_CASE("PreprintPredictor: add_entry FIFO trims to MAX_ENTRIES", "[print][predictor]") {
    PreprintPredictor predictor;
    // Load MAX_ENTRIES entries
    std::vector<PreprintEntry> entries;
    for (int i = 0; i < PreprintPredictor::MAX_ENTRIES; i++) {
        entries.push_back({100, 1700000000 + i, {{2, 10 + i}}});
    }
    predictor.load_entries(entries);
    REQUIRE(predictor.get_entries().size() == static_cast<size_t>(PreprintPredictor::MAX_ENTRIES));

    // Add one more — oldest should be evicted
    predictor.add_entry({100, 1700000000 + PreprintPredictor::MAX_ENTRIES, {{2, 99}}});
    auto result = predictor.get_entries();
    REQUIRE(result.size() == static_cast<size_t>(PreprintPredictor::MAX_ENTRIES));
    // First entry should now be the second original one
    REQUIRE(result.front().phase_durations.at(2) == 11);
    // Last entry should be the newly added one
    REQUIRE(result.back().phase_durations.at(2) == 99);
}

// ============================================================================
// 15-Minute Cap
// ============================================================================

TEST_CASE("PreprintPredictor: add_entry accepts any duration (MAD handles outliers)",
          "[print][predictor]") {
    PreprintPredictor predictor;

    // Large entries are no longer rejected at add time — MAD handles anomalies in prediction
    predictor.add_entry({901, 1700000000, {{2, 500}}});
    REQUIRE(predictor.has_predictions());
    REQUIRE(predictor.get_entries().size() == 1);

    predictor.add_entry({900, 1700000001, {{2, 500}}});
    REQUIRE(predictor.get_entries().size() == 2);
}

// ============================================================================
// Phases That Appear in Only Some Entries
// ============================================================================

TEST_CASE("PreprintPredictor: phases appearing in subset of entries", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({
        {100, 1700000000, {{2, 20}, {3, 80}}},           // has homing + heating
        {100, 1700000001, {{2, 25}}},                    // only homing
        {100, 1700000002, {{2, 30}, {3, 100}, {7, 40}}}, // homing + heating + mesh
    });

    auto phases = predictor.predicted_phases();

    // Phase 2 (homing): all three entries, newest=30 dominates
    REQUIRE(phases[2] >= 24);
    REQUIRE(phases[2] <= 30);

    // Phase 3 (heating): entries 0 and 2 only, newest=100 dominates
    REQUIRE(phases[3] >= 85);
    REQUIRE(phases[3] <= 100);

    // Phase 7 (mesh): only entry 2 — 100% of its weight
    REQUIRE(phases[7] == 40);
}

// ============================================================================
// Remaining Time: All Future Phases
// ============================================================================

TEST_CASE("PreprintPredictor: remaining_seconds with no progress", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{185, 1700000000, {{2, 25}, {3, 90}, {7, 30}, {9, 20}}}});

    // No completed phases, current=IDLE(0), no elapsed
    int remaining = predictor.remaining_seconds({}, 0, 0);
    // All phases are future: 25+90+30+20 = 165
    REQUIRE(remaining == 165);
}

// ============================================================================
// Remaining Time: Some Completed, Current Active
// ============================================================================

TEST_CASE("PreprintPredictor: remaining with completed and current phase", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{185, 1700000000, {{2, 25}, {3, 90}, {7, 30}, {9, 20}}}});

    // Homing done, currently heating bed for 30s
    std::set<int> completed = {2};
    int remaining = predictor.remaining_seconds(completed, 3, 30);
    // Current phase (3): max(0, 90-30) = 60
    // Future phases (7, 9): 30+20 = 50
    // Total: 60+50 = 110
    REQUIRE(remaining == 110);
}

// ============================================================================
// Remaining Time: Elapsed Exceeds Prediction
// ============================================================================

TEST_CASE("PreprintPredictor: elapsed exceeds prediction returns 0 for current",
          "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{185, 1700000000, {{2, 25}, {3, 90}, {7, 30}, {9, 20}}}});

    // Heating bed, but we've been at it for 120s (predicted 90s)
    std::set<int> completed = {2};
    int remaining = predictor.remaining_seconds(completed, 3, 120);
    // Current phase: max(0, 90-120) = 0
    // Future phases: 30+20 = 50
    REQUIRE(remaining == 50);
}

// ============================================================================
// Remaining Time: All Phases Completed
// ============================================================================

TEST_CASE("PreprintPredictor: all phases completed returns 0", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{185, 1700000000, {{2, 25}, {3, 90}, {7, 30}, {9, 20}}}});

    std::set<int> completed = {2, 3, 7, 9};
    int remaining = predictor.remaining_seconds(completed, 0, 0);
    REQUIRE(remaining == 0);
}

// ============================================================================
// Remaining Time: Current Phase Not in History
// ============================================================================

TEST_CASE("PreprintPredictor: unknown current phase contributes 0", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{100, 1700000000, {{2, 25}, {3, 90}}}});

    // Current phase 5 (QGL) not in history - contributes 0 predicted
    std::set<int> completed = {2};
    int remaining = predictor.remaining_seconds(completed, 5, 10);
    // Current (5): not in history -> 0
    // Future: phase 3 is future (not completed, not current) -> 90
    REQUIRE(remaining == 90);
}

// ============================================================================
// Single Phase Entry
// ============================================================================

TEST_CASE("PreprintPredictor: single phase entry", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{30, 1700000000, {{3, 30}}}});

    REQUIRE(predictor.predicted_total() == 30);

    auto phases = predictor.predicted_phases();
    REQUIRE(phases.size() == 1);
    REQUIRE(phases[3] == 30);

    // In the middle of the only phase
    int remaining = predictor.remaining_seconds({}, 3, 10);
    REQUIRE(remaining == 20);
}

// ============================================================================
// load_entries Replaces Existing
// ============================================================================

TEST_CASE("PreprintPredictor: load_entries replaces existing data", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{100, 1700000000, {{2, 50}}}});
    // predicted_total() returns wall-clock total_seconds, not phase sum
    REQUIRE(predictor.predicted_total() == 100);

    predictor.load_entries({{80, 1700000001, {{3, 30}}}});
    REQUIRE(predictor.predicted_total() == 80);

    auto phases = predictor.predicted_phases();
    REQUIRE(phases.count(2) == 0); // Old data gone
    REQUIRE(phases[3] == 30);
}

// ============================================================================
// load_entries Caps at MAX_ENTRIES
// ============================================================================

TEST_CASE("PreprintPredictor: load_entries caps at MAX_ENTRIES", "[print][predictor]") {
    PreprintPredictor predictor;
    std::vector<PreprintEntry> entries;
    for (int i = 0; i < PreprintPredictor::MAX_ENTRIES + 5; i++) {
        entries.push_back({100, 1700000000 + i, {{2, 10 + i}}});
    }
    predictor.load_entries(entries);

    // Should keep only the last MAX_ENTRIES
    REQUIRE(predictor.get_entries().size() == static_cast<size_t>(PreprintPredictor::MAX_ENTRIES));
}

// ============================================================================
// Zero Elapsed in Current Phase
// ============================================================================

TEST_CASE("PreprintPredictor: zero elapsed in current phase", "[print][predictor]") {
    PreprintPredictor predictor;
    predictor.load_entries({{100, 1700000000, {{2, 25}, {3, 90}}}});

    // Just entered phase 3, 0 elapsed
    std::set<int> completed = {2};
    int remaining = predictor.remaining_seconds(completed, 3, 0);
    // Current: 90-0=90, future: none
    REQUIRE(remaining == 90);
}

// ============================================================================
// Temperature Bucket Filtering
// ============================================================================

TEST_CASE("PreprintPredictor: load_entries filters by temp_bucket", "[print][predictor]") {
    PreprintPredictor predictor;

    std::vector<PreprintEntry> all_entries = {
        {60, 1700000000, {{2, 10}, {4, 50}}, 200},   // PLA (200°C bucket)
        {120, 1700000001, {{2, 15}, {4, 105}}, 250}, // ASA (250°C bucket)
        {65, 1700000002, {{2, 12}, {4, 53}}, 200},   // PLA (200°C bucket)
    };

    SECTION("filter to 200°C bucket gets PLA entries only") {
        predictor.load_entries(all_entries, 200);
        REQUIRE(predictor.get_entries().size() == 2);
        REQUIRE(predictor.predicted_total() < 80); // ~63s average
    }

    SECTION("filter to 250°C bucket gets ASA entry only") {
        predictor.load_entries(all_entries, 250);
        REQUIRE(predictor.get_entries().size() == 1);
        REQUIRE(predictor.predicted_total() == 120);
    }

    SECTION("filter to unknown bucket gets nothing") {
        predictor.load_entries(all_entries, 225);
        REQUIRE(predictor.get_entries().empty());
    }

    SECTION("no filter (bucket=0) gets all entries") {
        predictor.load_entries(all_entries, 0);
        REQUIRE(predictor.get_entries().size() == 3);
    }
}

TEST_CASE("PreprintPredictor: legacy entries (bucket=0) match any filter", "[print][predictor]") {
    PreprintPredictor predictor;

    std::vector<PreprintEntry> entries = {
        {80, 1700000000, {{2, 10}}, 0},    // legacy (no bucket)
        {120, 1700000001, {{2, 15}}, 250}, // ASA bucket
    };

    // Legacy entry matches the 250 bucket filter
    predictor.load_entries(entries, 250);
    REQUIRE(predictor.get_entries().size() == 2);
}

TEST_CASE("PreprintPredictor: temp_bucket stored in entry", "[print][predictor]") {
    PreprintPredictor predictor;

    PreprintEntry entry;
    entry.total_seconds = 90;
    entry.timestamp = 1700000000;
    entry.phase_durations = {{2, 15}, {4, 75}};
    entry.temp_bucket = 275;

    predictor.add_entry(entry);

    auto entries = predictor.get_entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].temp_bucket == 275);
}

// ============================================================================
// Time-Decay Weighting
// ============================================================================

TEST_CASE("PreprintPredictor time-decay weighting", "[preprint_predictor]") {
    PreprintPredictor predictor;
    std::vector<PreprintEntry> entries;
    int64_t base_time = 1700000000;
    int qgl_phase = static_cast<int>(PrintStartPhase::QGL);
    for (int i = 0; i < 5; i++) {
        PreprintEntry e;
        e.total_seconds = 60 + i * 10;
        e.timestamp = base_time + i * 3600;
        e.phase_durations[qgl_phase] = 60 + i * 10;
        entries.push_back(e);
    }
    predictor.load_entries(entries);
    auto phases = predictor.predicted_phases();
    REQUIRE(phases.count(qgl_phase) == 1);
    // Newest entries dominate — prediction should be closer to 100 than 60
    REQUIRE(phases[qgl_phase] > 80);
}

// ============================================================================
// MAD Anomaly Rejection
// ============================================================================

TEST_CASE("PreprintPredictor MAD anomaly rejection", "[preprint_predictor]") {
    PreprintPredictor predictor;
    int64_t base_time = 1700000000;
    std::vector<PreprintEntry> entries;
    int mesh_phase = static_cast<int>(PrintStartPhase::BED_MESH);
    for (int i = 0; i < 4; i++) {
        PreprintEntry e;
        e.total_seconds = 90;
        e.timestamp = base_time + i * 3600;
        e.phase_durations[mesh_phase] = 85 + i * 3;
        entries.push_back(e);
    }
    PreprintEntry anomaly;
    anomaly.total_seconds = 800;
    anomaly.timestamp = base_time + 5 * 3600;
    anomaly.phase_durations[mesh_phase] = 800;
    entries.push_back(anomaly);
    predictor.load_entries(entries);
    auto phases = predictor.predicted_phases();
    REQUIRE(phases[mesh_phase] < 120);
}

// ============================================================================
// Missing-Phase Normalization
// ============================================================================

TEST_CASE("PreprintPredictor missing-phase normalization", "[preprint_predictor]") {
    PreprintPredictor predictor;
    int64_t base_time = 1700000000;
    int mesh_phase = static_cast<int>(PrintStartPhase::BED_MESH);
    int qgl_phase = static_cast<int>(PrintStartPhase::QGL);
    PreprintEntry e1;
    e1.total_seconds = 150;
    e1.timestamp = base_time;
    e1.phase_durations[mesh_phase] = 90;
    e1.phase_durations[qgl_phase] = 60;
    PreprintEntry e2;
    e2.total_seconds = 60;
    e2.timestamp = base_time + 3600;
    e2.phase_durations[qgl_phase] = 55;
    predictor.load_entries({e1, e2});
    auto phases = predictor.predicted_phases();
    // Mesh only from e1
    REQUIRE(phases[mesh_phase] == 90);
    // QGL averages both
    REQUIRE(phases[qgl_phase] > 50);
    REQUIRE(phases[qgl_phase] < 65);
}

// ============================================================================
// Accepts Up to 10 Entries
// ============================================================================

TEST_CASE("PreprintPredictor accepts up to 10 entries", "[preprint_predictor]") {
    PreprintPredictor predictor;
    int64_t base_time = 1700000000;
    std::vector<PreprintEntry> entries;
    for (int i = 0; i < 12; i++) {
        PreprintEntry e;
        e.total_seconds = 100;
        e.timestamp = base_time + i * 3600;
        e.phase_durations[static_cast<int>(PrintStartPhase::HOMING)] = 20;
        entries.push_back(e);
    }
    predictor.load_entries(entries);
    REQUIRE(predictor.get_entries().size() == 10);
}

// ============================================================================
// Measurement-window bucketing
//
// The collector is armed either at the printer's own print-start edge (an
// externally started print) or at user commit (the screen started it). Commit
// arming puts any host-side pre-start block - a forced bed mesh, a setup macro
// - inside the measured window. On a printer that runs one, the two windows
// differ by minutes, so averaging them together produces an estimate that is
// wrong for both, and feeds a too-small predicted_total into the collector's
// adaptive timeout.
// ============================================================================

TEST_CASE("PreprintPredictor: host-pre-start window rejects printer-edge history",
          "[print][predictor]") {
    PreprintPredictor predictor;

    std::vector<PreprintEntry> entries = {
        // Externally started: window is the printer's own PRINT_START only.
        {300, 1700000000, {{2, 20}, {7, 100}}, 1, PreprintWindow::PrinterEdge},
        // Screen started on the same printer, with a host-side bed mesh in front.
        {1140, 1700000001, {{2, 20}, {7, 500}}, 1, PreprintWindow::HostPreStart},
    };

    predictor.load_entries(entries, 1, PreprintWindow::HostPreStart);

    auto kept = predictor.get_entries();
    REQUIRE(kept.size() == 1);
    REQUIRE(kept[0].total_seconds == 1140);
}

TEST_CASE("PreprintPredictor: printer-edge window rejects host-pre-start history",
          "[print][predictor]") {
    PreprintPredictor predictor;

    std::vector<PreprintEntry> entries = {
        {300, 1700000000, {{2, 20}, {7, 100}}, 1, PreprintWindow::PrinterEdge},
        {1140, 1700000001, {{2, 20}, {7, 500}}, 1, PreprintWindow::HostPreStart},
    };

    predictor.load_entries(entries, 1, PreprintWindow::PrinterEdge);

    auto kept = predictor.get_entries();
    REQUIRE(kept.size() == 1);
    REQUIRE(kept[0].total_seconds == 300);
}

TEST_CASE("PreprintPredictor: legacy entries count as printer-edge, not host-pre-start",
          "[print][predictor]") {
    // Every entry recorded before commit arming existed measured a printer-edge
    // window, so treating a missing field as PrinterEdge is a fact about the
    // data, not a guess. The asymmetry matters on upgrade: an existing history
    // stays usable for externally started prints, and cannot drag a host-side
    // block's estimate down to a duration that never included one.
    std::vector<PreprintEntry> entries = {
        {300, 1700000000, {{2, 20}}, 1, PreprintWindow::Unknown},
    };

    SECTION("a printer-edge query keeps them") {
        PreprintPredictor predictor;
        predictor.load_entries(entries, 1, PreprintWindow::PrinterEdge);
        REQUIRE(predictor.get_entries().size() == 1);
    }

    SECTION("a host-pre-start query discards them") {
        PreprintPredictor predictor;
        predictor.load_entries(entries, 1, PreprintWindow::HostPreStart);
        REQUIRE(predictor.get_entries().empty());
    }
}

TEST_CASE("PreprintPredictor: window filter composes with temp_bucket", "[print][predictor]") {
    PreprintPredictor predictor;

    std::vector<PreprintEntry> entries = {
        {1100, 1700000000, {{7, 500}}, 1, PreprintWindow::HostPreStart}, // cold + host
        {900, 1700000001, {{7, 400}}, 2, PreprintWindow::HostPreStart},  // warm + host
        {300, 1700000002, {{7, 100}}, 1, PreprintWindow::PrinterEdge},   // cold + edge
    };

    predictor.load_entries(entries, 1, PreprintWindow::HostPreStart);

    auto kept = predictor.get_entries();
    REQUIRE(kept.size() == 1);
    REQUIRE(kept[0].total_seconds == 1100);
}

TEST_CASE("PreprintPredictor: window survives add_entry round-trip", "[print][predictor]") {
    PreprintPredictor predictor;

    PreprintEntry entry;
    entry.total_seconds = 1140;
    entry.timestamp = 1700000000;
    entry.phase_durations = {{7, 500}};
    entry.temp_bucket = 1;
    entry.window = PreprintWindow::HostPreStart;

    predictor.add_entry(entry);

    auto kept = predictor.get_entries();
    REQUIRE(kept.size() == 1);
    REQUIRE(kept[0].window == PreprintWindow::HostPreStart);
}

TEST_CASE("PreprintPredictor: window defaults to Unknown when unspecified", "[print][predictor]") {
    PreprintEntry entry;
    REQUIRE(entry.window == PreprintWindow::Unknown);
}

TEST_CASE("PreprintPredictor: omitting the window filter keeps every entry", "[print][predictor]") {
    // Existing callers pass no window and must be unaffected.
    PreprintPredictor predictor;

    std::vector<PreprintEntry> entries = {
        {300, 1700000000, {{2, 20}}, 1, PreprintWindow::PrinterEdge},
        {1140, 1700000001, {{7, 500}}, 1, PreprintWindow::HostPreStart},
        {600, 1700000002, {{2, 30}}, 1, PreprintWindow::Unknown},
    };

    predictor.load_entries(entries, 1);

    REQUIRE(predictor.get_entries().size() == 3);
}
