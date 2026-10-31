// SPDX-License-Identifier: GPL-3.0-or-later

// Camera photos in print history (print_photos.h): who takes them, the
// record and auxiliary_data the plugin and the screen share, pruning, the
// camera both pick, and how the history detail card chooses what to show.

#include "ui_history_photos.h"
#include "ui_nav_manager.h"
#include "ui_panel_history_list.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "app_globals.h"
#include "helix_plugin_features.h"
#include "history_job_events.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "plugin_status_refresh.h"
#include "print_history_parse.h"
#include "print_photos.h"
#include "print_photos_controller.h"
#include "printer_plugin_status_state.h"
#include "printer_state.h"
#include "webcam_selection.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

constexpr double kT0 = 1700000000.25;

const std::string kJpeg = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(32, '\0');

PluginFeatureEntry entry(int version, bool available, bool enabled) {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    return e;
}

PluginFeatureState photos_state(bool plugin, int version, bool available, bool enabled) {
    PluginFeatureMap features;
    if (version > 0) {
        features[kPhotosFeature] = entry(version, available, enabled);
    }
    return plugin_feature_state(plugin, features, kPhotosFeature, kPhotosMinVersion);
}

WebcamInfo cam(const std::string& name, const std::string& service, const std::string& snapshot,
               const std::string& stream = "/s") {
    WebcamInfo c;
    c.name = name;
    c.service = service;
    c.snapshot_url = snapshot;
    c.stream_url = stream;
    return c;
}

json record(double start, json photos) {
    return json{{"version", 1},
                {"job_id", "00002A"},
                {"filename", "a.gcode"},
                {"start_time", start},
                {"photos", std::move(photos)}};
}

} // namespace

TEST_CASE("one taker of a job's photos per plugin state", "[plugin_photos]") {
    using R = FallbackSenderRole;
    const PluginFeatureState ready = photos_state(true, 1, true, true);
    const PluginFeatureState disabled = photos_state(true, 1, true, false);
    REQUIRE(ready == PluginFeatureState::Ready);
    REQUIRE(disabled == PluginFeatureState::Disabled);

    INFO("Ready: the plugin takes them, whatever this screen could do");
    CHECK(photo_taker(ready, true, true, R::Sender) == PhotoTaker::Plugin);
    CHECK(photo_taker(ready, false, false, R::Standby) == PhotoTaker::Plugin);

    INFO("Disabled: switched off, nobody takes them");
    CHECK(photo_taker(disabled, true, true, R::Sender) == PhotoTaker::None);

    for (PluginFeatureState absent :
         {photos_state(false, 1, true, true), photos_state(true, 0, false, false),
          photos_state(true, 1, false, false)}) {
        INFO("Absent: no plugin, an old plugin without the feature, or it cannot run here");
        REQUIRE(absent == PluginFeatureState::Absent);
        CHECK(photo_taker(absent, true, true, R::Sender) == PhotoTaker::App);
        CHECK(photo_taker(absent, false, true, R::Sender) == PhotoTaker::None);
        CHECK(photo_taker(absent, true, false, R::Sender) == PhotoTaker::None);
        CHECK(photo_taker(absent, true, true, R::Standby) == PhotoTaker::None);
    }
}

TEST_CASE("photos are on unless opted out: they only observe", "[plugin_photos]") {
    CHECK(plugin_feature_opted_in(json::object(), kPhotosFeature, false));
    CHECK_FALSE(plugin_feature_opted_in(json{{kPhotosFeature, false}}, kPhotosFeature, false));
    const auto it = std::find_if(
        kPluginSwitchFeatures.begin(), kPluginSwitchFeatures.end(),
        [](const PluginSwitchFeature& f) { return std::string(f.name) == kPhotosFeature; });
    REQUIRE(it != kPluginSwitchFeatures.end());
    const auto& row = *it;
    CHECK(row.has_fallback);
    CHECK_FALSE(row.acts);
}

