// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_service.cpp
 * @brief The screen server's settings and the service that starts and stops it.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/display_manager_test_access.h"
#include "../test_helpers/raw_http_client.h"
#include "config.h"
#include "display_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "remote_pointer.h"
#include "remote_screen_control.h"
#include "remote_screen_server_sink.h"
#include "remote_screen_service.h"
#include "remote_screen_settings_manager.h"
#include "remote_screen_webcam.h"
#include "system/debug_bundle_collector.h"
#include "webcam_selection.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

/// Fresh settings with the server on an ephemeral port, restored on exit.
struct RemoteScreenFixture : LVGLTestFixture {
    RemoteScreenFixture() {
        ::unsetenv("HELIX_REMOTE_SCREEN");
        ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
        auto& data = ConfigTestAccess::data(*Config::get_instance());
        data.erase("remote_screen");
        Config::get_instance()->set<int>("/remote_screen/port", 0);
        auto& m = RemoteScreenSettingsManager::instance();
        m.deinit_subjects();
        m.init_subjects();
    }
    ~RemoteScreenFixture() override {
        RemoteScreenService::instance().shutdown();
        ::unsetenv("HELIX_REMOTE_SCREEN");
        ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
        ConfigTestAccess::data(*Config::get_instance()).erase("remote_screen");
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "settings: defaults are off, port 7130",
                 "[remote_screen][settings]") {
    ConfigTestAccess::data(*Config::get_instance()).erase("remote_screen");
    auto& m = RemoteScreenSettingsManager::instance();
    m.deinit_subjects();
    m.init_subjects();
    CHECK_FALSE(m.get_enabled());
    CHECK_FALSE(m.get_control());
    CHECK(m.get_port() == 7130);
}

TEST_CASE_METHOD(RemoteScreenFixture, "settings: control cannot be on while view is off",
                 "[remote_screen][settings]") {
    auto& m = RemoteScreenSettingsManager::instance();
    m.set_control(true);
    CHECK_FALSE(m.get_control());

    m.set_enabled(true);
    m.set_control(true);
    CHECK(m.get_control());

    m.set_enabled(false);
    CHECK_FALSE(m.get_control());
    CHECK(lv_subject_get_int(m.subject_control()) == 0);
}

TEST_CASE_METHOD(RemoteScreenFixture,
                 "settings: a hand-edited config with control but no view loads off",
                 "[remote_screen][settings]") {
    Config::get_instance()->set<bool>("/remote_screen/enabled", false);
    Config::get_instance()->set<bool>("/remote_screen/control", true);
    auto& m = RemoteScreenSettingsManager::instance();
    m.deinit_subjects();
    m.init_subjects();
    CHECK_FALSE(m.get_control());
}

TEST_CASE("route: missing, blank and present route files", "[remote_screen][settings]") {
    const std::string path = "/tmp/helix_remote_screen_route_test";
    std::remove(path.c_str());
    CHECK(remote_screen_route(path).empty());
    {
        std::ofstream(path) << "  \n";
    }
    CHECK(remote_screen_route(path).empty());
    {
        std::ofstream(path) << "/helixscreen/screen/\n";
    }
    CHECK(remote_screen_route(path) == "/helixscreen/screen/");
    std::remove(path.c_str());
}

TEST_CASE("firmware_provides: both env spellings", "[remote_screen][settings]") {
    ::unsetenv("HELIX_REMOTE_SCREEN");
    ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
    CHECK_FALSE(remote_screen_firmware_provides());
    ::setenv("HELIX_REMOTE_SCREEN", "firmware", 1);
    CHECK(remote_screen_firmware_provides());
    ::setenv("HELIX_REMOTE_SCREEN", "builtin", 1);
    CHECK_FALSE(remote_screen_firmware_provides());
    ::unsetenv("HELIX_REMOTE_SCREEN");
    ::setenv("HELIX_REMOTE_SCREEN_FB0", "/dev/fb0", 1);
    CHECK(remote_screen_firmware_provides());
    ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
}

TEST_CASE_METHOD(RemoteScreenFixture, "service: enabling view starts the server live",
                 "[remote_screen][settings]") {
    auto& svc = RemoteScreenService::instance();
    svc.init(nullptr);
    CHECK_FALSE(svc.running());

    // The settings change alone starts it: the toggle's callback knows no server.
    RemoteScreenSettingsManager::instance().set_enabled(true);
    REQUIRE(svc.running());
    REQUIRE(svc.port() > 0);
    auto r = helix::test::raw_http(svc.port(), "GET", "/info");
    CHECK(r.status == 200);
}

TEST_CASE_METHOD(RemoteScreenFixture,
                 "service: disabling view frees the store and stops the server",
                 "[remote_screen][settings]") {
    auto& svc = RemoteScreenService::instance();
    RemoteScreenSettingsManager::instance().set_enabled(true);
    svc.init(nullptr);
    REQUIRE(svc.running());

    // A viewer has been served a frame.
    std::vector<uint8_t> px(16 * 16 * 4, 0x33);
    RemoteScreenFrame f;
    f.px_map = px.data();
    f.px_map_len = px.size();
    f.x2 = 15;
    f.y2 = 15;
    f.disp_w = 16;
    f.disp_h = 16;
    f.src_stride = 64;
    f.src_format = RemoteScreenPixelFormat::BGRA8888;
    svc.store()->note_request();
    svc.store()->on_frame(f);
    REQUIRE(svc.store()->snapshot(false, {5, 2}).has_value());
    REQUIRE(svc.store()->allocated_bytes() > 0);

    RemoteScreenSettingsManager::instance().set_enabled(false);
    CHECK_FALSE(svc.running());
    CHECK(svc.port() == 0);
    CHECK(svc.store()->allocated_bytes() == 0);
    CHECK_FALSE(svc.store()->wants_frames());
}

TEST_CASE_METHOD(RemoteScreenFixture, "service: firmware-provided screen never starts",
                 "[remote_screen][settings]") {
    ::setenv("HELIX_REMOTE_SCREEN", "firmware", 1);
    RemoteScreenSettingsManager::instance().set_enabled(true);
    auto& svc = RemoteScreenService::instance();
    CHECK_FALSE(svc.available());
    svc.init(nullptr);
    svc.apply_settings();
    CHECK_FALSE(svc.running());
}

// ---------------------------------------------------------------------------
// Remote touches reach LVGL's input pipeline
// ---------------------------------------------------------------------------

namespace {

/// The service with sharing and control on, facing the LAN, a browser paired, and
/// the store holding a frame the size of the test display.
struct TouchFixture : RemoteScreenFixture {
    std::string token;
    std::vector<uint8_t> px;

