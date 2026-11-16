// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "gcode_footer_summary.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "motor_ripple_store.h"
#include "print_start_checks.h"
#include "print_start_inputs.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::calibration::ripple::MotorId;
using helix::calibration::ripple::RippleBand;

namespace {

/// The Voron's saved bands, as a completed sweep stores them.
std::vector<RippleBand> voron_bands() {
    return {{MotorId::StepperY, 41.6f, 44.9f, 4, 43.2f, 905.0f, 216.0f},
            {MotorId::StepperX, 77.0f, 96.95f, 2, 86.3f, 3011.0f, 215.75f},
            {MotorId::StepperY, 77.0f, 96.95f, 2, 86.3f, 2805.0f, 215.75f},
            {MotorId::StepperY, 153.9f, 179.4f, 1, 172.5f, 1413.0f, 215.6f}};
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a file whose outer walls land in a shaky band warns",
                 "[print_start][ripple]") {
    PrintStartContext ctx;
    ctx.ripple_bands = voron_bands();
    ctx.outer_wall_speed_mm_s = 86.0f;
    const auto r = gate_outer_wall_speed(ctx);
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.body.find("86") != std::string::npos);
    CHECK(r.body.find("77-97") != std::string::npos);
    CHECK(r.body.find("76") != std::string::npos);
    CHECK(r.body.find("97") != std::string::npos);
    CHECK_FALSE(r.proceed_label.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "outer walls clear of every band, or nothing to compare, pass",
                 "[print_start][ripple]") {
    PrintStartContext ctx;
    ctx.ripple_bands = voron_bands();
    ctx.outer_wall_speed_mm_s = 120.0f;
    CHECK(gate_outer_wall_speed(ctx).verdict == CheckResult::Verdict::Pass);
    ctx.outer_wall_speed_mm_s.reset();
    CHECK(gate_outer_wall_speed(ctx).verdict == CheckResult::Verdict::Pass);
    ctx.outer_wall_speed_mm_s = 86.0f;
    ctx.ripple_bands.clear();
    CHECK(gate_outer_wall_speed(ctx).verdict == CheckResult::Verdict::Pass);
}

namespace {

/// Accepts a save without a database behind it.
class NoDatabaseAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;
    void database_post_item(const std::string&, const std::string&, const json&,
                            helix::SharedCallback<void()>, ErrorCallback) override {}
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "saved bands ask for the footer's wall speed and reach the check",
                 "[print_start][ripple]") {
    using helix::calibration::ripple::RippleResultCache;
    CHECK_FALSE(gcode::footer_read_need(true, true, true).any());
    CHECK(gcode::footer_read_need(true, true, true, true).any());

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    NoDatabaseAPI api(client, state);
    calibration::ripple::SweepResult saved;
    saved.bands = voron_bands();
    RippleResultCache::instance().save(api, saved);

    FileStartFacts facts;
    facts.outer_wall_speed_mm_s = 86.0f;
    PrintStartContext ctx;
    fill_file_context(ctx, facts);
    RippleResultCache::instance().clear();
    REQUIRE(ctx.outer_wall_speed_mm_s == 86.0f);
    REQUIRE(ctx.ripple_bands.size() == 4);
    CHECK(gate_outer_wall_speed(ctx).verdict == CheckResult::Verdict::Warn);
}