TEST_CASE("the photos switch keeps working without the plugin", "[plugin_photos]") {
    CHECK(plugin_switch_mode(false, {}, kPhotosFeature, kPhotosMinVersion, true, true) ==
          PluginSwitchMode::Switch);
    CHECK(plugin_switch_mode(false, {}, kPhotosFeature, kPhotosMinVersion, true, false) ==
          PluginSwitchMode::FallbackInstallHint);
    PluginFeatureMap features;
    features[kPhotosFeature] = entry(1, true, true);
    CHECK(plugin_switch_mode(true, features, kPhotosFeature, kPhotosMinVersion, true, false) ==
          PluginSwitchMode::Switch);
}

TEST_CASE("the overview leads the photos line to Devices", "[plugin_photos]") {
    PluginFeatureMap features;
    features[kPhotosFeature] = entry(1, true, true);
    const auto lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].label == "Print photos");
    CHECK(lines[0].home == PluginFeatureHome::Devices);
}

TEST_CASE("layer 2 is done only when the job is seen moving past it", "[plugin_photos]") {
    CHECK(layer_two_done(2, 3));
    CHECK(layer_two_done(1, 3));
    CHECK_FALSE(layer_two_done(1, 2));
    CHECK_FALSE(layer_two_done(2, 2));
    CHECK_FALSE(layer_two_done(0, 3));
    CHECK_FALSE(layer_two_done(std::nullopt, 3));
    CHECK_FALSE(layer_two_done(3, 4));
    CHECK_FALSE(layer_two_done(40, 41));
}

TEST_CASE("an end photo for completed, cancelled and failed jobs only", "[plugin_photos]") {
    CHECK(ends_with_photo("completed"));
    CHECK(ends_with_photo("cancelled"));
    CHECK(ends_with_photo("error"));
    CHECK_FALSE(ends_with_photo("klippy_shutdown"));
    CHECK_FALSE(ends_with_photo("in_progress"));
    CHECK_FALSE(ends_with_photo(""));
}

TEST_CASE("a photo's kind is read from its file name", "[plugin_photos]") {
    CHECK(photo_kind_from_path(".helix_photos/2A_1_layer2.jpg") == kPhotoKindLayer);
    CHECK(photo_kind_from_path(".helix_photos/2A_1_end.jpg") == kPhotoKindEnd);
    CHECK(photo_kind_from_path(".helix_photos/_end.png").empty());
    CHECK(photo_kind_from_path("_end.jpg").empty());
}

TEST_CASE("a newer version's record is left alone with its files", "[plugin_photos]") {
    json newer = record(5, json::array({{{"kind", "end"}, {"path", ".helix_photos/v2.jpg"}}}));
    newer["version"] = 2;
    CHECK(photo_record_is_newer(newer));
    CHECK_FALSE(photo_record_is_newer(record(5, json::array())));
    CHECK_FALSE(photo_record_is_newer(json{{"version", "2"}}));
    CHECK(photo_record_paths(newer).empty());
    const json records = {{"A", newer}, {"B", record(10, json::array())}};
    CHECK(photo_records_to_prune(records, 0) == std::vector<std::string>{"B"});
}

TEST_CASE("a still of a camera turns the way the camera widget turns it",
          "[plugin_photos][webcam]") {
    const json widget = {{"rotation", 180}, {"flip_h", true}};
    auto shown = cam("main", "mjpegstreamer", "/s.jpg");
    CHECK(webcam::view_config_for("main", shown, widget) == widget);
    CHECK(webcam::view_config_for("other", shown, widget) == json::object());
    CHECK(webcam::view_config_for("main", std::nullopt, widget) == json::object());
}

TEST_CASE("history's notifications read as job events", "[plugin_photos]") {
    auto msg = [](json payload) {
        return json{{"method", "notify_history_changed"}, {"params", json::array({payload})}};
    };
    auto added = parse_history_job_event(
        msg({{"action", "added"},
             {"job", {{"job_id", "00002A"}, {"filename", "a.gcode"}, {"start_time", kT0}}}}));
    REQUIRE(added);
    CHECK(added->action == "added");
    CHECK(added->job_id == "00002A");
    CHECK(added->filename == "a.gcode");
    CHECK(added->start_time == kT0);
    auto finished = parse_history_job_event(
        msg({{"action", "finished"}, {"job", {{"job_id", "00002A"}, {"status", "cancelled"}}}}));
    REQUIRE(finished);
    CHECK(finished->status == "cancelled");
    CHECK_FALSE(parse_history_job_event(msg({{"action", "deleted"}, {"job", {{"job_id", "1"}}}})));
    CHECK_FALSE(parse_history_job_event(msg({{"action", "added"}, {"job", {{"filename", "a"}}}})));
    CHECK_FALSE(parse_history_job_event(msg({{"action", "added"}})));
}