    TouchFixture() {
        auto& svc = RemoteScreenService::instance();
        svc.init(nullptr);
        auto& m = RemoteScreenSettingsManager::instance();
        m.set_enabled(true);
        m.set_control(true);
        REQUIRE(svc.running());
        helix::ui::UpdateQueue::instance().drain(); // the generated code reaches config

        px.assign(static_cast<size_t>(kTestDisplayWidth) * kTestDisplayHeight * 4, 0);
        RemoteScreenFrame f;
        f.px_map = px.data();
        f.px_map_len = px.size();
        f.x2 = kTestDisplayWidth - 1;
        f.y2 = kTestDisplayHeight - 1;
        f.disp_w = kTestDisplayWidth;
        f.disp_h = kTestDisplayHeight;
        f.src_stride = kTestDisplayWidth * 4;
        f.src_format = RemoteScreenPixelFormat::BGRA8888;
        svc.store()->note_request();
        svc.store()->on_frame(f);

        const std::string code = m.get_pair_code();
        REQUIRE(code.size() == 6);
        auto r = helix::test::raw_http(svc.port(), "POST", "/pair", json_headers(),
                                       "{\"code\":\"" + code + "\"}");
        REQUIRE(r.status == 200);
        token = nlohmann::json::parse(r.body, nullptr, false).value("token", "");
        REQUIRE(token.size() == 32);
    }

    static std::vector<std::pair<std::string, std::string>> json_headers() {
        return {{"Content-Type", "application/json"}, {"X-Helix-Touch", "1"}};
    }

    /// POST one touch from a client thread while this thread plays LVGL's main loop:
    /// the queued update runs, then the indevs are read until the request returns.
    int touch(const char* type, int x, int y) {
        auto headers = json_headers();
        headers.push_back({"X-Helix-Pair", token});
        const std::string body = std::string("{\"type\":\"") + type +
                                 "\",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                                 "}";
        std::atomic<int> status{0};
        const int port = RemoteScreenService::instance().port();
        std::thread client(
            [&] { status = helix::test::raw_http(port, "POST", "/touch", headers, body).status; });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (status.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            helix::ui::UpdateQueue::instance().drain();
            // An idle tick can run after the HTTP update but before the next
            // indev sample. A transport must not wake the display ahead of its gate.
            if (auto* dm = DisplayManager::instance()) {
                dm->check_display_sleep();
            }
            for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) {
                lv_indev_read(i);
            }
            if (auto* dm = DisplayManager::instance()) {
                dm->check_display_sleep();
            }
            lv_tick_inc(5);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return status.load();
    }
};

std::atomic<int> g_clicks{0};

struct WakeTouchFixture : TouchFixture {
    DisplayManager* previous = DisplayManager::instance();
    DisplayManager display;

