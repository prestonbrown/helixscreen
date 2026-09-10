// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_environment_overlay.h"
#include "ui_ams_zone_overview_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_zone_presentation.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "static_panel_registry.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using namespace helix::ui;

namespace helix::ui {
// Friend accessor: private model access without prod-header test methods (L088)
struct AmsEnvironmentOverlayTestAccess {
    static int acting_unit_index(const AmsEnvironmentOverlay& o) {
        return o.acting_unit_index();
    }
    static size_t zones_size(const AmsEnvironmentOverlay& o) {
        return o.zones_.size();
    }
};
} // namespace helix::ui

namespace {

std::vector<EnvironmentZone> capped_rig_zones() {
    AmsBackendMock backend;
    backend.set_multi_unit_mode(true);
    backend.set_environment_mode("capped");
    return backend.get_environment_zones();
}

/// The overlay is a process-lifetime singleton whose widgets belong to whichever
/// test screen built it, and XMLTestFixture gives each TEST_CASE a fresh screen.
/// Drop any instance an earlier case left behind so create() runs against this
/// case's screen instead of one already torn down.
void reset_overlay_singleton() {
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Switching zones does not rebuild the detail widgets",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();
    REQUIRE(zones.size() == 2);

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, true);
    // push_overlay() queues its whole body through UpdateQueue; drain before the
    // teardown below or the deferred push runs against an already-unregistered widget.
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* card = lv_obj_find_by_name(lv_screen_active(), "details_card");
    REQUIRE(card != nullptr);
    const uint32_t before = lv_obj_get_child_count(card);

    // The capped rig's two zones report different humidity, so the readout is the
    // observable that says the switch actually re-published.
    lv_subject_t* humidity = lv_xml_get_subject(nullptr, "ams_env_overlay_humidity_text");
    REQUIRE(humidity != nullptr);
    const std::string shown_first = lv_subject_get_string(humidity);

    overlay.select_zone(1);

    CHECK(overlay.selected_zone_index() == 1);
    // Same object, same children: a switch re-publishes, it does not rebuild.
    CHECK(lv_obj_find_by_name(lv_screen_active(), "details_card") == card);
    CHECK(lv_obj_get_child_count(card) == before);
    // Without this the case passes when select_zone() moves the index and publishes
    // nothing, which is the whole behaviour it exists to pin.
    CHECK(std::string(lv_subject_get_string(humidity)) != shown_first);

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "A selector is offered only when asked for",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();
    auto& overlay = get_ams_environment_overlay();

    overlay.show_zone(lv_screen_active(), zones, 0, true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.zone_count() == 2);

    // Drilled in from the overview: one zone, no strip, Back returns to the list.
    overlay.show_zone(lv_screen_active(), {zones[1]}, 0, false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.zone_count() == 0);

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "The temperature ceiling follows the shown zone",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();

