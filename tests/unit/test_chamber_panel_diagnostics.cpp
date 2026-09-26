// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_panel_diagnostics.cpp
 * @brief Chamber-heater diagnostics placement in the temp graph overlay
 *        (issue #1290): everything about the heater lives in the chamber
 *        card at every size - the landscape right column (all breakpoints,
 *        compacted at micro 480x272) and the portrait full-width band under
 *        the chart. Nothing renders under the chart itself.
 *
 * The card is pure declarative XML: the diagnostics block is bind-hidden
 * (the engine's if/else capture is flat, so no <if> can nest inside the
 * card's orientation branch). Inner visibility comes from bind_flag_if
 * (fault OR inhibited banner; portrait swaps the readout row for it) and
 * bind_flag_if_eq (element / filter-fan capability), readouts bind the
 * *_text formatter subjects, and the switch and the banner's Reset fire
 * TemperatureService XML callbacks that delegate to the globally-registered
 * TemperatureController: never the api directly.
 *
 * The unit-test display is 800x480; the geometry cases resize it through
 * ScopedResolution + theme_manager_refresh_layout_constants() to exercise
 * 480x320, micro landscape 480x272 and portrait 272x480 at their real
 * breakpoints.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "lvgl/src/widgets/label/lv_label_private.h" // lv_label_t::dot_begin: the ellipsization signal
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "temperature_controller.h"
#include "temperature_service.h"
#include "theme_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <lvgl.h>
#include <string>
#include <utility>

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

/// Absolute right edge in display coordinates, so a widget nested any depth
/// inside the card compares against the card itself rather than against its
/// own parent's content area.
int32_t abs_x2(lv_obj_t* obj) {
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    return coords.x2;
}

/// Absolute bottom edge. lv_obj_get_y() reads the STYLE position, which is 0
/// for flex-placed children: the flex offset lives only in the computed
/// coords, so a y+h<=height comparison against the parent never sees a
/// flex overflow.
int32_t abs_y2(lv_obj_t* obj) {
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    return coords.y2;
}

/// The widest data the chamber card renders: 2-digit current and target
/// ("37.7 / 60°C"), heating glyph with "100%", element at "106.2°C", fan at
/// "100%", fault banner carrying its reason, External marker shown.
void set_worst_case_chamber_data() {
    set_xml_int("chamber_temp", 377);
    set_xml_int("chamber_effective_target", 600);
    set_xml_int("chamber_status_state", 1);
    set_xml_string("chamber_status", "100%");
    set_xml_int("chamber_heater_externally_controlled", 1);
    set_xml_int("chamber_heater_fault", 1);
    set_xml_int("chamber_heater_inhibited", 0);
    set_xml_int("chamber_heater_offline", 0);
    set_xml_string("chamber_heater_fault_reason_text", lv_tr("Heater over-temperature"));
    set_xml_string("chamber_heater_element_temp_text", "106.2°C");
    set_xml_string("chamber_filter_fan_percent_text", "100%");
    set_xml_int("chamber_filter_fan_device_driven", 0);
}

/// Single-line natural width of a label's current text in the label's own
/// font, for labels that clip rather than ellipsize.
int32_t label_text_width(lv_obj_t* label) {
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    lv_point_t text_size{};
    lv_text_get_size(&text_size, lv_label_get_text(label), font, letter_space, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return text_size.x;
}

/// lv_label's "unset" marker for dot_begin (LV_LABEL_DOT_BEGIN_INV lives in
/// lv_label.c, unreached from here).
constexpr uint32_t kLabelDotBeginNone = 0xFFFFFFFF;

/// Every visible label in the subtree must render its whole text. Dots-mode
/// labels rewrite their own text in place when they ellipsize
/// (lv_label_set_dots), so lv_label_get_text() returns the SHORTENED string
/// and a width comparison against it is self-referential: it measures the
/// ellipsized text against the width that produced it. dot_begin is the
/// engine's own record of whether that rewrite ever fired, and is the only
/// honest signal. Clip-mode labels have no such marker, so they keep the
/// width comparison. The fault banner's reason label is the one designed
/// ellipsis (vendor reasons of unbounded length share a row with the Reset
/// action), so its subtree is skipped by name. Hidden subtrees are skipped
/// too: they take no layout, so their widths are stale.
void check_no_label_truncated(lv_obj_t* root) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        const char* child_name = lv_obj_get_name(child);
        if (child_name != nullptr && strcmp(child_name, "fault_banner") == 0)
            continue;
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            continue;
        check_no_label_truncated(child);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const lv_label_long_mode_t mode = lv_label_get_long_mode(child);
            if (mode == LV_LABEL_LONG_MODE_DOTS) {
                const lv_label_t* label = (const lv_label_t*)child;
                CAPTURE(child_name);
                CHECK(label->dot_begin == kLabelDotBeginNone);
            } else if (mode == LV_LABEL_LONG_MODE_CLIP) {
                const int32_t content_width = lv_obj_get_width(child) -
                                              lv_obj_get_style_pad_left(child, LV_PART_MAIN) -
                                              lv_obj_get_style_pad_right(child, LV_PART_MAIN);
                CAPTURE(child_name);
                CHECK(label_text_width(child) <= content_width);
            }
        }
    }
}

