// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "core/gcode_filename.h"
#include "helix_plugin_features.h"
#include "print_photos.h"
#include "storage_report.h"
#include "storage_report_service.h"

#include <ctime>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::storage;
using nlohmann::json;

namespace {

std::size_t consumer_at(const char* key) {
    const auto i = consumer_index(key);
    REQUIRE(i.has_value());
    return *i;
}

json status_with(const json& entry) {
    return json{{"api", 2}, {"features", {{kStorageFeature, entry}}}};
}

PluginFeatureState storage_state(bool installed, const json& status) {
    return plugin_feature_state(installed, parse_plugin_features(status), kStorageFeature,
                                kStorageMinVersion);
}

const json kReady = {{"version", 1}, {"available", true}, {"enabled", true}, {"reason", nullptr}};

constexpr uint64_t kMb = 1024ULL * 1024ULL;

} // namespace

// ============================================================================
// Where the report comes from
// ============================================================================

TEST_CASE("Storage reads the plugin's report only when its feature is ready",
          "[plugin_storage][plugin_feature]") {
    CHECK(report_source(storage_state(true, status_with(kReady))) == ReportSource::Plugin);

    json disabled = kReady;
    disabled["enabled"] = false;
    json unavailable = kReady;
    unavailable["available"] = false;
    unavailable["reason"] = "load_failed";
    json old = kReady;
    old["version"] = 0;

    CHECK(report_source(storage_state(false, status_with(kReady))) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, json{{"version", "1.2.0"}})) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, status_with(disabled))) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, status_with(unavailable))) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, status_with(old))) == ReportSource::FileApi);
}

TEST_CASE("Automatic cleanup is plugin-only: greyed with Install without the plugin",
          "[plugin_storage][plugin_feature]") {
    const PluginFeatureMap none;
    CHECK(plugin_switch_mode(false, none, kAutoCleanupFeature, kAutoCleanupMinVersion, false,
                             true) == PluginSwitchMode::NeedsPlugin);
    const PluginFeatureMap ready =
        parse_plugin_features(json{{"features", {{kAutoCleanupFeature, kReady}}}});
    CHECK(plugin_switch_mode(true, ready, kAutoCleanupFeature, kAutoCleanupMinVersion, false,
                             false) == PluginSwitchMode::Switch);
}

// ============================================================================
// The plugin's report
// ============================================================================

TEST_CASE("A plugin storage report parses per consumer", "[plugin_storage]") {
    const json result = {
        {"volume", {{"total", 1000 * kMb}, {"used", 600 * kMb}, {"free", 400 * kMb}}},
        {"floor", 100 * kMb},
        {"consumers", json::array({
                          {{"name", "helix_temp"},
                           {"bytes", 300},
                           {"files", 2},
                           {"available", true},
                           {"cleanable", true}},
                          {{"name", "user_gcode"},
                           {"bytes", 5000},
                           {"files", 7},
                           {"available", true},
                           {"cleanable", true}},
                          {{"name", "timelapse"},
                           {"bytes", 0},
                           {"files", 0},
                           {"available", false},
                           {"cleanable", false}},
                          {{"name", "from_a_newer_plugin"}, {"bytes", 9}, {"available", true}},
                      })},
    };
    const auto report = parse_storage_result(result);
    REQUIRE(report.has_value());
    CHECK(report->total == 1000 * kMb);
    CHECK(report->free == 400 * kMb);

    const ConsumerUsage& temp = report->consumers[consumer_at("helix_temp")];
    CHECK(temp.bytes == 300);
    CHECK(temp.files == 2);
    CHECK(temp.available);
    CHECK(temp.cleanable);

    // The app never offers to clean the user's own files, whatever a reply says.
    CHECK_FALSE(report->consumers[consumer_at("user_gcode")].cleanable);
    CHECK(report->consumers[consumer_at("user_gcode")].bytes == 5000);
    CHECK_FALSE(report->consumers[consumer_at("timelapse")].available);
    CHECK_FALSE(report->consumers[consumer_at("plr")].available);
}

