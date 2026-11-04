// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_stack_widget.h"

#include "ui_carousel.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_overlay_temp_graph.h"
#include "ui_temp_display.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "stacked_row_layout.h"
#include "temperature_service.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_temp_stack_widget() {
    register_widget_factory("temp_stack", [](const std::string&) {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TemperatureService>();
        return std::make_unique<TempStackWidget>(ps, tcp);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "temp_stack_nozzle_cb",
                             TempStackWidget::temp_stack_nozzle_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_bed_cb", TempStackWidget::temp_stack_bed_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_chamber_cb",
                             TempStackWidget::temp_stack_chamber_cb);
    lv_xml_register_event_cb(nullptr, "temp_carousel_page_cb",
                             TempStackWidget::temp_carousel_page_cb);
}
} // namespace helix

namespace {
// Text size tier for the stacked icon-above-temperature layout, and the tier
// the fit is measured at. One name so the measurement and the applied font can
// never drift apart.
constexpr const char* kSizeStacked = "md";

// Make all children of a page pass events through (not clickable, bubble to parent)
void make_children_passthrough(lv_obj_t* parent) {
    if (!parent)
        return;
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if (!child)
            continue;
        lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        make_children_passthrough(child);
    }
}
} // namespace

using namespace helix;

TempStackWidget::TempStackWidget(PrinterState& printer_state, TemperatureService* temp_panel)
    : printer_state_(printer_state), temp_control_panel_(temp_panel) {}

TempStackWidget::~TempStackWidget() {
    detach();
}

void TempStackWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

std::string TempStackWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_temp_carousel";
    }
    return "panel_widget_temp_stack";
}

bool TempStackWidget::on_edit_configure() {
    bool was_carousel = is_carousel_mode();
    nlohmann::json new_config = config_;
    if (was_carousel) {
        new_config.erase("display_mode");
    } else {
        new_config["display_mode"] = "carousel";
    }
    spdlog::info("[TempStackWidget] Toggling display_mode: {} → {}",
                 was_carousel ? "carousel" : "stack", was_carousel ? "stack" : "carousel");
    save_widget_config(new_config);
    return true;
}

