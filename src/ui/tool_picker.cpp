// SPDX-License-Identifier: GPL-3.0-or-later
#include "tool_picker.h"

#include "ui_subject_registry.h"

#include "ams_state.h"
#include "helix/ui/text_metrics.h"
#include "helix_psram_attr.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "target_tool.h"
#include "theme_manager.h"
#include "tool_heads_model.h"
#include "tool_state.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace helix::ui {

namespace {

// The picker's grid (fit_tool_picker_grid()) and its cells' band and second
// line, read by tool_switcher_picker.xml.
HELIX_PSRAM_BSS lv_subject_t s_cell_w_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cell_h_subject{};
HELIX_PSRAM_BSS lv_subject_t s_grid_w_subject{};
HELIX_PSRAM_BSS lv_subject_t s_grid_max_h_subject{};
HELIX_PSRAM_BSS lv_subject_t s_band_h_subject{};
HELIX_PSRAM_BSS lv_subject_t s_band_labelled_subject{};
HELIX_PSRAM_BSS lv_subject_t s_two_line_subject{};
HELIX_PSRAM_BSS lv_subject_t s_feeder_label_subject{};
// The footer's buttons grow to their content, so their labels always show:
// the second-line subject they bind is constantly 1.
HELIX_PSRAM_BSS lv_subject_t s_footer_labels_subject{};
// 1 in Target mode, which hides the footer.
HELIX_PSRAM_BSS lv_subject_t s_target_subject{};
// 1 while the cells refuse a tap: a print that blocks a change, Mount mode only.
HELIX_PSRAM_BSS lv_subject_t s_blocked_subject{};
WidgetSubjects s_subjects{"ToolPickerSubjects"};

int resolve_px_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// The feeder cell's border (style_border_width in tool_switcher_feeder_segment.xml).
constexpr int kFeederBorder = 2;

} // namespace

void ToolPicker::init_subjects() {
    s_subjects.ensure_init(
        [] {
            const std::pair<lv_subject_t*, const char*> ints[] = {
                {&s_cell_w_subject, "tool_switcher_picker_cell_w"},
                {&s_cell_h_subject, "tool_switcher_picker_cell_h"},
                {&s_grid_w_subject, "tool_switcher_picker_grid_w"},
                {&s_grid_max_h_subject, "tool_switcher_picker_grid_max_h"},
                {&s_band_h_subject, "tool_switcher_picker_band_h"},
                {&s_band_labelled_subject, "tool_switcher_picker_band_labelled"},
                {&s_two_line_subject, "tool_switcher_picker_two_line"},
                {&s_feeder_label_subject, "tool_switcher_picker_feeder_label"},
                {&s_target_subject, "tool_picker_target"},
                {&s_blocked_subject, "tool_picker_blocked"},
            };
            for (const auto& [subject, name] : ints) {
                lv_subject_init_int(subject, 0);
                s_subjects.publish(name, subject);
            }
            lv_subject_init_int(&s_footer_labels_subject, 1);
            s_subjects.publish("tool_switcher_footer_labels", &s_footer_labels_subject);
        },
        [] {});
}

ToolPicker& tool_picker() {
    static ToolPicker s_picker;
    return s_picker;
}

ToolSwitcherActions tool_switcher_actions_offered() {
    ToolSwitcherActions actions;
    if (const AmsBackend* backend = AmsState::instance().get_backend()) {
        actions.park = backend->supports_tool_park();
        actions.feeder = backend->supports_feeder_control();
    }
    return actions;
}

FeederLabelForm feeder_label_form_for(int cell_w) {
    // The label's own space_xxs padding inside the border, measured in the small font.
    const int room = cell_w - 2 * (kFeederBorder + resolve_px_token("space_xxs", 2));
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int long_px =
        std::max(text_width(lv_tr("Open feeder"), small), text_width(lv_tr("Close feeder"), small));
    return feeder_label_form(room, long_px, text_width(lv_tr("Feeder"), small));
}

bool ToolPicker::open(lv_obj_t* screen, lv_obj_t* anchor, AnchorAlign align, Mode mode,
                      std::function<void(int)> on_pick) {
    if (is_visible() || !screen || !anchor || ToolState::instance().tools().empty()) {
        return false;
    }
    init_subjects();
    mode_ = mode;
    on_pick_ = std::move(on_pick);

    auto& model = ToolHeadsModel::instance();
    const bool target = mode == Mode::Target;
    set_int_if_changed(&s_target_subject, target ? 1 : 0);
    int blocked = 0;
    if (!target) {
        if (lv_subject_t* s = lv_xml_get_subject(nullptr, "tool_switcher_change_blocked")) {
            blocked = lv_subject_get_int(s);
        }
    }
    set_int_if_changed(&s_blocked_subject, blocked);
    // The highlight: the mounted head, or the target tool's.
    auto& from = target ? model.target_state_pool() : model.state_pool();
    const int heads = std::min(model.head_count(), static_cast<int>(from.size()));
    model.pick_state_pool().ensure_size(static_cast<size_t>(heads));
    for (int i = 0; i < heads; ++i) {
        set_int_if_changed(model.pick_state_pool().at(i), lv_subject_get_int(from.at(i)));
    }
    return show_below_widget(screen, anchor, align);
}

void ToolPicker::choose(int head) {
    auto pick = std::move(on_pick_);
    on_pick_ = nullptr;
    hide();
    if (pick) {
        pick(head);
    }
}

void ToolPicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* grid = lv_obj_find_by_name(backdrop, "tool_list");
    lv_obj_t* menu_card = card();
    if (!grid || !menu_card) {
        spdlog::error("[ToolPicker] tool_list or card not found in picker XML");
        return;
    }

    auto& model = ToolHeadsModel::instance();
    // The cells the repeat built: one per head the model last counted.
    const int heads = model.head_count();
    const int min_target = resolve_px_token("touch_target_min", 48);
    const int margin = resolve_px_token("space_md", 10);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int body_px = line_px(body);
    const int small_px = line_px(small);

    // The room the grid has: the screen less ContextMenu's margin, the card's
    // padding, and everything else the card holds (its header and footer),
    // measured with the grid as the repeat built it.
    lv_obj_update_layout(menu_card);
    const int pad_w = lv_obj_get_style_pad_left(menu_card, LV_PART_MAIN) +
                      lv_obj_get_style_pad_right(menu_card, LV_PART_MAIN) +
                      2 * lv_obj_get_style_border_width(menu_card, LV_PART_MAIN);
    const int others_h = lv_obj_get_height(menu_card) - lv_obj_get_height(grid);
    const int avail_w = lv_obj_get_width(backdrop) - 2 * margin - pad_w;
    const int avail_h = lv_obj_get_height(backdrop) - 2 * margin - others_h;

    // Square cells large enough to name the material on their band where
    // there is room.
    const int band_pad = resolve_px_token("space_xxs", 2);
    const ToolPickerGrid fit =
        fit_tool_picker_grid(heads, avail_w, avail_h, min_target, kToolRailGap,
                             tool_picker_preferred_cell(min_target, small_px, band_pad));
    const ToolSwitcherBand band = tool_switcher_band(fit.cell, small_px, band_pad);
    const int label_room = fit.cell - 2 * resolve_px_token("space_xs", 4);
    set_int_if_changed(&s_cell_w_subject, fit.cell);
    set_int_if_changed(&s_cell_h_subject, fit.cell);
    set_int_if_changed(&s_grid_w_subject, fit.width);
    set_int_if_changed(&s_grid_max_h_subject, fit.max_height);
    set_int_if_changed(&s_band_h_subject, band.px);
    set_int_if_changed(&s_band_labelled_subject, band.labelled ? 1 : 0);
    set_int_if_changed(&s_two_line_subject, fit.cell - band.px >= body_px + small_px ? 1 : 0);

    // Mount mode names each head as the switcher does: the full form split
    // over the cell's two lines (the name over its G-code name and
    // temperature), else the G-code name over the temperature. Target mode
    // names it as the surface's nozzle row does ("Tool 2" or "2", over the
    // temperature), so the picker never disagrees with "Nozzle 2" beside it.
    // Every cell reads in one form.
    auto fits = [&](const std::pair<std::string, std::string>& lines) {
        return text_width(lines.first.c_str(), body) <= label_room &&
               text_width(lines.second.c_str(), small) <= label_room;
    };
    std::vector<std::pair<std::string, std::string>> lines(static_cast<size_t>(heads));
    if (mode_ == Mode::Target) {
        bool names = true;
        for (int i = 0; i < heads && names; ++i) {
            names = fits(model.target_lines(i, true));
        }
        for (int i = 0; i < heads; ++i) {
            lines[static_cast<size_t>(i)] = model.target_lines(i, names);
        }
    } else {
        const ToolLabelForm form = pick_tool_label_form([&](ToolLabelForm f) {
            for (int i = 0; i < heads; ++i) {
                if (!fits(model.picker_lines(i, f)))
                    return false;
            }
            return true;
        });
        for (int i = 0; i < heads; ++i) {
            lines[static_cast<size_t>(i)] = model.picker_lines(i, form);
        }
    }
    for (int i = 0; i < heads; ++i) {
        copy_string_if_changed(model.pick_pool().at(i),
                               lines[static_cast<size_t>(i)].first.c_str());
        copy_string_if_changed(model.pick_sub_pool().at(i),
                               lines[static_cast<size_t>(i)].second.c_str());
    }

    // The card is as wide as its grid, or as the footer's buttons at a grid
    // cell each where a narrow grid (one or two heads) would squeeze them.
    const int buttons = mode_ == Mode::Target ? 0 : tool_switcher_actions_offered().cells();
    const int footer_w = buttons > 0 ? buttons * fit.cell + (buttons - 1) * kToolRailGap : 0;
    const int inner_w = std::max(fit.width, footer_w);
    lv_obj_set_width(menu_card, inner_w + pad_w);
    const int button_w = (inner_w - (buttons > 1 ? kToolRailGap : 0)) / std::max(buttons, 1);
    set_int_if_changed(&s_feeder_label_subject, static_cast<int>(feeder_label_form_for(button_w)));

    // Scroll the highlighted head into view when the grid scrolls.
    const int shown = mode_ == Mode::Target ? TargetTool::instance().head() : model.mounted_head();
    if (fit.scrolls && shown >= 0 && shown < heads) {
        lv_obj_update_layout(grid);
        const std::string cell_name = fmt::format("tool_picker_cell_{}", shown);
        if (lv_obj_t* cell = lv_obj_find_by_name(grid, cell_name.c_str())) {
            lv_obj_scroll_to_view(cell, LV_ANIM_OFF);
        }
    }

    spdlog::debug("[ToolPicker] {} heads in {}x{} cells of {} px, {} px of {}x{}{}", heads,
                  fit.cols, fit.rows, fit.cell, fit.max_height, avail_w, avail_h,
                  fit.scrolls ? ", scrolling" : "");
}

bool open_target_tool_picker(lv_obj_t* anchor) {
    if (!anchor) {
        return false;
    }
    return tool_picker().open(lv_obj_get_screen(anchor), anchor, ContextMenu::AnchorAlign::Center,
                              ToolPicker::Mode::Target,
                              [](int head) { TargetTool::instance().pick_head(head); });
}

} // namespace helix::ui
