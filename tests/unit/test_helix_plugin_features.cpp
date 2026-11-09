// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_overview_overlay.h"

#include "helix_plugin_features.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::plugin_overview_row;
using json = nlohmann::json;

namespace {

PluginFeatureMap plr_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    PluginFeatureEntry entry;
    entry.version = version;
    entry.available = available;
    entry.enabled = enabled;
    entry.reason = available ? "" : "why";
    features["plr"] = entry;
    return features;
}

} // namespace

TEST_CASE("plugin_feature_state over plugin and feature states", "[plugin_feature]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        int min_version;
        PluginFeatureState expected;
    };
    const Row rows[] = {
        {"no plugin", false, plr_map(1, true, true), 1, PluginFeatureState::Absent},
        {"old plugin without a features map", true, {}, 1, PluginFeatureState::Absent},
        {"no minimum", true, plr_map(1, true, true), 0, PluginFeatureState::Ready},
        {"contract below the minimum", true, plr_map(1, true, true), 2, PluginFeatureState::Absent},
        {"contract at the minimum", true, plr_map(2, true, true), 2, PluginFeatureState::Ready},
        {"contract above the minimum", true, plr_map(3, true, true), 2, PluginFeatureState::Ready},
        {"unavailable", true, plr_map(1, false, false), 1, PluginFeatureState::Absent},
        {"unavailable but switched on", true, plr_map(1, false, true), 1,
         PluginFeatureState::Absent},
        {"switched off", true, plr_map(1, true, false), 1, PluginFeatureState::Disabled},
        {"old contract switched off", true, plr_map(1, true, false), 2, PluginFeatureState::Absent},
        {"ready", true, plr_map(1, true, true), 1, PluginFeatureState::Ready},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        CHECK(plugin_feature_state(row.plugin_available, row.features, "plr", row.min_version) ==
              row.expected);
    }

    INFO("feature not in the map");
    CHECK(plugin_feature_state(true, plr_map(1, true, true), "file_analysis", 1) ==
          PluginFeatureState::Absent);
}

TEST_CASE("an overview line carries the reason of an unavailable feature", "[plugin_feature]") {
    const std::vector<PluginFeatureLine> unavailable =
        plugin_feature_lines(plr_map(1, false, false));
    REQUIRE(unavailable.size() == 1);
    CHECK(unavailable[0].reason == "why");
    CHECK_FALSE(plr_map(1, false, false).at("plr").saving.has_value());
    const std::vector<PluginFeatureLine> ready = plugin_feature_lines(plr_map(1, true, true));
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].reason.empty());
}

TEST_CASE("parse_plugin_features reads the status features map", "[plugin_feature]") {
    const json status = {
        {"enabled", true},
        {"api", 2},
        {"features",
         {{"plr", {{"version", 2}, {"available", true}, {"enabled", false}, {"reason", nullptr}}},
          {"event_journal",
           {{"version", 1}, {"available", false}, {"enabled", false}, {"reason", "moonraker<0.9"}}},
          {"bogus", "not an object"}}},
    };

    const PluginFeatureMap features = parse_plugin_features(status);

    REQUIRE(features.size() == 2);
    const PluginFeatureEntry& plr = features.at("plr");
    CHECK(plr.version == 2);
    CHECK(plr.available);
    CHECK_FALSE(plr.enabled);
    CHECK(plr.reason.empty());
    const PluginFeatureEntry& journal = features.at("event_journal");
    CHECK_FALSE(journal.available);
    CHECK(journal.reason == "moonraker<0.9");
    CHECK(features.count("bogus") == 0);
}

TEST_CASE("parse_plugin_features tolerates old and malformed replies", "[plugin_feature]") {
    CHECK(parse_plugin_features(json{{"enabled", true}, {"version", "1.2.0"}}).empty());
    CHECK(parse_plugin_features(json{{"features", json::array({1, 2})}}).empty());
    CHECK(parse_plugin_features(json{{"features", nullptr}}).empty());
    CHECK(parse_plugin_features(json::array()).empty());

    const PluginFeatureMap features = parse_plugin_features(
        json{{"features", {{"plr", {{"version", "two"}, {"available", "yes"}}}}}});
    REQUIRE(features.count("plr") == 1);
    CHECK(features.at("plr").version == 0);
    CHECK_FALSE(features.at("plr").enabled);
    CHECK(plugin_feature_state(true, features, "plr", 1) == PluginFeatureState::Absent);
}