    WakeTouchFixture() {
        DisplayManagerTestAccess::set_active_instance(&display);
        display.set_dim_timeout(600);
    }
    ~WakeTouchFixture() override {
        DisplayManagerTestAccess::set_display_sleeping(display, false);
        DisplayManagerTestAccess::set_display_dimmed(display, false);
        DisplayManagerTestAccess::set_active_instance(previous);
    }

    void go_idle(bool dimmed) {
        lv_display_trigger_activity(nullptr);
        lv_tick_inc(5000);
        DisplayManagerTestAccess::set_display_sleeping(display, !dimmed);
        DisplayManagerTestAccess::set_display_dimmed(display, dimmed);
        display.check_display_sleep();
        REQUIRE((display.is_display_sleeping() || display.is_display_dimmed()));
    }

    void settle_release() {
        // Let the same debounce window as the physical touchscreen elapse.
        lv_tick_inc(WakeTouchLatch::kRearmMs + 1);
        for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) {
            lv_indev_read(i);
        }
    }
};

} // namespace

TEST_CASE_METHOD(WakeTouchFixture, "remote wake gesture never reaches the button underneath",
                 "[remote_screen][control][wake]") {
    const bool dimmed = GENERATE(false, true);
    const bool held = GENERATE(false, true);
    CAPTURE(dimmed, held);
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(btn, 100, 100);
    lv_obj_set_size(btn, 80, 60);
    lv_obj_update_layout(btn);
    int pressed = 0, clicked = 0, long_pressed = 0;
    lv_obj_add_event_cb(
        btn, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_PRESSED, &pressed);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_CLICKED, &clicked);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_LONG_PRESSED, &long_pressed);
    go_idle(dimmed);

    REQUIRE(touch("down", 140, 130) == 204);
    CHECK_FALSE(display.is_display_sleeping());
    CHECK_FALSE(display.is_display_dimmed());
    CHECK(pressed == 0);
    if (held) {
        // Holding past the debounce window cannot turn the same wake into a press.
        lv_tick_inc(1000);
        REQUIRE(touch("move", 145, 135) == 204);
    }
    REQUIRE(touch("up", 145, 135) == 204);
    CHECK(pressed == 0);
    CHECK(clicked == 0);
    CHECK(long_pressed == 0);

    settle_release();
    REQUIRE(touch("down", 140, 130) == 204);
    REQUIRE(touch("up", 140, 130) == 204);
    CHECK(pressed == 1);
    CHECK(clicked == 1);
    lv_obj_delete(btn);
}

TEST_CASE_METHOD(TouchFixture, "touch through LVGL: down/up at a button center fires CLICKED",
                 "[remote_screen][control]") {
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(btn, 100, 100);
    lv_obj_set_size(btn, 80, 60);
    lv_obj_update_layout(btn);
    g_clicks = 0;
    lv_obj_add_event_cb(btn, [](lv_event_t*) { g_clicks.fetch_add(1); }, LV_EVENT_CLICKED, nullptr);

    CHECK(touch("down", 140, 130) == 204);
    CHECK(touch("up", 140, 130) == 204);
    CHECK(g_clicks.load() == 1);
    lv_obj_delete(btn);
}

TEST_CASE_METHOD(TouchFixture,
                 "touch through LVGL: a drag across a list scrolls it and clicks nothing",
                 "[remote_screen][control]") {
    lv_obj_t* list = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, 300, 300);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    g_clicks = 0;
    for (int i = 0; i < 20; ++i) {
        lv_obj_t* item = lv_button_create(list);
        lv_obj_set_size(item, 260, 60);
        lv_obj_add_event_cb(
            item, [](lv_event_t*) { g_clicks.fetch_add(1); }, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_update_layout(list);
    REQUIRE(lv_obj_get_scroll_y(list) == 0);

    CHECK(touch("down", 150, 250) == 204);
    for (int y = 240; y >= 60; y -= 20) {
        CHECK(touch("move", 150, y) == 204);
    }
    CHECK(touch("up", 150, 60) == 204);

    CHECK(lv_obj_get_scroll_y(list) > 0);
    CHECK(g_clicks.load() == 0);
    lv_obj_delete(list);
}

TEST_CASE_METHOD(TouchFixture, "the remote-control indicator follows accepted touches",
                 "[remote_screen][control]") {
    auto& svc = RemoteScreenService::instance();
    lv_subject_t* active = lv_xml_get_subject(nullptr, "remote_screen_control_active");
    REQUIRE(active != nullptr);
    const auto now = std::chrono::steady_clock::now();
    svc.housekeeping_tick(now);
    CHECK(lv_subject_get_int(active) == 0);

    REQUIRE(touch("down", 10, 10) == 204);
    REQUIRE(touch("up", 10, 10) == 204);
    svc.housekeeping_tick(std::chrono::steady_clock::now());
    CHECK(lv_subject_get_int(active) == 1);

    svc.housekeeping_tick(std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kRemoteControlActiveMs + 100));
    CHECK(lv_subject_get_int(active) == 0);
}

