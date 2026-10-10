// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../test_helpers/mock_printer.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_cfs.h"
#include "app_globals.h"
#include "cfs_status_parse.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/mock_personas.h"
#include "test_helpers/moonraker_client_mock_test_access.h"
#include "test_helpers/update_queue_test_access.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Captures notify_status_update frames and waits for one carrying `box` that
/// satisfies a predicate. Dispatch may run on the mock's own thread.
class BoxFrames {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(n);
            cv_.notify_all();
        };
    }

    size_t mark() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    /// The first frame carrying `box` captured after `after` that satisfies `pred`.
    std::optional<json> wait_for(size_t after, const std::function<bool(const json&)>& pred,
                                 int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<json> hit;
        size_t next = after;
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (; next < frames_.size(); ++next) {
                const auto& params = frames_[next]["params"];
                if (params.is_array() && !params.empty() && params[0].contains("box") &&
                    pred(params[0])) {
                    hit = params[0];
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

/// The unit-1 bay the frame's box reports loaded ("None" when none is).
std::string loaded_bay(const json& status) {
    return status["box"]["T1"].value("filament", "");
}

bool nozzle_sees_filament(const json& status) {
    const char* key = "filament_switch_sensor filament_sensor";
    return status.contains(key) && status[key].value("filament_detected", false);
}

} // namespace

TEST_CASE("The k2 persona's CR_BOX load and unload scripts move the box frame",
          "[mock][persona][k2][cfs]") {
    helix::test::PersonaEnv env("k2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    BoxFrames frames;
    mock.register_notify_update(frames.callback());

    using helix::printer::AmsBackendCfs;
    using helix::printer::CfsMacroVariant;

    size_t mark = frames.mark();
    mock.gcode_script(AmsBackendCfs::load_gcode(0, CfsMacroVariant::K2));
    CHECK(frames.wait_for(
        mark, [](const json& st) { return loaded_bay(st) == "A" && nozzle_sees_filament(st); }));

    mark = frames.mark();
    mock.gcode_script(AmsBackendCfs::unload_gcode(CfsMacroVariant::K2));
    CHECK(frames.wait_for(mark, [](const json& st) {
        return loaded_bay(st) == "None" && !nozzle_sees_filament(st);
    }));

    // A load of another bay reports that bay, not the first.
    mark = frames.mark();
    mock.gcode_script(AmsBackendCfs::load_gcode(2, CfsMacroVariant::K2));
    CHECK(frames.wait_for(mark, [](const json& st) { return loaded_bay(st) == "C"; }));
}

TEST_CASE("The k2 persona's motor_control and fan_feedback frames parse",
          "[mock][persona][k2][cfs]") {
    helix::test::PersonaEnv env("k2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    json frame;
    const auto id =
        mock.register_notify_update([&frame](const json& n) { frame = n["params"][0]; });
    helix::MoonrakerClientMockTestAccess::dispatch_initial_state(mock);
    mock.unsubscribe_notify_update(id);
    REQUIRE(frame.contains("motor_control"));
    REQUIRE(frame.contains("fan_feedback"));

    const auto motor = helix::cfs::parse_motor_control(frame);
    REQUIRE(motor.has_value());
    CHECK(motor->motor_ready == std::optional<bool>(true));

    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.fan_state().init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2"});
    state.update_from_status(frame);
    for (const auto& fan : state.fan_state().get_fans()) {
        INFO(fan.object_name);
        CHECK(fan.rpm.has_value());
    }
}

TEST_CASE("The k2 persona reports the declared 350 bed through discovery", "[mock][persona][k2]") {
    helix::test::PersonaEnv env("k2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    mock.connect("ws://mock/websocket", [] {}, [] {});
    bool done = false;
    mock.discover_printer([&done] { done = true; });
    REQUIRE(done);

    const auto volume = mock.hardware().build_volume();
    CHECK(volume.declared_bed_x == Catch::Approx(350.0f));
    CHECK(volume.declared_bed_y == Catch::Approx(350.0f));
}

TEST_CASE("The k2 persona pushes a box script's frames before it answers",
          "[mock][persona][k2][cfs]") {
    helix::test::PersonaEnv env("k2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    mock.connect("ws://mock/websocket", [] {}, [] {});

    // The mock's periodic frames arrive on its simulation thread.
    std::mutex events_mutex;
    std::vector<std::string> events;
    auto record = [&events_mutex, &events](const char* event) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.emplace_back(event);
    };
    mock.register_notify_update([&record](const json& n) {
        const auto& params = n["params"];
        if (params.is_array() && !params.empty() && params[0].contains("box") &&
            loaded_bay(params[0]) == "A") {
            record("frame");
        }
    });
    mock.send_jsonrpc(
        "printer.gcode.script",
        {{"script",
          helix::printer::AmsBackendCfs::load_gcode(0, helix::printer::CfsMacroVariant::K2)}},
        [&record](const json&) { record("ack"); }, [](const MoonrakerError&) {});
    mock.disconnect();

    // The loaded bay is out before the answer, as on the wire. A periodic
    // frame can land on either side of the answer, so only the order of the
    // first frame and the answer is pinned.
    std::lock_guard<std::mutex> lock(events_mutex);
    INFO("events: " << json(events).dump());
    REQUIRE(std::count(events.begin(), events.end(), "ack") == 1);
    const auto ack = std::find(events.begin(), events.end(), "ack");
    CHECK(std::find(events.begin(), ack, "frame") != ack);
}

// The k2 mock answers a box script inside the same call that pushes its frames,
// so the status frame and the RPC answer reach the backend back to back. The
// backend applies frames on the main thread, so completion has to queue behind
// them and judge the toolhead switch the frame reported (#1761).
namespace {

/// The real CFS backend subscribed to the k2 mock, dispatching through the
/// real MoonrakerAPI path.
struct K2Cfs {
    helix::test::PersonaEnv env{"k2"};
    MockPrinter printer{MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS};
    helix::printer::AmsBackendCfs backend{&printer.api, &printer.client};

    K2Cfs() {
        helix::CfsTestAccess::set_macro_variant_k2(backend);
        REQUIRE(backend.start().success());
        drain();
    }
    ~K2Cfs() {
        backend.stop();
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Run @p script as an @p intent action with the toolhead switch reading
    /// @p at_nozzle beforehand, and return where the action settled.
    AmsSystemInfo run(AmsAction intent, bool at_nozzle, std::string script) {
        helix::CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, at_nozzle);
        helix::CfsTestAccess::force_phase_intent(backend, intent);
        REQUIRE(backend.get_system_info().filament_loaded == at_nozzle);
        REQUIRE(helix::CfsTestAccess::call_dispatch_action_script(backend, std::move(script))
                    .success());
        drain();
        return backend.get_system_info();
    }
};

} // namespace

TEST_CASE("A K2 CFS unload succeeds when the frame before its answer cleared the nozzle",
          "[ams][cfs][mock][persona][k2][1761]") {
    K2Cfs k2;
    const auto info =
        k2.run(AmsAction::UNLOADING, /*at_nozzle=*/true,
               helix::printer::AmsBackendCfs::unload_gcode(helix::printer::CfsMacroVariant::K2));
    CHECK_FALSE(info.filament_loaded);
    CHECK(info.operation_detail.empty());
    CHECK(info.action == AmsAction::IDLE);
}

TEST_CASE("A K2 CFS load succeeds when the frame before its answer reached the nozzle",
          "[ams][cfs][mock][persona][k2][1761]") {
    K2Cfs k2;
    const auto info =
        k2.run(AmsAction::LOADING, /*at_nozzle=*/false,
               helix::printer::AmsBackendCfs::load_gcode(0, helix::printer::CfsMacroVariant::K2));
    CHECK(info.filament_loaded);
    CHECK(info.operation_detail.empty());
    CHECK(info.action == AmsAction::IDLE);
}

TEST_CASE("CFS action completion runs from the UpdateQueue, not the answering thread",
          "[ams][cfs][mock][persona][k2][1761]") {
    K2Cfs k2;
    bool ran = false;
    const char* ran_from = nullptr;
    REQUIRE(helix::CfsTestAccess::dispatch_with_completion(
                k2.backend,
                helix::printer::AmsBackendCfs::unload_gcode(helix::printer::CfsMacroVariant::K2),
                [&ran, &ran_from]() {
                    ran = true;
                    ran_from = helix::ui::UpdateQueue::current_callback_tag();
                })
                .success());
    // The mock answered inside that call; completion waits for the queue.
    CHECK_FALSE(ran);
    K2Cfs::drain();
    REQUIRE(ran);
    CHECK(ran_from != nullptr);
}