TEST_CASE("photo paths carry the job and its start, and stay in the photos dir",
          "[plugin_photos]") {
    CHECK(photo_path("00002A", kT0, kPhotoKindEnd) == ".helix_photos/00002A_1700000000_end.jpg");
    CHECK(photo_file_name("00002A", kT0, kPhotoKindLayer) == "00002A_1700000000_layer2.jpg");
    CHECK(is_photo_path(".helix_photos/a.jpg"));
    CHECK_FALSE(is_photo_path(".helix_photos/"));
    CHECK_FALSE(is_photo_path(".helix_photos/../printer.cfg"));
    CHECK_FALSE(is_photo_path(".helix_photos/sub/a.jpg"));
    CHECK_FALSE(is_photo_path("benchy.gcode"));
    CHECK_FALSE(is_photo_path("x/.helix_photos/a.jpg"));
}

TEST_CASE("a photo record lists its photos, layer first, nothing outside", "[plugin_photos]") {
    const json rec = record(kT0, json::array({
                                     {{"kind", "end"}, {"path", ".helix_photos/e.jpg"}},
                                     {{"kind", "layer2"}, {"path", ".helix_photos/l.jpg"}},
                                     {{"kind", "end"}, {"path", "../etc/passwd"}},
                                     {{"kind", "weird"}, {"path", ".helix_photos/w.jpg"}},
                                     "junk",
                                 }));
    const auto photos = parse_photo_record(rec);
    REQUIRE(photos.size() == 2);
    CHECK(photos[0].kind == kPhotoKindLayer);
    CHECK(photos[1].path == ".helix_photos/e.jpg");

    json other = rec;
    other["version"] = 2;
    CHECK(parse_photo_record(other).empty());
    CHECK(parse_photo_record(json()).empty());
    CHECK(photo_record_paths(rec) == std::vector<std::string>{".helix_photos/e.jpg",
                                                              ".helix_photos/l.jpg",
                                                              ".helix_photos/w.jpg"});
}

TEST_CASE("the detail view shows the record and the history link together", "[plugin_photos]") {
    const std::vector<PrintPhoto> linked = {{kPhotoKindLayer, ".helix_photos/l_aux.jpg"}};
    const std::vector<PrintPhoto> recorded = {{kPhotoKindLayer, ".helix_photos/l_rec.jpg"},
                                              {kPhotoKindEnd, ".helix_photos/e_rec.jpg"}};
    const auto both = merge_photo_lists(linked, recorded);
    REQUIRE(both.size() == 2);
    CHECK(both[0].path == ".helix_photos/l_rec.jpg");
    CHECK(both[1].path == ".helix_photos/e_rec.jpg");

    const std::vector<PrintPhoto> end_only = {{kPhotoKindEnd, ".helix_photos/e.jpg"}};
    const auto from_aux = merge_photo_lists(end_only, {{kPhotoKindLayer, ".helix_photos/l.jpg"}});
    REQUIRE(from_aux.size() == 2);
    CHECK(from_aux[0].kind == kPhotoKindLayer);
    CHECK(from_aux[1].path == ".helix_photos/e.jpg");

    CHECK(same_photos(both, recorded));
    CHECK_FALSE(same_photos(linked, recorded));
    CHECK(merge_photo_lists({}, {}).empty());
}

TEST_CASE("a record answers only for its own job's start", "[plugin_photos]") {
    const json rec = record(kT0, json::array());
    CHECK(photo_record_matches(rec, kT0 + 0.5));
    CHECK_FALSE(photo_record_matches(rec, kT0 + 5000));
    CHECK_FALSE(photo_record_matches(json(), kT0));
}