bool TempStackWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void TempStackWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Pressed feedback: dim each clickable row on touch
    for (const char* name :
         {"temp_stack_nozzle_row", "temp_stack_bed_row", "temp_stack_chamber_row"}) {
        lv_obj_t* row = lv_obj_find_by_name(widget_obj_, name);
        if (row)
            lv_obj_set_style_opa(row, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    if (is_carousel_mode()) {
        attach_carousel(widget_obj);
    } else {
        attach_stack(widget_obj);
    }
}

void TempStackWidget::attach_stack(lv_obj_t* /*widget_obj*/) {
    nozzle_icon_binder_.bind(widget_obj_, printer_state_, HeaterType::Nozzle);
    bed_icon_binder_.bind(widget_obj_, printer_state_, HeaterType::Bed);
    chamber_icon_binder_.bind(widget_obj_, printer_state_, HeaterType::Chamber);

    int attached = static_cast<int>(nozzle_icon_binder_.is_bound()) +
                   static_cast<int>(bed_icon_binder_.is_bound()) +
                   static_cast<int>(chamber_icon_binder_.is_bound());
    spdlog::debug("[TempStackWidget] Attached stack with {} animators", attached);
}

void TempStackWidget::attach_carousel(lv_obj_t* widget_obj) {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj, "temp_carousel");
    if (!carousel) {
        spdlog::error("[TempStackWidget] Could not find temp_carousel in XML");
        return;
    }

    // Use carousel itself as temporary parent (ui_carousel_add_item reparents into tiles)
    lv_obj_t* page_parent = carousel;

    // Helper to create a carousel page with icon + temp_display
    auto create_temp_page = [&](const char* icon_src, const char* icon_name,
                                const char* bind_current, const char* bind_target,
                                const char* page_name) -> lv_obj_t* {
        // Create page container
        lv_obj_t* page = lv_obj_create(page_parent); // reparented by ui_carousel_add_item
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(page, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_name(page, page_name);

        // Click callback to open temp overlay
        lv_obj_add_event_cb(page, temp_carousel_page_cb, LV_EVENT_CLICKED, nullptr);

        // Icon
        const char* icon_attrs[] = {"src",       icon_src, "size",    "md",   "variant",
                                    "secondary", "name",   icon_name, nullptr};
        lv_xml_create(page, "icon", icon_attrs);

        // Temp display (larger, with target shown)
        const char* td_attrs[] = {"size",        "md",           "show_target",
                                  "true",        "bind_current", bind_current,
                                  "bind_target", bind_target,    nullptr};
        lv_xml_create(page, "temp_display", td_attrs);

        // Make children pass events through to the page (clicks + long-press)
        make_children_passthrough(page);

        return page;
    };

    // Nozzle page (use nozzle_icon component for the icon instead)
    lv_obj_t* nozzle_page = lv_obj_create(page_parent);
    lv_obj_set_size(nozzle_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(nozzle_page, 0, 0);
    lv_obj_set_style_bg_opa(nozzle_page, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(nozzle_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nozzle_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nozzle_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nozzle_page, theme_manager_get_spacing("space_xs"), 0);
    lv_obj_add_flag(nozzle_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_name(nozzle_page, "nozzle");
    lv_obj_add_event_cb(nozzle_page, temp_carousel_page_cb, LV_EVENT_CLICKED, nullptr);

    const char* nozzle_icon_attrs[] = {
        "size", "md", "badge_subject", "", "name", "carousel_nozzle_icon", nullptr};
    lv_xml_create(nozzle_page, "nozzle_icon", nozzle_icon_attrs);

    const char* nozzle_td_attrs[] = {
        "size",          "md",          "show_target",     "true", "bind_current",
        "extruder_temp", "bind_target", "extruder_target", nullptr};
    lv_xml_create(nozzle_page, "temp_display", nozzle_td_attrs);
    make_children_passthrough(nozzle_page);
    ui_carousel_add_item(carousel, nozzle_page);

    // Bind nozzle heating icon from its own page root. Carousel pages are
    // separate subtrees under widget_obj_ — binding from the page (rather than
    // widget_obj_) keeps this immune to any other same-named icon in the tree.
    nozzle_icon_binder_.bind(nozzle_page, printer_state_, HeaterType::Nozzle);

    // Bed page — glyph named "bed_icon_glyph" so the binder's default lookup
    // finds it directly (it is a leaf label; there is no wrapper child to dig
    // into, unlike the old lv_obj_get_child(bed_glyph, 0) attempt).
    lv_obj_t* bed_page =
        create_temp_page("radiator", "bed_icon_glyph", "bed_temp", "bed_target", "bed");
    ui_carousel_add_item(carousel, bed_page);
    bed_icon_binder_.bind(bed_page, printer_state_, HeaterType::Bed);

    // Chamber page (only if sensor present). Target is the effective target
    // subject (heater target while heating, cooling-fan ceiling while
    // maintaining) — previously this bound "chamber_temp" as both current AND
    // target, so the displayed target never reflected the real setpoint.
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        lv_obj_t* chamber_page =
            create_temp_page("fridge_industrial", "chamber_icon_glyph", "chamber_temp",
                             "chamber_effective_target", "chamber");
        ui_carousel_add_item(carousel, chamber_page);
        chamber_icon_binder_.bind(chamber_page, printer_state_, HeaterType::Chamber);
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[TempStackWidget] Attached carousel with {} pages", page_count);
}

void TempStackWidget::on_size_changed(int /*colspan*/, int rowspan, int /*width_px*/,
                                      int height_px) {
    if (!widget_obj_ || is_carousel_mode())
        return;

    static_assert(kIconAboveMinRowspan == GridLayout::TRACKS_PER_CELL + 1,
                  "The stacked layout starts one track above a whole authored cell");

    // Collect the rows up front: the layout decision needs the visible count
    // before anything can be applied. A hidden row - chamber on a printer with
    // no sensor - takes up no height, so it must not be budgeted for.
    lv_obj_t* rows[3] = {};
    int row_count = 0;
    int visible_rows = 0;
    for (const char* row_name :
         {"temp_stack_nozzle_row", "temp_stack_bed_row", "temp_stack_chamber_row"}) {
        lv_obj_t* row = lv_obj_find_by_name(widget_obj_, row_name);
        if (!row)
            continue;
        rows[row_count++] = row;
        if (!lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN))
            visible_rows++;
    }

    const int icon_gap_px = static_cast<int>(theme_manager_get_spacing("space_xxs"));
    const int row_gap_px = static_cast<int>(theme_manager_get_spacing("space_xs"));

    // Measure the layout being considered, not the one on screen. Stacking
    // buys its vertical room back as type, so the question is whether the
    // LARGER text fits - asking it at the current font would promote sizes
    // that then overflow the moment the type grows with them.
    const lv_font_t* tall_text_font =
        theme_manager_get_font(theme_manager_size_to_font_token(kSizeStacked, "xs"));
    const StackedRowMetrics tall_row{
        static_cast<int>(mdi_icons_24.line_height),
        tall_text_font ? static_cast<int>(tall_text_font->line_height) : 0, icon_gap_px};

    const bool icon_above =
        wants_icon_above(rowspan, height_px, visible_rows, tall_row, row_gap_px);

    // Text tier. Stacking goes a rung further than the two-cell height band
    // does on its own - the point of putting the icon on its own line is the
    // room it frees for the number underneath it.
    const bool tall_pixels = height_px >= widget_size::h_tall();
    const char* size = icon_above ? kSizeStacked : (tall_pixels ? "sm" : "xs");

    // Get text font for this size tier
    const char* font_token = theme_manager_size_to_font_token(size, "xs");
    const lv_font_t* text_font = theme_manager_get_font(font_token);
    if (!text_font)
        return;

    // Icon font: xs=16px, sm/md=24px. The glyph does not follow the text a
    // third rung up - a 32px icon over a body-size number reads as an icon
    // with a caption, not a temperature with a label.
    const lv_font_t* icon_font = (icon_above || tall_pixels) ? &mdi_icons_24 : &mdi_icons_16;

    for (int r = 0; r < row_count; r++) {
        lv_obj_t* row = rows[r];

        // DECLARATIVE_OK: measured layout. Which way a row flows depends on
        // the resolved font line heights and the pixel height the grid handed
        // us, neither of which XML can express - the same exception
        // decide_nozzle_layout() runs under. The row-mode values here restore
        // exactly what panel_widget_temp_stack.xml authors.
        lv_obj_set_flex_flow(row, icon_above ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(row, icon_above ? icon_gap_px : row_gap_px, 0);

        // Update temp_display fonts and icon fonts in this row
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
            if (ui_temp_display_is_valid(child)) {
                // Update all labels inside the temp_display container
                for (uint32_t j = 0; j < lv_obj_get_child_count(child); j++) {
                    lv_obj_t* label = lv_obj_get_child(child, static_cast<int32_t>(j));
                    lv_obj_set_style_text_font(label, text_font, 0);
                }
            } else if (lv_obj_get_child_count(child) > 0) {
                // Icon component (nozzle_icon/heater_icon wrapper) - the
                // glyph is its first child, whether or not that wrapper also
                // carries a tool badge (nozzle_icon.xml, heater_icon.xml).
                lv_obj_t* glyph = lv_obj_get_child(child, 0);
                if (glyph)
                    lv_obj_set_style_text_font(glyph, icon_font, 0);
            }
        }
    }

    spdlog::debug("[TempStackWidget] on_size_changed rowspan={} height_px={} visible_rows={} "
                  "-> size {} layout {}",
                  rowspan, height_px, visible_rows, size,
                  icon_above ? "icon-above" : "icon-beside");
}

void TempStackWidget::detach() {
    lifetime_.invalidate();
    nozzle_icon_binder_.unbind();
    bed_icon_binder_.unbind();
    chamber_icon_binder_.unbind();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[TempStackWidget] Detached");
}

void TempStackWidget::handle_nozzle_clicked() {
    spdlog::info("[TempStackWidget] Nozzle clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
}

void TempStackWidget::handle_bed_clicked() {
    spdlog::info("[TempStackWidget] Bed clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

void TempStackWidget::handle_chamber_clicked() {
    spdlog::info("[TempStackWidget] Chamber clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Chamber, parent_screen_);
}

void TempStackWidget::temp_stack_nozzle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_nozzle_cb");
    if (auto* self = panel_widget_from_event<TempStackWidget>(e)) {
        self->handle_nozzle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_bed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_bed_cb");
    if (auto* self = panel_widget_from_event<TempStackWidget>(e)) {
        self->handle_bed_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_chamber_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_chamber_cb");
    if (auto* self = panel_widget_from_event<TempStackWidget>(e)) {
        self->handle_chamber_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_carousel_page_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_carousel_page_cb");
    if (auto* self = panel_widget_from_event<TempStackWidget>(e)) {
        auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        const char* page_id = lv_obj_get_name(target);
        if (page_id) {
            if (std::strcmp(page_id, "nozzle") == 0) {
                self->handle_nozzle_clicked();
            } else if (std::strcmp(page_id, "bed") == 0) {
                self->handle_bed_clicked();
            } else if (std::strcmp(page_id, "chamber") == 0) {
                self->handle_chamber_clicked();
            }
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}
