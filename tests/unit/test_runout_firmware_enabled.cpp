// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_runout_firmware_enabled.cpp
 * @brief Runout decisions vs the firmware's per-sensor enabled flag.
 *
 * Run with: ./build/bin/helix-tests "[1714]"
 *
 * Klipper's filament_switch_sensor / filament_motion_sensor status carries
 * `enabled` (SET_FILAMENT_SENSOR ENABLE=0/1). A stood-down sensor still
 * reports filament_detected live but takes no runout action of its own, so
 * no HelixScreen runout decision may act on its reading either. The concrete
 * shape: multi-tool hardware (FlashForge Creator 5 on Z-Mod, fd_ex0..3) where
 * the firmware enables only the active head's sensor and the parked heads
 * read empty.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "print_start_checks.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

constexpr const char* HEAD0 = "filament_switch_sensor fd_ex0";
constexpr const char* HEAD1 = "filament_switch_sensor fd_ex1";

/// Substring of the removal toast; the full text is prefixed with the role's
/// display name.
constexpr const char* REMOVED_WARNING = "Filament removed";

/// One switch-sensor status frame. Omitted fields stay unset, which is the
/// shape a Moonraker delta frame has for anything it does not carry.
nlohmann::json sensor_frame(const char* klipper, bool filament_detected,
                            std::optional<bool> enabled) {
    nlohmann::json fields = nlohmann::json{{"filament_detected", filament_detected}};
    if (enabled.has_value()) {
        fields["enabled"] = *enabled;
    }
    return nlohmann::json{{klipper, fields}};
}

/// RAII capture of the toast severities through the hooks the notification
/// stubs call (tests/ui_test_utils.h).
class ToastCapture {
  public:
    ToastCapture() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings_.push_back(msg); });
    }
    ~ToastCapture() {
        helix::ui::set_test_notification_warning_hook(nullptr);
    }
    ToastCapture(const ToastCapture&) = delete;
    ToastCapture& operator=(const ToastCapture&) = delete;

    [[nodiscard]] int warnings_containing(const std::string& needle) const {
        return static_cast<int>(
            std::count_if(warnings_.begin(), warnings_.end(), [&](const std::string& m) {
                return m.find(needle) != std::string::npos;
            }));
    }

  private:
    std::vector<std::string> warnings_;
};

/// Real manager, no AMS backend, startup grace expired: the firmware-enabled
/// term is the only thing that can suppress a runout here.
class FirmwareEnabledFixture : public LVGLTestFixture {
  public:
    FilamentSensorManager& fsm = FilamentSensorManager::instance();

    FirmwareEnabledFixture() {
        AmsState::instance().clear_backends();
        fsm.init_subjects();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
    }

