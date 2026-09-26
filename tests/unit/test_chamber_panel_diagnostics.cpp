// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_panel_diagnostics.cpp
 * @brief Chamber-heater diagnostics placement in the temp graph overlay
 *        (issue #1290): above the micro/tiny landscape line everything about
 *        the heater lives in the right column's chamber card; portrait and
 *        micro landscape get a one-row strip under the chart instead.
 *
 * Both surfaces are pure declarative XML: the strip is a structural
 * <if cond="printer_has_chamber_heater_diagnostics and temp_graph_mode eq 3
 * and (ui_is_portrait or ui_breakpoint eq 0)"> under the chart, the card's
 * diagnostics block is bind-hidden (the engine's if/else capture is flat, so
 * no <if> can nest inside the card's orientation branch). Inner visibility
 * comes from bind_flag_if (fault OR inhibited banner, fault replaces the
 * strip's info row) and bind_flag_if_eq (element / filter-fan capability),
 * readouts bind the *_text formatter subjects, and the switch and the
 * banner's Reset fire TemperatureService XML callbacks that delegate to the
 * globally-registered TemperatureController: never the api directly.
 *
 * The unit-test display is 800x480 (card path); the geometry cases resize it
 * through ScopedResolution + theme_manager_refresh_layout_constants() to
 * exercise the 480x320 card and the 480x272 strip at their real breakpoints.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "temperature_controller.h"
#include "temperature_service.h"
#include "theme_manager.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterTemperatureState;
using helix::TemperatureController;

namespace {

/// Set an int subject in the XML registry: the name the overlay binds, which
/// may be this fixture's registration or a prior test's; either way it is the
/// live subject the overlay's bindings observe.
lv_subject_t* set_xml_int(const char* name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, value);
    return subject;
}

lv_subject_t* set_xml_string(const char* name, const char* value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    lv_subject_copy_string(subject, value);
    return subject;
}

bool hidden(lv_obj_t* obj) {
    return obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Nominal faulted dragonbreath frame (same shape as
/// test_chamber_diagnostics_subjects.cpp): latched fault, PTC element at
/// 106.2°C, filter fan purging at 100%, pin on.
nlohmann::json faulted_dragonbreath_status() {
    return nlohmann::json::parse(R"({
      "heater_generic dragonbreath": {"temperature": 25.5, "target": 30.0},
      "dragonbreath": {"fault": true, "inhibited": false, "fault_reason": "ptc_overtemp",
        "ptc_temp": 106.2, "fan_percent": 100, "fan_reason": "purge",
        "mode": "off", "source": "device", "lease_owned": false},
      "output_pin dragonbreath_filter": {"value": 1.0}})");
}

/// Drives the fixture display to a geometry and refreshes the derived layout
/// state (ui_breakpoint / ui_is_portrait / token tiers), restoring both on the
/// way out: the same discipline as test_is_portrait_subject.cpp's
/// RestoreDisplayConsts: theme_manager writes into a SHARED XML scope, so a
/// stale tier would decide layout for every later test in this binary.
class ScopedGeometry {
  public:
    ScopedGeometry(int32_t w, int32_t h)
        : disp_(lv_display_get_default()), w0_(lv_display_get_horizontal_resolution(disp_)),
          h0_(lv_display_get_vertical_resolution(disp_)) {
        lv_display_set_resolution(disp_, w, h);
        theme_manager_refresh_layout_constants(disp_);
    }

    ~ScopedGeometry() {
        lv_display_set_resolution(disp_, w0_, h0_);
        theme_manager_refresh_layout_constants(disp_);
    }

    ScopedGeometry(const ScopedGeometry&) = delete;
    ScopedGeometry& operator=(const ScopedGeometry&) = delete;

  private:
    lv_display_t* disp_;
    int32_t w0_;
    int32_t h0_;
};

/// Builds the real temp_graph_overlay.xml with its component dependencies,
/// in the same shape production's xml_registration.cpp uses. temp_graph_mode
/// is a TempGraphOverlay-owned subject in production; the fixture registers a
/// static stand-in (the XML subject registry is process-global and never
/// forgets an entry, so a fixture member would dangle after this test: same
/// reasoning as AdvancedPowerGroupFixture's host-power subject).
class ChamberOverlayFixture : public XMLTestFixture {
  public:
    ChamberOverlayFixture() : XMLTestFixture() {
        // Chamber mode BEFORE build: the strip's outer gate is a structural
        // <if cond="... temp_graph_mode eq 3"> evaluated at view creation.
        mode_subject_ = lv_xml_get_subject(nullptr, "temp_graph_mode");
        if (!mode_subject_) {
            static lv_subject_t mode_subject;
            lv_subject_init_int(&mode_subject, 3); // TempGraphOverlay::Mode::Chamber
            lv_xml_register_subject(nullptr, "temp_graph_mode", &mode_subject);
            mode_subject_ = &mode_subject;
        }
        // A prior test case may have left another mode on the shared static
        // subject: chamber is the default for every section here.
        lv_subject_set_int(mode_subject_, 3);

        REQUIRE(register_component("components/nozzle_icon"));
        REQUIRE(register_component("components/heater_icon"));
        REQUIRE(register_component("components/chamber_fault_banner"));
        REQUIRE(register_component("components/chamber_diagnostics_card"));
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("overlay_panel"));
        // The card's two diagnostics callbacks must exist before the overlay's
        // XML resolves them: same registration TemperatureService performs.
        lv_xml_register_event_cb(nullptr, "on_chamber_fault_reset_clicked",
                                 TemperatureService::on_chamber_fault_reset_clicked);
        lv_xml_register_event_cb(nullptr, "on_chamber_filter_fan_clicked",
                                 TemperatureService::on_chamber_filter_fan_clicked);
        // The overlay's own callbacks (no-ops here; production registers the
        // TempGraphOverlay handlers in xml_registration.cpp).
        lv_xml_register_event_cb(nullptr, "on_temp_graph_preset_clicked", xml_test_noop_event_cb);
        lv_xml_register_event_cb(nullptr, "on_temp_graph_custom_clicked", xml_test_noop_event_cb);
        REQUIRE(register_component("temp_graph_overlay"));
    }

    /// Creates the overlay at the display's CURRENT geometry. Call after any
    /// ScopedGeometry so the structural breakpoint gates evaluate against the
    /// resized screen. Capability gates default to 0 (surfaces not built);
    /// raise them before the build so the structural <if> fires: the
    /// hidden-when-off cases re-set them explicitly (the reactive cond
    /// rebuilds).
    lv_obj_t* build_overlay() {
        lv_subject_set_int(mode_subject_, 3);
        set_xml_int("printer_has_chamber_heater_diagnostics", 1);
        set_xml_int("printer_has_chamber_filter_fan", 1);
        set_xml_int("printer_has_chamber_element_temp", 1);

        overlay_ = create_component("temp_graph_overlay");
        REQUIRE(overlay_ != nullptr);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(overlay_);
        return overlay_;
    }

    lv_obj_t* overlay_ = nullptr;
    lv_subject_t* mode_subject_ = nullptr;

  private:
    static void xml_test_noop_event_cb(lv_event_t* /*e*/) {}
};

} // namespace

// ============================================================================
// Card-path visibility and content (landscape, diagnostics in the card)
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "temp graph overlay chamber card diagnostics visibility and content",
                 "[chamber][panel][xml]") {
    build_overlay();

    SECTION("chamber mode with diagnostics shows banner, reason, switch") {
        set_xml_int("chamber_heater_fault", 1);
        set_xml_int("chamber_heater_inhibited", 0);
        // Raw vendor code stays log-only; the banner binds the translated kind.
        set_xml_string("chamber_heater_fault_reason_text", lv_tr("Heater over-temperature"));
        set_xml_string("chamber_heater_element_temp_text", "106°C");
        set_xml_string("chamber_filter_fan_percent_text", "100%");
        helix::ui::UpdateQueue::instance().drain();

        lv_obj_t* block = lv_obj_find_by_name(overlay_, "chamber_diagnostics_block");
        REQUIRE(block != nullptr);
        CHECK_FALSE(hidden(block));

        lv_obj_t* banner = lv_obj_find_by_name(overlay_, "fault_banner");
        REQUIRE(banner != nullptr);
        CHECK_FALSE(hidden(banner));

        lv_obj_t* reason = lv_obj_find_by_name(overlay_, "fault_reason_label");
        REQUIRE(reason != nullptr);
        std::string reason_text = lv_label_get_text(reason);
        CHECK(reason_text == std::string(lv_tr("Heater over-temperature")));
        CHECK(reason_text.find("ptc_overtemp") == std::string::npos);

        REQUIRE(lv_obj_find_by_name(overlay_, "reset_fault_button") != nullptr);

        lv_obj_t* fan_switch = lv_obj_find_by_name(overlay_, "filter_fan_switch");
        REQUIRE(fan_switch != nullptr);
        CHECK_FALSE(hidden(fan_switch));

        // Readout labels bind the formatter subjects.
        lv_obj_t* element = lv_obj_find_by_name(overlay_, "element_temp_label");
        REQUIRE(element != nullptr);
        CHECK(std::string(lv_label_get_text(element)) == "106°C");
        lv_obj_t* percent = lv_obj_find_by_name(overlay_, "fan_percent_label");
        REQUIRE(percent != nullptr);
        CHECK(std::string(lv_label_get_text(percent)) == "100%");

        // Above the micro/tiny line the under-chart strip is not built at all.
        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);
    }

    SECTION("fault and inhibited both clear hides the banner, keeps the block") {
        set_xml_int("chamber_heater_fault", 1);
        set_xml_int("chamber_heater_inhibited", 1);
        helix::ui::UpdateQueue::instance().drain();

        set_xml_int("chamber_heater_fault", 0);
        set_xml_int("chamber_heater_inhibited", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_block")));
    }

    SECTION("inhibited alone keeps the banner (OR, not fault-only)") {
        set_xml_int("chamber_heater_fault", 0);
        set_xml_int("chamber_heater_inhibited", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
    }

    SECTION("offline banners without Reset; merely faulted keeps Reset") {
        // Merely faulted: banner + reason + Reset present, no offline message.
        set_xml_int("chamber_heater_fault", 1);
        set_xml_int("chamber_heater_inhibited", 0);
        set_xml_int("chamber_heater_offline", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "reset_fault_button")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "offline_label")));

        // Offline with no fault: the banner carries the offline message, and
        // the Reset button is HIDDEN: DRAGONBREATH_RESET clears a latched
        // fault ON the device and cannot reach one that is not answering.
        set_xml_int("chamber_heater_fault", 0);
        set_xml_int("chamber_heater_offline", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "reset_fault_button")));
        lv_obj_t* offline_label = lv_obj_find_by_name(overlay_, "offline_label");
        REQUIRE(offline_label != nullptr);
        CHECK_FALSE(hidden(offline_label));
        CHECK(std::string(lv_label_get_text(offline_label)) ==
              std::string(lv_tr("Heater offline")));
        // The fault-reason text does not show while offline: an unreachable
        // device is not a device reporting a fault.
        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_reason_label")));

        // Back online with nothing faulted: banner drops, Reset returns.
        set_xml_int("chamber_heater_offline", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "reset_fault_button")));
    }

    SECTION("no diagnostics capability hides the block, builds no strip") {
        // Dropping the capability bind-hides the card's diagnostics block;
        // the under-chart strip stays structurally unbuilt at this
        // breakpoint either way.
        set_xml_int("printer_has_chamber_heater_diagnostics", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_block")));
        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);
        CHECK(lv_obj_find_by_name(overlay_, "fault_banner") !=
              nullptr); // bind-hidden, not torn down
    }

    SECTION("non-chamber modes build no strip (structural mode gate)") {
        set_xml_int("temp_graph_mode", 1); // Nozzle
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);

        set_xml_int("temp_graph_mode", 0); // GraphOnly
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);

        // Back to chamber: the reactive cond still leaves the card path in
        // place (the strip belongs to the other breakpoint).
        set_xml_int("temp_graph_mode", 3);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_t* block = lv_obj_find_by_name(overlay_, "chamber_diagnostics_block");
        REQUIRE(block != nullptr);
        CHECK_FALSE(hidden(block));
        CHECK(lv_obj_find_by_name(overlay_, "reset_fault_button") != nullptr);
    }

    SECTION("no filter-fan capability hides the switch and its readout") {
        set_xml_int("printer_has_chamber_filter_fan", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_block")));
        // The switch and the percent label live inside the readout row, so
        // the row's own flag is what carries the capability gate here.
        CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
    }

    // A backend with no element temperature (stock Panda Breath) would otherwise
    // render a row that can only ever read "--".
    SECTION("no element-temp capability hides that readout, keeps the rest") {
        set_xml_int("printer_has_chamber_element_temp", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "element_readout")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_block")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
    }

    // The External badge annotates the heater from the card's header row, so
    // it survives a backend that reports no element temperature.
    SECTION("external control shows with no element readout beside it") {
        set_xml_int("printer_has_chamber_element_temp", 0);
        set_xml_int("chamber_heater_externally_controlled", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "element_readout")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "external_control_badge")));

        set_xml_int("chamber_heater_externally_controlled", 0);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(lv_obj_find_by_name(overlay_, "external_control_badge")));
    }

    SECTION("device-driven fan badges the readout and disables the switch") {
        lv_obj_t* fan_switch = lv_obj_find_by_name(overlay_, "filter_fan_switch");
        REQUIRE(fan_switch != nullptr);
        lv_obj_t* badge = lv_obj_find_by_name(overlay_, "fan_device_badge");
        REQUIRE(badge != nullptr);

        // Our request driving the fan: no badge, switch usable, checked state
        // follows the running subject.
        set_xml_int("chamber_filter_fan_on", 1);
        set_xml_int("chamber_filter_fan_device_driven", 0);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(badge));
        CHECK_FALSE(lv_obj_has_state(fan_switch, LV_STATE_DISABLED));
        CHECK(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));

        // Device's own initiative: badge appears beside the percent, switch
        // disables (a flip here cannot stop a fan the device is running).
        set_xml_int("chamber_filter_fan_on", 0);
        set_xml_int("chamber_filter_fan_device_driven", 1);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(hidden(badge));
        CHECK(lv_obj_has_state(fan_switch, LV_STATE_DISABLED));
        CHECK_FALSE(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));
    }
}

