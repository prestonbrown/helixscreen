// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_telemetry_gl_context.cpp
 * @brief GL renderer, render mode, on-screen and memory context in telemetry
 *
 * What a main_loop_hang, a memory_warning and a session event say about the
 * screen, the GL renderer and memory, so a software-GL 3D preview stall, a
 * page-in stall and a CPU hang can be told apart.
 */

#include "../test_helpers/telemetry_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"
#include "system/telemetry_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Config;
using helix::DisplaySettingsManager;

// ============================================================================
// GL renderer and render mode [telemetry][gl_renderer]
// ============================================================================

namespace {

/// Leaves the process-wide renderer and top overlay as a fresh process has them.
struct ScopedTelemetryScreenContext {
    ScopedTelemetryScreenContext() {
        reset();
    }
    ~ScopedTelemetryScreenContext() {
        reset();
    }
    static void reset() {
        helix::telemetry_context::set_gl_renderer(nullptr);
        helix::telemetry_context::set_top_overlay("");
        helix::telemetry_context::active_panel_int.store(-1);
    }
};

/// Restores the gcode render mode setting and the persisted 3D block.
struct ScopedRenderModeSetting {
    int saved_mode = DisplaySettingsManager::instance().get_gcode_render_mode();
    bool saved_blocked = Config::get_instance()->get<bool>("/display/gpu_3d_blocked", false);
    ~ScopedRenderModeSetting() {
        DisplaySettingsManager::instance().set_gcode_render_mode(saved_mode);
        Config::get_instance()->set<bool>("/display/gpu_3d_blocked", saved_blocked);
    }
};

} // namespace

TEST_CASE("normalize_gl_renderer keeps the GPU name and drops driver detail",
          "[telemetry][gl_renderer]") {
    struct Row {
        const char* raw;
        const char* want;
    };
    const Row rows[] = {
        {"llvmpipe (LLVM 15.0.6, 128 bits)", "llvmpipe"},
        {"V3D 4.2", "V3D 4.2"},
        {"V3D 7.1.10.2", "V3D 7.1.10.2"},
        {"Mali-G52 (Panfrost)", "Mali-G52"},
        {"VC4 V3D 2.1", "VC4 V3D 2.1"},
        {"Mesa Intel(R) UHD Graphics 620 (KBL GT2)", "Mesa Intel UHD Graphics 620"},
        {"Adreno (TM) 618", "Adreno 618"},
        {"AMD Radeon RX 6600 (navi23, LLVM 15.0.6, DRM 3.49, 6.1.0-18-amd64)",
         "AMD Radeon RX 6600"},
        {"SwiftShader Device (Subzero) (0x0000C0DE)", "SwiftShader Device"},
        {"  softpipe  ", "softpipe"},
        {"GPU /dev/dri/renderD128 \"x\"", "GPU devdrirenderD128 x"},
        {"A very long renderer name that goes on and on past forty",
         "A very long renderer name that goes on a"},
        {"(no name)", ""},
        {"", ""},
        {nullptr, ""},
    };
    for (const Row& r : rows) {
        INFO("raw: " << (r.raw ? r.raw : "(null)"));
        CHECK(helix::normalize_gl_renderer(r.raw) == r.want);
    }
}

TEST_CASE("gcode_render_mode_name names each setting value", "[telemetry][gl_renderer]") {
    CHECK(std::string(helix::gcode_render_mode_name(0)) == "auto");
    CHECK(std::string(helix::gcode_render_mode_name(1)) == "3d");
    CHECK(std::string(helix::gcode_render_mode_name(2)) == "2d");
    CHECK(std::string(helix::gcode_render_mode_name(3)) == "thumbnail");
    CHECK(std::string(helix::gcode_render_mode_name(4)) == "unknown");
    CHECK(std::string(helix::gcode_render_mode_name(-1)) == "unknown");
}

TEST_CASE("The recorded GL renderer is normalized and classed from the raw string",
          "[telemetry][gl_renderer]") {
    ScopedTelemetryScreenContext context;
    REQUIRE(helix::telemetry_context::gl_renderer().name.empty());

    helix::telemetry_context::set_gl_renderer("llvmpipe (LLVM 15.0.6, 128 bits)");
    CHECK(helix::telemetry_context::gl_renderer().name == "llvmpipe");
    CHECK(helix::telemetry_context::gl_renderer().software);

    helix::telemetry_context::set_gl_renderer("V3D 4.2");
    CHECK(helix::telemetry_context::gl_renderer().name == "V3D 4.2");
    CHECK_FALSE(helix::telemetry_context::gl_renderer().software);

    helix::telemetry_context::set_gl_renderer(nullptr);
    CHECK(helix::telemetry_context::gl_renderer().name.empty());
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Session event carries the GL renderer once known, and the render mode",
                 "[telemetry][gl_renderer][session]") {
    ScopedTelemetryScreenContext context;
    ScopedRenderModeSetting setting;
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    DisplaySettingsManager::instance().set_gcode_render_mode(3);
    Config::get_instance()->set<bool>("/display/gpu_3d_blocked", true);
    tm.record_session();
    {
        auto snapshot = tm.get_queue_snapshot();
        REQUIRE(snapshot.size() == 1);
        const auto& app = snapshot[0]["app"];
        CHECK_FALSE(app.contains("gl_renderer"));
        CHECK_FALSE(app.contains("gl_software"));
        CHECK(app["gcode_render_mode"] == "thumbnail");
        CHECK(app["gpu_3d_blocked"] == true);
    }
    tm.clear_queue();

    DisplaySettingsManager::instance().set_gcode_render_mode(1); // also clears the block
    helix::telemetry_context::set_gl_renderer("llvmpipe (LLVM 15.0.6, 128 bits)");
    tm.record_session();
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);
    const auto& app = snapshot[0]["app"];
    REQUIRE(app.contains("gl_renderer"));
    CHECK(app["gl_renderer"] == "llvmpipe");
    CHECK(app["gl_software"] == true);
    CHECK(app["gcode_render_mode"] == "3d");
    CHECK(app["gpu_3d_blocked"] == false);
}

