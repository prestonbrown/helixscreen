// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "app_globals.h"
#include "core/update_queue.h"
#include "display_backend.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "screensaver.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/screensaver_manager_test_access.h"

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

/// Gives the shared manager a scripted clock for the test and the real one back after it.
struct ScriptedGateClock {
    uint64_t wall_ns = 5000ULL * 1000000000ULL;
    ScreensaverManager& savers = ScreensaverManager::instance();

    ScriptedGateClock() {
        savers.stop();
        helix::ScreensaverManagerTestAccess::set_cpu_clock(
            savers, [this] { return helix::ui::CpuSample{0, wall_ns}; });
        helix::ScreensaverManagerTestAccess::reset_baseline(savers);
    }
    ~ScriptedGateClock() {
        helix::ScreensaverManagerTestAccess::set_cpu_clock(savers,
                                                           helix::ui::read_process_cpu_clock);
        helix::ScreensaverManagerTestAccess::reset_baseline(savers);
    }
};

/// Backend whose type and GPU flag the test picks, so the host's display-path key can be
/// asserted without hardware. Everything else on the interface is inert.
class FakeKeyBackend : public DisplayBackend {
  public:
    FakeKeyBackend(DisplayBackendType type, bool gpu) : m_type(type), m_gpu(gpu) {}

    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return m_type;
    }
    const char* name() const override {
        return "FakeKey";
    }
    bool is_available() const override {
        return true;
    }
    bool is_gpu_accelerated() const override {
        return m_gpu;
    }

  private:
    DisplayBackendType m_type;
    bool m_gpu;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "the display manager's idle-check tick samples the screensaver gate",
                 "[application][display][screensaver_gate]") {
    ScriptedGateClock clock;
    DisplayManager mgr;

    for (int i = 0; i < 4; i++) {
        lv_display_trigger_activity(nullptr);
        mgr.check_display_sleep();
        clock.wall_ns += 300000000ULL;
    }

    CHECK(helix::ScreensaverManagerTestAccess::baseline_samples(clock.savers) == 4);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "the screensaver host reports a print holding the machine and names a missing backend",
    "[application][display][screensaver_gate]") {
    helix::PrinterState& printer_state = get_printer_state();
    helix::PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    const auto drive = [&](const char* wire_state) {
        printer_state.update_from_status(nlohmann::json{{"print_stats", {{"state", wire_state}}}});
        printer_state.print_state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
        process_lvgl(10);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };

    const helix::ui::SaverHost host = DisplayManager::screensaver_host(nullptr);
    CHECK(host.display_backend == "unknown");
    REQUIRE(host.is_printing);

    drive("printing");
    REQUIRE(printer_state.print_state().get_print_lifecycle() == PrintState::Printing);
    CHECK(host.is_printing());

    drive("standby");
    REQUIRE(printer_state.print_state().get_print_lifecycle() == PrintState::Idle);
    CHECK_FALSE(host.is_printing());

    helix::PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the screensaver host keys its display path off the backend it is given",
                 "[application][display][screensaver_gate]") {
    FakeKeyBackend fbdev{DisplayBackendType::FBDEV, /*gpu=*/false};
    FakeKeyBackend egl{DisplayBackendType::DRM, /*gpu=*/true};

    const helix::ui::SaverHost software = DisplayManager::screensaver_host(&fbdev);
    CHECK(software.display_backend == "fbdev");
    REQUIRE(software.is_printing != nullptr);

    const helix::ui::SaverHost hardware = DisplayManager::screensaver_host(&egl);
    CHECK(hardware.display_backend == "egl");
}

#endif // HELIX_ENABLE_SCREENSAVER