// ============================================================================
// Strip path (micro landscape): one info row, fault replaces it
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture, "micro landscape renders the compact strip under the chart",
                 "[chamber][panel][xml]") {
    ScopedGeometry micro(480, 272);
    build_overlay();

    set_xml_string("chamber_heater_element_temp_text", "106°C");
    set_xml_string("chamber_filter_fan_percent_text", "100%");
    helix::ui::UpdateQueue::instance().drain();

    SECTION("healthy: info row with element, fan percent and switch") {
        lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_diagnostics_card");
        REQUIRE(strip != nullptr);
        CHECK_FALSE(hidden(strip));

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "strip_info_row")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));

        CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(overlay_, "element_temp_label"))) ==
              "106°C");
        CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(overlay_, "fan_percent_label"))) ==
              "100%");
        REQUIRE(lv_obj_find_by_name(overlay_, "filter_fan_switch") != nullptr);

        // The card's diagnostics block is the other breakpoint's surface.
        CHECK(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_block")));
    }

    SECTION("faulted: the banner replaces the info row") {
        set_xml_int("chamber_heater_fault", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "strip_info_row")));
        REQUIRE(lv_obj_find_by_name(overlay_, "reset_fault_button") != nullptr);
    }

    // A backend with no filter-fan pin must not offer a live switch.
    SECTION("no filter-fan capability hides the strip's switch") {
        set_xml_int("printer_has_chamber_filter_fan", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_switch")));
    }

    // "External" must show once at this size: the strip carries the marker,
    // the card's header badge stands down (the card's diagnostics block is
    // merely hidden here, so its badge widget still exists).
    SECTION("external marker shows on the strip, not the card header") {
        set_xml_int("chamber_heater_externally_controlled", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "external_control_badge")));
        lv_obj_t* right_column = lv_obj_find_by_name(overlay_, "chamber_control_strip");
        REQUIRE(right_column != nullptr);
        lv_obj_t* card_badge = lv_obj_find_by_name(right_column, "external_control_badge");
        REQUIRE(card_badge != nullptr);
        CHECK(hidden(card_badge));
    }
}