TEST_CASE("the screen writes the plugin's record shape", "[plugin_photos]") {
    const json first =
        photo_record_with(json(), "00002A", "a.gcode", kT0, kPhotoKindEnd, "p/end.jpg", 10, 99);
    CHECK(first["version"] == 1);
    CHECK(first["job_id"] == "00002A");
    CHECK(first["start_time"] == kT0);
    REQUIRE(first["photos"].size() == 1);
    CHECK(first["photos"][0] == json{{"kind", "end"},
                                     {"path", "p/end.jpg"},
                                     {"size", 10},
                                     {"taken_at", 99},
                                     {"source", "app"}});

    const json both =
        photo_record_with(first, "00002A", "a.gcode", kT0, kPhotoKindLayer, "p/l.jpg", 5, 100);
    REQUIRE(both["photos"].size() == 2);
    CHECK(both["photos"][0]["kind"] == "layer2");
    CHECK(both["photos"][1]["kind"] == "end");

    const json again =
        photo_record_with(both, "00002A", "a.gcode", kT0, kPhotoKindEnd, "p/end2.jpg", 11, 101);
    REQUIRE(again["photos"].size() == 2);
    CHECK(again["photos"][1]["path"] == "p/end2.jpg");

    INFO("a record left by an earlier job under the same id is replaced");
    const json reused =
        photo_record_with(both, "00002A", "b.gcode", kT0 + 5000, kPhotoKindEnd, "p/new.jpg", 1, 1);
    CHECK(reused["filename"] == "b.gcode");
    REQUIRE(reused["photos"].size() == 1);
}

TEST_CASE("the oldest jobs' photos go beyond the bound", "[plugin_photos]") {
    const json records = {
        {"A", record(30, json::array())},
        {"B", record(10, json::array())},
        {"C", record(20, json::array())},
        {"D", json{{"junk", 1}}},
    };
    auto drop = photo_records_to_prune(records, 2);
    std::sort(drop.begin(), drop.end());
    CHECK(drop == std::vector<std::string>{"B", "D"});
    CHECK(photo_records_to_prune(records, 3) == std::vector<std::string>{"D"});
    CHECK(photo_records_to_prune(json::array(), 1).empty());
}

TEST_CASE("history's auxiliary_data links the photos", "[plugin_photos]") {
    const json aux = json::array({
        {{"provider", "spoolman"}, {"name", "spool_ids"}, {"value", json::array({1})}},
        {{"provider", "helix_print"}, {"name", "modifications"}, {"value", json::object()}},
        {{"provider", "helix_print"},
         {"name", "photos"},
         {"value", json::array({".helix_photos/2A_1_end.jpg", ".helix_photos/2A_1_layer2.jpg",
                                "../x_end.jpg", 7})}},
    });
    const auto photos = parse_photo_aux(aux);
    REQUIRE(photos.size() == 2);
    CHECK(photos[0].kind == kPhotoKindLayer);
    CHECK(photos[1].kind == kPhotoKindEnd);
    CHECK(parse_photo_aux(json::object()).empty());

    json job = {{"job_id", "00002A"}, {"filename", "a.gcode"}, {"auxiliary_data", aux}};
    CHECK(parse_history_job(job).photos.size() == 2);
    job["auxiliary_data"] = nullptr;
    CHECK(parse_history_job(job).photos.empty());
}

TEST_CASE("the plugin and the screen photograph the same camera", "[plugin_photos][webcam]") {
    CHECK_FALSE(webcam::photo_pick({}).has_value());

    auto webrtc = cam("webrtc", "webrtc-camerastreamer", "/a.jpg");
    auto main = cam("main", "mjpegstreamer", "/main/?action=snapshot");
    auto pick = webcam::photo_pick({webrtc, main});
    REQUIRE(pick);
    CHECK(pick->name == "main");

    pick = webcam::photo_pick({webrtc});
    REQUIRE(pick);
    CHECK(pick->name == "webrtc");

    auto off = cam("off", "mjpegstreamer", "/off.jpg");
    off.enabled = false;
    auto down = cam("down", "mjpegstreamer", "/down.jpg");
    down.unavailable_reason = "service not running";
    auto page = cam("page", "mjpegstreamer", "/viewer.html");
    auto screen = cam(kRemoteScreenWebcamName, "mjpegstreamer", "/screen.jpg");
    CHECK_FALSE(webcam::photo_pick({off, down, page, screen}).has_value());
}

