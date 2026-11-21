// SPDX-License-Identifier: GPL-3.0-or-later

// The guided calibration run: which steps it offers and in what order, what it
// asks the owner to have ready, and the runner that takes the picked steps to
// one save or one discard.

#include "../lvgl_test_fixture.h"
#include "calibration_catalog.h"
#include "calibration_config_results.h"
#include "calibration_run.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_profile_state.h"
#include "printer_state.h"
#include "z_offset_persistence.h"
#include "z_offset_utils.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using helix::ZOffsetCalibrationStrategy;

namespace {

std::vector<std::string> ids(const std::vector<StepInfo>& steps) {
    std::vector<std::string> out;
    for (const auto& s : steps) {
        out.emplace_back(s.id);
    }
    return out;
}

/// Every capability the catalog knows, so every step applies.
CatalogInputs everything() {
    CatalogInputs in;
    in.has_probe = true;
    in.has_bed_mesh = true;
    in.has_accelerometer = true;
    in.can_tension_belts = true;
    in.can_calibrate_cutter = true;
    in.has_pa_cal = true;
    in.has_tool_offset_cal = true;
    in.has_screws_tilt = true;
    in.has_stepper_resonance_cal = true;
    in.supports_belt_compare = true;
    in.supports_ripple_check = true;
    return in;
}

StepInfo step(StepKind kind, const char* id, PrereqSet prereqs = kPrereqNone) {
    StepInfo s;
    s.kind = kind;
    s.id = id;
    s.name = id;
    s.prereqs = prereqs;
    return s;
}

/// A step the test finishes by hand.
class FakeStep : public CalibrationStep {
  public:
    explicit FakeStep(const char* id) : info_(step(StepKind::Heaters, id)) {}
    const StepInfo& info() const override {
        return info_;
    }
    void start(StepContext& ctx) override {
        ctx_ = &ctx;
        ++starts;
        if (immediate) {
            ctx.done(*immediate);
        }
    }
    void abort() override {
        ++aborts;
        if (ctx_) {
            ctx_->done(StepOutcome::aborted());
        }
    }
    bool abort_restarted_firmware() const override {
        return restarts_on_abort;
    }
    bool restores_on_discard() const override {
        return restorable;
    }
    void restore(helix::IMoonrakerAPI&, std::function<void(std::string)> done) override {
        ++restores;
        restored = std::move(done);
    }
    void finish(const std::string& summary) {
        ctx_->done(StepOutcome::finished({summary, {{"v", summary}}}));
    }
    void fail(const std::string& why) {
        ctx_->done(StepOutcome::failed(why));
    }

    StepInfo info_;
    StepContext* ctx_ = nullptr;
    int starts = 0;
    int aborts = 0;
    bool restarts_on_abort = false;
    bool restorable = false;
    int restores = 0;
    std::function<void(std::string)> restored;
    std::optional<StepOutcome> immediate;
};

class FakeFinisher : public RunFinisher {
  public:
    void save_all(helix::SharedCallback<void()> on_saved,
                  helix::SharedCallback<void(const std::string&)> on_failed) override {
        ++saves;
        saved = std::move(on_saved);
        failed = std::move(on_failed);
    }
    void discard_all(std::function<void()> on_done,
                     std::function<void(const std::string&)> on_failed) override {
        ++discards;
        discarded = std::move(on_done);
        discard_failed = std::move(on_failed);
    }
    void record(const StepInfo& s, const StepResult& r) override {
        recorded.push_back(std::string(s.id) + "=" + r.summary);
    }
    void read_pending(std::function<void(bool)> done) override {
        ++pending_reads;
        done(pending);
    }
    bool pending = true;
    int pending_reads = 0;
    int saves = 0;
    int discards = 0;
    std::vector<std::string> recorded;
    helix::SharedCallback<void()> saved;
    helix::SharedCallback<void(const std::string&)> failed;
    std::function<void()> discarded;
    std::function<void(const std::string&)> discard_failed;
};

class RunFixture : public LVGLTestFixture {
  public:
    RunFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPI>(client, state);
    }
    ~RunFixture() override {
        run.reset();
        api.reset();
    }