TEST_CASE("A reply that is not a storage report is refused", "[plugin_storage]") {
    CHECK_FALSE(parse_storage_result(json::object()).has_value());
    CHECK_FALSE(parse_storage_result(json{{"consumers", "nope"}}).has_value());
    CHECK_FALSE(parse_storage_result(json::array()).has_value());
    const auto no_volume = parse_storage_result(json{{"consumers", json::array()}});
    REQUIRE(no_volume.has_value());
    CHECK_FALSE(no_volume->total.has_value());
}

// ============================================================================
// File-API fallback
// ============================================================================

TEST_CASE("The fallback sorts gcodes files into the plugin's consumers", "[plugin_storage]") {
    gcode::set_plugin_link_dir("");
    CHECK(classify_gcodes_path(".helix_temp/x.gcode") == consumer_at("helix_temp"));
    CHECK(classify_gcodes_path("helix_staging/x.gcode") == consumer_at("helix_temp"));
    CHECK(classify_gcodes_path(".helix_print/full/sub/x.gcode") == consumer_at("helix_temp"));
    CHECK(classify_gcodes_path("helix_links/full/x.gcode") == consumer_at("user_gcode"));
    gcode::set_plugin_link_dir("helix_links");
    CHECK(classify_gcodes_path("helix_links/full/x.gcode") == consumer_at("helix_temp"));
    gcode::set_plugin_link_dir("");
    CHECK(classify_gcodes_path(".helix_index/sub/x.gcode.layers.jsonl") ==
          consumer_at("analysis_sidecars"));
    CHECK(classify_gcodes_path(std::string(kPhotosDir) + "/00002A_end.jpg") ==
          consumer_at("photos"));
    CHECK(classify_gcodes_path(".thumbs/x-32x32.png") == consumer_at("thumbnails"));
    CHECK(classify_gcodes_path("deep/er/.thumbs/x-300x300.png") == consumer_at("thumbnails"));
    CHECK(classify_gcodes_path("deep/.thumbsnot/x.png") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path("sub/.helix_temp/x.gcode") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path(".helix_temp") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path("x.thumbs") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path("benchy.gcode") == consumer_at("user_gcode"));
}

TEST_CASE("A get_directory listing parses in both shapes", "[plugin_storage]") {
    const json object = {
        {"dirs", json::array({{{"dirname", "sub"}}, {{"dirname", ".thumbs"}}})},
        {"files",
         json::array({{{"filename", "a.gcode"}, {"size", 10}, {"modified", 5.0}}, {{"size", 3}}})},
        {"disk_usage", {{"total", 100}, {"used", 40}, {"free", 60}}},
    };
    const auto listing = parse_directory_result(object);
    REQUIRE(listing.has_value());
    REQUIRE(listing->files.size() == 1);
    CHECK(listing->files[0].name == "a.gcode");
    CHECK(listing->files[0].size == 10);
    CHECK(listing->files[0].modified == 5.0);
    CHECK(listing->dirs == std::vector<std::string>{"sub", ".thumbs"});
    CHECK(listing->free == 60u);
    CHECK(listing->total == 100u);

    const json flat = json::array({{{"path", "v.mp4"}, {"size", 7}, {"modified", 1.0}}});
    const auto flat_listing = parse_directory_result(flat);
    REQUIRE(flat_listing.has_value());
    REQUIRE(flat_listing->files.size() == 1);
    CHECK(flat_listing->files[0].name == "v.mp4");
    CHECK(flat_listing->dirs.empty());
    CHECK_FALSE(flat_listing->free.has_value());

    CHECK_FALSE(parse_directory_result(json{{"dirs", json::array()}}).has_value());
}