TEST_CASE_METHOD(RemoteScreenFixture, "pairing secrets never reach a debug bundle",
                 "[remote_screen][settings]") {
    // A shared bundle that carried a browser token would let anyone on that LAN drive
    // the screen, so whatever keys the manager writes must be ones the sanitizer redacts.
    auto& m = RemoteScreenSettingsManager::instance();
    m.set_pair_code("123456");
    m.set_paired_tokens({"0123456789abcdef0123456789abcdef"});
    const auto& data = ConfigTestAccess::data(*Config::get_instance());
    REQUIRE(data.contains("remote_screen"));
    const std::string bundled =
        helix::DebugBundleCollector::sanitize_json(data["remote_screen"]).dump();
    CHECK(bundled.find("123456") == std::string::npos);
    CHECK(bundled.find("0123456789abcdef0123456789abcdef") == std::string::npos);
    CHECK(m.get_pair_code() == "123456");
    CHECK(m.get_paired_tokens() == std::vector<std::string>{"0123456789abcdef0123456789abcdef"});
}

TEST_CASE_METHOD(TouchFixture,
                 "turning sharing off mid-touch neither strands a press nor stalls the UI",
                 "[remote_screen][control]") {
    auto& svc = RemoteScreenService::instance();
    auto headers = json_headers();
    headers.push_back({"X-Helix-Pair", token});
    const int port = svc.port();
    // A press arrives and is queued for the UI thread, which is busy (nothing pumps it).
    std::thread client([&] {
        (void)helix::test::raw_http(port, "POST", "/touch", headers,
                                    R"({"type":"down","x":10,"y":10})");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The person at the printer turns sharing off in that moment.
    const auto start = std::chrono::steady_clock::now();
    RemoteScreenSettingsManager::instance().set_enabled(false);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300));
    client.join();

    // The queued press now runs, after the server is gone: it must not press anything.
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(helix::remote::RemotePointer::instance().pressed());
}

TEST_CASE_METHOD(TouchFixture, "a press nobody finished is released by the housekeeping tick",
                 "[remote_screen][control]") {
    auto& svc = RemoteScreenService::instance();
    REQUIRE(touch("down", 10, 10) == 204);
    REQUIRE(helix::remote::RemotePointer::instance().pressed());
    svc.housekeeping_tick(std::chrono::steady_clock::now());
    CHECK(helix::remote::RemotePointer::instance().pressed());
    svc.housekeeping_tick(std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kTouchStaleMs + 100));
    CHECK_FALSE(helix::remote::RemotePointer::instance().pressed());
}

TEST_CASE_METHOD(RemoteScreenFixture,
                 "webcam sync waits for the locality verdict, then names the entry by it",
                 "[remote_screen][webcam]") {
    auto& svc = RemoteScreenService::instance();
    svc.init(nullptr);
    RemoteScreenSettingsManager::instance().set_enabled(true);
    REQUIRE(svc.running());

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);
    auto screen_entries = [&] {
        std::vector<std::string> names;
        api.timelapse().get_webcam_list(
            [&](const std::vector<WebcamInfo>& cams) {
                for (const auto& c : cams) {
                    if (webcam::is_own_screen_webcam(c)) {
                        names.push_back(c.name);
                    }
                }
            },
            [](const MoonrakerError&) {});
        return names;
    };

    // Before the connection says where Moonraker is, a guess would post the wrong entry.
    sync_remote_screen_webcam(api, MoonrakerLocality::Unknown);
    CHECK(screen_entries().empty());

    sync_remote_screen_webcam(api, MoonrakerLocality::Remote);
    auto remote = screen_entries();
    REQUIRE(remote.size() == 1);
    CHECK(remote[0].rfind(std::string(kRemoteScreenWebcamName) + " (", 0) == 0);
}