TEST_CASE("the detail card opens on the end photo", "[plugin_photos]") {
    using ui::HistoryPhotoView;
    const std::vector<PrintPhoto> both = {{kPhotoKindLayer, "l"}, {kPhotoKindEnd, "e"}};
    const std::vector<PrintPhoto> layer = {{kPhotoKindLayer, "l"}};
    CHECK(ui::default_photo_view(both) == HistoryPhotoView::End);
    CHECK(ui::default_photo_view(layer) == HistoryPhotoView::Layer);
    CHECK(ui::default_photo_view({}) == HistoryPhotoView::Preview);
    CHECK(ui::photo_for_view(both, HistoryPhotoView::Layer)->path == "l");
    CHECK(ui::photo_for_view(layer, HistoryPhotoView::End) == nullptr);
    CHECK(ui::photo_for_view(both, HistoryPhotoView::Preview) == nullptr);
}

namespace {

/// The screen's fallback against the global PrinterState: a local screen
/// with no plugin and one camera, fetching through a recorder.
struct PhotosFx : LVGLTestFixture {
    std::vector<std::string> fetched;
    std::unique_ptr<PrintPhotosController> controller;

    PhotosFx() {
        auto& ps = get_printer_state();
        ps.capabilities_state().set_webcams({cam("main", "mjpegstreamer", "http://cam/snap.jpg")});
        ps.set_moonraker_is_remote(false);
        ps.plugin_status_state().set_stored_opt_ins(json::object());
        ui::UpdateQueue::instance().drain();
        controller = std::make_unique<PrintPhotosController>(
            [this](const std::string& url, std::function<void(std::string)> done) {
                fetched.push_back(url);
                done("");
            });
    }
    ~PhotosFx() override {
        controller.reset();
        auto& ps = get_printer_state();
        ps.capabilities_state().set_webcam_available(false);
        ps.clear_moonraker_locality();
        ps.plugin_status_state().set_stored_opt_ins(json::object());
        ui::UpdateQueue::instance().drain();
    }
};

const PrintPhotosController::Job kJob{"00002A", "a.gcode", kT0};

} // namespace

TEST_CASE_METHOD(PhotosFx, "the screen takes the layer 2 photo once, as the job passes it",
                 "[plugin_photos]") {
    controller->on_job_added(kJob);
    for (int layer : {1, 2, 3, 4, 3, 5}) {
        controller->on_slicer_layer(layer);
    }
    CHECK(fetched == std::vector<std::string>{"http://cam/snap.jpg"});
}

TEST_CASE_METHOD(PhotosFx, "a job first seen past layer 2 gets no layer photo", "[plugin_photos]") {
    controller->on_job_added(kJob);
    controller->on_slicer_layer(40);
    controller->on_slicer_layer(41);
    CHECK(fetched.empty());

    INFO("nor does a layer report with no job history added");
    PrintPhotosController other([this](const std::string& url, std::function<void(std::string)>) {
        fetched.push_back(url);
    });
    other.on_slicer_layer(2);
    other.on_slicer_layer(3);
    CHECK(fetched.empty());
}

TEST_CASE_METHOD(PhotosFx, "the end photo follows how the job ended", "[plugin_photos]") {
    controller->on_job_finished(kJob, "klippy_shutdown");
    CHECK(fetched.empty());
    controller->on_job_finished(kJob, "cancelled");
    CHECK(fetched.size() == 1);
}

TEST_CASE_METHOD(PhotosFx, "the screen takes nothing when opted out or without a camera",
                 "[plugin_photos]") {
    SECTION("opted out") {
        get_printer_state().plugin_status_state().set_stored_opt_ins(json{{kPhotosFeature, false}});
        controller->on_job_finished(kJob, "completed");
        CHECK(fetched.empty());
    }
    SECTION("no camera") {
        get_printer_state().capabilities_state().set_webcam_available(false);
        ui::UpdateQueue::instance().drain();
        controller->on_job_finished(kJob, "completed");
        CHECK(fetched.empty());
    }
    SECTION("a build that cannot fetch a snapshot") {
        PrintPhotosController blind(nullptr);
        blind.on_job_finished(kJob, "completed");
        CHECK(fetched.empty());
    }
}