TEST_CASE("Without the plugin only timelapse can be cleaned", "[plugin_storage]") {
    // The app cannot ask which copy, index or file a waiting power-loss
    // snapshot needs, photos belong to records only the plugin keeps, and
    // Moonraker's logs root refuses deletes.
    for (std::size_t i = 0; i < kConsumerCount; ++i) {
        const std::string key = kConsumers[i].key;
        CAPTURE(key);
        CHECK(fallback_cleanable(i) == (key == "timelapse"));
    }
    const double now = 100000.0;
    for (const char* kept : {"helix_temp", "analysis_sidecars", "photos", "user_gcode",
                             "thumbnails", "3mf_outputs", "plr", "logs"}) {
        CAPTURE(kept);
        CHECK_FALSE(fallback_removes(consumer_at(kept), {"old.mp4", 1, 0.0}, now, 0.0));
    }
    CHECK_FALSE(fallback_removes(kConsumerCount, {"old.mp4", 1, 0.0}, now, 0.0));
}

TEST_CASE("The fallback deletes only names its consumer owns, old enough", "[plugin_storage]") {
    const double now = 100000.0;
    const std::size_t timelapse = consumer_at("timelapse");
    CHECK_FALSE(fallback_removes(timelapse, {"benchy.mp4", 5, now - 10}, now, 60.0));
    CHECK(fallback_removes(timelapse, {"benchy.MP4", 5, 0.0}, now, 0.0));
    CHECK(fallback_removes(timelapse, {"benchy.jpg", 5, 0.0}, now, 0.0));
    CHECK_FALSE(fallback_removes(timelapse, {"stray.gcode", 5, 0.0}, now, 0.0));
    CHECK_FALSE(fallback_removes(timelapse, {"printer.cfg", 5, 0.0}, now, 0.0));
    CHECK_FALSE(
        fallback_removes(timelapse, {"rendering.mp4", 5, now - kFallbackMinAgeS + 1}, now, 0.0));
}

TEST_CASE("The app cleans nothing while the plugin refuses this printer's directories",
          "[plugin_storage]") {
    json unavailable = kReady;
    unavailable["available"] = false;
    unavailable["reason"] = "unsafe_root: /home/pi/printer_data/gcodes";
    CHECK_FALSE(fallback_may_clean(true, parse_plugin_features(status_with(unavailable))));
    CHECK(fallback_may_clean(false, parse_plugin_features(status_with(unavailable))));
    CHECK(fallback_may_clean(true, PluginFeatureMap{}));
    CHECK(fallback_may_clean(true, parse_plugin_features(status_with(kReady))));
}

TEST_CASE("Timelapse is cleaned only from a root of its own", "[plugin_storage]") {
    const auto roots = [](const char* timelapse) {
        json r = json::array({{{"name", "gcodes"}, {"path", "/home/pi/printer_data/gcodes"}},
                              {{"name", "config"}, {"path", "/home/pi/printer_data/config/"}}});
        if (timelapse != nullptr) {
            r.push_back({{"name", "timelapse"}, {"path", timelapse}});
        }
        return r;
    };
    CHECK(timelapse_root_safe(roots("/home/pi/timelapse")));
    CHECK(timelapse_root_safe(roots("/home/pi/printer_data/gcodes/timelapse")));
    CHECK(timelapse_root_safe(roots("/home/pi/printer_data/gcodesx")));
    CHECK_FALSE(timelapse_root_safe(roots("/home/pi/printer_data/gcodes")));
    CHECK_FALSE(timelapse_root_safe(roots("/home/pi/printer_data/gcodes/")));
    CHECK_FALSE(timelapse_root_safe(roots("/home/pi/printer_data/config")));
    CHECK_FALSE(timelapse_root_safe(roots("/home/pi/printer_data")));
    CHECK_FALSE(timelapse_root_safe(roots("/home/pi")));
    CHECK_FALSE(timelapse_root_safe(roots("/")));
    CHECK_FALSE(timelapse_root_safe(roots(nullptr)));
    CHECK_FALSE(timelapse_root_safe(json::object()));
    CHECK_FALSE(timelapse_root_safe(
        json::array({{{"name", "timelapse"}, {"path", "/home/pi/timelapse"}}})));
}

