// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_event_journal.h"
#include "system/debug_bundle_collector.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

constexpr double kT0 = 1761040800.0;

std::string fixture_path(const std::string& name) {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) : ".";
    return root + "/tests/fixtures/journal/" + name;
}

JournalEntry entry(const char* kind, double time, std::string text = {}) {
    JournalEntry e;
    e.kind = kind;
    e.time = time;
    e.text = std::move(text);
    return e;
}

PluginFeatureMap journal_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    features[kJournalFeatureName] =
        PluginFeatureEntry{version, available, enabled, {}, available ? "" : "why"};
    return features;
}

} // namespace

TEST_CASE("journal classifier matches the plugin's on the shared fixture",
          "[plugin_event_journal]") {
    std::ifstream f(fixture_path("error_line_classes.json"));
    REQUIRE(f.is_open());
    json fixture;
    f >> fixture;
    const json& cases = fixture["cases"];
    REQUIRE(cases.size() > 20);
    for (const auto& c : cases) {
        const std::string line = c["line"].get<std::string>();
        INFO(line);
        CHECK(classify_journal_error(line) == c["class"].get<std::string>());
        CHECK(is_mcu_disconnect(line) == c["mcu_disconnect"].get<bool>());
    }
}

TEST_CASE("journal source decision over every plugin state", "[plugin_event_journal]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        JournalSource source;
    };
    const Row rows[] = {
        {"no plugin", false, journal_map(1, true, true), JournalSource::Screen},
        {"old plugin without a features map", true, {}, JournalSource::Screen},
        {"switched off", true, journal_map(1, true, false), JournalSource::Screen},
        {"unavailable", true, journal_map(1, false, false), JournalSource::Screen},
        {"contract below the minimum", true, journal_map(0, true, true), JournalSource::Screen},
        {"ready", true, journal_map(1, true, true), JournalSource::Plugin},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        const auto state = plugin_feature_state(row.plugin_available, row.features,
                                                kJournalFeatureName, kJournalFeatureMinVersion);
        const JournalPlan plan = decide_journal_source(state, /*recorder_built=*/true);
        CHECK(plan.source == row.source);
        CHECK(plan.record_locally == (row.source == JournalSource::Screen));

        INFO("a build without the recorder has only the plugin's journal");
        const JournalPlan bare = decide_journal_source(state, /*recorder_built=*/false);
        CHECK(bare.source ==
              (row.source == JournalSource::Plugin ? JournalSource::Plugin : JournalSource::None));
        CHECK_FALSE(bare.record_locally);
    }
}

TEST_CASE("local journal bounds evict the oldest", "[plugin_event_journal]") {
    SECTION("count") {
        LocalEventJournal journal(3, 1e9);
        for (int i = 0; i < 5; ++i) {
            journal.append(entry(journal_kind::kReady, kT0 + i * 60), kT0 + i * 60);
        }
        const auto held = journal.snapshot();
        REQUIRE(held.size() == 3);
        CHECK(held.front().time == kT0 + 120);
        CHECK(held.back().time == kT0 + 240);
    }
    SECTION("age") {
        LocalEventJournal journal(100, 3600);
        journal.append(entry(journal_kind::kReady, kT0), kT0);
        journal.append(entry(journal_kind::kDisconnect, kT0 + 1800), kT0 + 1800);
        journal.append(entry(journal_kind::kReady, kT0 + 4000), kT0 + 4000);
        const auto held = journal.snapshot();
        REQUIRE(held.size() == 2);
        CHECK(held.front().kind == journal_kind::kDisconnect);
    }
}

