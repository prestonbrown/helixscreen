// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_override_rebind.cpp
 * @brief External re-bind clears our override; eject honors the retention
 * setting (#1281). Firmware truth must win back a lane another writer re-bound.
 *
 * merge_override()'s rule matrix (test_filament_slot_override_store.cpp) pins
 * the pure function. This file pins the WIRING: AmsBackendAfc's live status
 * path must consult it, drop the in-memory record on an external re-bind, and
 * gate the eject clear on the keep-spool-info setting.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/cfs_test_access.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AfcRebindHelper : public AmsBackendAfc {
  public:
    AfcRebindHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
    }

    void set_override(int slot_index, const helix::ams::FilamentSlotOverride& o) {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_[slot_index] = o;
    }

    /// Drive the live status path, so assertions see what the UI would paint.
    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    [[nodiscard]] int visible_spool_id(int slot_index) const {
        return get_slot_info(slot_index).spoolman_id;
    }

    [[nodiscard]] std::string visible_brand(int slot_index) const {
        return get_slot_info(slot_index).brand;
    }

    [[nodiscard]] bool has_override(int slot_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return overrides_.count(slot_index) > 0;
    }

    /// Drive AmsBackend::record_own_spool_write exactly the way
    /// set_slot_info's SET_SPOOL_ID path does: under mutex_, with the
    /// firmware-reported id captured before the mirror was updated.
    void record_own_write(int slot_index, int new_id, int previous_firmware_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        record_own_spool_write(slot_index, new_id, previous_firmware_id);
    }

    /// Consult AmsBackend::own_write_expectation under mutex_ (as
    /// apply_overrides does) to observe/consume the pending expectation.
    [[nodiscard]] std::pair<int, int> peek_expectation(int slot_index, int firmware_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return own_write_expectation(slot_index, firmware_id);
    }
};

namespace {

helix::ams::FilamentSlotOverride spool_override(int spoolman_id) {
    helix::ams::FilamentSlotOverride o;
    o.spoolman_id = spoolman_id;
    o.brand = "Polymaker";
    o.material = "PLA";
    return o;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AFC external re-bind clears our override (#1281 step 7)",
                 "[ams][afc][override-merge]") {
    // Post-change apply_overrides() reads the retention setting; give the
    // settings singleton the production-default world before any merge runs.
    SettingsManager::instance().init_subjects();

    AfcRebindHelper afc;
    afc.set_override(0, spool_override(42));
    // Firmware (via Mainsail/AFC macro) now reports a DIFFERENT spool:
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 169}});
    CHECK(afc.visible_spool_id(0) == 169); // firmware truth paints
    CHECK(afc.visible_brand(0).empty());   // our stale brand no longer shadows
    CHECK_FALSE(afc.has_override(0));      // record dropped
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC eject retains by default, clears with setting off (#1281)",
                 "[ams][afc][override-merge]") {
    auto& settings = SettingsManager::instance();
    settings.init_subjects();

    settings.set_ams_keep_spool_info_on_eject(true);
    AfcRebindHelper afc;
    afc.set_override(0, spool_override(42));
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 42}}); // firmware echoes our id
    CHECK(afc.visible_spool_id(0) == 42);
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", nullptr}}); // eject: spool_id=None
    CHECK(afc.visible_spool_id(0) == 42);                             // designed retention
    CHECK(afc.has_override(0));

    settings.set_ams_keep_spool_info_on_eject(false);
    afc.set_override(1, spool_override(7));
    afc.feed_stepper("lane2", nlohmann::json{{"spool_id", 7}});
    afc.feed_stepper("lane2", nlohmann::json{{"spool_id", nullptr}});
    CHECK(afc.visible_spool_id(1) == 0); // start fresh
    CHECK_FALSE(afc.has_override(1));
    settings.set_ams_keep_spool_info_on_eject(true);
}

