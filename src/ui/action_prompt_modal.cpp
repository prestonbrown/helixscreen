// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "action_prompt_modal.h"

#include "ui_breakpoint.h"
#include "ui_error_reporting.h"
#include "ui_fault_path.h"
#include "ui_swatch.h"

#include "color_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_size.h"
#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace helix::ui {

namespace {

/**
 * @brief Width the button label needs to render in full, in pixels.
 */
int32_t label_width_px(const std::string& label, const lv_font_t* font) {
    lv_point_t size{};
    lv_text_get_size(&size, label.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/**
 * @brief A color tile with no text: Z-Mod's color-picker swatch (#1806).
 */
bool is_swatch(const PromptButton& btn) {
    return btn.label.empty() && helix::parse_hex_color(btn.hex_color).has_value();
}

/**
 * @brief Measure a row for choose_button_row_layout().
 *
 * The row is width=100% of a card whose own width is a percentage of the
 * screen, so its content width is only real after a layout pass. A row that
 * cannot be measured yet reports available 0, which wraps: the one layout that
 * never clips.
 */
ButtonRowMeasure measure_button_row(lv_obj_t* row, const std::vector<PromptButton>& buttons) {
    ButtonRowMeasure m;
    m.count = static_cast<int32_t>(buttons.size());
    m.all_swatches = !buttons.empty() && std::all_of(buttons.begin(), buttons.end(), is_swatch);
    const lv_font_t* font = theme_manager_get_font("font_body");
    if (font == nullptr) {
        return m;
    }
    for (const auto& btn : buttons) {
        m.widest_label = std::max(m.widest_label, label_width_px(btn.label, font));
    }
    lv_obj_update_layout(row);
    m.available = lv_obj_get_content_width(row);
    m.gap = lv_obj_get_style_pad_column(row, LV_PART_MAIN);
    m.cell_pad = theme_manager_get_spacing("space_sm"); // per side, see create_button()
    return m;
}

/**
 * @brief Measured inputs for sizing a grid of swatch rows, in pixels.
 */
struct SwatchGridGeometry {
    int32_t card_w = 0;     ///< card width as the XML lays it out
    int32_t card_w_max = 0; ///< widest the card may grow on this screen
    int32_t row_inset = 0;  ///< card width minus a swatch row's content width
    int32_t cols = 0;       ///< most swatches sharing one row
    int32_t col_gap = 0;
    int32_t rows = 0;      ///< swatch rows
    int32_t free_h = 0;    ///< card height cap minus everything but the swatch rows
    int32_t touch_min = 0; ///< touch_target_min
    int32_t button_h = 0;  ///< button_height: the tile height when nothing constrains it
};

struct SwatchGridFit {
    int32_t card_w = 0;
    int32_t swatch_h = 0;
    bool scroll = false; ///< the rows need more height than the card has left
};

/// A prompt's button height. A 480x320 screen takes the compact one, so a color
/// prompt shows its whole swatch grid beside its text buttons and footer.
int32_t prompt_button_height() {
    const bool tiny = widget_size::current_breakpoint() == UiBreakpoint::Tiny;
    return theme_manager_get_spacing(tiny ? "button_height_sm" : "button_height");
}

/**
 * @brief Size the card and the swatch tiles to the room the screen has.
 *
 * The card widens only as far as it takes for every tile to be a full touch
 * target across, never past card_w_max. Tiles keep button_height, raised to
 * the touch target, while the free height holds that; otherwise they take
 * the largest height that fits. Below button_height they stop shrinking and
 * the grid scrolls instead.
 *
 * This is the one place touch_target_min yields on height: a color picker
 * that shows every color at once beats one that scrolls to reach 48px tiles,
 * so height drops toward button_height first while width still holds the
 * touch target (prestonbrown/helixscreen#1806).
 */
SwatchGridFit fit_swatch_grid(const SwatchGridGeometry& g) {
    SwatchGridFit fit;
    const int32_t need_w = g.cols * g.touch_min + (g.cols - 1) * g.col_gap + g.row_inset;
    fit.card_w = std::max(g.card_w, std::min(need_w, g.card_w_max));

    const int32_t wanted_h = std::max(g.button_h, g.touch_min);
    const int32_t room_h = g.rows > 0 ? g.free_h / g.rows : wanted_h;
    fit.swatch_h = std::min(wanted_h, room_h);
    if (fit.swatch_h < g.button_h) {
        fit.swatch_h = g.button_h;
        fit.scroll = true;
    }
    return fit;
}

/**
 * @brief Classify a close for the dismiss callback; nullopt for the owner's own hide().
 */
std::optional<PromptCloseKind> close_kind(ModalCloseReason reason, bool button_sent_gcode) {
    switch (reason) {
    case ModalCloseReason::Programmatic:
        return std::nullopt;
    case ModalCloseReason::ButtonPress:
        return button_sent_gcode ? PromptCloseKind::ButtonWithGcode
                                 : PromptCloseKind::ButtonWithoutGcode;
    case ModalCloseReason::BackdropTap:
    case ModalCloseReason::EscKey:
        return PromptCloseKind::UserDismiss;
    case ModalCloseReason::HotReload:
        return PromptCloseKind::HotReload;
    case ModalCloseReason::External:
        return PromptCloseKind::External;
    }
    return std::nullopt;
}

/**
 * @brief Split the regular (non-footer) buttons into rows.
 *
 * Each prompt_button_group gets a row of its own, and a run of ungrouped
 * buttons between groups shares one row, which is how Mainsail lays them out.
 */
std::vector<std::vector<PromptButton>> split_button_rows(const std::vector<PromptButton>& buttons) {
    std::vector<std::vector<PromptButton>> rows;
    for (const auto& btn : buttons) {
        if (btn.is_footer) {
            continue;
        }
        if (rows.empty() || rows.back().front().group_id != btn.group_id) {
            rows.emplace_back();
        }
        rows.back().push_back(btn);
    }
    return rows;
}

} // namespace

ButtonRowLayout choose_button_row_layout(const ButtonRowMeasure& m) {
    if (m.available <= 0 || m.count <= 0) {
        return {};
    }
    const auto fits_share = [&m](int32_t cols) {
        const int32_t cell = (m.available - m.gap * (cols - 1)) / cols;
        return m.widest_label + 2 * m.cell_pad <= cell;
    };
    if (!m.all_swatches && m.count >= kPromptGridMinButtons) {
        return {ButtonRowKind::Grid, fits_share(3) ? 3 : 2};
    }
    if (m.count >= 4 && fits_share(m.count)) {
        return {ButtonRowKind::EqualRow, m.count};
    }
    return {};
}

// ============================================================================
// Construction / Destruction
// ============================================================================

ActionPromptModal::ActionPromptModal() {
    spdlog::debug("[ActionPromptModal] Constructed");
}

ActionPromptModal::~ActionPromptModal() {
    // Modal destructor will call hide() if visible, which invalidates lifetime_
    // Note: No spdlog here - logger may be destroyed before us during shutdown [L010]
}

// ============================================================================
// Public API
// ============================================================================

void ActionPromptModal::set_gcode_callback(GcodeCallback callback) {
    gcode_callback_ = std::move(callback);
}

void ActionPromptModal::set_dismiss_callback(DismissCallback callback) {
    dismiss_callback_ = std::move(callback);
}

bool ActionPromptModal::show_prompt(lv_obj_t* parent, const PromptData& data) {
    prompt_data_ = data;
    // Before Modal::show(): the diagram binds its subject when the XML is built.
    publish_fault_path();

    if (!Modal::show(parent)) {
        fault_path_.release();
        return false;
    }

    spdlog::info("[ActionPromptModal] Shown with title: {}", prompt_data_.title);
    return true;
}

bool ActionPromptModal::show_owned_prompt(lv_obj_t* parent, const PromptData& data) {
    auto modal = std::make_unique<ActionPromptModal>();
    modal->prompt_data_ = data;
    modal->publish_fault_path();
    // A failed show frees the modal, whose hold hands the diagram back.
    return Modal::show_owned(std::move(modal), parent);
}

void ActionPromptModal::publish_fault_path() {
    std::string text;
    for (const auto& line : prompt_data_.text_lines) {
        text += line;
        text += '\n';
    }
    fault_path_.publish(text);
}

// ============================================================================
// Modal Hooks
// ============================================================================

void ActionPromptModal::on_show() {
    button_sent_gcode_ = false;
    populate_content();
}

void ActionPromptModal::on_hide() {
    clear_dynamic_content();
    fault_path_.release();
    spdlog::debug("[ActionPromptModal] on_hide()");
    if (!dismiss_callback_) {
        return;
    }
    if (auto kind = close_kind(close_reason_, button_sent_gcode_)) {
        dismiss_callback_(*kind);
    }
}

// ============================================================================
// Content Population
// ============================================================================

void ActionPromptModal::populate_content() {
    if (!dialog_) {
        return;
    }

    // Set title. kModalTitleWidgetName is the canonical name ui_notification.cpp's
    // duplicate-title suppression looks up, so every modal that can sit on top
    // of the stack must expose its title under it (#1389).
    lv_obj_t* title_label = find_widget(kModalTitleWidgetName);
    if (title_label) {
        lv_label_set_text(title_label, prompt_data_.title.c_str());
    }

    // Error-severity affordance: show the red error icon only for severity "error".
    // The modal instance is reused across shows, so reset both ways to clear any
    // prior error state on a subsequent neutral (action:prompt) show.
    if (lv_obj_t* err_icon = find_widget("icon_error")) {
        if (prompt_data_.severity == "error") {
            lv_obj_remove_flag(err_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(err_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Create text lines
    create_text_lines();

    // Create buttons
    create_buttons();
}

void ActionPromptModal::create_text_lines() {
    lv_obj_t* content_container = find_widget("content_container");
    if (!content_container) {
        spdlog::warn("[ActionPromptModal] content_container not found");
        return;
    }

    // Create a label for each text line
    for (const auto& line : prompt_data_.text_lines) {
        lv_obj_t* label = lv_label_create(content_container);
        lv_label_set_text(label, line.c_str());
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        // Apply body text styling
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);

        created_text_labels_.push_back(label);
    }

    // Hide content container if no text lines
    if (prompt_data_.text_lines.empty()) {
        lv_obj_add_flag(content_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void ActionPromptModal::create_buttons() {
    lv_obj_t* button_container = find_widget("button_container");
    lv_obj_t* footer_container = find_widget("footer_container");
    lv_obj_t* footer_divider = find_widget("footer_divider");

    if (!button_container) {
        spdlog::warn("[ActionPromptModal] button_container not found");
        return;
    }

    bool has_footer_buttons = false;
    int footer_button_count = 0;

    // Regular buttons go in rows inside button_container, each laid out by
    // choose_button_row_layout(). Grid cells get their width in fit_rows(),
    // once the card width is final.
    bool has_regular_buttons = false;
    RowFit rows;
    for (const auto& row_buttons : split_button_rows(prompt_data_.buttons)) {
        auto* row = static_cast<lv_obj_t*>(
            lv_xml_create(button_container, "action_prompt_button_row", nullptr));
        if (!row) {
            // Without rows the footer still has to be built, or the prompt has no way out.
            spdlog::warn("[ActionPromptModal] action_prompt_button_row not registered");
            break;
        }
        has_regular_buttons = true;
        const ButtonRowMeasure measure = measure_button_row(row, row_buttons);
        const ButtonRowLayout layout = choose_button_row_layout(measure);
        if (layout.kind == ButtonRowKind::EqualRow) {
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        }
        for (const auto& btn : row_buttons) {
            create_button(btn, row, layout.kind);
        }
        if (layout.kind == ButtonRowKind::Grid) {
            rows.grid_rows.push_back({row, layout.cols});
        } else if (layout.kind == ButtonRowKind::EqualRow && measure.all_swatches) {
            rows.swatch_rows.push_back(row);
            rows.swatch_cols = std::max(rows.swatch_cols, layout.cols);
        } else if (layout.kind == ButtonRowKind::Wrap) {
            rows.wrap_rows.push_back(row);
        }
    }

    for (const auto& btn : prompt_data_.buttons) {
        if (btn.is_footer && footer_container) {
            // Add vertical divider between footer buttons
            if (footer_button_count > 0) {
                lv_obj_t* divider = lv_obj_create(footer_container);
                lv_obj_set_size(divider, 1, lv_pct(100));
                lv_obj_set_style_bg_color(divider, theme_manager_get_color("border"), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
                lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
            }
            create_button(btn, footer_container);
            has_footer_buttons = true;
            footer_button_count++;
        }
    }

    // Show/hide footer based on whether there are footer buttons
    if (footer_container) {
        lv_obj_set_height(footer_container, prompt_button_height());
        if (has_footer_buttons) {
            lv_obj_remove_flag(footer_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(footer_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (footer_divider) {
        if (has_footer_buttons) {
            lv_obj_remove_flag(footer_divider, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(footer_divider, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide button container if no regular buttons
    if (!has_regular_buttons) {
        lv_obj_add_flag(button_container, LV_OBJ_FLAG_HIDDEN);
    }

    fit_rows(rows);
    size_swatch_discs();
}

// DECLARATIVE_OK: measured layout. A swatch circle is as large as its tile's
// shorter side, which only a layout pass knows.
void ActionPromptModal::size_swatch_discs() {
    lv_obj_t* screen = dialog_ ? lv_obj_get_parent(dialog_) : nullptr;
    if (screen == nullptr) {
        return;
    }
    lv_obj_update_layout(screen);
    for (lv_obj_t* button : created_buttons_) {
        if (!button) {
            continue;
        }
        lv_obj_t* disc = lv_obj_find_by_name(button, "color_swatch_disc");
        if (disc == nullptr) {
            continue;
        }
        const int32_t side =
            std::min(lv_obj_get_content_width(button), lv_obj_get_content_height(button));
        if (side > 0) {
            lv_obj_set_size(disc, side, side);
        }
    }
}

// DECLARATIVE_OK: measured layout. The card width, grid cells, tile heights
// and the scroll area's cap all depend on the pixels the rest of the prompt
// leaves, which only a layout pass knows.
void ActionPromptModal::fit_rows(const RowFit& rows) {
    lv_obj_t* screen = dialog_ ? lv_obj_get_parent(dialog_) : nullptr;
    lv_obj_t* scroll = find_widget("prompt_scroll");
    if (screen == nullptr || scroll == nullptr) {
        return;
    }
    lv_obj_update_layout(screen);

    SwatchGridGeometry g;
    const bool has_swatches = !rows.swatch_rows.empty() && rows.swatch_cols > 0;
    if (has_swatches) {
        g.card_w = lv_obj_get_width(dialog_);
        g.card_w_max = lv_obj_get_content_width(screen) - 2 * theme_manager_get_spacing("space_lg");
        g.row_inset = g.card_w - lv_obj_get_content_width(rows.swatch_rows.front());
        g.cols = rows.swatch_cols;
        g.col_gap = lv_obj_get_style_pad_column(rows.swatch_rows.front(), LV_PART_MAIN);
        g.rows = static_cast<int32_t>(rows.swatch_rows.size());
        g.touch_min = theme_manager_get_spacing("touch_target_min");
        g.button_h = prompt_button_height();

        const SwatchGridFit fit = fit_swatch_grid(g);
        if (fit.card_w != g.card_w) {
            lv_obj_set_style_max_width(dialog_, fit.card_w, LV_PART_MAIN);
            lv_obj_set_width(dialog_, fit.card_w);
            lv_obj_update_layout(screen); // text may rewrap at the new width
        }
    }

    for (const GridRow& grid : rows.grid_rows) {
        const int32_t gap = lv_obj_get_style_pad_column(grid.row, LV_PART_MAIN);
        const int32_t cell =
            (lv_obj_get_content_width(grid.row) - gap * (grid.cols - 1)) / grid.cols;
        const uint32_t n = lv_obj_get_child_count(grid.row);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_set_width(lv_obj_get_child(grid.row, static_cast<int32_t>(i)), cell);
        }
    }
    // A content-sized button grows with its label, so one label wider than the row
    // would widen the button past the card. Such a label keeps one line and ends in
    // an ellipsis instead.
    for (lv_obj_t* row : rows.wrap_rows) {
        const int32_t row_w = lv_obj_get_content_width(row);
        const uint32_t n = lv_obj_get_child_count(row);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t* button = lv_obj_get_child(row, static_cast<int32_t>(i));
            lv_obj_t* label = lv_obj_get_child_by_type(button, 0, &lv_label_class);
            const int32_t cap = row_w - lv_obj_get_style_pad_left(button, LV_PART_MAIN) -
                                lv_obj_get_style_pad_right(button, LV_PART_MAIN);
            if (label == nullptr || cap <= 0 || lv_obj_get_width(label) <= cap) {
                continue;
            }
            lv_obj_set_size(
                label, cap,
                lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        }
    }
    lv_obj_update_layout(screen);

    // The card is height=content and clamps at its max_height without
    // shrinking any child, so the room the scroll area may take is that cap
    // less everything else on the card (including any overflow below it).
    const int32_t cap_style = lv_obj_get_style_max_height(dialog_, LV_PART_MAIN);
    const int32_t cap = LV_COORD_IS_PCT(cap_style)
                            ? lv_pct_to_px(cap_style, lv_obj_get_content_height(screen))
                            : cap_style;
    const int32_t content_h =
        lv_obj_get_height(dialog_) + std::max<int32_t>(0, lv_obj_get_scroll_bottom(dialog_));
    int32_t scroll_h = lv_obj_get_height(scroll);
    const int32_t room = cap - (content_h - scroll_h);

    if (has_swatches) {
        int32_t rows_h = 0;
        for (lv_obj_t* row : rows.swatch_rows) {
            rows_h += lv_obj_get_height(row);
        }
        g.free_h = room - (scroll_h - rows_h);
        const SwatchGridFit fit = fit_swatch_grid(g);
        for (lv_obj_t* row : rows.swatch_rows) {
            const uint32_t n = lv_obj_get_child_count(row);
            for (uint32_t i = 0; i < n; ++i) {
                lv_obj_set_height(lv_obj_get_child(row, static_cast<int32_t>(i)), fit.swatch_h);
            }
        }
        lv_obj_update_layout(screen);
        scroll_h = lv_obj_get_height(scroll);
        spdlog::debug("[ActionPromptModal] Swatch grid: {} rows x {}, card {}px, tiles {}px tall",
                      g.rows, g.cols, fit.card_w, fit.swatch_h);
    }

    if (scroll_h > room) {
        // The room can be negative when the header and footer alone fill the
        // card; one button height stays visible so the area can still scroll.
        const int32_t floor_h = prompt_button_height();
        lv_obj_set_style_max_height(scroll, std::max(room, floor_h), LV_PART_MAIN);
        spdlog::debug("[ActionPromptModal] Content {}px scrolls in {}px", scroll_h,
                      std::max(room, floor_h));
    }
}

void ActionPromptModal::create_button(const PromptButton& btn, lv_obj_t* container,
                                      ButtonRowKind kind) {
    lv_obj_t* button = lv_button_create(container);

    if (btn.is_footer) {
        // Footer buttons: full-width modal style with dividers
        lv_obj_set_height(button, lv_pct(100));
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    } else if (kind != ButtonRowKind::Wrap) {
        // Equal-width cells with compact space_sm padding. On an EqualRow,
        // grow=1 with width 0 shares the non-wrapping line (#1043); a Grid cell
        // gets its fixed width from fit_rows() and row_wrap lines them up.
        lv_obj_set_width(button, 0);
        lv_obj_set_height(button, prompt_button_height());
        if (kind == ButtonRowKind::EqualRow) {
            lv_obj_set_flex_grow(button, 1);
        }
        lv_obj_set_style_pad_left(button, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_pad_right(button, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    } else {
        // Content-sized with padding, which row_wrap spreads over as many
        // lines as the labels need.
        lv_obj_set_size(button, LV_SIZE_CONTENT, prompt_button_height());
        lv_obj_set_style_pad_left(button, theme_manager_get_spacing("space_lg"), LV_PART_MAIN);
        lv_obj_set_style_pad_right(button, theme_manager_get_spacing("space_lg"), LV_PART_MAIN);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    }
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    // A hex fill takes priority over the named style.
    const std::optional<uint32_t> fill_rgb = helix::parse_hex_color(btn.hex_color);
    const lv_color_t bg_color = fill_rgb ? lv_color_hex(*fill_rgb) : get_button_color(btn.color);
    if (is_swatch(btn)) {
        // A color tile with no text is a swatch: the tile stays the touch
        // target and shows a circle of its color, which the swatch painter
        // edges when it blends into the card. size_swatch_discs() fits the
        // circle to the tile once the grid is laid out.
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
        // A button's height until size_swatch_discs() measures the tile.
        const std::string side = std::to_string(prompt_button_height());
        const char* disc_attrs[] = {"name",  "color_swatch_disc", "align",  "center",
                                    "width", side.c_str(),        "height", side.c_str(),
                                    nullptr};
        auto* disc = static_cast<lv_obj_t*>(lv_xml_create(button, "swatch_paint", disc_attrs));
        helix::ui::apply_swatch_color(disc, *fill_rgb, "");
    } else {
        lv_obj_set_style_bg_color(button, bg_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    }

    // Create label inside button with contrast-aware text color
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, btn.label.c_str());
    if (kind == ButtonRowKind::Grid) {
        // A label wider than its cell ends in an ellipsis rather than spilling.
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label, theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), bg_color),
        LV_PART_MAIN);

    // Create callback data with owned copy of gcode string and lifetime token.
    //
    // An empty gcode means DO NOTHING: the button closes the modal and sends no
    // command (#1172). Klipper's label-as-gcode convention is applied in
    // ActionPromptManager::parse_button_spec(), so a wire prompt never reaches
    // here with a blank gcode; only PromptData built in C++ does.
    auto cbd = std::make_unique<ButtonCallbackData>();
    cbd->modal = this;
    cbd->token = lifetime_.token();
    cbd->gcode = btn.gcode;

    // Add click callback with ButtonCallbackData as user_data
    lv_obj_add_event_cb(button, on_button_cb, LV_EVENT_CLICKED, cbd.get());

    // Transfer ownership to the vector (pointer remains stable)
    button_callback_data_.push_back(std::move(cbd));

    created_buttons_.emplace_back(button);

    spdlog::debug("[ActionPromptModal] Created button: {} (gcode: {}, color: {})", btn.label,
                  btn.gcode.empty() ? "<none>" : btn.gcode,
                  !btn.hex_color.empty() ? btn.hex_color
                  : btn.color.empty()    ? std::string("primary")
                                         : btn.color);
}

lv_color_t ActionPromptModal::get_button_color(const std::string& color_name) {
    // Map Klipper color hints to design tokens
    if (color_name == "primary" || color_name.empty()) {
        return theme_manager_get_color("primary");
    } else if (color_name == "secondary") {
        return theme_manager_get_color("secondary");
    } else if (color_name == "info") {
        return theme_manager_get_color("info");
    } else if (color_name == "warning") {
        return theme_manager_get_color("warning");
    } else if (color_name == "error") {
        return theme_manager_get_color("danger");
    }

    // Unknown color - default to primary
    spdlog::debug("[ActionPromptModal] Unknown color '{}', using primary", color_name);
    return theme_manager_get_color("primary");
}

void ActionPromptModal::clear_dynamic_content() {
    // Remove click callbacks from each button before freeing their user_data.
    // Without this, LVGL's async teardown can still hold references to the
    // ButtonCallbackData pointers that button_callback_data_.clear() frees,
    // risking use-after-free if any queued event dispatches against a button
    // after on_hide() returns.
    for (lv_obj_t* button : created_buttons_) {
        if (button) {
            lv_obj_remove_event_cb(button, on_button_cb);
        }
    }
    created_buttons_.clear();
    created_text_labels_.clear();
    button_callback_data_.clear();
}

// ============================================================================
// Event Handler
// ============================================================================

void ActionPromptModal::handle_button_click(const std::string& gcode) {
    // An empty gcode is a dismiss affordance: close, send nothing (#1172).
    if (gcode.empty()) {
        spdlog::info("[ActionPromptModal] Dismiss button clicked (no gcode)");
        hide(ModalCloseReason::ButtonPress);
        return;
    }

    spdlog::info("[ActionPromptModal] Button clicked, gcode: {}", gcode);

    if (gcode_callback_) {
        gcode_callback_(gcode);
        button_sent_gcode_ = true;
    }

    hide(ModalCloseReason::ButtonPress);
}

// ============================================================================
// Static Callbacks
// ============================================================================

void ActionPromptModal::on_button_cb(lv_event_t* e) {
    auto* cbd = static_cast<ButtonCallbackData*>(lv_event_get_user_data(e));
    if (!cbd) {
        spdlog::warn("[ActionPromptModal] Button callback data is null");
        return;
    }

    // Check if the modal is still alive (guards against use-after-free).
    // LifetimeToken is safe to query even after the guard is destroyed (#437).
    if (!cbd->token || cbd->token->expired()) {
        spdlog::debug("[ActionPromptModal] Modal destroyed before button callback fired");
        return;
    }

    SoundManager::instance().play("button_tap");
    cbd->modal->handle_button_click(cbd->gcode);
}

// ============================================================================
// Failure reporting
// ============================================================================

void report_action_prompt_gcode_failure(const std::string& error_message) {
    // Same presentation as every other failed macro in the UI (ui_panel_controls,
    // ui_panel_filament). Main thread only: it shows a toast.
    //
    // Klipper's own wording is the useful part ("Extruder not hot enough"); the
    // generic string only stands in when the transport gave us nothing.
    const std::string detail =
        error_message.empty() ? std::string(lv_tr("Unknown error")) : error_message;
    NOTIFY_ERROR(lv_tr("Macro failed: {}"), detail);
}

} // namespace helix::ui