    /// A run over @p n fake steps; the fakes stay reachable through steps.
    void make_run(size_t n) {
        std::vector<std::unique_ptr<CalibrationStep>> owned;
        static const char* kIds[] = {"a", "b", "c", "d"};
        for (size_t i = 0; i < n; ++i) {
            auto s = std::make_unique<FakeStep>(kIds[i]);
            fakes.push_back(s.get());
            owned.push_back(std::move(s));
        }
        run = std::make_unique<CalibrationRun>(*api, std::move(owned), [this]() { ++changes; });
    }
    StepStatus status_of(size_t i) const {
        return run->entries()[i].status;
    }

    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::vector<FakeStep*> fakes;
    std::unique_ptr<CalibrationRun> run;
    int changes = 0;
};

} // namespace

TEST_CASE("a guided run offers Z offset, heaters, bed mesh and input shaper only",
          "[calibration_run]") {
    CHECK(ids(guided_run_steps(applicable_steps(everything()))) ==
          std::vector<std::string>{"z_offset", "heaters", "bed_mesh", "input_shaper"});
    // Without the hardware a step is not offered at all.
    CHECK(ids(guided_run_steps(applicable_steps(CatalogInputs{}))) ==
          std::vector<std::string>{"heaters"});
}

TEST_CASE("a guided run leaves out a step the printer runs only on request", "[calibration_run]") {
    CatalogInputs in = everything();
    in.tier_overrides = {{"bed_mesh", Tier::OnRequest}, {"heaters", Tier::Optional}};
    const auto steps = guided_run_steps(applicable_steps(in));
    CHECK(ids(steps) == std::vector<std::string>{"z_offset", "heaters", "input_shaper"});
    CHECK_FALSE(picked_by_default(steps[1]));
    CHECK(picked_by_default(steps[0]));
}

TEST_CASE("the hands-on Z offset runs first whatever order the steps arrive in",
          "[calibration_run]") {
    const std::vector<StepInfo> shuffled{
        step(StepKind::InputShaper, "input_shaper"), step(StepKind::BedMesh, "bed_mesh"),
        step(StepKind::ZOffset, "z_offset"), step(StepKind::Heaters, "heaters")};
    CHECK(ids(guided_run_steps(shuffled)) ==
          std::vector<std::string>{"z_offset", "heaters", "bed_mesh", "input_shaper"});
}

TEST_CASE("before you start lists each prerequisite once with the steps that need it",
          "[calibration_run]") {
    CatalogInputs in = everything();
    in.z_strategy = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    const auto picked = guided_run_steps(applicable_steps(in));
    const auto lines = prereq_lines(picked);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].prereq == kPrereqBedClear);
    CHECK(ids({*lines[0].for_steps[0], *lines[0].for_steps[1], *lines[0].for_steps[2]}) ==
          std::vector<std::string>{"z_offset", "bed_mesh", "input_shaper"});
    CHECK(lines[1].prereq == kPrereqBuildPlate);
    CHECK(lines[1].for_steps.size() == 3);
    CHECK(lines[2].prereq == kPrereqPaper);
    REQUIRE(lines[2].for_steps.size() == 1);
    CHECK(std::string(lines[2].for_steps[0]->id) == "z_offset");
    CHECK(lines[3].prereq == kPrereqAccelerometer);
}

TEST_CASE("a probe that touches with the nozzle asks for no paper", "[calibration_run]") {
    CatalogInputs in = everything();
    in.nozzle_contact_probe = true;
    for (const auto& line : prereq_lines(guided_run_steps(applicable_steps(in)))) {
        CHECK(line.prereq != kPrereqPaper);
    }
    in.nozzle_contact_probe = false;
    in.z_strategy = ZOffsetCalibrationStrategy::FIRMWARE_MANAGED;
    for (const auto& line : prereq_lines(guided_run_steps(applicable_steps(in)))) {
        CHECK(line.prereq != kPrereqPaper);
    }
}