// ============================================================================
// Geometry: the fit contract at each landscape breakpoint
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture, "chamber card fits the right column at 480x320 faulted",
                 "[chamber][panel][geometry]") {
    ScopedGeometry tiny(480, 320);
    build_overlay();

    // Worst case: banner + element + fan rows all visible.
    set_xml_int("chamber_heater_fault", 1);
    set_xml_int("chamber_heater_inhibited", 1);
    set_xml_int("chamber_heater_offline", 0);
    set_xml_int("chamber_filter_fan_device_driven", 1);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(overlay_);

    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "element_readout")));
    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));

    lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
    REQUIRE(strip != nullptr);
    // chamber_btn_custom is the column's last child: its bottom must not pass
    // the strip's, or the column has overflowed into a scroll.
    lv_obj_t* custom = lv_obj_find_by_name(overlay_, "chamber_btn_custom");
    REQUIRE(custom != nullptr);
    const int32_t content_bottom = lv_obj_get_y(custom) + lv_obj_get_height(custom);
    CHECK(content_bottom <= lv_obj_get_height(strip));

    // The chart keeps (nearly) the whole left column with the diagnostics
    // gone from under it: 230px measured faulted at this size against 97px
    // when the diagnostics card sat under the chart. The floor leaves room
    // for token drift without re-admitting the old layout.
    lv_obj_t* graph = lv_obj_find_by_name(overlay_, "graph_container");
    REQUIRE(graph != nullptr);
    CHECK(lv_obj_get_height(graph) >= 220);

    // Fits means no scroll: the left column's scrollable must have nothing
    // below the fold. A layout regression that squeezes the chart does not
    // necessarily shrink it past the floor above: it overflows instead,
    // which is the failure the fitting rule exists to catch.
    lv_obj_t* left_column = lv_obj_find_by_name(overlay_, "graph_outer_container");
    REQUIRE(left_column != nullptr);
    CHECK(lv_obj_get_scroll_bottom(left_column) == 0);
    CHECK(lv_obj_get_scroll_y(left_column) == 0);
}

