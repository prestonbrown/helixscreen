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

TEST_CASE("The Reforge persona's mock AMS is a tool changer", "[mock][creator5]") {
    MockTopologyGuards guards;
    helix::ScopedEnv reforge{"HELIX_MOCK_PRINTER", "creator5"};

    ScopedRuntimeConfig scoped_config;
    auto* config = get_runtime_config();
    config->test_mode = true;
    config->use_real_ams = false;
    REQUIRE(config->should_mock_ams());

    // No HELIX_MOCK_AMS: the persona, not the generic Happy Hare default, picks
    // the mock's topology.
    auto backend = helix::AmsBackend::create(helix::AmsType::NONE, nullptr, nullptr);
    REQUIRE(backend != nullptr);
    CHECK(backend->get_system_info().type == helix::AmsType::TOOL_CHANGER);
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