TEST_CASE("backfill de-duplicates by time and text and marks what it adds",
          "[plugin_event_journal]") {
    LocalEventJournal journal;
    REQUIRE(journal.append(entry(journal_kind::kGcodeError, kT0, "Move out of range"), kT0));
    // Live, the same line again is a repeat: it counts on the entry.
    REQUIRE(
        journal.append(entry(journal_kind::kGcodeError, kT0 + 1, "Move out of range"), kT0 + 1));

    std::vector<JournalEntry> backfill = {
        entry(journal_kind::kGcodeError, kT0 + 2, "Move out of range"),   // seen live
        entry(journal_kind::kGcodeError, kT0 + 120, "Move out of range"), // a later time
        entry(journal_kind::kGcodeError, kT0 - 30, "Must home axis first"),
        entry(journal_kind::kGcodeError, kT0 - 30, "Must home axis first"), // twice in one read
    };
    CHECK(journal.merge_backfill(backfill, kT0 + 120) == 2);

    const auto held = journal.snapshot();
    REQUIRE(held.size() == 3);
    CHECK(held[0].text == "Must home axis first");
    CHECK(held[0].recovered);
    CHECK_FALSE(held[1].recovered);
    CHECK(held[1].count == 2);
    CHECK(held[1].last_time == kT0 + 1);
    CHECK(held[2].time == kT0 + 120);
    CHECK(held[2].recovered);
    CHECK(journal.latest_time() == kT0 + 120);
}

TEST_CASE("repeated !! lines collapse as the plugin's journal collapses them",
          "[plugin_event_journal]") {
    std::ifstream f(fixture_path("repeats.json"));
    REQUIRE(f.good());
    json fixture;
    f >> fixture;
    const json& cases = fixture["cases"];
    REQUIRE(cases.size() >= 5);

    for (const auto& c : cases) {
        const std::string name = c["name"].get<std::string>();
        CAPTURE(name);
        LocalEventJournal journal;
        for (const auto& a : c["appends"]) {
            JournalEntry e = entry(a["kind"].get<std::string>().c_str(),
                                   kT0 + a["time"].get<double>(), a["text"].get<std::string>());
            if (a["class"].is_string()) {
                e.cls = a["class"].get<std::string>();
            }
            if (a.contains("detail")) {
                e.detail = a["detail"].get<std::string>();
            }
            if (a["job"].is_object()) {
                e.job_id = a["job"]["job_id"].get<std::string>();
                e.filename = a["job"]["filename"].get<std::string>();
            }
            const double now = e.time;
            journal.append(std::move(e), now);
        }
        const auto held = journal.snapshot();
        const json& expected = c["expected"];
        REQUIRE(held.size() == expected.size());
        for (std::size_t i = 0; i < held.size(); ++i) {
            CAPTURE(i);
            CHECK(held[i].time == kT0 + expected[i]["time"].get<double>());
            CHECK(held[i].text == expected[i]["text"].get<std::string>());
            CHECK(held[i].count == expected[i]["count"].get<int>());
            if (expected[i].contains("last_time")) {
                CHECK(held[i].last_time == kT0 + expected[i]["last_time"].get<double>());
            }
        }
    }
}

TEST_CASE("a counted entry survives the wire and reads as repeated", "[plugin_event_journal]") {
    const json plugin_entry = {{"id", 4},
                               {"time", kT0},
                               {"kind", "gcode_error"},
                               {"text", "Unknown command"},
                               {"class", "command"},
                               {"count", 12},
                               {"last_time", kT0 + 40},
                               {"job", nullptr}};
    const auto e = journal_entry_from_json(plugin_entry);
    REQUIRE(e.has_value());
    CHECK(e->count == 12);
    CHECK(e->last_time == kT0 + 40);
    CHECK(describe_journal_entry(*e).repeat_count == 12);
    CHECK(journal_entry_to_json(*e)["count"] == 12);
    CHECK(journal_entry_to_json(*e)["last_time"] == kT0 + 40);

    const auto once = journal_entry_from_json(json{{"time", kT0}, {"kind", "gcode_error"}});
    REQUIRE(once.has_value());
    CHECK(describe_journal_entry(*once).repeat_count == 1);
    CHECK_FALSE(journal_entry_to_json(*once).contains("count"));

    // A gcode_store backfill re-reading the twelve lines adds none of them.
    LocalEventJournal journal;
    REQUIRE(journal.load_json(json{{"entries", json::array({plugin_entry})}}, kT0 + 50));
    std::vector<JournalEntry> again;
    for (int i = 0; i < 12; ++i) {
        again.push_back(entry(journal_kind::kGcodeError, kT0 + i * 3.5, "Unknown command"));
        again.back().cls = "command";
    }
    CHECK(journal.merge_backfill(again, kT0 + 50) == 0);
    REQUIRE(journal.snapshot().size() == 1);
    CHECK(journal.snapshot()[0].count == 12);
}

