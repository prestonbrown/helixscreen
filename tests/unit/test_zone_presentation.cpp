// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_environment_overlay.h"
#include "ui_ams_zone_overview_overlay.h"
#include "ui_update_queue.h"
#include "ui_zone_presentation.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "static_panel_registry.h"

#include <lvgl/src/others/translation/lv_translation.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix::ui;

namespace {

EnvironmentZone passive_lane(int gate, float humidity) {
    EnvironmentZone z;
    z.gates = {gate};
    z.unit_index = 0;
    z.env.humidity_pct = humidity;
    z.env.has_humidity = true;
    return z;
}

/// The overview and detail overlays are process-lifetime singletons whose widgets
/// belong to whichever test screen built them, and XMLTestFixture gives each
/// TEST_CASE a fresh screen. Drop any instance an earlier case left behind so
/// create() runs against this case's screen instead of one already torn down.
void reset_overlay_singletons() {
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

// Lets a test change unit 0's reported temperature after the overlay has already
// read it once, so a later re-fetch has a single deterministic value to catch.
class RereadMock : public AmsBackendMock {
  public:
    void set_unit0_temp(float temp_c) {
        override_temp_c_ = temp_c;
    }

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (override_temp_c_ && !info.units.empty() && info.units[0].environment.has_value()) {
            info.units[0].environment->temperature_c = *override_temp_c_;
        }
        return info;
    }

  private:
    std::optional<float> override_temp_c_;
};

} // namespace

TEST_CASE("A zone's verdict follows its humidity", "[ams][zones][presentation]") {
    CHECK(zone_verdict(passive_lane(0, 25.0f)) == ZoneVerdict::Ok);
    CHECK(zone_verdict(passive_lane(0, 48.0f)) == ZoneVerdict::Marginal);
    CHECK(zone_verdict(passive_lane(0, 62.0f)) == ZoneVerdict::TooHumid);
}

TEST_CASE("The humidity bands meet exactly at their boundaries", "[ams][zones][presentation]") {
    // Both boundaries are inclusive-below, so each band owns its upper value and no
    // reading falls between two bands. Testing only mid-band values leaves a <= that
    // could become < with nothing red.
    CHECK(zone_verdict(passive_lane(0, kZoneHumidityOkMax)) == ZoneVerdict::Ok);
    CHECK(zone_verdict(passive_lane(0, kZoneHumidityOkMax + 0.1f)) == ZoneVerdict::Marginal);
    CHECK(zone_verdict(passive_lane(0, kZoneHumidityMarginalMax)) == ZoneVerdict::Marginal);
    CHECK(zone_verdict(passive_lane(0, kZoneHumidityMarginalMax + 0.1f)) == ZoneVerdict::TooHumid);
}

TEST_CASE("A zone with no humidity sensor has no verdict", "[ams][zones][presentation]") {
    EnvironmentZone z = passive_lane(0, 0.0f);
    z.env.has_humidity = false;
    // Unknown rather than Ok: a box nobody is measuring is not a dry box.
    CHECK(zone_verdict(z) == ZoneVerdict::Unknown);
}

TEST_CASE("A drying zone carries no severity however wet it reads", "[ams][zones][presentation]") {
    EnvironmentZone z = passive_lane(0, 70.0f);
    z.dryer.supported = true;
    z.dryer.active = true;
    REQUIRE(zone_verdict(z) == ZoneVerdict::TooHumid);

    const ZoneStatus status = zone_status(z);
    CHECK(status.kind == ZoneStatusKind::Drying);
    // The row says "Drying"; colouring it for danger would flag the cycle that is
    // already fixing the reading.
    CHECK(status.severity == ZoneVerdict::Ok);
}

TEST_CASE("A passive zone keeps the verdict it cannot act on", "[ams][zones][presentation]") {
    EnvironmentZone z = passive_lane(0, 70.0f);
    REQUIRE_FALSE(z.dryer.supported);

    const ZoneStatus status = zone_status(z);
    CHECK(status.kind == ZoneStatusKind::Passive);
    // The colour is the only signal a watched-but-undriveable box gives.
    CHECK(status.severity == ZoneVerdict::TooHumid);
}

TEST_CASE("A driveable zone reports its own verdict", "[ams][zones][presentation]") {
    EnvironmentZone dry = passive_lane(0, 25.0f);
    dry.dryer.supported = true;
    CHECK(zone_status(dry).kind == ZoneStatusKind::Verdict);
    CHECK(zone_status(dry).severity == ZoneVerdict::Ok);

    EnvironmentZone wet = passive_lane(0, 70.0f);
    wet.dryer.supported = true;
    CHECK(zone_status(wet).severity == ZoneVerdict::TooHumid);

    EnvironmentZone unmeasured = passive_lane(0, 0.0f);
    unmeasured.dryer.supported = true;
    unmeasured.env.has_humidity = false;
    CHECK(zone_status(unmeasured).severity == ZoneVerdict::Unknown);
}