TEST_CASE_METHOD(LVGLTestFixture, "Non-id backends: eject rule inert, field merge intact",
                 "[ams][cfs][override-merge]") {
    // CFS keeps firmware spoolman_id at 0 (it never reports one) yet carries a
    // real override. Even with retention OFF nothing clears — and the CFS
    // EMPTY->AVAILABLE promotion tail must keep working. Fixture mirrors the
    // "override loaded at init" test in test_ams_backend_cfs.cpp.
    auto& settings = SettingsManager::instance();
    settings.init_subjects();
    settings.set_ams_keep_spool_info_on_eject(false);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::printer::AmsBackendCfs backend(&api, nullptr);
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker"; // a real user assignment — no id, no locks
    ovr.material = "PLA";
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Every bay reads EMPTY: untagged 3rd-party spools report no vender and no
    // remaining length, and CFS firmware never reports a spool id (0).
    const nlohmann::json box = nlohmann::json::parse(R"({
        "state": "connect", "filament": 1, "auto_refill": 1, "enable": 1,
        "filament_useup": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect", "filament": "None", "temperature": "27",
            "dry_and_humidity": "40", "version": "1.1.3", "sn": "SERIAL",
            "mode": "0",
            "vender": ["none", "none", "none", "none"],
            "remain_len": ["-1", "-1", "-1", "-1"],
            "color_value": ["-1", "-1", "-1", "-1"],
            "material_type": ["-1", "-1", "-1", "-1"],
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
    CfsTestAccess::handle_status(
        backend,
        nlohmann::json{{"params", nlohmann::json::array({nlohmann::json{{"box", box}}, 0})}});

    const auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");            // override still paints
    CHECK(info.spoolman_id == 0);                // firmware id 0 — nothing cleared it
    CHECK(info.status == SlotStatus::AVAILABLE); // CFS tail promoted EMPTY->AVAILABLE
    // The in-memory override entry survived the merge.
    const auto survived = CfsTestAccess::get_override(backend, 0);
    REQUIRE(survived.has_value());
    CHECK(survived->brand == "Polymaker");

    settings.set_ams_keep_spool_info_on_eject(true);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC own re-link survives the echo race (own-write expectation)",
                 "[ams][afc][override-merge]") {
    // F2 regression: HelixScreen itself re-links a lane (42 -> 169 via
    // SET_SPOOL_ID); in-flight status frames still report the old firmware
    // id 42, and Rule 1 must not read that stale frame as an external
    // re-bind and destroy the just-saved override. Mirrors
    // SlotFingerprintTracker::expect() semantics.
    SettingsManager::instance().init_subjects();

    AfcRebindHelper afc;
    // The editor path: stage the override, then record the write the way
    // set_slot_info does when it emits SET_SPOOL_ID (previous id = what
    // firmware last reported).
    afc.set_override(0, spool_override(169));
    afc.record_own_write(0, 169, 42);

    // In-flight stale frame: firmware still reports the OLD id.
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 42}});
    CHECK(afc.has_override(0));            // not destroyed by our own write
    CHECK(afc.visible_spool_id(0) == 169); // override paints normally
    CHECK(afc.visible_brand(0) == "Polymaker");

    // The echo lands: firmware reports the NEW id. The expectation is
    // consumed (single-shot, like the fingerprint tracker's).
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 169}});
    CHECK(afc.has_override(0));
    CHECK(afc.visible_spool_id(0) == 169);
    CHECK(afc.peek_expectation(0, 169) == std::make_pair(0, 0)); // echo consumed it

    // A genuine external change to a third id ends the expectation: Rule 1
    // fires again and firmware truth wins back the lane.
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 200}});
    CHECK_FALSE(afc.has_override(0));
    CHECK(afc.visible_spool_id(0) == 200);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "own-write expectation: chained re-link keeps the original previous id",
                 "[ams][afc][override-merge]") {
    SettingsManager::instance().init_subjects();

    AfcRebindHelper afc;
    // 42 -> 169, then a second write before the echo landed: 169 -> 180.
    // The stored pair must keep the ORIGINAL previous id (42) so stale
    // frames reporting 42 stay suppressed.
    afc.record_own_write(0, 169, 42);
    afc.record_own_write(0, 180, 169);
    const auto chained = afc.peek_expectation(0, 42);
    CHECK(chained.first == 42);
    CHECK(chained.second == 180);

    // A record whose previous matches neither stored id (firmware truly
    // moved elsewhere in between) replaces the pair wholesale.
    afc.record_own_write(0, 300, 55);
    const auto replaced = afc.peek_expectation(0, 55);
    CHECK(replaced.first == 55);
    CHECK(replaced.second == 300);

    // An id-less write (an unlink) drops the expectation entirely — nothing
    // is left to echo, and Rule 1 cannot fire on firmware id 0 anyway.
    afc.record_own_write(0, 0, 300);
    CHECK(afc.peek_expectation(0, 55) == std::make_pair(0, 0));
}