namespace {

/// The fallback against a mock Moonraker published as the app's: uploads and
/// database writes land in the mock, fetches answer a JPEG at once. The UI
/// fixture initializes the global PrinterState's subjects, so a plugin probe
/// reaches the observers that subscribe to history.
struct PhotosApiFx : LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    std::unique_ptr<MoonrakerAPIMock> api;
    IMoonrakerAPI* previous_api = nullptr;
    std::unique_ptr<PrintPhotosController> controller;

    PhotosApiFx() {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
        previous_api = get_moonraker_api();
        set_moonraker_api(api.get());
        auto& ps = get_printer_state();
        ps.capabilities_state().set_webcams({cam("main", "mjpegstreamer", "http://cam/snap.jpg")});
        ps.set_moonraker_is_remote(false);
        ps.plugin_status_state().set_stored_opt_ins(json::object());
        drain();
        controller = std::make_unique<PrintPhotosController>(
            [](const std::string&, std::function<void(std::string)> done) { done(kJpeg); });
    }
    ~PhotosApiFx() override {
        controller.reset();
        drain();
        set_moonraker_api(previous_api);
        auto& ps = get_printer_state();
        ps.capabilities_state().set_webcam_available(false);
        ps.clear_moonraker_locality();
        drain();
        client.stop_temperature_simulation();
        client.disconnect();
    }

    static void drain() {
        for (int i = 0; i < 20; ++i) {
            ui::UpdateQueue::instance().drain();
        }
    }

    json stored(const std::string& key) {
        return client.mock_db_get(kPhotosNamespace, key);
    }
};

} // namespace

TEST_CASE_METHOD(PhotosApiFx, "the screen uploads the photo on the bulk lane and records it",
                 "[plugin_photos]") {
    controller->on_job_finished(kJob, "completed");
    drain();
    const auto& uploads = api->transfers_mock().named_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].root == "gcodes");
    CHECK(uploads[0].path == ".helix_photos/00002A_1700000000_end.jpg");
    CHECK(uploads[0].filename == "00002A_1700000000_end.jpg");
    CHECK(uploads[0].content == kJpeg);
    CHECK(uploads[0].lane == ITransfersAPI::UploadLane::Bulk);

    const json rec = stored("00002A");
    REQUIRE(parse_photo_record(rec).size() == 1);
    CHECK(rec["photos"][0]["source"] == "app");
    CHECK(rec["start_time"] == kT0);
}

TEST_CASE_METHOD(PhotosApiFx, "two photos landing together both reach the record",
                 "[plugin_photos]") {
    controller->on_job_added(kJob);
    controller->on_slicer_layer(2);
    controller->on_slicer_layer(3);
    controller->on_job_finished(kJob, "completed");
    drain();
    REQUIRE(api->transfers_mock().named_uploads().size() == 2);
    const auto photos = parse_photo_record(stored("00002A"));
    REQUIRE(photos.size() == 2);
    CHECK(photos[0].kind == kPhotoKindLayer);
    CHECK(photos[1].kind == kPhotoKindEnd);
}