TEST_CASE_METHOD(RunFixture, "the run takes each step in turn, past a failure",
                 "[calibration_run]") {
    make_run(3);
    run->start();
    CHECK(run->phase() == RunPhase::Running);
    CHECK(fakes[0]->starts == 1);
    CHECK(fakes[1]->starts == 0);

    fakes[0]->finish("0.120 mm");
    CHECK(status_of(0) == StepStatus::Finished);
    CHECK(run->current() == 1);
    fakes[1]->fail("heater fault");
    CHECK(status_of(1) == StepStatus::Failed);
    CHECK(run->entries()[1].message == "heater fault");
    CHECK(fakes[2]->starts == 1);

    fakes[2]->finish("MZV 52 Hz");
    CHECK(run->phase() == RunPhase::Results);
    CHECK(run->finished_count() == 2);
    CHECK(run->can_save());
    CHECK(run->entries()[2].result.summary == "MZV 52 Hz");
}

TEST_CASE_METHOD(RunFixture, "a step that ends inside its own start moves the run on",
                 "[calibration_run]") {
    make_run(3);
    fakes[0]->immediate = StepOutcome::skipped();
    fakes[1]->immediate = StepOutcome::failed("no printer connection");
    run->start();
    CHECK(status_of(0) == StepStatus::Skipped);
    CHECK(status_of(1) == StepStatus::Failed);
    CHECK(status_of(2) == StepStatus::Running);
    CHECK(fakes[2]->starts == 1);
}

TEST_CASE_METHOD(RunFixture, "stop aborts the running step and keeps what finished",
                 "[calibration_run]") {
    make_run(3);
    run->start();
    fakes[0]->finish("0.120 mm");
    run->stop();
    CHECK(fakes[1]->aborts == 1);
    CHECK(fakes[2]->starts == 0);
    CHECK(run->phase() == RunPhase::Results);
    CHECK(status_of(0) == StepStatus::Finished);
    CHECK(status_of(1) == StepStatus::Aborted);
    CHECK(status_of(2) == StepStatus::NotRun);
    CHECK_FALSE(run->results_lost());
    CHECK(run->can_save());
}

TEST_CASE_METHOD(RunFixture, "a stop that restarts the firmware loses what finished",
                 "[calibration_run]") {
    make_run(2);
    fakes[1]->restarts_on_abort = true;
    run->start();
    fakes[0]->finish("0.120 mm");
    run->stop();
    CHECK(run->results_lost());
    CHECK_FALSE(run->can_save());

    FakeFinisher finisher;
    run->save_all(finisher);
    CHECK(finisher.saves == 0);
    // Klipper already dropped them; discarding needs no second restart.
    finisher.pending = false;
    run->discard_all(finisher);
    CHECK(finisher.discards == 0);
    CHECK(run->phase() == RunPhase::Discarded);
}

TEST_CASE_METHOD(RunFixture, "save all saves once and records the history only once it saved",
                 "[calibration_run]") {
    make_run(2);
    run->start();
    fakes[0]->finish("0.120 mm");
    fakes[1]->fail("timed out");

    FakeFinisher finisher;
    run->save_all(finisher);
    CHECK(finisher.saves == 1);
    CHECK(run->phase() == RunPhase::Saving);
    CHECK(finisher.recorded.empty());
    run->save_all(finisher);
    CHECK(finisher.saves == 1);

    finisher.saved();
    CHECK(run->phase() == RunPhase::Saved);
    CHECK(finisher.recorded == std::vector<std::string>{"a=0.120 mm"});
}

TEST_CASE_METHOD(RunFixture, "a failed save records nothing and can still be discarded",
                 "[calibration_run]") {
    make_run(1);
    run->start();
    fakes[0]->finish("0.120 mm");
    FakeFinisher finisher;
    run->save_all(finisher);
    finisher.failed("Klipper did not come back");
    CHECK(run->phase() == RunPhase::SaveFailed);
    CHECK(run->error() == "Klipper did not come back");
    CHECK(finisher.recorded.empty());

    run->discard_all(finisher);
    CHECK(finisher.discards == 1);
}