/// Every visible widget in the card must end inside the card's content area.
/// A row whose natural width exceeds the column (a third text beside the
/// readout and the status) overflows instead of shrinking: LVGL keeps the
/// children's sizes and lets the last ones run past the edge, which no
/// truncation check sees because the labels themselves never ellipsize.
/// Hidden subtrees are skipped: they take no layout, so their coordinates
/// are stale.
void check_no_descendant_past_card_content(lv_obj_t* node, int32_t content_right) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t* child = lv_obj_get_child(node, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            continue;
        check_no_descendant_past_card_content(child, content_right);
        CAPTURE(lv_obj_get_name(child));
        CHECK(abs_x2(child) <= content_right);
    }
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
        // Chamber mode BEFORE build: the chamber strip's hidden bind and the
        // graph column's width bind both observe it, and the orientation
        // branch is structural, evaluated at view creation.
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

        // TemperatureService owns the heater duty/status-state subjects in
        // production; this fixture never runs it, so the card's heater_status
        // bindings need static stand-ins (registered BEFORE the overlay is
        // built, the same stand-in discipline as temp_graph_mode above).
        if (!lv_xml_get_subject(nullptr, "chamber_status")) {
            static lv_subject_t chamber_status;
            static char chamber_status_buf[32];
            lv_subject_init_string(&chamber_status, chamber_status_buf, nullptr,
                                   sizeof(chamber_status_buf), "");
            lv_xml_register_subject(nullptr, "chamber_status", &chamber_status);
        }
        if (!lv_xml_get_subject(nullptr, "chamber_status_state")) {
            static lv_subject_t chamber_status_state;
            lv_subject_init_int(&chamber_status_state, 0);
            lv_xml_register_subject(nullptr, "chamber_status_state", &chamber_status_state);
        }

        REQUIRE(register_component("components/nozzle_icon"));
        REQUIRE(register_component("components/heater_icon"));
        REQUIRE(register_component("components/chamber_fault_banner"));
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
    /// ScopedGeometry so the structural orientation branch evaluates against
    /// the resized screen. Capability gates default to 0 (surfaces not
    /// built); raise them before the build so the bind-hidden block shows:
    /// the hidden-when-off cases re-set them explicitly.
    lv_obj_t* build_overlay() {
        lv_subject_set_int(mode_subject_, 3);
        set_xml_int("printer_has_chamber_heater", 1);
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

        // Nothing about the heater renders under the chart: the diagnostics
        // widgets exist in the card but not inside the graph column.
        lv_obj_t* graph_outer = lv_obj_find_by_name(overlay_, "graph_outer_container");
        REQUIRE(graph_outer != nullptr);
        CHECK(lv_obj_find_by_name(graph_outer, "filter_fan_switch") == nullptr);
        CHECK(lv_obj_find_by_name(graph_outer, "element_temp_label") == nullptr);
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

    SECTION("no diagnostics capability hides the block, keeps the card") {
        // Dropping the capability bind-hides the card's diagnostics block
        // (banner, readouts, switch); the display header row stays.
        set_xml_int("printer_has_chamber_heater_diagnostics", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_block")));
        CHECK(lv_obj_find_by_name(overlay_, "fault_banner") !=
              nullptr); // bind-hidden, not torn down
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_temp_display")));
    }

    SECTION("non-chamber modes hide the chamber strip (mode bind)") {
        lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
        REQUIRE(strip != nullptr);

        set_xml_int("temp_graph_mode", 1); // Nozzle
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(strip));

        set_xml_int("temp_graph_mode", 0); // GraphOnly
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(strip));

        // Back to chamber: the strip returns with the card's diagnostics
        // block still in place.
        set_xml_int("temp_graph_mode", 3);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(hidden(strip));
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

    // The External badge annotates the heater from inside the card, so it
    // survives a backend that reports no element temperature.
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
        lv_obj_t* percent = lv_obj_find_by_name(overlay_, "fan_percent_label");
        REQUIRE(percent != nullptr);

        // Our request driving the fan: percent shows, no badge, switch
        // usable, checked state follows the running subject.
        set_xml_int("chamber_filter_fan_on", 1);
        set_xml_int("chamber_filter_fan_device_driven", 0);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(hidden(percent));
        CHECK(hidden(badge));
        CHECK_FALSE(lv_obj_has_state(fan_switch, LV_STATE_DISABLED));
        CHECK(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));

        // Device's own initiative: the badge REPLACES the percent (the
        // number describes our pin request, which the device is overriding)
        // and the switch disables (a flip here cannot stop a fan the device
        // is running).
        set_xml_int("chamber_filter_fan_on", 0);
        set_xml_int("chamber_filter_fan_device_driven", 1);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(percent));
        CHECK_FALSE(hidden(badge));
        CHECK(lv_obj_has_state(fan_switch, LV_STATE_DISABLED));
        CHECK_FALSE(lv_obj_has_state(fan_switch, LV_STATE_CHECKED));
    }
}