TEST_CASE("a backfilled error with no job matches the live one stamped with its job",
          "[plugin_event_journal]") {
    LocalEventJournal journal;
    JournalEntry live = entry(journal_kind::kGcodeError, kT0, "Move out of range");
    live.job_id = "00002A";
    REQUIRE(journal.append(live, kT0));

    // gcode_store carries no job, and a remote screen's clock is a little off.
    CHECK(journal.merge_backfill({entry(journal_kind::kGcodeError, kT0 + 3, "Move out of range")},
                                 kT0 + 60) == 0);

    INFO("two different jobs' errors are still two events");
    JournalEntry other = entry(journal_kind::kGcodeError, kT0 + 1, "Move out of range");
    other.job_id = "00002B";
    CHECK(journal.append(other, kT0 + 1));
}

TEST_CASE("local journal survives a save and load", "[plugin_event_journal]") {
    LocalEventJournal journal;
    JournalEntry e = entry(journal_kind::kJobEnd, kT0);
    e.detail = "cancelled";
    e.job_id = "00002A";
    e.filename = "benchy.gcode";
    journal.append(e, kT0);
    journal.merge_backfill({entry(journal_kind::kShutdown, kT0 - 10, "Timer too close")}, kT0);

    const std::string path =
        "/tmp/helix_event_journal_test_" + std::to_string(::getpid()) + ".json";
    REQUIRE(journal.save_file(path));

    LocalEventJournal loaded;
    REQUIRE(loaded.load_file(path, kT0));
    std::remove(path.c_str());
    const auto held = loaded.snapshot();
    REQUIRE(held.size() == 2);
    CHECK(held[0].kind == journal_kind::kShutdown);
    CHECK(held[0].recovered);
    CHECK(held[1].detail == "cancelled");
    CHECK(held[1].job_id == "00002A");
    CHECK(held[1].filename == "benchy.gcode");

    CHECK_FALSE(loaded.load_json(json::array(), kT0));
}

TEST_CASE("backfill from the gcode store keeps new !! responses", "[plugin_event_journal]") {
    const std::vector<GcodeStoreEntry> store = {
        {"!! Must home axis first: 0 0 0", kT0 - 100, "response"},
        {"!! Heater extruder not heating at expected rate", kT0 + 5, "response"},
        {"// Klipper state: Ready", kT0 + 6, "response"},
        {"!! looks like an error", kT0 + 7, "command"},
        {"!! Lost communication with MCU 'mcu'", kT0 + 8, "response"},
    };
    const auto out = backfill_from_gcode_store(store, kT0);
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == journal_kind::kGcodeError);
    CHECK(out[0].text == "Heater extruder not heating at expected rate");
    CHECK(out[0].cls == "heater");
    CHECK(out[1].cls == "mcu");
}