TEST_CASE_METHOD(TelemetryTestFixture, "A main loop hang names the screen and the GL renderer",
                 "[telemetry][gl_renderer][error]") {
    ScopedTelemetryScreenContext context;
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    helix::telemetry_context::active_panel_int.store(3);
    helix::telemetry_context::set_top_overlay("print_file_detail");
    helix::telemetry_context::set_gl_renderer("Mali-G52 (Panfrost)");
    helix::HangMemoryContext memory;
    memory.mem_available_mb = 19;
    memory.pressure = helix::MemoryPressureLevel::critical;
    memory.major_faults = 81234;
    tm.record_main_loop_hang(92500, memory);

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);
    const auto& e = snapshot[0];
    CHECK(e["event"] == "error_encountered");
    CHECK(e["category"] == "ui");
    CHECK(e["code"] == "main_loop_hang");
    CHECK(e["context"] == "stalled_92s");
    CHECK(e["active_panel"] == 3);
    REQUIRE(e.contains("active_overlay"));
    CHECK(e["active_overlay"] == "print_file_detail");
    REQUIRE(e.contains("gl_renderer"));
    CHECK(e["gl_renderer"] == "Mali-G52");
    CHECK(e["gl_software"] == false);
    REQUIRE(e.contains("mem_available_mb"));
    CHECK(e["mem_available_mb"] == 19);
    CHECK(e["pressure_level"] == "critical");
    REQUIRE(e.contains("major_faults"));
    CHECK(e["major_faults"] == 81234);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "A main loop hang before any overlay or GL context omits them",
                 "[telemetry][gl_renderer][error]") {
    ScopedTelemetryScreenContext context;
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    helix::telemetry_context::active_panel_int.store(0);
    tm.record_main_loop_hang(30000, {});
    // The "ui" category is rate-limited like any other.
    tm.record_main_loop_hang(60000, {});

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);
    const auto& e = snapshot[0];
    CHECK(e["context"] == "stalled_30s");
    CHECK(e["active_panel"] == 0);
    CHECK_FALSE(e.contains("active_overlay"));
    CHECK_FALSE(e.contains("gl_renderer"));
    CHECK_FALSE(e.contains("gl_software"));
    CHECK_FALSE(e.contains("mem_available_mb"));
    CHECK_FALSE(e.contains("major_faults"));
    CHECK(e["pressure_level"] == "none");
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "The performance snapshot carries the GL renderer the session event missed",
                 "[telemetry][gl_renderer][performance]") {
    ScopedTelemetryScreenContext context;
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_frame_time(8000);
    tm.record_performance_snapshot();
    helix::telemetry_context::set_gl_renderer("llvmpipe (LLVM 15.0.6, 128 bits)");
    tm.record_frame_time(8000);
    tm.record_performance_snapshot();

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK_FALSE(snapshot[0].contains("gl_renderer"));
    REQUIRE(snapshot[1].contains("gl_renderer"));
    CHECK(snapshot[1]["gl_renderer"] == "llvmpipe");
    CHECK(snapshot[1]["gl_software"] == true);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "A memory warning at warning or critical names the largest processes",
                 "[telemetry][memory_warning]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    helix::MemoryWarningEvent warning;
    warning.reason = "system_low";
    for (auto level : {helix::MemoryPressureLevel::elevated, helix::MemoryPressureLevel::warning,
                       helix::MemoryPressureLevel::critical}) {
        warning.level = level;
        tm.record_memory_warning(warning);
    }

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 3);
    CHECK_FALSE(snapshot[0].contains("top_processes"));
    for (size_t i = 1; i < 3; ++i) {
        INFO("level: " << snapshot[i]["level"]);
        // This test process holds resident memory, so there is at least one.
        REQUIRE(snapshot[i].contains("top_processes"));
        const auto& top = snapshot[i]["top_processes"];
        REQUIRE(top.is_array());
        REQUIRE_FALSE(top.empty());
        CHECK(top.size() <= 3);
        for (size_t j = 0; j < top.size(); ++j) {
            CHECK(top[j]["name"].is_string());
            CHECK(top[j]["rss_mb"].is_number_integer());
            if (j > 0) {
                CHECK(top[j - 1]["rss_mb"].get<int64_t>() >= top[j]["rss_mb"].get<int64_t>());
            }
        }
    }
}