// ============================================================================
// Micro landscape (480x272): the same card, compacted, still everything
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "micro landscape renders the compact card, nothing under the chart",
                 "[chamber][panel][xml]") {
    ScopedGeometry micro(480, 272);
    build_overlay();

    set_xml_string("chamber_heater_element_temp_text", "106°C");
    set_xml_string("chamber_filter_fan_percent_text", "100%");
    helix::ui::UpdateQueue::instance().drain();

    SECTION("healthy: compact card with element, fan percent and switch") {
        lv_obj_t* block = lv_obj_find_by_name(overlay_, "chamber_diagnostics_block");
        REQUIRE(block != nullptr);
        CHECK_FALSE(hidden(block));

        CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(overlay_, "element_temp_label"))) ==
              "106°C");
        CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(overlay_, "fan_percent_label"))) ==
              "100%");
        REQUIRE(lv_obj_find_by_name(overlay_, "filter_fan_switch") != nullptr);
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));

        // The compact sm temp variant shows in the header row; the md one
        // stands down (fonts are creation-time, so the swap is bind-hidden
        // widgets). No standalone row variant exists at any size.
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_temp_display_compact")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "chamber_temp_display")));
        CHECK(lv_obj_find_by_name(overlay_, "chamber_temp_display_row") == nullptr);

        // Nothing under the chart at this size either.
        lv_obj_t* graph_outer = lv_obj_find_by_name(overlay_, "graph_outer_container");
        REQUIRE(graph_outer != nullptr);
        CHECK(lv_obj_find_by_name(graph_outer, "filter_fan_switch") == nullptr);
    }

    SECTION("faulted: the banner shows inside the card") {
        set_xml_int("chamber_heater_fault", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        REQUIRE(lv_obj_find_by_name(overlay_, "reset_fault_button") != nullptr);
    }

    // A backend with no filter-fan pin must not offer a live switch.
    SECTION("no filter-fan capability hides the fan readout") {
        set_xml_int("printer_has_chamber_filter_fan", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
    }

    // The External marker stays inside the card at this size too, not on a
    // second surface.
    SECTION("external marker shows inside the card") {
        set_xml_int("chamber_heater_externally_controlled", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "external_control_badge")));
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
    CHECK(abs_y2(custom) <= abs_y2(strip));

    // The chart keeps (nearly) the whole left column: nothing about the
    // heater renders under it at any size. 230px measured faulted at this
    // size. The floor leaves room for token drift without re-admitting a
    // compact-under-chart fallback.
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

    // Device-driven (set above): the badge replaces the percent, so the
    // switch cannot be pushed past the card's right edge.
    lv_obj_t* card = lv_obj_find_by_name(overlay_, "chamber_display_card");
    REQUIRE(card != nullptr);
    lv_obj_t* fan_switch = lv_obj_find_by_name(overlay_, "filter_fan_switch");
    REQUIRE(fan_switch != nullptr);
    CHECK(lv_obj_get_x2(fan_switch) <= lv_obj_get_x2(card));
    CHECK(hidden(lv_obj_find_by_name(overlay_, "fan_percent_label")));
}

