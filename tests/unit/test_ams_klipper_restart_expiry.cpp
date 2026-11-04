// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A Klipper restart ends every firmware-side filament operation, so a backend
// still reporting one busy when Klipper comes back ready would refuse every
// tool change for nothing. AmsState ends it once, for every backend, on the
// klippy_state transition into READY.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/afc_test_access.h"
#include "../test_helpers/cfs_test_access.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "printer_state.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsAction;
using helix::AmsState;
using helix::KlippyState;
using json = nlohmann::json;

namespace {

class RestartRecordingBackend : public helix::AmsBackendMock {
  public:
    RestartRecordingBackend() : helix::AmsBackendMock(4) {}

    int restarts = 0;
    void on_klipper_restarted() override {
        ++restarts;
    }
};

class KlipperRestartFixture : public LVGLTestFixture {
  public:
    KlipperRestartFixture() {
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        AmsState::instance().init_subjects(true);
    }

    ~KlipperRestartFixture() override {
        AmsState::instance().set_backend(nullptr);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void restart_klipper() {
        get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        settle();
    }
};

json u1_feed_status(int extruder_idx, const std::string& channel_state) {
    const char* feed_key = (extruder_idx <= 1) ? "filament_feed left" : "filament_feed right";
    return json{{feed_key, json{{"extruder" + std::to_string(extruder_idx),
                                 json{{"filament_detected", true},
                                      {"channel_state", channel_state},
                                      {"channel_error", "ok"}}}}}};
}

} // namespace

TEST_CASE_METHOD(KlipperRestartFixture, "Klipper coming back ready ends the backends' operations",
                 "[ams][klipper_restart_expiry]") {
    auto owned = std::make_unique<RestartRecordingBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    SECTION("a restart: STARTUP, then READY") {
        restart_klipper();
        CHECK(backend->restarts == 1);
    }
    SECTION("a shutdown recovered to READY") {
        get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        settle();
        CHECK(backend->restarts == 1);
    }
    SECTION("Klipper still starting ends nothing yet") {
        get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
        settle();
        CHECK(backend->restarts == 0);
    }
    SECTION("READY repeated without leaving it ends nothing") {
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        settle();
        CHECK(backend->restarts == 0);
    }
}

TEST_CASE_METHOD(KlipperRestartFixture, "The first READY after start is a connect, not a restart",
                 "[ams][klipper_restart_expiry]") {
    // klippy_state starts at SHUTDOWN, so the first READY of a process (or of
    // a printer switched to) is a SHUTDOWN->READY edge too. Backends can exist
    // by then, and the firmware may be mid-operation.
    get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    AmsState::instance().init_subjects(true); // re-installs the observer, as a session start does
    settle();

    auto owned = std::make_unique<RestartRecordingBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    get_printer_state().set_klippy_state_sync(KlippyState::READY);
    settle();
    CHECK(backend->restarts == 0);

    restart_klipper();
    CHECK(backend->restarts == 1);
}

TEST_CASE_METHOD(KlipperRestartFixture, "A deinit and re-init leave exactly one restart observer",
                 "[ams][klipper_restart_expiry]") {
    // The printer-switch cycle. Klipper is READY and the observer has seen it.
    AmsState::instance().deinit_subjects();

    // Deinitialized: a restart edge reaches nothing.
    auto first = std::make_unique<RestartRecordingBackend>();
    auto* stale = first.get();
    AmsState::instance().set_backend(std::move(first));
    restart_klipper();
    CHECK(stale->restarts == 0);

    AmsState::instance().init_subjects(true);
    settle();
    auto second = std::make_unique<RestartRecordingBackend>();
    auto* backend = second.get();
    AmsState::instance().set_backend(std::move(second));

    restart_klipper();
    CHECK(backend->restarts == 1);
}

TEST_CASE_METHOD(KlipperRestartFixture, "A U1 operation latched busy ends with a Klipper restart",
                 "[ams][snapmaker][klipper_restart_expiry]") {
    auto owned = std::make_unique<helix::AmsBackendSnapmaker>(nullptr, nullptr);
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    helix::SnapmakerTestAccess::handle_status(*backend, u1_feed_status(1, "load_feeding"));
    REQUIRE(helix::ams_action_is_busy(backend->get_current_action()));

    restart_klipper();

    CHECK(backend->get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(KlipperRestartFixture, "AFC ends a busy operation on a Klipper restart",
                 "[ams][afc][klipper_restart_expiry]") {
    helix::AmsBackendAfc backend(nullptr, nullptr);
    helix::AfcTestAccess::system_info(backend).action = AmsAction::LOADING;

    backend.on_klipper_restarted();

    CHECK(backend.get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(KlipperRestartFixture,
                 "AD5X IFS ends its operation and phase tracking on a restart",
                 "[ams][ad5x][klipper_restart_expiry]") {
    helix::AmsBackendAd5xIfs backend(nullptr, nullptr);
    helix::Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(helix::Ad5xIfsTestAccess::phase_active(backend));

    backend.on_klipper_restarted();

    CHECK(helix::Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    CHECK_FALSE(helix::Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(KlipperRestartFixture, "CFS ends its operation and phase tracking on a restart",
                 "[ams][cfs][klipper_restart_expiry]") {
    helix::printer::AmsBackendCfs backend(nullptr, nullptr);
    helix::CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    REQUIRE(helix::CfsTestAccess::phase_active(backend));

    backend.on_klipper_restarted();

    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(helix::CfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(KlipperRestartFixture, "AFC drops its optimistic dispatch on a Klipper restart",
                 "[ams][afc][klipper_restart_expiry]") {
    helix::AmsBackendAfc backend(nullptr, nullptr);
    helix::AfcTestAccess::system_info(backend).action = AmsAction::SELECTING;
    helix::AfcTestAccess::pending_dispatch_action(backend) = AmsAction::SELECTING;

    backend.on_klipper_restarted();

    CHECK_FALSE(helix::AfcTestAccess::pending_dispatch_action(backend).has_value());
    CHECK(backend.get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(KlipperRestartFixture,
                 "ToolChanger drops its optimistic dispatch on a Klipper restart",
                 "[ams][toolchanger][klipper_restart_expiry]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    helix::ToolChangerTestAccess::arm_dispatch(backend, AmsAction::SELECTING);
    REQUIRE(helix::ToolChangerTestAccess::has_pending_dispatch(backend));

    backend.on_klipper_restarted();

    CHECK_FALSE(helix::ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(backend.get_current_action() == AmsAction::IDLE);
}