TEST_CASE("backfill from history keeps jobs that ended in the gap", "[plugin_event_journal]") {
    const json result = {{"count", 3},
                         {"jobs",
                          {{{"job_id", "00002B"},
                            {"filename", "late.gcode"},
                            {"status", "klippy_shutdown"},
                            {"end_time", kT0 + 50}},
                           {{"job_id", "00002A"},
                            {"filename", "benchy.gcode"},
                            {"status", "completed"},
                            {"end_time", kT0 + 10}},
                           {{"job_id", "000029"},
                            {"filename", "old.gcode"},
                            {"status", "completed"},
                            {"end_time", kT0 - 10}},
                           {{"job_id", "00002C"},
                            {"filename", "now.gcode"},
                            {"status", "in_progress"},
                            {"end_time", nullptr}}}}};
    const auto out = backfill_from_history(result, kT0);
    REQUIRE(out.size() == 2);
    CHECK(out[0].job_id == "00002A");
    CHECK(out[0].detail == "completed");
    CHECK(out[1].filename == "late.gcode");

    CHECK(backfill_needs_klippy_log(false, out));
    CHECK_FALSE(backfill_needs_klippy_log(false, {out[0]}));
    CHECK(backfill_needs_klippy_log(true, {}));
    CHECK(backfill_from_history(json::object(), 0).empty());
}

TEST_CASE("backfill from klippy.log places shutdowns on the wall clock", "[plugin_event_journal]") {
    const std::string log = "Transition to shutdown state: before any start line\n"
                            "Start printer at Tue Oct 21 10:00:00 2026 (1761040800.0 1000.0)\n"
                            "Stats 1100.0: gcodein=0 mcu: mcu_awake=0.001\n"
                            "Stats 1200.5: gcodein=0 mcu: mcu_awake=0.001\n"
                            "Transition to shutdown state: MCU 'mcu' shutdown: Timer too close\n"
                            "Start printer at Tue Oct 21 11:00:00 2026 (1761044400.0 5000.0)\n"
                            "Transition to shutdown state: Lost communication with MCU 'mcu'\n";
    const auto out = backfill_from_klippy_log(log, 0);
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == journal_kind::kShutdown);
    CHECK(out[0].time == 1761040800.0 + 200.5);
    CHECK(out[0].text == "MCU 'mcu' shutdown: Timer too close");
    CHECK(out[0].cls == "mcu");
    CHECK(out[1].kind == journal_kind::kMcuDisconnect);
    CHECK(out[1].time == 1761044400.0);

    CHECK(backfill_from_klippy_log(log, 1761041500.0).size() == 1);
}

TEST_CASE("view and bundle read the plugin's journal and the screen's alike",
          "[plugin_event_journal]") {
    const json plugin_result = {{"entries",
                                 {{{"id", 7},
                                   {"time", kT0},
                                   {"kind", "klippy_shutdown"},
                                   {"text", "Timer too close"},
                                   {"class", "mcu"},
                                   {"detail", nullptr},
                                   {"job", {{"job_id", "00002A"}, {"filename", "benchy.gcode"}}}},
                                  {{"id", 8}, {"kind", "klippy_ready"}},
                                  "not an entry"}},
                                {"truncated", false},
                                {"latest_id", 8}};
    const auto from_plugin = parse_journal_result(plugin_result);
    REQUIRE(from_plugin.size() == 1);
    CHECK(from_plugin[0].id == 7);
    CHECK(from_plugin[0].job_id == "00002A");

    LocalEventJournal screen;
    JournalEntry e = entry(journal_kind::kShutdown, kT0, "Timer too close");
    e.cls = "mcu";
    e.job_id = "00002A";
    e.filename = "benchy.gcode";
    screen.append(e, kT0);
    const auto from_screen = parse_journal_result(screen.to_json());
    REQUIRE(from_screen.size() == 1);

    for (const auto* source : {&from_plugin, &from_screen}) {
        const JournalRow row = describe_journal_entry(source->front());
        CHECK(row.severity == JournalSeverity::Error);
        CHECK(std::string(row.title_tag) == "Klipper shut down");
        CHECK(row.message == "Timer too close");
    }

    const json section = build_event_journal_section(JournalSource::Plugin, from_plugin);
    CHECK(section["source"] == "plugin");
    CHECK(section["entries"][0]["job"]["filename"] == "benchy.gcode");
    CHECK(build_event_journal_section(JournalSource::Screen, from_screen)["source"] == "screen");
    CHECK(build_event_journal_section(JournalSource::None, {})["source"] == "none");
}