TEST_CASE("A timelapse root holding anything but renders is not timelapse's own",
          "[plugin_storage]") {
    DirectoryListing own;
    own.files = {{"benchy.mp4", 5, 0.0}, {"benchy.jpg", 1, 0.0}};
    CHECK(timelapse_listing_is_own(own));
    CHECK(timelapse_listing_is_own(DirectoryListing{}));
    DirectoryListing gcodes = own;
    gcodes.files.push_back({"benchy.gcode", 9, 0.0});
    CHECK_FALSE(timelapse_listing_is_own(gcodes));
    DirectoryListing config = own;
    config.files.push_back({"printer.cfg", 9, 0.0});
    CHECK_FALSE(timelapse_listing_is_own(config));
    DirectoryListing with_dir = own;
    with_dir.dirs = {".thumbs"};
    CHECK_FALSE(timelapse_listing_is_own(with_dir));
}

TEST_CASE("A start refused for space shows the space message", "[plugin_storage]") {
    CHECK(std::string(print_start_failure_tag(kNoSpaceRpcCode)) == kNoUploadSpaceMessage);
    CHECK(std::string(print_start_failure_tag(500)) == "Failed to start print: {}");
    CHECK(std::string(print_start_failure_tag(0)) == "Failed to start print: {}");
}

TEST_CASE("A cleanup says what it freed and what failed", "[plugin_storage]") {
    CHECK(cleanup_message({100, 0, false}) == CleanupMessage::Freed);
    CHECK(cleanup_message({100, 2, false}) == CleanupMessage::FreedPartly);
    CHECK(cleanup_message({0, 2, false}) == CleanupMessage::NothingFreed);
    CHECK(cleanup_message({0, 0, true}) == CleanupMessage::NothingFreed);
    CHECK(cleanup_message({0, 0, false}) == CleanupMessage::NothingToFree);
}

TEST_CASE("Only the plugin's out-of-space answer is a final refusal", "[plugin_storage]") {
    CHECK(is_no_space_refusal(507));
    for (int code : {0, 400, 409, 500, 503, -32601}) {
        CAPTURE(code);
        CHECK_FALSE(is_no_space_refusal(code));
    }
}

TEST_CASE("An upload must leave the free-space floor", "[plugin_storage]") {
    CHECK(upload_space(kFreeSpaceFloorBytes + 10, 10) == UploadSpace::Ok);
    CHECK(upload_space(kFreeSpaceFloorBytes + 9, 10) == UploadSpace::TooFull);
    CHECK(upload_space(5, 10) == UploadSpace::TooFull);
    CHECK(upload_space(std::nullopt, 10) == UploadSpace::Unknown);
}

// ============================================================================
// The service
// ============================================================================

namespace {

/// The first call to @p method still waiting for its reply, removed.
test::FakeMoonrakerClient::RpcCall take_call(test::FakeMoonrakerClient& client,
                                             const std::string& method) {
    for (auto it = client.rpc_calls.begin(); it != client.rpc_calls.end(); ++it) {
        if (it->method == method) {
            auto call = *it;
            client.rpc_calls.erase(it);
            return call;
        }
    }
    FAIL("no " << method << " call");
    return {};
}

json listing(json files, json dirs = json::array()) {
    return json{{"result", {{"files", std::move(files)}, {"dirs", std::move(dirs)}}}};
}

/// Answer the fallback's server.files.roots with the timelapse root at @p path.
void answer_roots(test::FakeMoonrakerClient& client, const char* path) {
    take_call(client, "server.files.roots")
        .success_cb(json{{"result", json::array({{{"name", "gcodes"}, {"path", "/pd/gcodes"}},
                                                 {{"name", "config"}, {"path", "/pd/config"}},
                                                 {{"name", "timelapse"}, {"path", path}}})}});
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "The fallback lists the known roots and sums them",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    std::optional<StorageReport> got;
    std::optional<ReportSource> from;
    service.fetch(ReportSource::FileApi, true, [&](ReportSource s, std::optional<StorageReport> r) {
        from = s;
        got = r;
    });
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };
    answer_roots(client, "/home/pi/timelapse");