// ============================================================================
// Switch and Reset actions delegate to the globally-registered
// TemperatureController
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture, "chamber card controls drive the controller",
                 "[chamber][panel][xml][actions]") {
    build_overlay();

    // The XML callbacks reach the controller through app_globals, exactly as
    // production wires it: register the fixture's controller there.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    TemperatureController controller(state(), &api);
    controller.set_chamber_actions("DRAGONBREATH_RESET", "output_pin dragonbreath_filter", 60.0);
    // execute_gcode gates on klippy state; the subject defaults to SHUTDOWN.
    state().set_klippy_state_sync(helix::KlippyState::READY);
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        &controller);

    set_xml_int("chamber_heater_fault", 1);
    set_xml_int("chamber_heater_inhibited", 0);
    helix::ui::UpdateQueue::instance().drain();

    SECTION("reset button sends the backend reset gcode") {
        lv_obj_t* reset = lv_obj_find_by_name(overlay_, "reset_fault_button");
        REQUIRE(reset != nullptr);

        client.clear_gcode_script_history();
        lv_obj_send_event(reset, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();

        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "DRAGONBREATH_RESET");
    }

    // ui_switch is checkable: a user flip arrives as VALUE_CHANGED, the
    // trigger the XML callback is bound to. Inverting the REQUEST (not the
    // reported running state) is the controller's contract.
    SECTION("filter-fan switch toggles our request, not the device's running state") {
        lv_obj_t* fan_switch = lv_obj_find_by_name(overlay_, "filter_fan_switch");
        REQUIRE(fan_switch != nullptr);

        // Request off (pin 0) -> flip turns our request on.
        set_xml_int("chamber_filter_fan_requested", 0);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=1");

        // Request on -> flip turns it off.
        set_xml_int("chamber_filter_fan_requested", 1);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=0");

        // The running state is the DEVICE's business: a fan the device runs
        // at full speed while our request is still off must not flip the next
        // toggle to VALUE=0: the toggle inverts the request.
        set_xml_int("chamber_filter_fan_on", 1);
        set_xml_int("chamber_filter_fan_requested", 0);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=1");
    }

    // The switch flips ITSELF on tap, but chamber_filter_fan_on only changes
    // when a status frame confirms the new state. A click whose request no
    // frame has confirmed yet (rejected SET_PIN, Klippy not ready, device
    // reporting 0%) must snap the switch back to what the subject says, not
    // leave the tapped position.
    SECTION("filter-fan switch snaps back when the request changes nothing") {
        lv_obj_t* fan_switch = lv_obj_find_by_name(overlay_, "filter_fan_switch");
        REQUIRE(fan_switch != nullptr);

        set_xml_int("chamber_filter_fan_requested", 0);
        set_xml_int("chamber_filter_fan_on", 0);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE_FALSE(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));

        // The tap's optimistic flip: checked, with no confirmation behind it.
        lv_obj_add_state(fan_switch, LV_STATE_CHECKED);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        // Preconditions: the click reached the controller, and no status
        // frame confirmed the flip (nothing here processes the SET_PIN into
        // a chamber_filter_fan_on change).
        REQUIRE(client.gcode_script_history().size() == 1);
        REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "chamber_filter_fan_on")) == 0);
        CHECK_FALSE(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));

        // The same notify follows the truth upward: a confirmed on-state
        // leaves the switch checked.
        set_xml_int("chamber_filter_fan_on", 1);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_send_event(fan_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));
    }

    // Drop the registration so later tests' get_temperature_controller()
    // sees no controller instead of this fixture's soon-destroyed one.
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        std::shared_ptr<TemperatureController>{});
}

// ============================================================================
// Formatter subjects: the parse block writes display-ready strings alongside
// the raw ints (raw ints render bare; XML has no deci/percent formatter).
// ============================================================================

TEST_CASE("diagnostics parse block writes display text subjects", "[chamber][subjects][text]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(faulted_dragonbreath_status());

    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
          std::string(lv_tr("Heater over-temperature")));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "106°C"); // canonical decimal-drop rule: whole degrees at/above 100
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
          "100%");

    // The pin is a request, not the fan: a pin-only delta updates the
    // request, and the running state follows the reported fan speed (100%
    // above) until a diagnostics frame says otherwise.
    ts.update_from_status({{"output_pin dragonbreath_filter", {{"value", 0.0}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_requested_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "106°C");

    // Sub-100 element temp keeps its one decimal; the fan stopping is what
    // flips the running state.
    ts.update_from_status(nlohmann::json::parse(
        R"({"dragonbreath": {"fault": false, "fault_reason": null, "ptc_temp": 39.4,
                            "fan_percent": 0, "fan_reason": "off"}})"));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "39.4°C");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 0);
}