TEST_CASE_METHOD(RunFixture, "discard all restarts once and records nothing", "[calibration_run]") {
    make_run(2);
    run->start();
    fakes[0]->finish("0.120 mm");
    fakes[1]->finish("MZV 52 Hz");
    FakeFinisher finisher;
    run->discard_all(finisher);
    CHECK(finisher.discards == 1);
    CHECK(finisher.saves == 0);
    CHECK(run->phase() == RunPhase::Discarding);
    finisher.discarded();
    CHECK(run->phase() == RunPhase::Discarded);
    CHECK(finisher.recorded.empty());
}

TEST_CASE_METHOD(RunFixture, "a failed discard goes back to the choice with the reason",
                 "[calibration_run]") {
    make_run(1);
    run->start();
    fakes[0]->finish("0.120 mm");
    FakeFinisher finisher;
    run->discard_all(finisher);
    finisher.discard_failed("Klippy disconnected");
    CHECK(run->phase() == RunPhase::Results);
    CHECK(run->error() == "Klippy disconnected");
    CHECK(run->can_save());
}

TEST_CASE_METHOD(RunFixture, "discard all puts a firmware-persisted result back before restarting",
                 "[calibration_run]") {
    make_run(2);
    fakes[0]->restorable = true;
    run->start();
    fakes[0]->finish("Accepted at Z 1.235 mm");
    fakes[1]->finish("Nozzle PID");
    FakeFinisher finisher;
    run->discard_all(finisher);
    REQUIRE(fakes[0]->restores == 1);
    // The restart waits for the old offset to be back.
    CHECK(finisher.discards == 0);
    fakes[0]->restored("Restored to -1.245 mm");
    CHECK(run->entries()[0].restore_note == "Restored to -1.245 mm");
    CHECK(finisher.discards == 1);
    finisher.discarded();
    CHECK(run->phase() == RunPhase::Discarded);
    CHECK(finisher.recorded.empty());
}

TEST_CASE_METHOD(RunFixture, "a run whose only result was restored needs no restart",
                 "[calibration_run]") {
    make_run(1);
    fakes[0]->restorable = true;
    run->start();
    fakes[0]->finish("Accepted at Z 1.235 mm");
    FakeFinisher finisher;
    finisher.pending = false; // the firmware persisted it; Klipper holds nothing
    run->discard_all(finisher);
    fakes[0]->restored("Restored to -1.245 mm");
    CHECK(finisher.discards == 0);
    CHECK(run->phase() == RunPhase::Discarded);
}

TEST_CASE("a firmware-managed Z offset is set, and restored, by one command",
          "[calibration_run][zoffset]") {
    const helix::PrinterDiscovery plain;
    CHECK(helix::zoffset::firmware_managed_offset_gcode(-1.245, plain, false) ==
          "SET_GCODE_OFFSET Z=-1.245");

    helix::PrinterDiscovery persisting;
    persisting.parse_objects(nlohmann::json::array({"gcode_macro SAVE_ZMOD_DATA"}));
    const std::string clear = helix::zoffset::stale_probe_delta_clear_gcode(persisting);
    REQUIRE_FALSE(clear.empty());
    CHECK(helix::zoffset::firmware_managed_offset_gcode(0.05, persisting, false) ==
          clear + "\nSET_GCODE_OFFSET Z=0.050");
    // Under a print the delta is live and stays.
    CHECK(helix::zoffset::firmware_managed_offset_gcode(0.05, persisting, true) ==
          "SET_GCODE_OFFSET Z=0.050");
}

TEST_CASE("a guided run's results read as the measured values", "[calibration_run]") {
    CHECK(format_z_offset(-1.3) == "-1.300 mm");
    CHECK(format_pid(22.137, 1.0789, 113.4) == "22.1 / 1.08 / 113");
    CHECK(format_pid(68.2, 0.92, 1265.3) == "68.2 / 0.92 / 1265");
    CHECK(format_mpc(18.43) == "MPC 18.4 J/K");
    CHECK(join_shaper_axes(format_shaper_axis("mzv", 36.71), format_shaper_axis("ei", 47.6)) ==
          "X: MZV 36.7 · Y: EI 47.6");
    CHECK(join_shaper_axes(format_shaper_axis("mzv", 36.7), "") == "X: MZV 36.7");
    CHECK(format_mesh_range(0.183) == "0.18 mm range");
}