    // The two boxes must disagree, or this case cannot fail: a ceiling hardcoded to
    // either value, and one that never re-publishes on selection, both pass against a
    // rig whose boxes share a range.
    REQUIRE(zones.size() >= 2);
    REQUIRE(zones[0].dryer.max_temp_c == Catch::Approx(65.0f));
    REQUIRE(zones[1].dryer.max_temp_c == Catch::Approx(90.0f));

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, true);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* label = lv_obj_find_by_name(lv_screen_active(), "temp_range_label");
    REQUIRE(label != nullptr);

    const std::string first = lv_label_get_text(label);
    CHECK(first.find("35") != std::string::npos);
    CHECK(first.find("65") != std::string::npos);
    // The DryerInfo default, which is what a ceiling read off the wrong box would show.
    CHECK(first.find("70") == std::string::npos);

    overlay.select_zone(1);
    helix::ui::UpdateQueue::instance().drain();

    const std::string second = lv_label_get_text(label);
    CHECK(second.find("40") != std::string::npos);
    CHECK(second.find("90") != std::string::npos);
    CHECK(second.find("65") == std::string::npos);

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "Clicking a tab selects that zone", "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();
    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, true);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.selected_zone_index() == 0);

    lv_obj_t* strip = lv_obj_find_by_name(lv_screen_active(), "zone_tab_strip");
    REQUIRE(strip != nullptr);
    REQUIRE(lv_obj_get_child_count(strip) == 2);

    // Through the widget, not through select_zone(): the index arrives as the XML
    // event_cb's string user_data, and reading it the wrong way is invisible to any
    // test that calls select_zone() itself.
    lv_obj_send_event(lv_obj_get_child(strip, 1), LV_EVENT_CLICKED, nullptr);

    CHECK(overlay.selected_zone_index() == 1);

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "The dryer mirror follows the selected zone",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();

    // AmsState's own subjects (dryer_current_temp_ among them) are separate from this
    // overlay's and separate from XMLTestFixture's per-instance PrinterState, so nothing
    // else in this suite initializes them. Skipping init_subjects() leaves them at
    // lv_subject_t's zero value, whose `type` field is not LV_SUBJECT_TYPE_INT, so
    // lv_subject_set_int() silently no-ops on every write (verified against
    // lib/lvgl/src/core/lv_observer.c). Order matches
    // test_ams_env_overlay_unit_binding.cpp's install_split_humidity_backend(): deinit
    // first (a previous test may have left the singleton half-initialized), install the
    // backend, THEN init_subjects, THEN the initial sync.
    AmsState::instance().deinit_subjects();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("capped");
    REQUIRE(backend->start().success());
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    auto zones = raw->get_environment_zones(-1);
    REQUIRE(zones.size() == 2);
    REQUIRE(zones[0].unit_index != zones[1].unit_index);

    // AmsState's dryer-follow unit defaults to unit 0, and set_dryer_mirror_unit() no-ops
    // when the value does not change, so opening straight on zone 0 would never actually
    // resync against this test's backend. Point it elsewhere first so the first real call
    // below is a genuine transition.
    AmsState::instance().set_dryer_mirror_unit(-1);

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, true);
    helix::ui::UpdateQueue::instance().drain();
    // AmsState has no public getter for the mirrored unit; assert through the dryer
    // subject sync_dryer_from_backend() actually publishes. The capped rig's two
    // dryers differ here (unit 0 is mid-cycle at 55C, unit 1 sits at the DryerInfo
    // default of 0), so the value is the observable that the mirror moved.
    CHECK(lv_subject_get_int(AmsState::instance().get_dryer_current_temp_subject()) == 55);

    overlay.select_zone(1);
    CHECK(lv_subject_get_int(AmsState::instance().get_dryer_current_temp_subject()) == 0);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "Switching tabs retargets the unit the controls act on",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("capped");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto zones = raw->get_environment_zones(-1);
    REQUIRE(zones.size() == 2);
    REQUIRE(zones[0].unit_index != zones[1].unit_index);

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, true);
    overlay.select_zone(1);

    // Whatever Start/Stop Drying, the preset dropdown, and Material Comfort would act on
    // must be unit 1's, not the unit the overlay opened on. Title/temp/humidity going
    // zone-based is not enough: the dryer-control call sites still read this directly.
    CHECK(AmsEnvironmentOverlayTestAccess::acting_unit_index(overlay) == zones[1].unit_index);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A refresh keeps a selector's zone set intact",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("capped");
    AmsState::instance().set_backend(std::move(backend));

    auto zones = AmsState::instance().get_backend()->get_environment_zones(-1);
    REQUIRE(zones.size() == 2);

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, true);
    REQUIRE(overlay.zone_count() == 2);

    // refresh() is what on_activate()'s live-update observers call on every environment
    // subject change, the temperature ticking during a dry among them. A re-derivation
    // that narrows by unit_index_ would collapse the tab strip to one zone right here.
    overlay.refresh();
    CHECK(overlay.zone_count() == 2);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A refresh does not narrow a non-selector cross-unit set",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("capped");
    AmsState::instance().set_backend(std::move(backend));

    auto zones = AmsState::instance().get_backend()->get_environment_zones(-1);
    REQUIRE(zones.size() == 2);
    REQUIRE(zones[0].unit_index != zones[1].unit_index);

    // with_selector = false: drilled-into detail view (Task 7's shape) or a List
    // presentation too large for tabs (Task 8's). zone_count() reads 0 either way once
    // with_selector_ is false, so it can't tell 1 zone from 2 here; check zones_ itself.
    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 0, false);
    REQUIRE(AmsEnvironmentOverlayTestAccess::zones_size(overlay) == 2);

    overlay.refresh();
    CHECK(AmsEnvironmentOverlayTestAccess::zones_size(overlay) == 2);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A one-zone unit opens detail with no selector",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    AmsState::instance().set_backend(std::move(backend));

    auto& overlay = get_ams_environment_overlay();

    // Unit 1 first: two passive lanes, so a selector. Establishing a non-zero count
    // makes the assertion below a transition rather than a value the overlay would
    // report anyway, and the overlay is a singleton that can carry state between tests.
    open_environment_for_unit(1);
    REQUIRE(overlay.zone_count() == 2);

    // Unit 0 of the mixed rig is the single heated enclosure: one zone, no selector.
    open_environment_for_unit(0);

    CHECK(overlay.zone_count() == 0);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "details_card") != nullptr);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A unit whose lanes are separate zones opens the list",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("emu");
    AmsState::instance().set_backend(std::move(backend));

    auto& overlay = get_ams_environment_overlay();

    // Night Owl (unit 1) has 2 passive lanes: uniform and few, so tabs rather than a
    // list. Establishing a non-zero tab count first makes the List assertion below a
    // transition rather than a value the overlay would report anyway.
    open_environment_for_unit(1);
    REQUIRE(overlay.zone_count() == 2);

    // The multi rig's other unit (Box Turtle 1) has 4 lanes, exactly kMaxZoneTabs, so
    // it tabs too - neither of this rig's real units can demonstrate List on its own.
    // Swap in a single unit with one more lane than a tab strip holds.
    auto many_lanes = std::make_unique<AmsBackendMock>(static_cast<int>(kMaxZoneTabs) + 1);
    many_lanes->set_environment_mode("emu");
    AmsState::instance().set_backend(std::move(many_lanes));

    open_environment_for_unit(0);
    // The List route never touches AmsEnvironmentOverlay, so its own tab count is
    // whatever the earlier Tabs call left it at; the observable that List actually
    // ran is the overview overlay receiving every lane.
    lv_subject_t* overview_count = lv_xml_get_subject(nullptr, "zone_ov_count");
    REQUIRE(overview_count != nullptr);
    CHECK(lv_subject_get_int(overview_count) == static_cast<int>(kMaxZoneTabs) + 1);

    helix::ui::UpdateQueue::instance().drain();
    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "The cross-unit affordance leaves the list drillable",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("mixed");
    AmsState::instance().set_backend(std::move(backend));

    // Unit 0's single zone offers the affordance: the printer has zones on unit 1 too.
    open_environment_for_unit(0);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* subtitle = lv_obj_find_by_name(lv_screen_active(), "subtitle_label");
    REQUIRE(subtitle != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(subtitle, LV_OBJ_FLAG_HIDDEN));

    // Through the widget, not a direct call: on_all_zones_clicked is only reachable
    // as an XML event_cb.
    lv_obj_send_event(subtitle, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // Left in the stack underneath, the detail overlay would make a row's own
    // show_zone() call below a duplicate push NavigationManager silently ignores,
    // and the tap would appear to do nothing.
    REQUIRE_FALSE(
        NavigationManager::instance().is_panel_in_stack(get_ams_environment_overlay().get_root()));
    REQUIRE(
        NavigationManager::instance().is_panel_on_top(get_ams_zone_overview_overlay().get_root()));

    lv_obj_t* row_button = lv_obj_find_by_name(lv_screen_active(), "row_button");
    REQUIRE(row_button != nullptr);
    lv_obj_send_event(row_button, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(NavigationManager::instance().is_panel_on_top(get_ams_environment_overlay().get_root()));

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A queued zone says what it is waiting for",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();
    REQUIRE(zones[1].state == ZoneDryingState::Queued);

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 1, true);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* banner = lv_obj_find_by_name(lv_screen_active(), "queued_banner");
    REQUIRE(banner != nullptr);
    CHECK_FALSE(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));

    // The banner names the zone being waited on, not a generic "please wait".
    const std::string text =
        lv_label_get_text(lv_obj_find_by_name(lv_screen_active(), "queued_banner_label"));
    CHECK(text.find(zone_display_label(zones[0], "Unit", "Slot", "")) != std::string::npos);

    // Selecting the running zone puts it away.
    overlay.select_zone(0);
    CHECK(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "A badge scoped to one unit still names the real blocker",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_multi_unit_mode(true);
    backend->set_environment_mode("capped");
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    auto all_zones = raw->get_environment_zones(-1);
    REQUIRE(all_zones.size() == 2);
    REQUIRE(all_zones[1].state == ZoneDryingState::Queued);

    // The badge for unit 1 alone, the way open_environment_for_unit() opens it - not the
    // full-rig set the fixture above hands to show_zone() directly. zones_ therefore never
    // contains unit 0's running zone, so the blocker must be found some other way.
    open_environment_for_unit(all_zones[1].unit_index);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(AmsEnvironmentOverlayTestAccess::zones_size(get_ams_environment_overlay()) == 1);

    const std::string text =
        lv_label_get_text(lv_obj_find_by_name(lv_screen_active(), "queued_banner_label"));
    CHECK(text.find(zone_display_label(all_zones[0], "Unit", "Slot", "")) != std::string::npos);
    // Not the fallback that fires when no blocker is found.
    CHECK(text.find("another zone") == std::string::npos);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "An idle zone shows no queued banner", "[ams][zones][overlay]") {
    reset_overlay_singleton();
    AmsBackendMock backend;
    backend.set_multi_unit_mode(true);
    backend.set_environment_mode("mixed");
    auto zones = backend.get_environment_zones();

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 1, false);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* banner = lv_obj_find_by_name(lv_screen_active(), "queued_banner");
    // Proves the banner exists and is being driven, rather than the lookup silently
    // returning null and the assertion passing for the wrong reason.
    REQUIRE(banner != nullptr);
    CHECK(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "A queued zone shows no progress bar", "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();
    auto& overlay = get_ams_environment_overlay();

    overlay.show_zone(lv_screen_active(), zones, 1, true);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* bar = lv_obj_find_by_name(lv_screen_active(), "drying_progress_bar");
    REQUIRE(bar != nullptr);
    CHECK(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));

    // The active zone still shows it, so the gate is not simply always-hidden.
    overlay.select_zone(0);
    CHECK_FALSE(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "An empty zone set clears a stale queued banner",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto zones = capped_rig_zones();
    REQUIRE(zones[1].state == ZoneDryingState::Queued);

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), zones, 1, true);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* banner = lv_obj_find_by_name(lv_screen_active(), "queued_banner");
    REQUIRE(banner != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));

    // Re-shown with nothing to display, the way a disconnect or a stale caller might -
    // the banner must not go on naming a zone that is no longer there.
    overlay.show_zone(lv_screen_active(), {}, 0, false);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));
    lv_subject_t* banner_text = lv_xml_get_subject(nullptr, "env_queued_banner_text");
    REQUIRE(banner_text != nullptr);
    CHECK(std::string(lv_subject_get_string(banner_text)).empty());

    reset_overlay_singleton();
}

TEST_CASE_METHOD(XMLTestFixture, "A zone with no attributable unit refuses to start drying",
                 "[ams][zones][overlay]") {
    reset_overlay_singleton();
    auto backend = std::make_unique<AmsBackendMock>();
    backend->set_dryer_enabled(true);
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));

    // A zone whose gates fall outside every known unit's slot range (HappyHare can
    // report this for a stale gate_count mid-reconfiguration): dryer-capable, but with
    // no single unit to attribute the command to.
    EnvironmentZone indeterminate;
    indeterminate.id = "unattributable";
    indeterminate.unit_index = -1;
    indeterminate.dryer.supported = true;
    indeterminate.dryer.min_temp_c = 35.0f;
    indeterminate.dryer.max_temp_c = 65.0f;
    indeterminate.dryer.max_duration_min = 720;

    auto& overlay = get_ams_environment_overlay();
    overlay.show_zone(lv_screen_active(), {indeterminate}, 0, false);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(raw->get_dryer_info(0).active);

    lv_obj_t* btn = lv_obj_find_by_name(lv_screen_active(), "btn_start_stop");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);

    // Not commanded: heating an unrelated box would be worse than doing nothing.
    CHECK_FALSE(raw->get_dryer_info(0).active);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}