    ~FirmwareEnabledFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().clear_backends();
    }

    /// Discover @p klipper names and give every sensor the RUNOUT role, the
    /// way a preset/settings.json restore does (no single-RUNOUT exclusivity).
    void seed_runout_sensors(std::initializer_list<const char*> klipper_names) {
        std::vector<std::string> names;
        names.reserve(klipper_names.size());
        for (const char* name : klipper_names) {
            names.emplace_back(name);
        }
        fsm.discover_sensors(names);
        for (const auto& name : names) {
            PostUnloadGraceTestAccess::force_role(fsm, name, FilamentSensorRole::RUNOUT);
        }
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
    }

    void feed(const nlohmann::json& frame) {
        fsm.update_from_status(frame);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(FirmwareEnabledFixture,
                 "stood-down empty runout sensor raises no runout; re-enabled it does",
                 "[runout][1714]") {
    seed_runout_sensors({HEAD0});

    // Firmware disabled the sensor and it reads empty (a parked head).
    feed(sensor_frame(HEAD0, false, false));
    CHECK_FALSE(fsm.has_any_runout());
    CHECK_FALSE(fsm.has_real_runout());

    // The firmware re-enables it while it still reads empty: Klipper resumes
    // acting on this sensor, so the empty reading becomes a real runout.
    feed(sensor_frame(HEAD0, false, true));
    CHECK(fsm.has_any_runout());
    CHECK(fsm.has_real_runout());
}

TEST_CASE_METHOD(FirmwareEnabledFixture, "a sensor that never reported enabled counts as running",
                 "[runout][1714]") {
    seed_runout_sensors({HEAD0});

    // No `enabled` field in any frame (older Moonraker / object without it):
    // the state default (enabled=true) must keep the sensor in the decision.
    feed(sensor_frame(HEAD0, false, std::nullopt));
    CHECK(fsm.has_any_runout());
    CHECK(fsm.has_real_runout());
}

TEST_CASE_METHOD(FirmwareEnabledFixture,
                 "delta frame without enabled keeps the prior firmware state", "[runout][1714]") {
    seed_runout_sensors({HEAD0});

    feed(sensor_frame(HEAD0, true, false));         // stood down, filament present
    feed(sensor_frame(HEAD0, false, std::nullopt)); // delta: detected only
    // Still stood down: the delta must not reset the firmware state.
    CHECK_FALSE(fsm.has_any_runout());

    feed(sensor_frame(HEAD0, false, true));
    CHECK(fsm.has_any_runout());
}

TEST_CASE_METHOD(FirmwareEnabledFixture, "two runout sensors: only the running one counts",
                 "[runout][1714]") {
    seed_runout_sensors({HEAD0, HEAD1});

    // Head 0 stood down and empty, head 1 running and present -> no runout.
    feed(sensor_frame(HEAD0, false, false));
    feed(sensor_frame(HEAD1, true, true));
    CHECK_FALSE(fsm.has_any_runout());
    CHECK_FALSE(fsm.has_real_runout());

    // Only a stood-down sensor is empty -> never a runout.
    feed(sensor_frame(HEAD1, true, false));
    feed(sensor_frame(HEAD0, true, true));
    CHECK_FALSE(fsm.has_any_runout());

    // The running sensor empties -> runout.
    feed(sensor_frame(HEAD0, true, false));
    feed(sensor_frame(HEAD1, false, true));
    CHECK(fsm.has_any_runout());
    CHECK(fsm.has_real_runout());
}

TEST_CASE_METHOD(FirmwareEnabledFixture,
                 "runout subject speaks for the running sensor, falls back to the first holder",
                 "[runout][1714][subject]") {
    seed_runout_sensors({HEAD0, HEAD1});

    // First RUNOUT sensor stood down and EMPTY; second running and present.
    // The subject must report the running head (1 = loaded), not the parked
    // one's reading (0).
    feed(sensor_frame(HEAD0, false, false));
    feed(sensor_frame(HEAD1, true, true));
    REQUIRE(fsm.get_runout_detected_subject() != nullptr);
    CHECK(lv_subject_get_int(fsm.get_runout_detected_subject()) == 1);

    // Every holder of the role stood down: the subject is the sensor tile's
    // display, so it keeps the first holder's live reading (HEAD0 reads
    // present) instead of muting. Runout decisions do not read this value.
    feed(sensor_frame(HEAD0, true, false));
    feed(sensor_frame(HEAD1, true, false));
    CHECK(lv_subject_get_int(fsm.get_runout_detected_subject()) == 1);

    // The same fallback with the first holder empty: the tile shows the
    // reading (0 = red), while has_any_runout stays false because no sensor
    // is monitoring.
    feed(sensor_frame(HEAD0, false, std::nullopt));
    CHECK(lv_subject_get_int(fsm.get_runout_detected_subject()) == 0);
    CHECK_FALSE(fsm.has_any_runout());
    CHECK_FALSE(fsm.has_real_runout());
}

TEST_CASE_METHOD(FirmwareEnabledFixture,
                 "pre-print gate: stood-down runout sensor raises no warning",
                 "[runout][1714][print-start]") {
    seed_runout_sensors({HEAD0});

    // The same three levers ui_print_start_controller gathers from the manager.
    auto gather_runout_ctx = [this] {
        PrintStartContext ctx;
        ctx.runout_enabled = fsm.is_master_enabled();
        ctx.runout_available = fsm.is_sensor_available(FilamentSensorRole::RUNOUT);
        ctx.runout_detected = fsm.is_filament_detected(FilamentSensorRole::RUNOUT);
        return ctx;
    };
    const auto& gates = default_print_start_gates();
    const auto gate_it = std::find_if(gates.begin(), gates.end(), [](const PrintStartGate& g) {
        return g.name == "required_filament_present";
    });
    REQUIRE(gate_it != gates.end());

    // Stood down and empty: unavailable for the runout question -> Pass.
    feed(sensor_frame(HEAD0, false, false));
    auto ctx = gather_runout_ctx();
    REQUIRE(ctx.runout_enabled);
    REQUIRE_FALSE(ctx.runout_available);
    CHECK(gate_it->evaluate(ctx).verdict == CheckResult::Verdict::Pass);

    // Control: running and empty -> the gate still warns.
    feed(sensor_frame(HEAD0, false, true));
    ctx = gather_runout_ctx();
    REQUIRE(ctx.runout_available);
    REQUIRE_FALSE(ctx.runout_detected);
    CHECK(gate_it->evaluate(ctx).verdict == CheckResult::Verdict::Warn);
}

TEST_CASE_METHOD(FirmwareEnabledFixture, "no removal toast for an edge on a stood-down sensor",
                 "[runout][1714][toast]") {
    seed_runout_sensors({HEAD0});
    feed(sensor_frame(HEAD0, true, true));
    REQUIRE_FALSE(fsm.is_in_startup_grace_period());
    REQUIRE_FALSE(is_wizard_active());
    REQUIRE_FALSE(AmsState::instance().is_filament_operation_active());

    // The stand-down and the empty reading land in one frame: no toast.
    ToastCapture toasts;
    feed(sensor_frame(HEAD0, false, false));
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);

    // Re-enable while still empty: a runout for the decisions, but the removal
    // was not observed, so it is not announced as one.
    feed(sensor_frame(HEAD0, false, true));
    CHECK(fsm.has_any_runout());
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);

    // Control on the same fixture: a removal observed while monitoring
    // announces, exactly once.
    feed(sensor_frame(HEAD0, true, true));
    feed(sensor_frame(HEAD0, false, true));
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);
}
