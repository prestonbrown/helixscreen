// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_position_classifier.h"

#include <algorithm>
#include <catch_amalgamated.hpp>
#include <fstream>
#include <tuple>
#include <vector>

#include "hv/json.hpp"

using helix::PositionActivity;
using helix::PrintStartPositionClassifier;

namespace {

struct CorpusWindow {
    std::vector<std::tuple<uint64_t, float, float, float>> samples; // (ms, x, y, z)
};

/// Load one window of tests/fixtures/print_start_position_corpus.json as
/// time-ordered (ms-offset, x, y, z) samples. The corpus was extracted from a
/// K1C klippy.log capture (2026-08-19); timestamps are klippy wall clock.
CorpusWindow load_window(const std::string& name) {
    std::ifstream f("tests/fixtures/print_start_position_corpus.json");
    REQUIRE(f.good());
    nlohmann::json data = nlohmann::json::parse(f);
    auto& w = data.at("windows").at(name);

    uint64_t base_ms = 0;
    CorpusWindow out;
    for (const auto& s : w.at("samples")) {
        const std::string t = s.at("t").get<std::string>();
        uint64_t ms = (static_cast<uint64_t>(std::stoi(t.substr(0, 2))) * 3600 +
                       static_cast<uint64_t>(std::stoi(t.substr(3, 2))) * 60 +
                       static_cast<uint64_t>(std::stoi(t.substr(6, 2)))) *
                          1000 +
                      std::stoul(t.substr(9, 3));
        if (base_ms == 0) {
            base_ms = ms;
        }
        out.samples.emplace_back(ms - base_ms, s.at("x").get<float>(), s.at("y").get<float>(),
                                 s.at("z").get<float>());
    }
    REQUIRE(out.samples.size() > 50);
    return out;
}

/// Feed a whole window, returning the activity after each sample.
std::vector<PositionActivity> replay(PrintStartPositionClassifier& c, const CorpusWindow& corpus) {
    std::vector<PositionActivity> timeline;
    timeline.reserve(corpus.samples.size());
    for (const auto& [t, x, y, z] : corpus.samples) {
        c.note_position(x, y, z, t);
        timeline.push_back(c.activity());
    }
    return timeline;
}

size_t first_index_of(const std::vector<PositionActivity>& tl, PositionActivity a) {
    auto it = std::find(tl.begin(), tl.end(), a);
    return it == tl.end() ? SIZE_MAX : static_cast<size_t>(it - tl.begin());
}

} // namespace

// ============================================================================
// Corpus replay tests — real K1C geometry from the 2026-08-19 klippy capture.
// Mesh bounds on this machine: 5..215 both axes; the wipe strip sits at
// Y≈225, beyond the mesh area. The prep chain order observed on hardware:
// nozzle wipe → centre Z probes (ACCURATE_G28) → 4-corner mesh validation
// (full-chain prints) → serpentine mesh sweep.
// ============================================================================

TEST_CASE("Position classifier tracks the pre-fix K1C prep chain", "[print][collector][position]") {
    auto corpus = load_window("prefetch_d");
    PrintStartPositionClassifier c;
    c.set_bounds(5.0f, 215.0f, 5.0f, 215.0f);
    auto tl = replay(c, corpus);

    size_t wipe = first_index_of(tl, PositionActivity::WIPE);
    size_t center = first_index_of(tl, PositionActivity::CENTER_PROBE);
    size_t corner = first_index_of(tl, PositionActivity::CORNER_PROBE);
    size_t raster = first_index_of(tl, PositionActivity::RASTER);

    REQUIRE(wipe != SIZE_MAX);
    REQUIRE(center != SIZE_MAX);
    REQUIRE(corner != SIZE_MAX);
    REQUIRE(raster != SIZE_MAX);

    // The observed chain order: wipe → centre probes → corners → sweep.
    CHECK(wipe < center);
    CHECK(center < corner);
    CHECK(corner < raster);
    // RASTER latches through the end of the sweep.
    CHECK(tl.back() == PositionActivity::RASTER);
}

TEST_CASE("Position classifier tracks a second pre-fix print identically",
          "[print][collector][position]") {
    auto corpus = load_window("prefetch_a");
    PrintStartPositionClassifier c;
    c.set_bounds(5.0f, 215.0f, 5.0f, 215.0f);

    auto tl = replay(c, corpus);

    size_t wipe = first_index_of(tl, PositionActivity::WIPE);
    size_t center = first_index_of(tl, PositionActivity::CENTER_PROBE);
    size_t corner = first_index_of(tl, PositionActivity::CORNER_PROBE);
    size_t raster = first_index_of(tl, PositionActivity::RASTER);

    REQUIRE(wipe != SIZE_MAX);
    REQUIRE(center != SIZE_MAX);
    REQUIRE(corner != SIZE_MAX);
    REQUIRE(raster != SIZE_MAX);

    // The observed chain order: wipe → centre probes → corners → sweep.
    CHECK(wipe < center);
    CHECK(center < corner);
    CHECK(corner < raster);
    // RASTER latches through the end of the sweep.
    CHECK(tl.back() == PositionActivity::RASTER);
}

TEST_CASE("Position classifier tracks the flowrate print prep chain",
          "[print][collector][position]") {
    auto corpus = load_window("flowrate");
    PrintStartPositionClassifier c;
    c.set_bounds(5.0f, 215.0f, 5.0f, 215.0f);

    auto tl = replay(c, corpus);

    size_t wipe = first_index_of(tl, PositionActivity::WIPE);
    size_t center = first_index_of(tl, PositionActivity::CENTER_PROBE);
    size_t corner = first_index_of(tl, PositionActivity::CORNER_PROBE);
    size_t raster = first_index_of(tl, PositionActivity::RASTER);

    // This print started through the pre-start gcode block, whose
    // CX_ROUGH_G28 still runs the levelling calibration: the same
    // wipe → centre → corner-tour → sweep chain as the stock macro.
    REQUIRE(wipe != SIZE_MAX);
    REQUIRE(center != SIZE_MAX);
    REQUIRE(corner != SIZE_MAX);
    REQUIRE(raster != SIZE_MAX);

    CHECK(wipe < center);
    CHECK(center < corner);
    CHECK(corner < raster);
    CHECK(tl.back() == PositionActivity::RASTER);
}

TEST_CASE("Position classifier stays silent without mesh bounds", "[print][collector][position]") {
    auto corpus = load_window("prefetch_d");
    PrintStartPositionClassifier c; // no set_bounds

    for (const auto& [t, x, y, z] : corpus.samples) {
        c.note_position(x, y, z, t);
    }
    CHECK(c.activity() == PositionActivity::NONE);
    CHECK(c.window_size() > 0); // samples retained, verdicts withheld
}
