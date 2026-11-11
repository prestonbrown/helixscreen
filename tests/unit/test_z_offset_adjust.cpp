// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/app_globals.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/z_offset_utils.h"
#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "save_config_restart.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

class ZOffsetFixture : public LVGLTestFixture {
  public:
    ZOffsetFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        // SaveConfigWatch observes the process-wide PrinterState's klippy
        // subject, not this fixture's local one, so that one has to exist too.
        // init_subjects() is idempotent.
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~ZOffsetFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    void set_homed(const char* axes) {
        lv_subject_copy_string(state.motion_state().get_homed_axes_subject(), axes);
    }

    const std::string& last_sent() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

} // namespace

TEST_CASE_METHOD(ZOffsetFixture, "adjust clamps at the safe limit", "[z_offset][adjust][mock]") {
    // Opened at 0, already at +1.99mm: another +0.05 must stop at +2.0 of travel.
    auto r = helix::zoffset::adjust(api.get(), &state, 0.0, 1.99, 0.05);

    REQUIRE(r.new_offset_mm == Catch::Approx(2.0));
    REQUIRE(r.applied_delta_mm == Catch::Approx(0.01));
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust refuses a no-op move at the limit",
                 "[z_offset][adjust][mock]") {
    auto r = helix::zoffset::adjust(api.get(), &state, 0.0, 2.0, 0.05);

    REQUIRE(r.sent == false);
    REQUIRE(r.clamped_to_noop == true);
    REQUIRE(last_sent().find("Z_ADJUST") == std::string::npos);
}

TEST_CASE_METHOD(ZOffsetFixture, "the guard bounds travel from the session base, not the offset",
                 "[z_offset][adjust][mock]") {
    // A toolhead legitimately parked at -3.5mm. Clamping the *absolute* offset
    // snapped it to -2.0 on the first tap and drove the nozzle into the print;
    // the guard bounds how far this session may travel instead.
    auto r = helix::zoffset::adjust(api.get(), &state, -3.5, -3.5, -0.05);

    REQUIRE(r.sent == true);
    REQUIRE(r.clamped_to_noop == false);
    REQUIRE(r.applied_delta_mm == Catch::Approx(-0.05));
    REQUIRE(r.new_offset_mm == Catch::Approx(-3.55));
}

TEST_CASE_METHOD(ZOffsetFixture, "travel past the session limit is refused, not snapped",
                 "[z_offset][adjust][mock]") {
    // Opened at -3.5 and already walked the full 2mm of travel: the next step
    // stops at the limit rather than jumping anywhere.
    auto r = helix::zoffset::adjust(api.get(), &state, -3.5, -5.5, -0.05);

    REQUIRE(r.sent == false);
    REQUIRE(r.clamped_to_noop == true);
    REQUIRE(r.new_offset_mm == Catch::Approx(-5.5));
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "a clamped-to-noop result is distinguishable from a null-api "
                 "result",
                 "[z_offset][adjust][mock]") {
    // Both cases share sent == false; clamped_to_noop is what tells them apart
    // (see AdjustResult in z_offset_utils.h). A caller that used float equality
    // on applied_delta_mm to infer "nothing happened" could not tell these
    // apart without it.
    auto clamped = helix::zoffset::adjust(api.get(), &state, 0.0, 2.0, 0.05);
    REQUIRE(clamped.sent == false);
    REQUIRE(clamped.clamped_to_noop == true);

    auto null_api = helix::zoffset::adjust(nullptr, &state, 0.0, 0.0, 0.05);
    REQUIRE(null_api.sent == false);
    REQUIRE(null_api.clamped_to_noop == false);
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust rounds to the nearest micron",
                 "[z_offset][adjust][mock]") {
    // Repeated float addition would drift; the result must land on a micron.
    auto r = helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.0123456);

    REQUIRE(r.new_offset_mm == Catch::Approx(0.012));
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust omits MOVE=1 when axes are not homed",
                 "[z_offset][adjust][mock]") {
    set_homed("xy"); // Z missing — MOVE=1 would make Klipper error

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);

    REQUIRE(last_sent().find("MOVE=1") == std::string::npos);
    REQUIRE(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=0.050") != std::string::npos);
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust appends MOVE=1 when all axes are homed",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);

    REQUIRE(last_sent() == "SET_GCODE_OFFSET Z_ADJUST=0.050 MOVE=1");
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust accumulates the pending delta",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);
    helix::zoffset::adjust(api.get(), &state, 0.0, 0.05, -0.01);

    // +50um then -10um = +40um still unsaved.
    REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 40);
}

