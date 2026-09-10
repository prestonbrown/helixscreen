// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preprint_history_names.cpp
 * @brief The on-disk shape of a prediction-history entry's "phases" object.
 *
 * Phases are stored by name. A name is stable across a phase being inserted in
 * the middle of the enum; an ordinal is not, and a stored ordinal read back
 * under a renumbered enum attributes a measured duration to the wrong phase.
 *
 * The loader has to survive documents it does not fully understand: a config
 * written by a newer build names phases this build has never heard of, and
 * dropping the whole entry over one such key would throw away the history a
 * user's ETA is built from.
 */

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "preprint_predictor.h"
#include "print_start_phase.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

/// Points Config::get_instance() at a Config this test owns, so the history
/// document under test cannot leak into another test's singleton.
class HistoryConfigFixture {
  protected:
    Config config;
    Config* saved_instance_ = nullptr;
    std::string temp_dir;

    void seed_entries(const json& entries) {
        ConfigTestAccess::data(config) = json{{"config_version", helix::CURRENT_CONFIG_VERSION},
                                              {"active_printer_id", "voron"},
                                              {"print_start_history", {{"entries", entries}}}};
    }

  public:
    HistoryConfigFixture() {
        temp_dir =
            (fs::temp_directory_path() / ("helix_history_names_" + std::to_string(::getpid())))
                .string();
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);
        ConfigTestAccess::path(config) = temp_dir + "/settings.json";
        saved_instance_ = ConfigTestAccess::instance_ref();
        ConfigTestAccess::instance_ref() = &config;
    }
    ~HistoryConfigFixture() {
        ConfigTestAccess::instance_ref() = saved_instance_;
        fs::remove_all(temp_dir);
    }
};

int phase_key(PrintStartPhase phase) {
    return static_cast<int>(phase);
}

} // namespace

TEST_CASE_METHOD(HistoryConfigFixture, "Preprint history: named phases load onto their enum values",
                 "[print][predictor][history]") {
    seed_entries(json::array({json{
        {"total", 300},
        {"timestamp", 1700000000},
        {"temp_bucket", 1},
        {"phases",
         {{"HOMING", 31}, {"SOAKING", 240}, {"QGL", 62}, {"BED_MESH", 93}, {"PURGING", 16}}}}}));

    const auto entries = PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 1);

    const auto& durations = entries[0].phase_durations;
    CHECK(durations.at(phase_key(PrintStartPhase::HOMING)) == 31);
    CHECK(durations.at(phase_key(PrintStartPhase::SOAKING)) == 240);
    CHECK(durations.at(phase_key(PrintStartPhase::QGL)) == 62);
    CHECK(durations.at(phase_key(PrintStartPhase::BED_MESH)) == 93);
    CHECK(durations.at(phase_key(PrintStartPhase::PURGING)) == 16);
    CHECK(entries[0].total_seconds == 300);
}

TEST_CASE_METHOD(HistoryConfigFixture, "Preprint history: an unknown phase name is skipped",
                 "[print][predictor][history]") {
    // A newer build's phase name. The entry it appears in still counts — the
    // known phases in it are real measurements.
    seed_entries(json::array({json{{"total", 120},
                                   {"timestamp", 1700000000},
                                   {"temp_bucket", 1},
                                   {"phases", {{"HOMING", 30}, {"FUTURE_PHASE", 90}}}}}));

    const auto entries = PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].total_seconds == 120);
    CHECK(entries[0].phase_durations.size() == 1);
    CHECK(entries[0].phase_durations.at(phase_key(PrintStartPhase::HOMING)) == 30);
}

TEST_CASE_METHOD(HistoryConfigFixture, "Preprint history: a leftover ordinal key is skipped",
                 "[print][predictor][history]") {
    // Only a document that never went through the migration still holds these.
    // Reading "5" as a phase index is the exact misattribution the named format
    // exists to prevent.
    seed_entries(json::array({json{{"total", 120},
                                   {"timestamp", 1700000000},
                                   {"temp_bucket", 1},
                                   {"phases", {{"5", 62}, {"BED_MESH", 40}}}}}));

    const auto entries = PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].phase_durations.size() == 1);
    CHECK(entries[0].phase_durations.at(phase_key(PrintStartPhase::BED_MESH)) == 40);
    CHECK(entries[0].phase_durations.count(phase_key(PrintStartPhase::QGL)) == 0);
    CHECK(entries[0].phase_durations.count(phase_key(PrintStartPhase::HEATING_NOZZLE)) == 0);
}

TEST_CASE("Preprint history: entries serialize with phase names as keys",
          "[print][predictor][history]") {
    PreprintEntry entry;
    entry.total_seconds = 300;
    entry.timestamp = 1700000000;
    entry.temp_bucket = 2;
    entry.window = PreprintWindow::HostPreStart;
    entry.phase_durations = {
        {phase_key(PrintStartPhase::HOMING), 31},
        {phase_key(PrintStartPhase::SOAKING), 240},
        {phase_key(PrintStartPhase::QGL), 62},
        {phase_key(PrintStartPhase::PURGING), 16},
    };

    const json out = PreprintPredictor::entries_to_json({entry});
    REQUIRE(out.is_array());
    REQUIRE(out.size() == 1);

    const json phases = out[0]["phases"];
    INFO("serialized phases: " << phases.dump());
    CHECK(phases.value("HOMING", -1) == 31);
    CHECK(phases.value("SOAKING", -1) == 240);
    CHECK(phases.value("QGL", -1) == 62);
    CHECK(phases.value("PURGING", -1) == 16);

    // A number as a key is the format this replaces; seeing one back means a
    // caller wrote an ordinal that the next enum insertion would re-point.
    for (auto it = phases.begin(); it != phases.end(); ++it) {
        CAPTURE(it.key());
        CHECK(it.key().find_first_of("0123456789") == std::string::npos);
    }

    CHECK(out[0]["total"] == 300);
    CHECK(out[0]["timestamp"] == 1700000000);
    CHECK(out[0]["temp_bucket"] == 2);
    CHECK(out[0]["window"] == static_cast<int>(PreprintWindow::HostPreStart));
}

TEST_CASE_METHOD(HistoryConfigFixture, "Preprint history: a save/load round trip keeps every phase",
                 "[print][predictor][history]") {
    PreprintEntry entry;
    entry.total_seconds = 200;
    entry.timestamp = 1700000001;
    entry.temp_bucket = 1;
    entry.phase_durations = {
        {phase_key(PrintStartPhase::HOMING), 30},
        {phase_key(PrintStartPhase::SOAKING), 120},
        {phase_key(PrintStartPhase::Z_TILT), 45},
        {phase_key(PrintStartPhase::CLEANING), 20},
    };

    seed_entries(PreprintPredictor::entries_to_json({entry}));

    const auto loaded = PreprintPredictor::load_entries_from_config();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].phase_durations == entry.phase_durations);
    CHECK(loaded[0].total_seconds == entry.total_seconds);
    CHECK(loaded[0].temp_bucket == entry.temp_bucket);
}