    // gcodes root: one file, two folders; the volume rides along.
    auto root = take_call(client, "server.files.get_directory");
    CHECK(root.params["path"] == "gcodes");
    json root_reply = listing(json::array({{{"filename", "benchy.gcode"}, {"size", 1000}}}),
                              json::array({{{"dirname", ".helix_temp"}}, {{"dirname", "sub"}}}));
    root_reply["result"]["disk_usage"] = {{"total", 9000}, {"free", 4000}};
    root.success_cb(root_reply);
    pump();

    auto timelapse = take_call(client, "server.files.get_directory");
    CHECK(timelapse.params["path"] == "timelapse");
    timelapse.error_cb(MoonrakerError::unknown("Not Found", "server.files.get_directory"));
    pump();

    auto logs = take_call(client, "server.files.get_directory");
    CHECK(logs.params["path"] == "logs");
    logs.success_cb(listing(json::array({{{"filename", "klippy.log"}, {"size", 50}},
                                         {{"filename", "klippy.log.1"}, {"size", 5}}})));
    pump();

    auto temp = take_call(client, "server.files.get_directory");
    CHECK(temp.params["path"] == "gcodes/.helix_temp");
    temp.success_cb(listing(json::array({{{"filename", "mod.gcode"}, {"size", 300}}})));
    pump();

    auto sub = take_call(client, "server.files.get_directory");
    CHECK(sub.params["path"] == "gcodes/sub");
    sub.success_cb(listing(json::array({{{"filename", "part.gcode"}, {"size", 20}}}),
                           json::array({{{"dirname", ".thumbs"}}})));
    pump();

    auto thumbs = take_call(client, "server.files.get_directory");
    CHECK(thumbs.params["path"] == "gcodes/sub/.thumbs");
    thumbs.success_cb(listing(json::array({{{"filename", "part-32x32.png"}, {"size", 2}}})));
    pump();

    REQUIRE(got.has_value());
    CHECK(from == ReportSource::FileApi);
    CHECK(got->free == 4000u);
    CHECK(got->total == 9000u);
    CHECK(got->consumers[consumer_at("user_gcode")].bytes == 1020);
    CHECK(got->consumers[consumer_at("user_gcode")].files == 2);
    CHECK(got->consumers[consumer_at("helix_temp")].bytes == 300);
    CHECK_FALSE(got->consumers[consumer_at("helix_temp")].cleanable);
    CHECK_FALSE(got->consumers[consumer_at("logs")].cleanable);
    CHECK(got->consumers[consumer_at("thumbnails")].bytes == 2);
    CHECK(got->consumers[consumer_at("logs")].bytes == 55);
    CHECK_FALSE(got->consumers[consumer_at("timelapse")].available);
    CHECK_FALSE(got->consumers[consumer_at("3mf_outputs")].available);
    CHECK_FALSE(got->consumers[consumer_at("plr")].available);
    CHECK_FALSE(got->partial);
}