TEST_CASE("plugin_reason_text says what a reason code means", "[plugin_feature]") {
    CHECK(plugin_reason_text("no_virtual_sdcard") == "Klipper has no [virtual_sdcard] section.");
    CHECK(plugin_reason_text("not_listed") == "Not set up for this printer model yet.");
    // A code with a detail suffix maps on the code before the colon.
    CHECK(plugin_reason_text("snapshot_unreachable: /webcam/?action=snapshot") ==
          "Can't reach the camera, so this screen takes photos.");
    CHECK(plugin_reason_text("load_failed: SyntaxError: bad input") ==
          "Part of the plugin failed to load. Reinstall the plugin.");
    CHECK(plugin_reason_text("unsafe_staging_dir: .") ==
          "A folder name in moonraker.conf is unsafe.");
    CHECK(plugin_reason_text("unsafe_root: /home/pi/printer_data/gcodes") ==
          "Timelapse or logs folder overlaps a protected one.");
    // Anything else reads as the plugin wrote it.
    CHECK(plugin_reason_text("moonraker<0.9") == "moonraker<0.9");
    CHECK(plugin_reason_text("").empty());
}

TEST_CASE("every plugin reason code is listed once", "[plugin_feature]") {
    for (size_t i = 0; i < std::size(kPluginReasonTexts); ++i) {
        for (size_t j = i + 1; j < std::size(kPluginReasonTexts); ++j) {
            CHECK(std::string(kPluginReasonTexts[i].code) != kPluginReasonTexts[j].code);
        }
    }
}

TEST_CASE("an overview row groups by status and gives only an unavailable feature a reason",
          "[plugin_feature]") {
    PluginFeatureMap features;
    auto entry = [](bool available, bool enabled, std::string reason) {
        PluginFeatureEntry e;
        e.version = 1;
        e.available = available;
        e.enabled = enabled;
        e.reason = std::move(reason);
        return e;
    };
    features["photos"] = entry(false, false, "snapshot_unreachable: /webcam/?action=snapshot");
    features["plr"] = entry(false, false, "not_listed");
    features["print_hooks"] = entry(true, false, "");
    features["file_analysis"] = entry(true, true, "");
    features["mystery"] = entry(false, false, "");

    const std::vector<PluginFeatureLine> lines = plugin_feature_lines(features);
    auto row = [&](const std::string& name) {
        const auto it = std::find_if(lines.begin(), lines.end(),
                                     [&](const PluginFeatureLine& l) { return l.name == name; });
        REQUIRE(it != lines.end());
        return plugin_overview_row(*it);
    };

    CHECK(row("photos").status == PluginFeatureStatus::Unavailable);
    CHECK(row("photos").reason == "Can't reach the camera, so this screen takes photos.");
    CHECK(row("plr").status == PluginFeatureStatus::Unavailable);
    CHECK(row("plr").reason == "Not set up for this printer model yet.");
    CHECK(row("print_hooks").status == PluginFeatureStatus::Off);
    CHECK(row("print_hooks").reason.empty());
    CHECK(row("file_analysis").status == PluginFeatureStatus::Active);
    CHECK(row("file_analysis").reason.empty());
    // Unavailable with no reason given still says something in plain words.
    CHECK(row("mystery").status == PluginFeatureStatus::Unavailable);
    CHECK(row("mystery").reason == "The plugin didn't say why.");

    // A reason on a feature that can run is never shown.
    PluginFeatureLine off;
    off.name = "print_hooks";
    off.status = PluginFeatureStatus::Off;
    off.reason = "load_failed";
    CHECK(plugin_overview_row(off).reason.empty());
}