TEST_CASE("A named unit labels its own zone", "[ams][zones][presentation]") {
    EnvironmentZone z;
    z.label = "QuattroBox";
    z.gates = {0, 1, 2, 3};
    z.unit_index = 0;
    CHECK(zone_display_label(z, "Unit", "Slot", "Happy Hare") == "QuattroBox");
}

TEST_CASE("A single-gate zone with no name is a lane", "[ams][zones][presentation]") {
    EnvironmentZone z = passive_lane(4, 31.0f);
    // One-based for the user; gates are zero-based internally.
    CHECK(zone_display_label(z, "Unit", "Slot", "AFC") == "Slot 5");
}

TEST_CASE("An unnamed multi-gate zone falls back to its unit ordinal",
          "[ams][zones][presentation]") {
    EnvironmentZone z;
    z.gates = {0, 1};
    z.unit_index = 1;
    CHECK(zone_display_label(z, "Unit", "Slot", "AFC") == "AFC Unit 2");
}

TEST_CASE("Two unnamed multi-gate zones get labels that tell them apart",
          "[ams][zones][presentation]") {
    // The fallback is only useful if it still distinguishes; a shared label on the
    // screen where you choose between boxes is the failure this guards.
    EnvironmentZone a;
    a.gates = {0, 1};
    a.unit_index = 0;
    EnvironmentZone b = a;
    b.unit_index = 1;
    CHECK(zone_display_label(a, "Unit", "Slot", "AFC") !=
          zone_display_label(b, "Unit", "Slot", "AFC"));
}

TEST_CASE("An unresolved unit index drops the unit number rather than showing it",
          "[ams][zones][presentation]") {
    // Happy Hare can report a representative gate that doesn't resolve to a known
    // unit, leaving unit_index at its -1 default. Neither the old sentinel-plus-one
    // ("Unit 0") nor the raw sentinel ("Unit -1") is a unit number anyone should
    // trust, so the label drops it and keeps just the system type.
    EnvironmentZone z;
    z.gates = {0, 1};
    z.unit_index = -1;
    const std::string label = zone_display_label(z, "Unit", "Slot", "AFC");
    CHECK(label == "AFC");
    CHECK(label.find("-1") == std::string::npos);
    CHECK(label.find("Unit") == std::string::npos);
}

TEST_CASE("Zones spanning units are detected", "[ams][zones][presentation]") {
    CHECK_FALSE(zones_span_units({}));
    CHECK_FALSE(zones_span_units({passive_lane(0, 30.0f)}));

    std::vector<EnvironmentZone> same = {passive_lane(0, 30.0f), passive_lane(1, 30.0f)};
    CHECK_FALSE(zones_span_units(same));

    std::vector<EnvironmentZone> across = same;
    across[1].unit_index = 1;
    CHECK(zones_span_units(across));
}

TEST_CASE("A gate range reads as a range, a single gate as one number",
          "[ams][zones][presentation]") {
    EnvironmentZone span;
    span.gates = {0, 1, 2, 3};
    CHECK(zone_slot_text(span, "Slots", "Slot") == "Slots 1-4");

    EnvironmentZone one;
    one.gates = {4};
    CHECK(zone_slot_text(one, "Slots", "Slot") == "Slot 5");

    EnvironmentZone none;
    CHECK(zone_slot_text(none, "Slots", "Slot").empty());
}

