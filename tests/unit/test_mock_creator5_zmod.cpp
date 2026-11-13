// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"
#include "ams_types.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/scoped_runtime_config.h"
#include "toolchanger_addon.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Captures notify_status_update frames and waits for a zmod_color one that
/// satisfies a predicate. Dispatch may run on the mock's own thread.
class ZmodFrames {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(n);
            cv_.notify_all();
        };
    }

    /// Frame count right now: wait_for searches only frames captured after a
    /// mark taken before the command, so an assertion cannot be satisfied by a
    /// frame that still carries the seeded slot values.
    size_t mark() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    std::optional<json> wait_for(size_t after, const std::function<bool(const json&)>& pred,
                                 int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<json> hit;
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (size_t i = frames_.size(); i-- > after;) {
                const auto& params = frames_[i]["params"];
                if (params.is_array() && !params.empty() && params[0].contains("zmod_color") &&
                    pred(params[0]["zmod_color"])) {
                    hit = params[0]["zmod_color"];
                    return true;
                }
            }
            return false;
        });
        return hit;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<json> frames_;
};

std::optional<json> slot_with_id(const json& zc, const char* id) {
    if (!zc.contains("slots")) {
        return std::nullopt;
    }
    for (const auto& s : zc["slots"]) {
        if (s.value("ID", "") == id) {
            return s;
        }
    }
    return std::nullopt;
}

/// The persona's own objects, not whatever another test in this shard left in
/// the mock-topology env vars (a leaked HELIX_MOCK_AMS=toolchanger would stand
/// the toolchanger objects up beside Z-Mod's).
struct MockTopologyGuards {
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
};

} // namespace

TEST_CASE("The Z-Mod C5 persona reports Z-Mod's objects", "[mock][creator5][zmod]") {
    MockTopologyGuards guards;

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    const helix::PrinterDiscovery hw = mock.hardware();
    REQUIRE(helix::toolchanger_addon::present(hw));
    CHECK(helix::toolchanger_addon::machine_name(hw) == "Creator 5 Pro");
    CHECK(helix::toolchanger_addon::resolve_material_source(hw).present);
    // The firmware reports the head count here, not the 24-entry palette size.
    CHECK(mock.zmod_color_status().value("color_limit", -1) == 4);
    CHECK_FALSE(hw.has_tool_changer());
    CHECK(hw.tool_names().size() == 4);

    // Z-Mod 1.7.4's afclite shim is on the object list (bundle 5WSPTZBL), and
    // discovery yields its AFC claim to the tool changer: the machine is four
    // heads, not a hub.
    REQUIRE(hw.has_zmod_afclite());
    REQUIRE_FALSE(hw.has_mmu());
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    CHECK(systems[0].type == helix::AmsType::TOOL_CHANGER);
}

TEST_CASE("The Z-Mod C5 persona mounts, parks and stores colours", "[mock][creator5][zmod]") {
    MockTopologyGuards guards;

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    ZmodFrames frames;
    mock.register_notify_update(frames.callback());

    size_t mark = frames.mark();
    mock.gcode_script("_T_IN T=2");
    CHECK(
        frames.wait_for(mark, [](const json& zc) { return zc.value("active_tool_id", -9) == 2; }));

    mark = frames.mark();
    mock.gcode_script("_T_OUT");
    CHECK(
        frames.wait_for(mark, [](const json& zc) { return zc.value("active_tool_id", -9) == -1; }));

    mark = frames.mark();
    mock.gcode_script("CHANGE_ZCOLOR SLOT=2 HEX=F72224 TYPE=PETG SILENT=1");
    auto stored = frames.wait_for(mark, [](const json& zc) {
        auto s = slot_with_id(zc, "2");
        return s && s->value("Material", "") == "PETG";
    });
    REQUIRE(stored.has_value());
    CHECK(slot_with_id(*stored, "2")->value("HEX", "") == "F72224");

    // An off-palette colour is stored as index 0, white, exactly as the firmware
    // does. Slot 3 seeds as 161616, so only the command's own frame can carry
    // FFFFFF here.
    mark = frames.mark();
    mock.gcode_script("CHANGE_ZCOLOR SLOT=3 HEX=123456 TYPE=ABS SILENT=1");
    auto white = frames.wait_for(mark, [](const json& zc) {
        auto s = slot_with_id(zc, "3");
        return s && s->value("HEX", "") == "FFFFFF";
    });
    CHECK(white.has_value());
}