TEST_CASE_METHOD(ChamberOverlayFixture, "micro landscape fits the faulted card with zero scroll",
                 "[chamber][panel][geometry]") {
    ScopedGeometry micro(480, 272);
    build_overlay();

    // Worst case at the tightest landscape target: banner + element + fan
    // rows all visible in the compacted card. Every subject is stated:
    // they are process-global, so an unset one reads whatever an earlier
    // case left behind.
    set_xml_int("chamber_heater_fault", 1);
    set_xml_int("chamber_heater_inhibited", 1);
    set_xml_int("chamber_heater_offline", 0);
    set_xml_int("chamber_filter_fan_device_driven", 1);
    set_xml_int("chamber_heater_externally_controlled", 1);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(overlay_);

    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "element_readout")));
    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));

    lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
    REQUIRE(strip != nullptr);
    // chamber_btn_custom is the column's last child: its bottom must not
    // pass the strip's, or the column has overflowed.
    lv_obj_t* custom = lv_obj_find_by_name(overlay_, "chamber_btn_custom");
    REQUIRE(custom != nullptr);
    CHECK(abs_y2(custom) <= abs_y2(strip));

    lv_obj_t* left_column = lv_obj_find_by_name(overlay_, "graph_outer_container");
    REQUIRE(left_column != nullptr);
    CHECK(lv_obj_get_scroll_bottom(left_column) == 0);
    CHECK(lv_obj_get_scroll_y(left_column) == 0);

    // The switch stays inside the compacted card with the badge in place of
    // the percent.
    lv_obj_t* card = lv_obj_find_by_name(overlay_, "chamber_display_card");
    REQUIRE(card != nullptr);
    lv_obj_t* fan_switch = lv_obj_find_by_name(overlay_, "filter_fan_switch");
    REQUIRE(fan_switch != nullptr);
    CHECK(lv_obj_get_x2(fan_switch) <= lv_obj_get_x2(card));
    CHECK(hidden(lv_obj_find_by_name(overlay_, "fan_percent_label")));
}

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "portrait 272x480 fits the faulted band with a usable chart",
                 "[chamber][panel][geometry]") {
    ScopedGeometry portrait(272, 480);
    build_overlay();

    // Faulted portrait: the banner replaces the readout row inside the
    // full-width band, and the chart keeps every spare pixel above it.
    // External is stated explicitly: the marker's row claims chart pixels,
    // and a shuffled earlier case must not decide this floor.
    set_xml_int("chamber_heater_fault", 1);
    set_xml_int("chamber_heater_inhibited", 1);
    set_xml_int("chamber_heater_offline", 0);
    set_xml_int("chamber_heater_externally_controlled", 0);
    set_xml_int("chamber_filter_fan_device_driven", 1);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(overlay_);

    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
    CHECK(hidden(lv_obj_find_by_name(overlay_, "chamber_readout_row")));

    lv_obj_t* graph = lv_obj_find_by_name(overlay_, "graph_container");
    REQUIRE(graph != nullptr);
    CHECK(lv_obj_get_height(graph) >= 110);

    lv_obj_t* left_column = lv_obj_find_by_name(overlay_, "graph_outer_container");
    REQUIRE(left_column != nullptr);
    CHECK(lv_obj_get_scroll_bottom(left_column) == 0);
    CHECK(lv_obj_get_scroll_y(left_column) == 0);

    // The band's own column must not overflow either: Custom is its last
    // child and nothing grows below the card.
    lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
    REQUIRE(strip != nullptr);
    lv_obj_t* custom = lv_obj_find_by_name(overlay_, "chamber_btn_custom");
    REQUIRE(custom != nullptr);
    CHECK(abs_y2(custom) <= abs_y2(strip));
}