namespace {

/// A FileApi fetch whose timelapse root is at @p timelapse_path and holds
/// @p files; gcodes and logs are empty.
std::optional<StorageReport> fallback_fetch(test::FakeMoonrakerClient& client,
                                            StorageReportService& service, bool may_clean,
                                            const char* timelapse_path, json files) {
    std::optional<StorageReport> got;
    service.fetch(ReportSource::FileApi, may_clean,
                  [&](ReportSource, std::optional<StorageReport> r) { got = r; });
    if (may_clean) {
        answer_roots(client, timelapse_path);
    }
    take_call(client, "server.files.get_directory").success_cb(listing(json::array()));
    helix::ui::UpdateQueue::instance().drain();
    take_call(client, "server.files.get_directory").success_cb(listing(std::move(files)));
    helix::ui::UpdateQueue::instance().drain();
    take_call(client, "server.files.get_directory").success_cb(listing(json::array()));
    helix::ui::UpdateQueue::instance().drain();
    return got;
}

const json kOldVideos = json::array({{{"filename", "benchy.mp4"}, {"size", 5}}});

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "The fallback cleanup deletes only old timelapse renders",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };
    const auto got = fallback_fetch(client, service, true, "/home/pi/timelapse", kOldVideos);
    REQUIRE(got.has_value());
    CHECK(got->consumers[consumer_at("timelapse")].cleanable);

    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::FileApi, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    auto del = take_call(client, "server.files.delete_file");
    CHECK(del.params["path"] == "timelapse/benchy.mp4");
    CHECK(del.timeout_ms == kCleanupRpcTimeoutMs);
    del.success_cb(json{{"result", json::object()}});
    pump();
    CHECK(client.rpc_calls.empty());
    REQUIRE(outcome.has_value());
    CHECK(outcome->freed == 5u);
    CHECK(outcome->errors == 0u);
}

TEST_CASE_METHOD(LVGLTestFixture, "A fallback delete that fails is counted, not reported freed",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };
    fallback_fetch(client, service, true, "/home/pi/timelapse", kOldVideos);

    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::FileApi, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    take_call(client, "server.files.delete_file")
        .error_cb(MoonrakerError::unknown("Method Not Allowed", "server.files.delete_file"));
    pump();
    REQUIRE(outcome.has_value());
    CHECK(outcome->freed == 0u);
    CHECK(outcome->errors == 1u);
    CHECK(cleanup_message(*outcome) == CleanupMessage::NothingFreed);
}

TEST_CASE_METHOD(LVGLTestFixture, "Without the plugin print copies are never deleted",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };
    service.fetch(ReportSource::FileApi, true, [](ReportSource, std::optional<StorageReport>) {});
    answer_roots(client, "/home/pi/timelapse");
    take_call(client, "server.files.get_directory")
        .success_cb(listing(json::array(), json::array({{{"dirname", ".helix_temp"}}})));
    pump();
    take_call(client, "server.files.get_directory").success_cb(listing(json::array()));
    pump();
    take_call(client, "server.files.get_directory").success_cb(listing(json::array()));
    pump();
    take_call(client, "server.files.get_directory")
        .success_cb(listing(json::array({{{"filename", "mod.gcode"}, {"size", 9}}})));
    pump();

    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::FileApi, consumer_at("helix_temp"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    pump();
    CHECK(client.rpc_calls.empty());
    REQUIRE(outcome.has_value());
    CHECK(outcome->refused);
}