/// Captures notify_gcode_response lines, mirroring ZmodFrames for the console.
class MarkerLines {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto& params = n["params"];
            if (params.is_array() && !params.empty() && params[0].is_string()) {
                lines_.push_back(params[0].get<std::string>());
                cv_.notify_all();
            }
        };
    }

    bool wait_for_line_containing(const std::string& needle, int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (const auto& l : lines_) {
                if (l.find(needle) != std::string::npos) {
                    return true;
                }
            }
            return false;
        });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> lines_;
};

TEST_CASE("The Z-Mod C5 persona narrates its swaps for the step bar", "[mock][creator5][zmod]") {
    MockTopologyGuards guards;

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    MarkerLines markers;
    mock.register_method_callback("notify_gcode_response", "zmod_marker_test", markers.callback());

    mock.gcode_script("_T_IN T=1");
    CHECK(markers.wait_for_line_containing("Grab attempt for T1"));
    CHECK(markers.wait_for_line_containing("_T_IN"));

    mock.gcode_script("_T_OUT");
    CHECK(markers.wait_for_line_containing("Unload attempt for T1"));
    CHECK(markers.wait_for_line_containing("_T_OUT"));
}

TEST_CASE("The mock persona and HELIX_MOCK_AMS pick the mock AMS's type",
          "[mock][creator5][ams][afc]") {
    MockTopologyGuards guards;
    ScopedRuntimeConfig scoped_config;
    auto* config = get_runtime_config();
    config->test_mode = true;
    config->use_real_ams = false;
    REQUIRE(config->should_mock_ams());

    // The AFC topology still simulated by the mock backend; `afc` itself is mock hardware.
    auto [variable, value, type] = GENERATE(table<const char*, const char*, helix::AmsType>(
        {{"HELIX_MOCK_AMS", "quattro_box", helix::AmsType::AFC},
         {"HELIX_MOCK_AMS", "quattrobox", helix::AmsType::AFC}}));
    CAPTURE(variable, value);
    helix::ScopedEnv env{variable, value};
    auto backend = helix::AmsBackend::create(helix::AmsType::NONE, nullptr, nullptr);
    REQUIRE(backend != nullptr);
    CHECK(backend->get_system_info().type == type);
}

TEST_CASE("A mock-hardware persona declines the mock AMS backend", "[mock][creator5][zmod]") {
    MockTopologyGuards guards;
    helix::ScopedEnv zmod{"HELIX_MOCK_PRINTER", "creator5_zmod"};

    ScopedRuntimeConfig scoped_config;
    auto* config = get_runtime_config();
    config->test_mode = true;
    config->use_real_ams = false;
    REQUIRE(config->should_mock_ams());

    // creator5_zmod models hardware the production AmsBackendToolChanger drives,
    // so with mock AMS on and no HELIX_MOCK_AMS the factory must defer to real
    // discovery instead of standing a generic mock in front of it.
    CHECK(helix::AmsBackend::create(helix::AmsType::NONE, nullptr, nullptr) == nullptr);
}

TEST_CASE("The mock AD5X backend carries the tools map only on Z-Mod's built-in AFC",
          "[mock][ad5x][zmod][2033]") {
    MockTopologyGuards guards;
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", "ad5x"};
    ScopedRuntimeConfig scoped_config;
    auto* config = get_runtime_config();
    config->test_mode = true;
    config->use_real_ams = false;

    {
        helix::ScopedEnv afc{"HELIX_MOCK_ZMOD_AFC", "1"};
        auto backend = helix::AmsBackend::create(helix::AmsType::NONE, nullptr, nullptr);
        REQUIRE(backend != nullptr);
        CHECK(backend->print_start_params({{0, 2}}) == helix::PrintStartParams{{"tools", "3"}});
    }
    helix::ScopedEnv afc{"HELIX_MOCK_ZMOD_AFC", nullptr};
    auto backend = helix::AmsBackend::create(helix::AmsType::NONE, nullptr, nullptr);
    REQUIRE(backend != nullptr);
    CHECK(backend->print_start_params({{0, 2}}).empty());
}

