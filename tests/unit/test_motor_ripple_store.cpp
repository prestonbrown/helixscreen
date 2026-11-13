// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "motor_ripple_store.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::calibration::ripple;
using json = nlohmann::json;

namespace {

SweepResult sample_result() {
    SweepResult r;
    r.sample_rate_hz = 3105.0f;
    r.ceiling_hz = 1397.0f;
    r.unix_time = 1762990000;
    MotorSweep x;
    x.motor = MotorId::StepperX;
    SpeedResult s;
    s.speed_mm_s = 86.3f;
    s.ripple = {192.0f, 3011.0f, std::nullopt};
    s.broadband = 4.2f;
    s.frames_used = 50;
    x.speeds.push_back(s);
    r.sweeps.push_back(x);
    r.compensated_sweeps.push_back(x);
    r.bands.push_back({MotorId::StepperY, 77.0f, 96.95f, 2, 86.3f, 2805.0f, 215.75f});
    r.skipped.push_back({MotorId::StepperY, "no accelerometer rides the Y axis"});
    return r;
}

/// Serves one stored document and records what gets saved.
class DatabaseAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;
    void database_get_item(const std::string& ns, const std::string& key,
                           helix::SharedCallback<void(const json&)> on_success,
                           ErrorCallback) override {
        asked = ns + "/" + key;
        on_success(stored);
    }
    void database_post_item(const std::string& ns, const std::string& key, const json& value,
                            helix::SharedCallback<void()> on_success, ErrorCallback) override {
        posted = ns + "/" + key;
        stored = value;
        if (on_success) {
            on_success();
        }
    }
    json stored;
    std::string asked;
    std::string posted;
};

} // namespace

TEST_CASE("a saved sweep result reads back as it was, unmeasured ripples included",
          "[ripple][store]") {
    const auto j = ripple_result_to_json(sample_result());
    CHECK(j["sweeps"][0]["speeds"][0]["r"][2].is_null());
    const auto back = ripple_result_from_json(j);
    REQUIRE(back.has_value());
    CHECK(back->sample_rate_hz == Catch::Approx(3105.0f));
    CHECK(back->unix_time == 1762990000);
    REQUIRE(back->sweeps.size() == 1);
    REQUIRE(back->sweeps[0].speeds.size() == 1);
    const auto& s = back->sweeps[0].speeds[0];
    CHECK(*s.ripple[1] == Catch::Approx(3011.0f));
    CHECK_FALSE(s.ripple[2].has_value());
    CHECK(back->compensated_sweeps.size() == 1);
    REQUIRE(back->bands.size() == 1);
    CHECK(back->bands[0].motor == MotorId::StepperY);
    CHECK(back->bands[0].hi_mm_s == Catch::Approx(96.95f));
    CHECK(back->bands[0].harmonic == 2);
    REQUIRE(back->skipped.size() == 1);
    CHECK(back->skipped[0].reason == "no accelerometer rides the Y axis");
}

TEST_CASE("a damaged or foreign saved result reads as no result", "[ripple][store]") {
    auto j = ripple_result_to_json(sample_result());
    j["version"] = 2;
    CHECK_FALSE(ripple_result_from_json(j).has_value());
    j = ripple_result_to_json(sample_result());
    j.erase("bands");
    CHECK_FALSE(ripple_result_from_json(j).has_value());
    j = ripple_result_to_json(sample_result());
    j["sweeps"] = "x";
    CHECK_FALSE(ripple_result_from_json(j).has_value());
    j = ripple_result_to_json(sample_result());
    j["bands"][0]["motor"] = "stepper_z";
    CHECK_FALSE(ripple_result_from_json(j).has_value());
    CHECK_FALSE(ripple_result_from_json(json::parse("[1, 2", nullptr, false)).has_value());
    CHECK_FALSE(ripple_result_from_json(json()).has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the result cache serves bands only once a result is loaded",
                 "[ripple][store]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    DatabaseAPI api(client, state);
    RippleResultCache cache;
    CHECK(cache.bands().empty());

    api.stored = ripple_result_to_json(sample_result());
    cache.load(api);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(api.asked == "helix-screen/speed_sweep");
    REQUIRE(cache.bands().size() == 1);

    auto newer = sample_result();
    newer.bands.clear();
    cache.save(api, newer);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(api.posted == "helix-screen/speed_sweep");
    CHECK(cache.bands().empty());

    api.stored = json{{"version", 1}};
    cache.load(api);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(cache.result().has_value());
}