// ============================================================================
// Round-4 layout contracts: the header stays inside the card, no label
// ellipsizes, and the button block holds a fixed height across states
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "chamber card header keeps the status inside the card at the narrow widths",
                 "[chamber][panel][geometry]") {
    // The widest header the card renders: "37.7 / 60°C" beside a glyph and
    // "100%". Micro and tiny both carry the readout inside the header row
    // (tiny on the xs icon + sm readout), so the assertion guards a
    // regression that overflows the row again at either width.
    const std::pair<int32_t, int32_t> sizes[] = {{480, 272}, {480, 320}};
    for (const auto& [w, h] : sizes) {
        CAPTURE(w);
        CAPTURE(h);
        ScopedGeometry geo(w, h);
        build_overlay();
        set_worst_case_chamber_data();
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(overlay_);

        lv_obj_t* card = lv_obj_find_by_name(overlay_, "chamber_display_card");
        REQUIRE(card != nullptr);
        lv_obj_t* status = lv_obj_find_by_name(overlay_, "chamber_status_msg");
        REQUIRE(status != nullptr);
        REQUIRE_FALSE(hidden(status));
        // The readout must be on screen for this to measure the real worst
        // case: the compact sm variant in the header row at both sizes.
        REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_temp_display_compact")));

        const int32_t content_right = abs_x2(card) - lv_obj_get_style_pad_right(card, LV_PART_MAIN);
        CHECK(abs_x2(status) <= content_right);
    }
}