TEST_CASE_METHOD(PhotosApiFx,
                 "the screen keeps photos of at most 500 jobs, deleting only its own files",
                 "[plugin_photos]") {
    for (size_t i = 0; i < kMaxPhotoJobs; ++i) {
        const std::string key = fmt::format("{:06X}", 0x100 + i);
        client.mock_db_set(kPhotosNamespace, key,
                           record(static_cast<double>(i + 1),
                                  json::array({{{"kind", "end"},
                                                {"path", ".helix_photos/" + key + "_end.jpg"}}})));
    }
    json newer = record(0.5, json::array({{{"kind", "end"}, {"path", ".helix_photos/v2.jpg"}}}));
    newer["version"] = 2;
    client.mock_db_set(kPhotosNamespace, "NEWER", newer);
    client.mock_db_set(kPhotosNamespace, "JUNK",
                       json{{"version", 1}, {"photos", json::array({{{"path", "../x.gcode"}}})}});

    controller->on_job_finished(kJob, "completed");
    drain();

    const auto& deleted = api->files_mock().deleted_files();
    CHECK(deleted == std::vector<std::string>{"gcodes/.helix_photos/000100_end.jpg"});
    CHECK(stored("000100").is_null());
    CHECK(stored("JUNK").is_null());
    CHECK_FALSE(stored("NEWER").is_null());
    CHECK_FALSE(stored("000101").is_null());
    CHECK_FALSE(stored("00002A").is_null());
}

TEST_CASE_METHOD(PhotosApiFx, "deleting a job here deletes its photos, not a later job's",
                 "[plugin_photos]") {
    client.mock_db_set(
        kPhotosNamespace, "00002A",
        record(kT0, json::array({{{"kind", "layer2"}, {"path", ".helix_photos/2A_layer2.jpg"}},
                                 {{"kind", "end"}, {"path", "../printer.cfg"}}})));
    PrintHistoryJob later;
    later.job_id = "00002A";
    later.start_time = kT0 + 5000;
    ui::HistoryPhotoViewer::delete_photos(later);
    drain();
    CHECK(api->files_mock().deleted_files().empty());
    CHECK_FALSE(stored("00002A").is_null());

    PrintHistoryJob job;
    job.job_id = "00002A";
    job.start_time = kT0;
    ui::HistoryPhotoViewer::delete_photos(job);
    drain();
    CHECK(api->files_mock().deleted_files() ==
          std::vector<std::string>{"gcodes/.helix_photos/2A_layer2.jpg"});
    CHECK(stored("00002A").is_null());
}

namespace {

/// The fallback with the plugin installed, its status read through the mock,
/// and the app's status refresh running.
struct PhotosStatusFx : PhotosApiFx {
    std::unique_ptr<PluginStatusRefresh> refresh;

    PhotosStatusFx() {
        setenv("HELIX_MOCK_HELIX_PLUGIN", "1", 1);
        unsetenv("HELIX_MOCK_PHOTOS");
        get_printer_state().set_helix_plugin_installed(true);
        drain();
        refresh = std::make_unique<PluginStatusRefresh>();
        refresh->refresh();
        drain();
    }
    ~PhotosStatusFx() override {
        refresh.reset();
        unsetenv("HELIX_MOCK_HELIX_PLUGIN");
        unsetenv("HELIX_MOCK_PHOTOS");
        get_printer_state().set_helix_plugin_features({});
        get_printer_state().set_helix_plugin_installed(false);
        drain();
    }

    PluginFeatureState photos_state() {
        return get_printer_state().plugin_status_state().plugin_feature(kPhotosFeature,
                                                                        kPhotosMinVersion);
    }

    void history(const std::string& action) {
        client.dispatch_method_callback(
            "notify_history_changed",
            json{{"method", "notify_history_changed"},
                 {"params", json::array({{{"action", action},
                                          {"job",
                                           {{"job_id", kJob.job_id},
                                            {"filename", kJob.filename},
                                            {"start_time", kJob.start_time},
                                            {"status", "completed"}}}}})}});
        drain();
    }
};

} // namespace

TEST_CASE_METHOD(PhotosStatusFx,
                 "a plugin that loses the camera after connect hands photos over at the next job",
                 "[plugin_photos]") {
    REQUIRE(photos_state() == PluginFeatureState::Ready);

    setenv("HELIX_MOCK_PHOTOS", "unavailable", 1);
    history("added");
    CHECK(photos_state() == PluginFeatureState::Absent);
    history("finished");
    CHECK(api->transfers_mock().named_uploads().size() == 1);
}

