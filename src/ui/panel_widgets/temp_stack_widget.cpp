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
#include "json_utils.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "stacked_row_layout.h"
#include "temperature_service.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
void register_temp_stack_widget() {
    register_widget_factory("temp_stack", [](const std::string& instance_id) {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TemperatureService>();
        return std::make_unique<TempStackWidget>(instance_id, ps, tcp);
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

// Text tiers side-by-side columns try, largest first.
constexpr const char* kColumnTiers[] = {"xl", "lg", "md", "sm"};

// Widest value a heater column ever shows, measured instead of the live number
// so the columns do not reflow as a temperature climbs.
constexpr const char* kWidestValueText = "88.8\u00B0C";

// Make all children of a page pass events through (not clickable, bubble to parent)
void make_children_passthrough(lv_obj_t* parent) {
    if (!parent)
        return;
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if (!child)
            continue;
        lv_obj_set_clickable(child, false);
        lv_obj_set_event_bubble(child, true);
        make_children_passthrough(child);
    }
}
} // namespace

using namespace helix;
using helix::ui::measure_text_px;

TempStackWidget::TempStackWidget(const std::string& instance_id, PrinterState& printer_state,
                                 TemperatureService* temp_panel)
    : instance_id_(instance_id), printer_state_(printer_state), temp_control_panel_(temp_panel) {
    // Published before the manager parses the component, which drops a
    // binding whose subject is missing at parse time.
    shape_.publish(subjects_, instance_id_ + "_shape", attr_storage_, true);
    for (const auto& a : attr_storage_) {
        attrs_.push_back(a.c_str());
    }
    attrs_.push_back(nullptr);
}

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
    return helix::json_util::read_field<std::string>(config_, "display_mode") == "carousel";
}

void TempStackWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

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

    // The pages are the component's; their children pass taps and long-presses
    // to the page. Each icon binder binds from its own page root, which keeps it
    // immune to any other same-named icon in the tree.
    auto page = [carousel](const char* name) {
        lv_obj_t* p = helix::ui::find_required(carousel, name, "TempStackWidget");
        make_children_passthrough(p);
        return p;
    };
    if (lv_obj_t* nozzle_page = page("nozzle")) {
        nozzle_icon_binder_.bind(nozzle_page, printer_state_, HeaterType::Nozzle);
    }
    // The bed glyph is named "bed_icon_glyph" so the binder's default lookup
    // finds it directly: it is a leaf label with no wrapper child.
    if (lv_obj_t* bed_page = page("bed")) {
        bed_icon_binder_.bind(bed_page, printer_state_, HeaterType::Bed);
    }

    // The chamber page, last, is in reach only on a printer with a chamber. Its
    // target is the effective target subject (heater target while heating,
    // cooling-fan ceiling while maintaining), so it shows the real setpoint.
    lv_obj_t* chamber_page = page("chamber");
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
    const bool has_chamber = chamber_gate && lv_subject_get_int(chamber_gate) != 0;
    helix::ui::carousel_set_trailing_tiles_reachable(carousel, has_chamber);
    ui_carousel_set_real_page_count(carousel, has_chamber ? -1 : 2);
    if (has_chamber && chamber_page) {
        chamber_icon_binder_.bind(chamber_page, printer_state_, HeaterType::Chamber);
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[TempStackWidget] Attached carousel with {} pages", page_count);
}

void TempStackWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    if (!widget_obj_ || is_carousel_mode())
        return;

    static_assert(kIconAboveMinRowspan == GridLayout::TRACKS_PER_CELL + 1,
                  "The stacked layout starts one track above a whole authored cell");
    static_assert(kColumnsMinColspan == GridLayout::TRACKS_PER_CELL + 1,
                  "The side-by-side layout starts one track above a whole authored cell");

    // Collect the rows up front: the layout decision needs the visible count
    // before anything can be applied. A hidden row - chamber on a printer with
    // no sensor - takes up no height, so it must not be budgeted for.
    lv_obj_t* rows[3] = {};
    lv_obj_t* labels[3] = {};
    int row_count = 0;
    int visible_rows = 0;
    const char* row_names[] = {"temp_stack_nozzle_row", "temp_stack_bed_row",
                               "temp_stack_chamber_row"};
    const char* label_names[] = {"temp_stack_nozzle_label", "temp_stack_bed_label",
                                 "temp_stack_chamber_label"};
    for (size_t n = 0; n < 3; n++) {
        lv_obj_t* row = lv_obj_find_by_name(widget_obj_, row_names[n]);
        if (!row)
            continue;
        labels[row_count] = lv_obj_find_by_name(row, label_names[n]);
        rows[row_count++] = row;
        if (!lv_obj_is_hidden(row))
            visible_rows++;
    }

    const int icon_gap_px = static_cast<int>(theme_manager_get_spacing("space_xxs"));
    const int row_gap_px = static_cast<int>(theme_manager_get_spacing("space_xs"));

    // Measure the layout being considered, not the one on screen. Stacking
    // buys its vertical room back as type, so the question is whether the
    // LARGER text fits - asking it at the current font would promote sizes
    // that then overflow the moment the type grows with them.
    const lv_font_t* caption_font =
        theme_manager_get_font(theme_manager_size_to_font_token("xs", "xs"));
    const int caption_px = caption_font ? static_cast<int>(caption_font->line_height) : 0;

    // One candidate arrangement: the text tier and the glyph font it is
    // measured with.
    auto decide = [&](const char* tier, const lv_font_t* icon) {
        const lv_font_t* font =
            theme_manager_get_font(theme_manager_size_to_font_token(tier, "xs"));
        const int icon_px = static_cast<int>(icon->line_height);
        const StackedRowMetrics row{icon_px, font ? static_cast<int>(font->line_height) : 0,
                                    icon_gap_px, caption_px};
        // Width one column needs: the widest of the glyph, the value at its
        // widest, and (captioned) the caption.
        ColumnWidths cols;
        // width_px is measured inside the widget's side padding, the band outside it.
        const int pad_px = 2 * static_cast<int>(theme_manager_get_spacing("space_xs"));
        cols.min_box_px = widget_size::w_normal() - pad_px;
        if (font && caption_font) {
            cols.plain_px = std::max(icon_px, measure_text_px(kWidestValueText, font));
            int widest_caption = 0;
            for (int r = 0; r < row_count; r++) {
                const char* text = labels[r] ? lv_label_get_text(labels[r]) : nullptr;
                if (text && !lv_obj_is_hidden(rows[r]))
                    widest_caption = std::max(widest_caption, measure_text_px(text, caption_font));
            }
            cols.captioned_px = std::max(cols.plain_px, widest_caption);
            // Rows: the glyph, then the value, then the caption, along one line.
            cols.beside_plain_px = icon_px + row_gap_px + measure_text_px(kWidestValueText, font);
            cols.beside_captioned_px = cols.beside_plain_px + row_gap_px + widest_caption;
        }
        // Keep a visible margin above the first row and below the last: big
        // glyphs that fit to the pixel read as clipped against the box.
        cols.margin_px = 2 * static_cast<int>(theme_manager_get_spacing("space_sm"));
        return classify_stack_layout(colspan, rowspan, width_px - pad_px, height_px, visible_rows,
                                     row, row_gap_px, cols);
    };

    // The vertical answer (or Beside) is the fallback, measured at the stacked
    // tier. Side by side, then big rows, each take the largest text tier whose
    // arrangement fits the width and height, with the largest glyph that suits
    // that text, so a wider or taller box reads bigger.
    const char* big_size = kSizeStacked;
    const lv_font_t* big_icon = &mdi_icons_24;
    StackLayout layout = decide(kSizeStacked, &mdi_icons_24);
    // The stacked tier says which side-by-side shape the box takes, if any; the
    // search below then only decides how big it can be drawn.
    const bool big_shape = layout.shape == StackShape::Columns || layout.shape == StackShape::Rows;
    if (big_shape) {
        const StackShape want = layout.shape;
        bool found = false;
        for (const char* tier : kColumnTiers) {
            const lv_font_t* font =
                theme_manager_get_font(theme_manager_size_to_font_token(tier, "xs"));
            if (!font)
                continue;
            for (lv_font_t* icon : {&mdi_icons_48, &mdi_icons_32, &mdi_icons_24}) {
                // A glyph more than twice the text's height reads as a pictogram
                // with a caption, not a temperature with a label.
                if (icon->line_height > 2 * font->line_height && icon != &mdi_icons_24)
                    continue;
                const StackLayout candidate = decide(tier, icon);
                if (candidate.shape == want) {
                    layout = candidate;
                    big_size = tier;
                    big_icon = icon;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }
    const bool rows_big = layout.shape == StackShape::Rows;
    const bool columns = layout.shape == StackShape::Columns;
    const bool icon_above =
        layout.shape == StackShape::IconAbove || layout.shape == StackShape::Columns;

    // Text tier. Stacking goes a rung further than the two-cell height band
    // does on its own - the point of putting the icon on its own line is the
    // room it frees for the number underneath it.
    const bool tall_pixels = height_px >= widget_size::h_tall();
    const char* size = (columns || rows_big)
                           ? big_size
                           : (icon_above ? kSizeStacked : (tall_pixels ? "sm" : "xs"));

    // Get text font for this size tier
    const char* font_token = theme_manager_size_to_font_token(size, "xs");
    const lv_font_t* text_font = theme_manager_get_font(font_token);
    if (!text_font)
        return;

    // Icon font: 16px beside the text of a one-cell box, 24px for the stacked
    // rows, and in columns or big rows the largest glyph (48, 32 or 24px) that
    // is at most twice the text's height and fits the box.
    const lv_font_t* icon_font =
        (columns || rows_big) ? big_icon
                              : ((icon_above || tall_pixels) ? &mdi_icons_24 : &mdi_icons_16);

    // The component binds the flow, column sharing and captions to these.
    shape_.apply(layout);

    for (int r = 0; r < row_count; r++) {
        lv_obj_t* row = rows[r];

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

    spdlog::debug("[TempStackWidget] on_size_changed {}x{} tracks {}x{}px visible_rows={} "
                  "-> size {} layout {}{}",
                  colspan, rowspan, width_px, height_px, visible_rows, size,
                  columns ? "columns"
                          : (rows_big ? "big-rows" : (icon_above ? "icon-above" : "icon-beside")),
                  (columns || rows_big) && layout.captions ? " +captions" : "");
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