TEST_CASE_METHOD(ZOffsetFixture, "a firmware-managed save also clears the pending delta",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");
    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);
    REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 50);

    bool saved = false;
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::apply_and_save(
        api.get(), save_watch, ZOffsetCalibrationStrategy::FIRMWARE_MANAGED,
        [&]() { saved = true; }, [](const std::string&) {}, &state);

    REQUIRE(saved);
    REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 0);
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "a probe-calibrate save chains APPLY -> SAVE_CONFIG and clears "
                 "the pending delta",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");
    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);
    REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 50);

    bool saved = false;
    std::string error;
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::apply_and_save(
        api.get(), save_watch, ZOffsetCalibrationStrategy::PROBE_CALIBRATE, [&]() { saved = true; },
        [&](const std::string& msg) { error = msg; }, &state);

    // The APPLY rpc's success callback hops to the main thread to start the
    // watch, so the save is not in flight until the queue drains.
    REQUIRE(wait_until([&] { return last_sent() == "SAVE_CONFIG"; }, 3000));

    // SAVE_CONFIG's rpc is failed by the mock by design - Klipper drops the
    // reply when it restarts. That must NOT settle as a failure, and must not
    // settle as a success either: nothing is known until Klipper is back.
    process_lvgl(50);
    CHECK(error.empty());
    CHECK_FALSE(saved);

    // Klipper comes back. THAT is what says the save worked, and only then may
    // the pending delta be cleared.
    get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
    get_printer_state().set_klippy_state_sync(KlippyState::READY);
    REQUIRE(wait_until([&] { return saved; }, 3000));

    CHECK(error.empty());
    CHECK(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 0);
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "an idle step on a per-head changer adjusts the mounted head relatively",
                 "[z_offset][adjust][mock][per_head]") {
    // Z-Mod on a Creator 5: the mounted head runs at +3.413 live while the
    // machine-wide save-variable still holds a stale -0.150 from another head.
    state.set_printer_type_sync("FlashForge Creator 5 Pro");
    auto hint = [&] {
        return static_cast<ZOffsetTuneHint>(
            lv_subject_get_int(state.profile_state().get_z_offset_tune_hint_subject()));
    };
    REQUIRE(hint() == ZOffsetTuneHint::AUTO_SAVED);
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json{"gcode_move", "toolhead", "save_variables", "zmod_color",
                                    "gcode_button extruder_grab1", "gcode_macro SAVE_ZMOD_DATA"});
    state.set_hardware(hw);
    // Discovering the per-head storage republishes the tune hint.
    CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED_PER_HEAD);
    set_homed("xyz");
    lv_subject_set_int(state.motion_state().get_gcode_z_offset_subject(), 3413);
    lv_subject_set_int(state.motion_state().get_persisted_z_offset_subject(), -150);
    lv_subject_set_int(state.motion_state().get_persisted_z_offset_valid_subject(), 1);

    const int shown = helix::zoffset::displayed_z_offset_microns(state);
    REQUIRE(shown == 3413);

    auto r = helix::zoffset::adjust(api.get(), &state, shown / 1000.0, shown / 1000.0, -0.01);

    REQUIRE(r.sent);
    // Only the step: the probe-delta variable a global ZMOD clears first feeds
    // this firmware's pickup offset and its per-head save base.
    CHECK(last_sent() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
    CHECK(lv_subject_get_int(state.motion_state().get_persisted_z_offset_subject()) == -150);

    // A surface still holding the stored value as its current offset steps
    // relatively all the same, and leaves the stored value alone.
    auto stale = helix::zoffset::adjust(api.get(), &state, -0.15, -0.15, -0.01);
    REQUIRE(stale.sent);
    CHECK(last_sent() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
    CHECK(lv_subject_get_int(state.motion_state().get_persisted_z_offset_subject()) == -150);
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "an idle step on a printer that re-applies head offsets is always relative",
                 "[z_offset][adjust][mock][per_head]") {
    // The U1 sets the mounted head's stored offset absolutely at every tool
    // change; an absolute Z= from a stale base would overwrite it.
    state.set_printer_type_sync("Snapmaker U1");
    set_homed("xyz");
    lv_subject_set_int(state.motion_state().get_gcode_z_offset_subject(), 120);

    auto r = helix::zoffset::adjust(api.get(), &state, -0.05, -0.05, -0.01);

    REQUIRE(r.sent);
    CHECK(last_sent() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}

TEST_CASE_METHOD(LVGLTestFixture, "the z step index round-trips through Config",
                 "[z_offset][step]") {
    helix::zoffset::set_persisted_step_index(3);
    REQUIRE(helix::zoffset::persisted_step_index() == 3);

    // Out-of-range writes are rejected outright, not clamped-and-stored: the
    // previously persisted value (3) must survive untouched. Clamping on write
    // would let a future caller bug silently destroy the user's real setting;
    // the read path already defends against a corrupt on-disk value on its own.
    helix::zoffset::set_persisted_step_index(99);
    REQUIRE(helix::zoffset::persisted_step_index() == 3);

    helix::zoffset::set_persisted_step_index(-1);
    REQUIRE(helix::zoffset::persisted_step_index() == 3);
}

TEST_CASE_METHOD(ZOffsetFixture,
                 "a probe save on a printer without Z_OFFSET_APPLY_PROBE reports it and saves "
                 "nothing",
                 "[z_offset][adjust][mock][eddy]") {
    set_homed("xyz");
    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);
    mock_client.set_gcode_command_unregistered("Z_OFFSET_APPLY_PROBE");

    bool saved = false;
    std::string error;
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::apply_and_save(
        api.get(), save_watch, ZOffsetCalibrationStrategy::PROBE_CALIBRATE, [&]() { saved = true; },
        [&](const std::string& msg) { error = msg; }, &state);

    REQUIRE(wait_until([&] { return !error.empty(); }, 3000));
    CHECK(error.find("Z_OFFSET_APPLY_PROBE") != std::string::npos);
    CHECK_FALSE(saved);
    CHECK(last_sent() == "SET_GCODE_OFFSET Z_ADJUST=0.050 MOVE=1");
    CHECK(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 50);
}

namespace {

/// A two-tool changer whose ToolState is built from @p extra objects on top of
/// klipper-toolchanger's.
class ChangerSaveFixture : public ZOffsetFixture {
  public:
    explicit ChangerSaveFixture(std::vector<std::string> extra = {}) {
        auto objects = nlohmann::json::array({"gcode_move", "toolhead", "extruder", "extruder1",
                                              "toolchanger", "tool T0", "tool T1"});
        for (auto& o : extra) {
            objects.push_back(o);
        }
        PrinterDiscovery hw;
        hw.parse_objects(objects);
        state.set_hardware(hw);
        auto& ts = ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(true);
        ts.init_tools(state.get_discovery());
    }
    ~ChangerSaveFixture() override {
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// klipper-toolchanger: SAVE_TOOL_PARAMETER only stages, and the machine-wide
/// SAVE_CONFIG commits.
class ToolchangerSaveFixture : public ChangerSaveFixture {};

/// A Creator 5 on Reforge: ff_toolchange publishes the same objects, and its
/// calibration stages its own result.
class ReforgeSaveFixture : public ChangerSaveFixture {
  public:
    ReforgeSaveFixture() : ChangerSaveFixture({"ff_toolchange"}) {}
};

} // namespace

TEST_CASE_METHOD(ToolchangerSaveFixture,
                 "a toolchanger without Z_OFFSET_APPLY_PROBE still commits its tool offsets",
                 "[z_offset][adjust][mock][eddy][tool_offsets]") {
    auto& ts = ToolState::instance();
    ts.set_tool_offset_local(1, Axis::Z, 100);
    REQUIRE(ts.tool_offset_dirty(1, Axis::Z));
    set_homed("xyz");
    helix::zoffset::adjust(api.get(), &state, 0.0, 0.0, 0.05);
    mock_client.set_gcode_command_unregistered("Z_OFFSET_APPLY_PROBE");

    bool saved = false;
    std::string error;
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::save_dirty_offsets(
        api.get(), save_watch, ZOffsetCalibrationStrategy::PROBE_CALIBRATE, state.get_discovery(),
        /*global_dirty=*/true, [&]() { saved = true; },
        [&](const std::string& msg) { error = msg; }, &state);

    REQUIRE(wait_until([&] { return last_sent() == "SAVE_CONFIG"; }, 3000));
    CHECK(error.empty());
    get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
    get_printer_state().set_klippy_state_sync(KlippyState::READY);
    REQUIRE(wait_until([&] { return !error.empty(); }, 3000));

    // The tools are saved; the machine-wide offset is not, and says why.
    CHECK(error.find("Z_OFFSET_APPLY_PROBE") != std::string::npos);
    CHECK_FALSE(saved);
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::Z));
    CHECK(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 50);
}

TEST_CASE_METHOD(ReforgeSaveFixture,
                 "Reforge: a calibration result is saved with SAVE_CONFIG and nothing else",
                 "[z_offset][mock][tool_offsets][reforge]") {
    auto& ts = ToolState::instance();
    // Nothing per tool to babystep: the tune panel keeps the global offset.
    CHECK(lv_subject_get_int(ts.get_per_tool_axis_supported_subject(Axis::Z)) == 0);

    // Connect frame, then the frame CALIBRATE_TOOL_OFFSETS leaves behind.
    ts.update_from_status(
        {{"tool T1", {{"gcode_x_offset", 0.1}, {"gcode_y_offset", 0.2}, {"gcode_z_offset", 3.2}}}});
    REQUIRE_FALSE(ts.tool_offset_dirty(1, Axis::X));
    ts.update_from_status({{"tool T1", {{"gcode_x_offset", 0.12}, {"gcode_z_offset", 3.25}}}});
    REQUIRE(ts.tool_offset_dirty(1, Axis::X));
    REQUIRE(ts.tool_offset_dirty(1, Axis::Z));

    mock_client.clear_gcode_script_history();
    bool saved = false;
    std::string error;
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::save_dirty_offsets(
        api.get(), save_watch, ZOffsetCalibrationStrategy::PROBE_CALIBRATE, state.get_discovery(),
        /*global_dirty=*/false, [&]() { saved = true; },
        [&](const std::string& msg) { error = msg; }, &state);

    REQUIRE(wait_until([&] { return last_sent() == "SAVE_CONFIG"; }, 3000));
    CHECK(mock_client.gcode_script_history() == std::vector<std::string>{"SAVE_CONFIG"});
    get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
    get_printer_state().set_klippy_state_sync(KlippyState::READY);
    REQUIRE(wait_until([&] { return saved; }, 3000));
    CHECK(error.empty());
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::X));
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::Z));
}