TEST_CASE_METHOD(ChamberOverlayFixture, "no chamber card label is truncated at any breakpoint",
                 "[chamber][panel][geometry]") {
    // Every size the overlay ships to: the landscape ladder plus both
    // portrait widths. The element word rides along only where it fits in
    // full (small and up); narrower columns show icon + value. The fan word
    // is gone at every size: it never fits beside the percent and switch, so
    // its row is icon + value everywhere.
    const std::pair<int32_t, int32_t> sizes[] = {{480, 272},  {480, 320}, {800, 480},
                                                 {1024, 600}, {272, 480}, {320, 480}};
    for (const auto& [w, h] : sizes) {
        CAPTURE(w);
        CAPTURE(h);
        ScopedGeometry geo(w, h);
        build_overlay();
        set_worst_case_chamber_data();
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(overlay_);

        const bool words_expected = std::min(w, h) >= 480;
        lv_obj_t* element_label = lv_obj_find_by_name(overlay_, "element_readout_label");
        REQUIRE(element_label != nullptr);
        if (words_expected) {
            CHECK_FALSE(hidden(element_label));
        } else {
            CHECK(hidden(element_label));
        }
        CHECK(lv_obj_find_by_name(overlay_, "fan_readout_label") == nullptr);

        // Percent and the Device badge are mutually exclusive; both shapes of
        // the fan row must fit.
        for (int device_driven : {0, 1}) {
            CAPTURE(device_driven);
            set_xml_int("chamber_filter_fan_device_driven", device_driven);
            helix::ui::UpdateQueue::instance().drain();
            lv_obj_update_layout(overlay_);
            lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
            REQUIRE(strip != nullptr);
            check_no_label_truncated(strip);
        }
    }
}

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "chamber card keeps every visible widget inside the card at every size and state",
                 "[chamber][panel][geometry]") {
    // The full matrix the card ships to: both portrait widths and the
    // landscape ladder, each in healthy/fault/offline, with the fan row in
    // both shapes (percent vs Device badge) and the External marker on and
    // off. Content-width labels overflow rather than ellipsize, so this edge
    // walk is the only check that sees a row wider than the column; at the
    // landscape sizes it also proves the column fits vertically, since the
    // strip is not scrollable and an overflow runs past the edge instead
    // of scrolling.
    const std::pair<int32_t, int32_t> sizes[] = {{480, 272},  {480, 320}, {800, 480},
                                                 {1024, 600}, {272, 480}, {320, 480}};
    for (const auto& [w, h] : sizes) {
        CAPTURE(w);
        CAPTURE(h);
        ScopedGeometry geo(w, h);
        build_overlay();
        for (int state = 0; state < 3; ++state) {
            // 0 healthy, 1 fault, 2 offline.
            for (int device_driven : {0, 1}) {
                for (int external : {0, 1}) {
                    CAPTURE(state);
                    CAPTURE(device_driven);
                    CAPTURE(external);
                    set_worst_case_chamber_data();
                    set_xml_int("chamber_heater_fault", state == 1 ? 1 : 0);
                    set_xml_int("chamber_heater_inhibited", 0);
                    set_xml_int("chamber_heater_offline", state == 2 ? 1 : 0);
                    set_xml_int("chamber_filter_fan_device_driven", device_driven);
                    set_xml_int("chamber_heater_externally_controlled", external);
                    helix::ui::UpdateQueue::instance().drain();
                    lv_obj_update_layout(overlay_);

                    lv_obj_t* card = lv_obj_find_by_name(overlay_, "chamber_display_card");
                    REQUIRE(card != nullptr);
                    const int32_t content_right =
                        abs_x2(card) - lv_obj_get_style_pad_right(card, LV_PART_MAIN);
                    check_no_descendant_past_card_content(card, content_right);

                    if (w > h) {
                        // Vertical fit at every landscape size: the column's
                        // last child must stay inside the strip, compared as
                        // absolute edges (see abs_y2). The visibility
                        // precondition matters: a hidden child keeps stale
                        // creation coords, which would pass this vacuously.
                        lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
                        REQUIRE(strip != nullptr);
                        lv_obj_t* custom = lv_obj_find_by_name(overlay_, "chamber_btn_custom");
                        REQUIRE(custom != nullptr);
                        CHECK_FALSE(hidden(custom));
                        CHECK(abs_y2(custom) <= abs_y2(strip));
                    }
                }
            }
        }
    }
}

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "portrait readout row hides the fan labels without a filter fan",
                 "[chamber][panel][xml]") {
    ScopedGeometry portrait(272, 480);
    build_overlay();

    // Healthy row: element value, fan percent, switch. The row is flat in
    // portrait (one row, per-widget gates), so a backend with no filter-fan
    // pin must gate the percent and the Device badge individually or the row
    // renders a stray percent beside the hidden icon and switch.
    set_xml_int("chamber_heater_fault", 0);
    set_xml_int("chamber_heater_inhibited", 0);
    set_xml_int("chamber_heater_offline", 0);
    set_xml_int("chamber_filter_fan_device_driven", 0);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_readout_row")));

    set_xml_int("printer_has_chamber_filter_fan", 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(hidden(lv_obj_find_by_name(overlay_, "fan_percent_label")));
    CHECK(hidden(lv_obj_find_by_name(overlay_, "fan_device_badge")));
    CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_switch")));
    CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "element_temp_label")));

    set_xml_int("printer_has_chamber_filter_fan", 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fan_percent_label")));
}

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "chamber buttons hold a fixed height between healthy and faulted at 480x320",
                 "[chamber][panel][geometry]") {
    ScopedGeometry tiny(480, 320);
    build_overlay();

    auto button_heights = [&]() {
        lv_obj_update_layout(overlay_);
        return std::array<int32_t, 4>{
            lv_obj_get_height(lv_obj_find_by_name(overlay_, "chamber_preset_row_1")),
            lv_obj_get_height(lv_obj_find_by_name(overlay_, "chamber_preset_row_2")),
            lv_obj_get_height(lv_obj_find_by_name(overlay_, "chamber_btn_custom")),
            lv_obj_get_height(lv_obj_find_by_name(overlay_, "chamber_preset_1"))};
    };

    // Healthy baseline: the chamber_preset_h token at tiny (43), above the
    // generic #button_height (32) these rows would otherwise fall back to.
    const auto healthy = button_heights();
    for (const int32_t height : healthy)
        CHECK(height == 43);

    // Tallest card content: banner with reason plus every readout row and
    // the External marker.
    set_worst_case_chamber_data();
    set_xml_int("chamber_heater_inhibited", 1);
    helix::ui::UpdateQueue::instance().drain();
    const auto faulted = button_heights();

    CHECK(faulted == healthy);

    // The taller card fits: the spacer kept at least #space_xs (2 at tiny)
    // and the column's last child stays inside the strip. The strip is not
    // scrollable, so an overflowing column never scrolls - it runs past the
    // edge, and only this comparison sees it.
    lv_obj_t* strip = lv_obj_find_by_name(overlay_, "chamber_control_strip");
    REQUIRE(strip != nullptr);
    lv_obj_t* spacer = lv_obj_find_by_name(overlay_, "chamber_strip_spacer");
    REQUIRE(spacer != nullptr);
    CHECK(lv_obj_get_height(spacer) >= 2);
    lv_obj_t* custom = lv_obj_find_by_name(overlay_, "chamber_btn_custom");
    REQUIRE(custom != nullptr);
    CHECK(abs_y2(custom) <= abs_y2(strip));
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