TEST_CASE_METHOD(LVGLTestFixture, "The plugin path asks the plugin and cleans through it",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };

    service.cleanup(ReportSource::Plugin, consumer_at("user_gcode"),
                    [](const CleanupOutcome& o) { CHECK(o.refused); });
    CHECK(client.rpc_calls.empty());

    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::Plugin, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    auto call = take_call(client, "server.helix.storage_cleanup");
    CHECK(call.params == json{{"consumer", "timelapse"}, {"older_than", 0}});
    // A reply that never comes ends in the tracker's timeout, which clears the page.
    CHECK(call.timeout_ms == kCleanupRpcTimeoutMs);
    call.success_cb(json{{"result", {{"freed", 77}, {"errors", 3}}}});
    pump();
    REQUIRE(outcome.has_value());
    CHECK(outcome->freed == 77u);
    CHECK(outcome->errors == 3u);

    service.cleanup(ReportSource::Plugin, consumer_at("logs"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    take_call(client, "server.helix.storage_cleanup")
        .error_cb(MoonrakerError::unknown("unsafe logs root", "server.helix.storage_cleanup"));
    pump();
    CHECK(outcome->refused);
}

TEST_CASE_METHOD(LVGLTestFixture, "A page refresh does not cancel a cleanup in flight",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::Plugin, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    auto call = take_call(client, "server.helix.storage_cleanup");
    service.fetch(ReportSource::Plugin, true, [](ReportSource, std::optional<StorageReport>) {});
    call.success_cb(json{{"result", {{"freed", 5}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(outcome.has_value());
    CHECK(outcome->freed == 5u);
}

TEST_CASE_METHOD(LVGLTestFixture, "An upload check reads the gcodes volume", "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    AsyncLifetimeGuard guard;
    std::optional<UploadSpace> verdict;
    check_upload_space(&client, ".helix_temp", 50, guard.token(),
                       [&](UploadSpace s) { verdict = s; });
    // The target directory, not the whole gcodes root: disk use is per volume.
    auto call = take_call(client, "server.files.get_directory");
    CHECK(call.params["path"] == "gcodes/.helix_temp");
    json reply = listing(json::array());
    reply["result"]["disk_usage"] = {{"total", 10 * kFreeSpaceFloorBytes},
                                     {"free", kFreeSpaceFloorBytes + 49}};
    call.success_cb(reply);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(verdict == UploadSpace::TooFull);
}

TEST_CASE_METHOD(LVGLTestFixture, "An upload check falls back to the root for a new directory",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    AsyncLifetimeGuard guard;
    std::optional<UploadSpace> verdict;
    check_upload_space(&client, "usb_prints", 50, guard.token(),
                       [&](UploadSpace s) { verdict = s; });
    take_call(client, "server.files.get_directory")
        .error_cb(MoonrakerError::unknown("Not Found", "server.files.get_directory"));
    helix::ui::UpdateQueue::instance().drain();
    auto root = take_call(client, "server.files.get_directory");
    CHECK(root.params["path"] == "gcodes");
    json reply = listing(json::array());
    reply["result"]["disk_usage"] = {{"total", 10 * kFreeSpaceFloorBytes},
                                     {"free", 5 * kFreeSpaceFloorBytes}};
    root.success_cb(reply);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(verdict == UploadSpace::Ok);
}

TEST_CASE_METHOD(LVGLTestFixture, "The fallback never cleans a timelapse root over gcodes",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto got = fallback_fetch(client, service, true, "/pd/gcodes", kOldVideos);
    REQUIRE(got.has_value());
    CHECK(got->consumers[consumer_at("timelapse")].available);
    CHECK_FALSE(got->consumers[consumer_at("timelapse")].cleanable);
    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::FileApi, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    CHECK(client.rpc_calls.empty());
    REQUIRE(outcome.has_value());
    CHECK(outcome->refused);
}

TEST_CASE_METHOD(LVGLTestFixture, "The fallback cleans nothing when the plugin refused",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    // may_clean false: no roots read, the report only.
    const auto got = fallback_fetch(client, service, false, "/home/pi/timelapse", kOldVideos);
    REQUIRE(got.has_value());
    for (std::size_t i = 0; i < kConsumerCount; ++i) {
        CAPTURE(kConsumers[i].key);
        CHECK_FALSE(got->consumers[i].cleanable);
    }
    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::FileApi, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    CHECK(client.rpc_calls.empty());
    REQUIRE(outcome.has_value());
    CHECK(outcome->refused);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "The fallback leaves a timelapse root that is gcodes under another spelling",
                 "[plugin_storage]") {
    // The roots reply names two different strings for one directory: only its
    // contents give it away.
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const json gcodes_files = json::array(
        {{{"filename", "benchy.gcode"}, {"size", 9}}, {{"filename", "benchy.png"}, {"size", 1}}});
    const auto got = fallback_fetch(client, service, true, "/home/pi/pd-link/gcodes", gcodes_files);
    REQUIRE(got.has_value());
    CHECK_FALSE(got->consumers[consumer_at("timelapse")].cleanable);
    std::optional<CleanupOutcome> outcome;
    service.cleanup(ReportSource::FileApi, consumer_at("timelapse"),
                    [&](const CleanupOutcome& o) { outcome = o; });
    CHECK(client.rpc_calls.empty());
    REQUIRE(outcome.has_value());
    CHECK(outcome->refused);
}