TEST_CASE("each journal kind describes itself", "[plugin_event_journal]") {
    auto describe = [](const char* kind, std::string detail = {}) {
        JournalEntry e = entry(kind, kT0, "text");
        e.detail = std::move(detail);
        return describe_journal_entry(e);
    };
    CHECK(describe(journal_kind::kMcuDisconnect).severity == JournalSeverity::Error);
    CHECK(describe(journal_kind::kDisconnect).severity == JournalSeverity::Warning);
    CHECK(describe(journal_kind::kReady).severity == JournalSeverity::Success);
    CHECK(describe(journal_kind::kFirmwareRestart).severity == JournalSeverity::Info);
    CHECK(describe(journal_kind::kGcodeError).message == "text");
    CHECK(describe(journal_kind::kRunout, "encoder").message == "encoder");
    CHECK(std::string(describe(journal_kind::kPause, "runout").message_tag) == "Filament runout");
    CHECK(describe(journal_kind::kPause, "unknown").message_tag == nullptr);
    CHECK(describe(journal_kind::kJobEnd, "completed").severity == JournalSeverity::Success);
    CHECK(describe(journal_kind::kJobEnd, "cancelled").severity == JournalSeverity::Info);
    const JournalRow failed = describe(journal_kind::kJobEnd, "klippy_shutdown");
    CHECK(failed.severity == JournalSeverity::Error);
    CHECK(std::string(failed.message_tag) == "Klipper shut down");
    CHECK(describe(journal_kind::kJobEnd, "error").message == "error");
    CHECK(describe(journal_kind::kJobEnd, "error").message_tag == nullptr);

    const auto ordered = journal_view_order(
        {entry(journal_kind::kReady, kT0), entry(journal_kind::kDisconnect, kT0 + 5)});
    CHECK(ordered.front().kind == journal_kind::kDisconnect);
}

TEST_CASE("pause cause from the screen's recent entries", "[plugin_event_journal]") {
    const double now = kT0 + 100;
    CHECK(journal_pause_cause(
              {entry(journal_kind::kRunout, now - 2), entry(journal_kind::kGcodeError, now - 1)},
              now) == "runout");
    CHECK(journal_pause_cause({entry(journal_kind::kGcodeError, now - 1)}, now) == "error");
    CHECK(journal_pause_cause({entry(journal_kind::kRunout, now - 60)}, now) == "unknown");
}

TEST_CASE("debug bundle journal reads the plugin when ready and redacts it",
          "[plugin_event_journal]") {
    const json plugin_reply = {{"result",
                                {{"entries",
                                  {{{"id", 3},
                                    {"time", kT0},
                                    {"kind", "gcode_error"},
                                    {"text", "Webhook to user@example.com failed"},
                                    {"class", "other"}}}}}}};
    const std::vector<JournalEntry> screen = {entry(journal_kind::kReady, kT0 + 1)};

    const json from_plugin = DebugBundleCollector::build_event_journal(true, plugin_reply, screen);
    CHECK(from_plugin["source"] == "plugin");
    REQUIRE(from_plugin["entries"].size() == 1);
    CHECK(from_plugin["entries"][0]["text"] == "Webhook to [REDACTED_EMAIL] failed");

    const json failed = DebugBundleCollector::build_event_journal(
        true, json{{"error", "HTTP 404 from /server/helix/journal"}}, screen);
    CHECK(failed["source"] == "screen");
    CHECK(failed["entries"][0]["kind"] == "klippy_ready");

    const json not_ready = DebugBundleCollector::build_event_journal(false, plugin_reply, screen);
    CHECK(not_ready["source"] == "screen");
}