TEST_CASE("a mesh's range is the spread of its stored points", "[calibration_run]") {
    const auto range = mesh_points_range(nlohmann::json{{0.1, -0.08}, {0.02, 0.05}});
    REQUIRE(range.has_value());
    CHECK(*range == Catch::Approx(0.18));
    CHECK_FALSE(mesh_points_range(nlohmann::json::array()).has_value());
    CHECK_FALSE(mesh_points_range(nlohmann::json()).has_value());
}

TEST_CASE("the Z offset a calibration staged is read from the pending config",
          "[calibration_run]") {
    using nlohmann::json;
    CHECK(staged_z_offset(json{{"probe", {{"z_offset", "-1.300"}}}}) == -1.3);
    CHECK(staged_z_offset(json{{"beacon model default", {{"model_offset", "0.120"}}}}) == 0.12);
    CHECK(staged_z_offset(json{{"stepper_z", {{"position_endstop", "0.5"}}}}) == 0.5);
    // A probe's own offset wins over an endstop, and nothing else is a Z offset.
    CHECK(staged_z_offset(json{{"stepper_z", {{"position_endstop", "0.5"}}},
                               {"probe", {{"z_offset", "-1.2"}}}}) == -1.2);
    CHECK_FALSE(staged_z_offset(json{{"bed_mesh default", {{"version", "1"}}}}).has_value());
    CHECK_FALSE(staged_z_offset(json::object()).has_value());
}

TEST_CASE_METHOD(RunFixture, "discard all restarts exactly when Klipper holds something",
                 "[calibration_run]") {
    make_run(2);
    run->start();
    fakes[0]->fail("bed heater fault");
    fakes[1]->fail("timed out");
    FakeFinisher finisher;
    // Nothing finished, but the failed step had staged part of its result.
    finisher.pending = true;
    run->discard_all(finisher);
    CHECK(finisher.pending_reads == 1);
    CHECK(finisher.discards == 1);

    make_run(1);
    run->start();
    fakes.back()->finish("Nozzle PID");
    FakeFinisher nothing_pending;
    nothing_pending.pending = false;
    run->discard_all(nothing_pending);
    CHECK(nothing_pending.discards == 0);
    CHECK(run->phase() == RunPhase::Discarded);
}

TEST_CASE_METHOD(RunFixture, "a failed step's staged part is saved and shown",
                 "[calibration_run]") {
    make_run(1);
    run->start();
    fakes[0]->ctx_->done(
        StepOutcome::failed_after("bed heater fault", {"Nozzle 22.1 / 1.08 / 113", {}}));
    CHECK(run->entries()[0].status == StepStatus::Failed);
    CHECK(run->entries()[0].result.summary == "Nozzle 22.1 / 1.08 / 113");
    CHECK(run->can_save());
    FakeFinisher finisher;
    run->save_all(finisher);
    finisher.saved();
    // Only a finished step goes into the history.
    CHECK(finisher.recorded.empty());
}

TEST_CASE_METHOD(RunFixture, "a restart from outside the run loses what it staged",
                 "[calibration_run]") {
    make_run(2);
    run->start();
    fakes[0]->finish("-1.300 mm");
    run->klippy_restarted();
    CHECK(run->results_lost());
    fakes[1]->fail("Klippy shutdown");
    CHECK(run->phase() == RunPhase::Results);
    CHECK_FALSE(run->can_save());
}

TEST_CASE_METHOD(RunFixture, "the restart Save all causes is the save's own", "[calibration_run]") {
    make_run(1);
    run->start();
    fakes[0]->finish("-1.300 mm");
    FakeFinisher finisher;
    run->save_all(finisher);
    run->klippy_restarted();
    CHECK_FALSE(run->results_lost());
    finisher.saved();
    CHECK(run->phase() == RunPhase::Saved);
    CHECK(finisher.recorded == std::vector<std::string>{"a=-1.300 mm"});
}