TEST_CASE_METHOD(XMLTestFixture, "Clicking an overview row opens that zone",
                 "[ams][zones][overview]") {
    reset_overlay_singletons();

    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto zones = raw->get_environment_zones(-1);
    REQUIRE(zones.size() == 3);
    get_ams_zone_overview_overlay().show(lv_screen_active(), zones);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* rows = lv_obj_find_by_name(lv_screen_active(), "rows_container");
    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_get_child_count(rows) == 3);

    lv_obj_t* row = lv_obj_get_child(rows, 2);
    REQUIRE(row != nullptr);
    lv_obj_t* button = lv_obj_find_by_name(row, "row_button");
    REQUIRE(button != nullptr);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    auto& detail = get_ams_environment_overlay();
    CHECK(detail.zone_count() == 0);
    CHECK(detail.selected_zone_index() == 0);

    // zone_count()/selected_zone_index() read 0 whether or not the click did anything
    // (a freshly-reset detail overlay starts at exactly those values), so they cannot
    // tell "opened zones[2]" apart from "did nothing". The published title can: a
    // single-slot zone's label is derived from its own slot number, which differs
    // between zones[1] and zones[2], so it pins both that show_zone() ran and which
    // row triggered it.
    //
    // Matched as a prefix, not for equality: the title appends a noun for a zone that
    // reads humidity, and rebuilding that here would assert the composition against
    // itself rather than against which row was clicked.
    REQUIRE(zones[2].gates.size() == 1);
    const std::string opened = zone_display_label(zones[2], lv_tr("Unit"), lv_tr("Slot"),
                                                  raw->get_system_info().type_name);
    const std::string neighbour = zone_display_label(zones[1], lv_tr("Unit"), lv_tr("Slot"),
                                                     raw->get_system_info().type_name);
    REQUIRE(opened != neighbour);
    lv_subject_t* title = lv_xml_get_subject(nullptr, "ams_env_overlay_title_text");
    REQUIRE(title != nullptr);
    const std::string shown = lv_subject_get_string(title);
    CHECK(shown.rfind(opened, 0) == 0);
    CHECK(shown.rfind(neighbour, 0) != 0);

    reset_overlay_singletons();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "Unit headers appear only where the set spans units",
                 "[ams][zones][overview]") {
    reset_overlay_singletons();

    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto& overlay = get_ams_zone_overview_overlay();

    // Every zone on the printer: 3 zones over 2 units, so the first row of each unit
    // carries a header and the rest do not.
    overlay.show(lv_screen_active(), raw->get_environment_zones(-1));
    lv_subject_t* count = lv_xml_get_subject(nullptr, "zone_ov_count");
    REQUIRE(count != nullptr);
    REQUIRE(lv_subject_get_int(count) == 3);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "zone_ov_group_hidden_0")) == 0);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "zone_ov_group_hidden_1")) == 0);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "zone_ov_group_hidden_2")) == 1);

    // One unit's zones only: nothing to group by, so no header on any row.
    overlay.show(lv_screen_active(), raw->get_environment_zones(1));
    REQUIRE(lv_subject_get_int(count) == 2);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "zone_ov_group_hidden_0")) == 1);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "zone_ov_group_hidden_1")) == 1);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singletons();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "A zone overview group header for an unresolved unit drops the number",
                 "[ams][zones][overview]") {
    reset_overlay_singletons();

    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    const std::string type_name = raw->get_system_info().type_name;

    EnvironmentZone resolved;
    resolved.gates = {0, 1};
    resolved.unit_index = 0;

    EnvironmentZone unresolved;
    unresolved.gates = {2, 3};
    unresolved.unit_index = -1;

    // Two zones with different unit_index values (0 and -1) span units, so both
    // start a group and both group headers get built.
    get_ams_zone_overview_overlay().show(lv_screen_active(), {resolved, unresolved});
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* header_for_unresolved = lv_xml_get_subject(nullptr, "zone_ov_group_text_1");
    REQUIRE(header_for_unresolved != nullptr);
    const std::string text = lv_subject_get_string(header_for_unresolved);
    // The system type alone - not "Unit -1", and not the pre-lane_number "Unit 0" a
    // naive +1 would have produced for this sentinel. Equality against the mock's own
    // type_name (which happens to contain the substring "Unit") is the precise check;
    // find("-1") on top guards the sentinel specifically.
    CHECK(text == type_name);
    CHECK(text.find("-1") == std::string::npos);

    reset_overlay_singletons();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "The overview subtitle has no plural to get wrong",
                 "[ams][zones][overview]") {
    reset_overlay_singletons();

    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto zones = raw->get_environment_zones(-1);
    REQUIRE(zones.size() == 3);
    get_ams_zone_overview_overlay().show(lv_screen_active(), zones);
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* subtitle = lv_xml_get_subject(nullptr, "zone_ov_subtitle");
    REQUIRE(subtitle != nullptr);
    const std::string text = lv_subject_get_string(subtitle);
    CHECK(text.find('3') != std::string::npos);
    // The old "%d units - %d zones - %d dryers" needed a plural branch per count,
    // multiplied across every translated language; a labeled form does not.
    CHECK(text.find(" - ") == std::string::npos);

    reset_overlay_singletons();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "Returning to the list re-fetches its zones",
                 "[ams][zones][overview]") {
    reset_overlay_singletons();

    auto backend = std::make_unique<RereadMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("capped");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto zones = raw->get_environment_zones(-1);
    REQUIRE(zones.size() == 2);

    auto& overlay = get_ams_zone_overview_overlay();
    overlay.show(lv_screen_active(), zones);
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* reading0 = lv_xml_get_subject(nullptr, "zone_ov_reading_0");
    REQUIRE(reading0 != nullptr);
    const std::string before = lv_subject_get_string(reading0);
    CHECK(before.find("99") == std::string::npos);

    // Simulate the reading moving while a drilled-into row sat on top of this list,
    // then come back to the top of the stack the way NavigationManager::go_back()
    // would drive it.
    raw->set_unit0_temp(99.0f);
    overlay.on_activate();
    helix::ui::UpdateQueue::instance().drain();

    const std::string after = lv_subject_get_string(reading0);
    CHECK(after != before);
    CHECK(after.find("99") != std::string::npos);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singletons();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A re-fetch on activation does not narrow the shown set",
                 "[ams][zones][overview]") {
    reset_overlay_singletons();

    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto& overlay = get_ams_zone_overview_overlay();
    // Shown with every zone on the printer, not one unit's - the scope on_activate()
    // must preserve.
    overlay.show(lv_screen_active(), raw->get_environment_zones(-1));
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* count = lv_xml_get_subject(nullptr, "zone_ov_count");
    REQUIRE(count != nullptr);
    REQUIRE(lv_subject_get_int(count) == 3);

    overlay.on_activate();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(count) == 3);

    reset_overlay_singletons();
    AmsState::instance().set_backend(nullptr);
}