TEST_CASE("The Z-Mod C5 persona answers T_INFO with the tool-state block",
          "[mock][creator5][zmod]") {
    MockTopologyGuards guards;

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    MarkerLines markers;
    mock.register_method_callback("notify_gcode_response", "zmod_tinfo_test", markers.callback());

    mock.gcode_script("T_INFO");
    CHECK(markers.wait_for_line_containing("T0: home"));
    mock.gcode_script("_T_IN T=3");
    CHECK(markers.wait_for_line_containing("Grab attempt for T3"));
    mock.gcode_script("T_INFO");
    // The mounted head's row flips to HEAD, and the block still ends with the
    // Offset line the carriage-empty answer hangs off.
    CHECK(markers.wait_for_line_containing("T3: HEAD"));
    CHECK(markers.wait_for_line_containing("Offset: X=0.0000"));
}

TEST_CASE("The Z-Mod C5 persona can run the built-in AFC", "[mock][creator5][zmod][2033]") {
    MockTopologyGuards guards;
    helix::ScopedEnv afc{"HELIX_MOCK_ZMOD_AFC", "1"};

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    const helix::PrinterDiscovery hw = mock.hardware();
    REQUIRE(hw.has_zmod_afc());
    CHECK_FALSE(hw.has_zmod_afclite());
    CHECK(hw.afc_lane_names() == std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
    REQUIRE_FALSE(hw.has_mmu());
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    CHECK(systems[0].type == helix::AmsType::TOOL_CHANGER);

    // Load on head 1 feeds through lane2, and the mock mounts that head.
    const auto feed = helix::toolchanger_addon::resolve_tool_feed(hw);
    REQUIRE(feed.head_gcode.size() == 4);
    REQUIRE(feed.head_gcode[1] == "CHANGE_TOOL LANE=lane2");
    ZmodFrames frames;
    mock.register_notify_update(frames.callback());
    const size_t mark = frames.mark();
    mock.gcode_script(feed.head_gcode[1]);
    CHECK(
        frames.wait_for(mark, [](const json& zc) { return zc.value("active_tool_id", -9) == 1; }));
}

TEST_CASE("The AD5X persona can run Z-Mod's built-in AFC", "[mock][ad5x][zmod][2033]") {
    MockTopologyGuards guards;
    helix::ScopedEnv afc{"HELIX_MOCK_ZMOD_AFC", "1"};

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_AD5X);
    const helix::PrinterDiscovery hw = mock.hardware();
    REQUIRE(hw.has_zmod_afc());
    CHECK(hw.mmu_type() == helix::AmsType::AD5X_IFS);
}

// Z-Mod builds older than the built-in AFC keep today's behaviour: the afclite
// shim's E0..E3 lanes and its own lane picking on the C5, and the IFS backend on
// an AD5X with neither AFC surface.
TEST_CASE("An older Z-Mod with the afclite shim behaves as before",
          "[mock][creator5][zmod][2033]") {
    MockTopologyGuards guards;
    helix::ScopedEnv afc{"HELIX_MOCK_ZMOD_AFC", nullptr};

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    const helix::PrinterDiscovery hw = mock.hardware();
    REQUIRE(hw.has_zmod_afclite());
    CHECK_FALSE(hw.has_zmod_afc());
    CHECK(hw.afc_lane_names() == std::vector<std::string>{"E0", "E1", "E2", "E3"});
    REQUIRE(hw.detected_ams_systems().size() == 1);
    CHECK(hw.detected_ams_systems()[0].type == helix::AmsType::TOOL_CHANGER);
    CHECK(helix::toolchanger_addon::firmware_picks_lanes_at_print(hw));
    const auto feed = helix::toolchanger_addon::resolve_tool_feed(hw);
    REQUIRE(feed.head_gcode.size() == 4);
    for (int head = 0; head < 4; ++head) {
        CHECK(feed.head_gcode[static_cast<size_t>(head)] ==
              "CHANGE_TOOL LANE=E" + std::to_string(head));
    }
}

TEST_CASE("An older Z-Mod AD5X without an AFC surface keeps the IFS backend",
          "[mock][ad5x][zmod][2033]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"zmod", "zmod_color", "zmod_ifs",
                                  "filament_motion_sensor ifs_motion_sensor", "extruder"}));
    CHECK_FALSE(hw.has_zmod_afc());
    CHECK(hw.mmu_type() == helix::AmsType::AD5X_IFS);
}