TEST_CASE_METHOD(PhotosStatusFx, "the plugin's change notice hands photos back to it",
                 "[plugin_photos]") {
    setenv("HELIX_MOCK_PHOTOS", "unavailable", 1);
    refresh->refresh();
    drain();
    REQUIRE(photos_state() == PluginFeatureState::Absent);

    unsetenv("HELIX_MOCK_PHOTOS");
    client.dispatch_method_callback("notify_helix_features_changed",
                                    json{{"method", "notify_helix_features_changed"},
                                         {"params", json::array({json::object()})}});
    drain();
    CHECK(photos_state() == PluginFeatureState::Ready);
    controller->on_job_finished(kJob, "completed");
    drain();
    CHECK(api->transfers_mock().named_uploads().empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a re-read that changes nothing announces nothing",
                 "[plugin_photos][plugin_feature]") {
    PrinterPluginStatusState status;
    status.init_subjects(false);
    auto seq = [&status] {
        return lv_subject_get_int(status.get_helix_plugin_probe_seq_subject());
    };
    PluginFeatureMap ready;
    ready[kPhotosFeature] = entry(1, true, true);
    PluginFeatureMap lost = ready;
    lost[kPhotosFeature].available = false;
    lost[kPhotosFeature].enabled = false;
    lost[kPhotosFeature].reason = "snapshot_unreachable: /webcam/?action=snapshot";

    status.set_features(ready);
    const int first = seq();
    status.set_features(ready, FeatureAnnounce::IfChanged);
    CHECK(seq() == first);
    status.set_features(lost, FeatureAnnounce::IfChanged);
    CHECK(seq() == first + 1);
    CHECK(status.features().at(kPhotosFeature).reason == lost[kPhotosFeature].reason);
    status.set_features(lost);
    CHECK(seq() == first + 2);
    status.deinit_subjects();
}

TEST_CASE_METHOD(PhotosStatusFx,
                 "a job start that changes nothing in the plugin's status touches nothing",
                 "[plugin_photos]") {
    auto seq = [] {
        return lv_subject_get_int(
            get_printer_state().plugin_status_state().get_helix_plugin_probe_seq_subject());
    };
    const int before = seq();
    history("added");
    CHECK(seq() == before);
    setenv("HELIX_MOCK_PHOTOS", "unavailable", 1);
    history("added");
    CHECK(seq() == before + 1);
}

TEST_CASE_METHOD(PhotosApiFx, "history's detail card offers the job's photos",
                 "[plugin_photos][history]") {
    PrintHistoryJob job;
    job.job_id = "00002A";
    job.filename = "a.gcode";
    job.start_time = kT0;
    job.status = PrintJobStatus::COMPLETED;
    job.photos = {{kPhotoKindEnd, ".helix_photos/2A_end.jpg"}};

    HistoryListPanel panel;
    panel.init_subjects();
    ui::HistoryListPanelTestAccess::show_detail_overlay(panel, test_screen(), job);
    drain();
    process_lvgl(10);
    drain();

    lv_obj_t* tabs = lv_obj_find_by_name(test_screen(), "photo_tabs");
    lv_obj_t* photo = lv_obj_find_by_name(test_screen(), "photo_layer");
    REQUIRE(tabs != nullptr);
    REQUIRE(photo != nullptr);
    // The card opens on the end photo, with the tabs to go back to the preview.
    CHECK_FALSE(lv_obj_has_flag(tabs, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(photo, LV_OBJ_FLAG_HIDDEN));

    NavigationManager::instance().go_back();
    drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(PhotosApiFx, "deleting a job from history deletes its photos",
                 "[plugin_photos][history]") {
    client.mock_db_set(
        kPhotosNamespace, "00002A",
        record(kT0, json::array({{{"kind", "end"}, {"path", ".helix_photos/2A_end.jpg"}}})));
    PrintHistoryJob job;
    job.job_id = "00002A";
    job.filename = "a.gcode";
    job.start_time = kT0;

    HistoryListPanel panel;
    panel.init_subjects();
    ui::HistoryListPanelTestAccess::select_job(panel, {job}, 0);
    ui::HistoryListPanelTestAccess::confirm_delete(panel);
    drain();

    CHECK(api->files_mock().deleted_files() ==
          std::vector<std::string>{"gcodes/.helix_photos/2A_end.jpg"});
    CHECK(stored("00002A").is_null());
}
