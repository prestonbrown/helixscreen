// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_switcher_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix/ui/text_metrics.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_psram_attr.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "theme_manager.h"
#include "tool_change_request.h"
#include "tool_heads_model.h"
#include "tool_picker.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {

using helix::xml::IndexedSubjectPool;

// The published verdict and per-head data, read by panel_widget_tool_switcher.xml
// and the picker. Module-level, not members: they belong to the widget KIND,
// rows read the CURRENT values the moment a <repeat> creates them, and
// registration rides SubjectInitializer's panel-subject phase so they exist
// before any XML that binds them is parsed. Mode and label form are the
// ToolSwitcherMode and ToolLabelForm enum values.
HELIX_PSRAM_BSS lv_subject_t s_mode_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cols_subject{};
HELIX_PSRAM_BSS lv_subject_t s_rows_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cell_w_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cell_h_subject{};
HELIX_PSRAM_BSS lv_subject_t s_rail_h_subject{};
HELIX_PSRAM_BSS lv_subject_t s_rail_w_subject{};
HELIX_PSRAM_BSS lv_subject_t s_detail_subject{};
HELIX_PSRAM_BSS lv_subject_t s_detail_rows_subject{};
HELIX_PSRAM_BSS lv_subject_t s_label_form_subject{};
HELIX_PSRAM_BSS lv_subject_t s_park_supported_subject{};
HELIX_PSRAM_BSS lv_subject_t s_feeder_supported_subject{};
// The rail feeder cell's FeederLabelForm.
HELIX_PSRAM_BSS lv_subject_t s_feeder_label_subject{};
HELIX_PSRAM_BSS lv_subject_t s_change_blocked_subject{};
// The mounted head as the Pair and Single faces read it.
HELIX_PSRAM_BSS lv_subject_t s_face_label_subject{};
HELIX_PSRAM_BSS char s_face_label_buf[48] = {};
HELIX_PSRAM_BSS lv_subject_t s_face_sub_subject{};
HELIX_PSRAM_BSS char s_face_sub_buf[16] = {};
HELIX_PSRAM_BSS lv_subject_t s_face_color_subject{};
HELIX_PSRAM_BSS lv_subject_t s_face_has_color_subject{};
HELIX_PSRAM_BSS lv_subject_t s_face_colors_subject{};
HELIX_PSRAM_BSS char s_face_colors_buf[96] = {};
HELIX_PSRAM_BSS lv_subject_t s_face_finish_subject{};
HELIX_PSRAM_BSS char s_face_finish_buf[160] = {};
HELIX_PSRAM_BSS lv_subject_t s_face_material_subject{};
HELIX_PSRAM_BSS char s_face_material_buf[24] = {};
HELIX_PSRAM_BSS lv_subject_t s_face_band_text_subject{};
// The face's size: its cell along the long axis, the whole view across it.
HELIX_PSRAM_BSS lv_subject_t s_face_w_subject{};
HELIX_PSRAM_BSS lv_subject_t s_face_h_subject{};
// The tile's filament band (rail segments and the face): its height and
// whether it carries the material name (tool_switcher_band()).
HELIX_PSRAM_BSS lv_subject_t s_band_h_subject{};
HELIX_PSRAM_BSS lv_subject_t s_band_labelled_subject{};
// 1 where every cell shown holds its second line (a segment's sub, the face's
// temperature) below its label, on both axes.
HELIX_PSRAM_BSS lv_subject_t s_two_line_subject{};
// The rail's share of the heads: all of them where the rail can draw them,
// none where the tile shows the face instead.
HELIX_PSRAM_BSS lv_subject_t s_rail_heads_subject{};
helix::WidgetSubjects s_subjects{"ToolSwitcherWidgetSubjects"};

// Per detail row, in display order (tool_switcher_detail_order()).
IndexedSubjectPool s_detail_text{"tool_switcher_detail", IndexedSubjectPool::Type::String, 96,
                                 "_text"};
IndexedSubjectPool s_detail_color{"tool_switcher_detail", IndexedSubjectPool::Type::Color, 0,
                                  "_color"};
IndexedSubjectPool s_detail_has_color{"tool_switcher_detail", IndexedSubjectPool::Type::Int, 0,
                                      "_has_color"};
IndexedSubjectPool s_detail_colors{"tool_switcher_detail", IndexedSubjectPool::Type::String, 96,
                                   "_colors"};
IndexedSubjectPool s_detail_hot{"tool_switcher_detail", IndexedSubjectPool::Type::Int, 0, "_hot"};
IndexedSubjectPool s_detail_main{"tool_switcher_detail", IndexedSubjectPool::Type::Int, 0, "_main"};

IndexedSubjectPool* const s_pools[] = {&s_detail_text,   &s_detail_color, &s_detail_has_color,
                                       &s_detail_colors, &s_detail_hot,   &s_detail_main};

void tool_switcher_widget_init_subjects() {
    s_subjects.ensure_init(
        [] {
            lv_subject_init_int(&s_mode_subject, static_cast<int>(helix::ToolSwitcherMode::Single));
            s_subjects.publish("tool_switcher_mode",
                               &s_mode_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_cols_subject, 0);
            s_subjects.publish("tool_switcher_cols",
                               &s_cols_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_rows_subject, 0);
            s_subjects.publish("tool_switcher_rows",
                               &s_rows_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_cell_w_subject, 0);
            s_subjects.publish("tool_switcher_cell_w",
                               &s_cell_w_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_cell_h_subject, 0);
            s_subjects.publish("tool_switcher_cell_h",
                               &s_cell_h_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_rail_h_subject, 0);
            s_subjects.publish("tool_switcher_rail_h",
                               &s_rail_h_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_rail_w_subject, 0);
            s_subjects.publish("tool_switcher_rail_w",
                               &s_rail_w_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_detail_subject, 0);
            s_subjects.publish("tool_switcher_detail",
                               &s_detail_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_detail_rows_subject, 0);
            s_subjects.publish(
                "tool_switcher_detail_rows",
                &s_detail_rows_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_label_form_subject, 0);
            s_subjects.publish(
                "tool_switcher_label_form",
                &s_label_form_subject); // SUBJECT_OK: published for the rail XML (#1784)
            lv_subject_init_int(&s_park_supported_subject, 0);
            s_subjects.publish("tool_switcher_park_supported", &s_park_supported_subject);
            lv_subject_init_int(&s_feeder_supported_subject, 0);
            s_subjects.publish("tool_switcher_feeder_supported", &s_feeder_supported_subject);
            lv_subject_init_int(&s_feeder_label_subject, 0);
            s_subjects.publish("tool_switcher_feeder_label", &s_feeder_label_subject);
            lv_subject_init_int(&s_change_blocked_subject, 0);
            s_subjects.publish("tool_switcher_change_blocked", &s_change_blocked_subject);
            lv_subject_init_string(&s_face_label_subject, s_face_label_buf, nullptr,
                                   sizeof(s_face_label_buf), "");
            s_subjects.publish("tool_switcher_face_label", &s_face_label_subject);
            lv_subject_init_string(&s_face_sub_subject, s_face_sub_buf, nullptr,
                                   sizeof(s_face_sub_buf), "");
            s_subjects.publish("tool_switcher_face_sub", &s_face_sub_subject);
            lv_subject_init_color(&s_face_color_subject, lv_color_hex(0));
            s_subjects.publish("tool_switcher_face_color", &s_face_color_subject);
            lv_subject_init_int(&s_face_has_color_subject, 0);
            s_subjects.publish("tool_switcher_face_has_color", &s_face_has_color_subject);
            lv_subject_init_string(&s_face_colors_subject, s_face_colors_buf, nullptr,
                                   sizeof(s_face_colors_buf), "");
            s_subjects.publish("tool_switcher_face_colors", &s_face_colors_subject);
            lv_subject_init_string(&s_face_finish_subject, s_face_finish_buf, nullptr,
                                   sizeof(s_face_finish_buf), "");
            s_subjects.publish("tool_switcher_face_finish", &s_face_finish_subject);
            lv_subject_init_string(&s_face_material_subject, s_face_material_buf, nullptr,
                                   sizeof(s_face_material_buf), "");
            s_subjects.publish("tool_switcher_face_material", &s_face_material_subject);
            lv_subject_init_color(&s_face_band_text_subject, lv_color_hex(0));
            s_subjects.publish("tool_switcher_face_band_text", &s_face_band_text_subject);
            lv_subject_init_int(&s_face_w_subject, LV_PCT(100));
            s_subjects.publish("tool_switcher_face_w", &s_face_w_subject);
            lv_subject_init_int(&s_face_h_subject, LV_PCT(100));
            s_subjects.publish("tool_switcher_face_h", &s_face_h_subject);
            lv_subject_init_int(&s_band_h_subject, helix::tool_switcher_strip_px(0));
            s_subjects.publish("tool_switcher_band_h", &s_band_h_subject);
            lv_subject_init_int(&s_band_labelled_subject, 0);
            s_subjects.publish("tool_switcher_band_labelled", &s_band_labelled_subject);
            lv_subject_init_int(&s_two_line_subject, 0);
            s_subjects.publish("tool_switcher_two_line", &s_two_line_subject);
            lv_subject_init_int(&s_rail_heads_subject, 0);
            s_subjects.publish("tool_switcher_rail_heads", &s_rail_heads_subject);
            // Every detail row exists before any XML is parsed; publish_heads()
            // grows the pools past this before it publishes a count that needs
            // more.
            for (IndexedSubjectPool* pool : s_pools) {
                pool->ensure_size(helix::ToolSwitcherWidget::kMaxRailHeads);
            }
        },
        [] {
            for (IndexedSubjectPool* pool : s_pools) {
                pool->reclaim();
            }
        });
}

void set_int_if_changed(lv_subject_t* subject, int value) {
    if (lv_subject_get_int(subject) != value) {
        lv_subject_set_int(subject, value);
    }
}

void set_color_if_changed(lv_subject_t* subject, uint32_t rgb) {
    if ((lv_color_to_u32(lv_subject_get_color(subject)) & 0xFFFFFF) != rgb) {
        lv_subject_set_color(subject, lv_color_hex(rgb));
    }
}

/// Resolve a responsive token to pixels, with a fallback.
int resolve_px_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// A head count no box can lay out as a rail: the decision then picks the
/// face, whose sheet lists every head.
constexpr int kUnrailableHeads = 1 << 20;

/// A detail line: the head, then what it is loaded with and how hot it is,
/// leaving out whichever of the two is unknown.
std::string detail_line(const std::string& head, const std::string& material,
                        const std::string& temp) {
    if (!material.empty() && !temp.empty())
        return fmt::format(lv_tr("{} · {} · {}"), head, material, temp);
    if (!material.empty() || !temp.empty())
        return fmt::format(lv_tr("{} · {}"), head, material.empty() ? temp : material);
    return head;
}

} // namespace

namespace helix {

// Static instance for event callback routing
ToolSwitcherWidget* ToolSwitcherWidget::s_active_instance = nullptr;

void register_tool_switcher_widget() {
    register_widget_factory("tool_switcher", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ToolSwitcherWidget>(ps);
    });
    register_widget_subjects("tool_switcher", tool_switcher_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tool_switcher_segment_cb",
                             ToolSwitcherWidget::tool_segment_cb);
    lv_xml_register_event_cb(nullptr, "tool_switcher_face_cb", ToolSwitcherWidget::tool_face_cb);
    lv_xml_register_event_cb(nullptr, "tool_switcher_park_cb", ToolSwitcherWidget::tool_park_cb);
    lv_xml_register_event_cb(nullptr, "tool_switcher_feeder_cb",
                             ToolSwitcherWidget::tool_feeder_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : printer_state_(printer_state) {
    // A widget built outside the panel-subject phase still publishes into live
    // subjects. A no-op once they exist.
    ui::ToolHeadsModel::instance().init_subjects();
    ui::ToolPicker::init_subjects();
    tool_switcher_widget_init_subjects();
}

std::string ToolSwitcherWidget::tool_text(const ToolInfo& tool, ui::ToolLabelForm form) {
    return ui::active_tool_label(tool.name, tool.index, form);
}

std::string ToolSwitcherWidget::name_text(const ToolInfo& tool) {
    return ui::active_tool_name_label(tool.name, tool.index);
}

std::string ToolSwitcherWidget::short_text(const ToolInfo& tool) {
    return tool_text(tool, ui::ToolLabelForm::Code);
}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    forget_heads_listener();
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    install_delete_hook(widget_obj);
    s_active_instance = this;

    auto token = lifetime_.token();

    // The heads' data (labels' inputs, filament, temperatures, the mounted
    // head) comes from the shared model; a change re-runs the whole size path
    // at the size the tile holds, a temperature only its head.
    auto& model = ui::ToolHeadsModel::instance();
    if (heads_listener_ != 0) {
        model.remove_listener(heads_listener_);
    }
    heads_listener_ = model.add_listener(
        [this, token]() {
            if (!token.expired())
                publish();
        },
        [this, token](int head) {
            if (!token.expired())
                publish_head_temp(head);
        });
    model.attach(printer_state_);

    // Re-publish the gate on every print-state transition. PanelWidget
    // instances are RECYCLED across home-panel rebuilds, so registering here
    // (rather than only reacting to on_size_changed) is what keeps a reused
    // instance from carrying the previous screen's gating. print_lifecycle
    // rather than print_state_enum: the gate refuses during Preparing, and the
    // raw enum does not move on the Idle -> Preparing edge.
    //
    // Takes the lifetime token. print_lifecycle is one of PrinterPrintState's
    // static subjects, torn down by deinit_subjects() between test cases, and an
    // ObserverGuard that outlives that cycle calls lv_observer_remove() on freed
    // memory (#705).
    print_state_observer_ = helix::ui::observe<int>(
        printer_state_.print_state().get_print_lifecycle_subject(), this,
        [token](ToolSwitcherWidget* self, int /*state*/) {
            if (token.expired())
                return;
            self->refresh_print_gating();
        },
        printer_state_.print_state().get_static_subjects_lifetime());

    // Spools laid on the bed block a head change as a print does.
    spool_latch_observer_ = helix::ui::observe<int>(
        printer_state_.print_state().get_spool_latch_subject(), this,
        [token](ToolSwitcherWidget* self, int /*latched*/) {
            if (token.expired())
                return;
            self->refresh_print_gating();
        },
        printer_state_.print_state().get_static_subjects_lifetime());

    // A recycled instance keeps the size it was last granted.
    publish();
}

void ToolSwitcherWidget::detach() {
    lifetime_.invalidate();
    hide_mount_picker();
    forget_heads_listener();
    print_state_observer_.reset();
    spool_latch_observer_.reset();
    uninstall_delete_hook();
    forget_tile_widgets();
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::forget_heads_listener() {
    if (heads_listener_ != 0) {
        ui::ToolHeadsModel::instance().remove_listener(heads_listener_);
        heads_listener_ = 0;
    }
}

void ToolSwitcherWidget::hide_mount_picker() {
    if (auto* picker = ui::ContextMenu::active_as<ui::ToolPicker>();
        picker && picker->mode() == ui::ToolPicker::Mode::Mount) {
        picker->hide();
    }
}

bool ToolSwitcherWidget::has_overlay_open() const {
    auto* picker = ui::ContextMenu::active_as<ui::ToolPicker>();
    return picker && picker->mode() == ui::ToolPicker::Mode::Mount;
}

void ToolSwitcherWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the pending deferred observer
    // callbacks and drop the cached pointers only. The observers themselves
    // stay registered on their (still live) subjects until detach() or the
    // destructor resets them — every callback checks its token first.
    lifetime_.invalidate();
    forget_tile_widgets();
}

void ToolSwitcherWidget::forget_tile_widgets() {
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

bool ToolSwitcherWidget::fits_at(int width_px, int height_px) const {
    // The smallest mode is one face, which must hold a touch target.
    const int inset = resolve_px_token("space_md", 10);
    const int min_target = resolve_px_token("touch_target_min", 48);
    return width_px - 2 * inset >= min_target && height_px - 2 * inset >= min_target;
}

void ToolSwitcherWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                         int height_px) {
    current_width_px_ = width_px;
    current_height_px_ = height_px;
    publish();
}

void ToolSwitcherWidget::publish() {
    refresh_action_support();
    refresh_print_gating();
    publish_layout();
    publish_heads();
}

void ToolSwitcherWidget::publish_layout() {
    const auto& heads = ToolState::instance().physical_heads();
    const int count = static_cast<int>(heads.size());
    const int inset = resolve_px_token("space_md", 10);
    const int min_target = resolve_px_token("touch_target_min", 48);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int body_px = line_px(body);
    const int small_px = line_px(small);
    const int content_w = current_width_px_ - 2 * inset;
    const int content_h = current_height_px_ - 2 * inset;

    // A printer with more heads than the rail draws gets the face, whose
    // picker lists every head.
    const int decided_heads = count > kMaxRailHeads ? kUnrailableHeads : count;
    // Only the action cells the backend offers are reserved.
    ToolSwitcherActions actions;
    actions.park = lv_subject_get_int(&s_park_supported_subject) != 0;
    actions.feeder = lv_subject_get_int(&s_feeder_supported_subject) != 0;
    layout_ = decide_tool_switcher_layout(decided_heads, content_w, content_h, min_target,
                                          kToolRailGap, body_px, small_px, actions);
    const bool rail =
        layout_.mode == ToolSwitcherMode::RailWithActions || layout_.mode == ToolSwitcherMode::Rail;

    // A segment's label has its cell less space_xs on each side. Rail
    // segments read "Tool 1" (or the configured name) over "T0" where every
    // head's two lines fit, else "T0"; the face reads "Tool 1 (T0)" where
    // every head's fits, else "T0".
    const int label_room = layout_.cell_w - 2 * resolve_px_token("space_xs", 4);
    const auto& model = ui::ToolHeadsModel::instance();
    label_form_ = ui::pick_tool_label_form([&](ui::ToolLabelForm form) {
        for (int i = 0; i < count; ++i) {
            const ToolInfo tool = model.head_tool(i);
            const std::string code = short_text(tool);
            int widest = 0;
            if (form == ui::ToolLabelForm::Code) {
                widest = ui::text_width(code.c_str(), body);
            } else if (rail) {
                widest = std::max(ui::text_width(name_text(tool).c_str(), body),
                                  ui::text_width(code.c_str(), body));
            } else {
                widest = ui::text_width(tool_text(tool, form).c_str(), body);
            }
            if (widest > label_room)
                return false;
        }
        return true;
    });

    set_int_if_changed(&s_mode_subject, static_cast<int>(layout_.mode));
    set_int_if_changed(&s_cols_subject, layout_.cols);
    set_int_if_changed(&s_rows_subject, layout_.rows);
    set_int_if_changed(&s_cell_w_subject, layout_.cell_w);
    set_int_if_changed(&s_cell_h_subject, layout_.cell_h);
    set_int_if_changed(&s_rail_h_subject, layout_.rail_h);
    set_int_if_changed(&s_rail_w_subject, layout_.rail_w);
    set_int_if_changed(&s_detail_subject, layout_.detail ? 1 : 0);
    const ToolSwitcherBand band =
        tool_switcher_band(layout_.cell_h, small_px, resolve_px_token("space_xxs", 2));
    set_int_if_changed(&s_band_h_subject, band.px);
    set_int_if_changed(&s_band_labelled_subject, band.labelled ? 1 : 0);
    set_int_if_changed(&s_feeder_label_subject,
                       static_cast<int>(ui::feeder_label_form_for(layout_.cell_w)));

    // The face takes its cell along a Pair's long axis and fills the view
    // across it, so it matches Park even where the box the tile was told
    // differs from the one it lays out by a pixel.
    const bool pair = layout_.mode == ToolSwitcherMode::Pair;
    set_int_if_changed(&s_face_w_subject,
                       pair && layout_.pair_horizontal ? layout_.cell_w : LV_PCT(100));
    set_int_if_changed(&s_face_h_subject,
                       pair && !layout_.pair_horizontal ? layout_.cell_h : LV_PCT(100));
    set_int_if_changed(&s_label_form_subject, static_cast<int>(label_form_));

    spdlog::debug("[ToolSwitcher] {}x{} box, {} heads: mode {} {}x{} cells {}x{} rail_h {} "
                  "detail {} form {}",
                  content_w, content_h, count, static_cast<int>(layout_.mode), layout_.cols,
                  layout_.rows, layout_.cell_w, layout_.cell_h, layout_.rail_h, layout_.detail,
                  static_cast<int>(label_form_));
}

std::string ToolSwitcherWidget::head_temp_text(int i) const {
    return ui::ToolHeadsModel::instance().head_temp_text(i);
}

bool ToolSwitcherWidget::head_hot(int i) const {
    return ui::ToolHeadsModel::instance().head_hot(i);
}

std::string ToolSwitcherWidget::detail_head_text(int i, const ToolInfo& tool, bool main) const {
    // The mounted head's line is in the body font and reads
    // "Tool 2 (T1) · PLA · 215°C" where the content box is wide enough, else
    // "T1 · ..."; the rest are small and always read "T0 · ...".
    const std::string& material = ui::ToolHeadsModel::instance().filament(i).material;
    const std::string temp = head_temp_text(i);
    if (main) {
        std::string text = detail_line(tool_text(tool, ui::ToolLabelForm::Full), material, temp);
        const int content_w = current_width_px_ - 2 * resolve_px_token("space_md", 10);
        if (ui::text_width(text.c_str(), theme_manager_get_font("font_body")) <= content_w)
            return text;
    }
    return detail_line(short_text(tool), material, temp);
}

void ToolSwitcherWidget::publish_heads() {
    if (!s_subjects.initialized())
        return;
    auto& model = ui::ToolHeadsModel::instance();
    const int heads = model.head_count();
    const int active = model.mounted_head();
    const std::vector<int> order = tool_switcher_detail_order(heads, active);
    // Every detail row gets its slots before a count that reaches it is
    // published; the model sizes the per-head pools.
    for (IndexedSubjectPool* pool : s_pools) {
        pool->ensure_size(order.size());
    }
    model.label_pool().ensure_size(static_cast<size_t>(heads));
    model.sub_pool().ensure_size(static_cast<size_t>(heads));

    for (int i = 0; i < heads; ++i) {
        const ToolInfo tool = model.head_tool(i);
        // Full: the configured name or "Tool 1", over "T0". Code: "T0" over
        // the temperature, shown where the segment is tall enough.
        std::string label = short_text(tool);
        std::string sub = head_temp_text(i);
        if (label_form_ == ui::ToolLabelForm::Full) {
            label = name_text(tool);
            sub = short_text(tool);
        }
        ui::copy_string_if_changed(model.label_pool().at(i), label.c_str());
        ui::copy_string_if_changed(model.sub_pool().at(i), sub.c_str());
    }
    set_int_if_changed(&s_rail_heads_subject, heads <= kMaxRailHeads ? heads : 0);

    // Detail rows, mounted head first.
    const int rows = static_cast<int>(order.size());
    const int content_w = current_width_px_ - 2 * resolve_px_token("space_md", 10);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int body_px = line_px(body);
    const int small_px = line_px(small);
    for (int r = 0; r < rows; ++r) {
        const int i = order[r];
        const bool main = r == 0 && (i == active || i == kToolSwitcherEmptyCarriageRow);
        std::string text;
        if (i == kToolSwitcherEmptyCarriageRow) {
            // The hint goes where the block is too narrow for it.
            text = lv_tr("Carriage empty · tap a head");
            if (ui::text_width(text.c_str(), body) > content_w)
                text = lv_tr("Carriage empty");
        } else {
            text = detail_head_text(i, model.head_tool(i), main);
        }
        const ui::HeadFilament& f = model.filament(i);
        const bool has_color = i >= 0 && f.has_color;
        ui::copy_string_if_changed(s_detail_text.at(r), text.c_str());
        set_int_if_changed(s_detail_main.at(r), main ? 1 : 0);
        set_int_if_changed(s_detail_hot.at(r), head_hot(i) ? 1 : 0);
        set_int_if_changed(s_detail_has_color.at(r), has_color ? 1 : 0);
        if (has_color) {
            set_color_if_changed(s_detail_color.at(r), f.color_rgb);
        }
        ui::copy_string_if_changed(s_detail_colors.at(r), has_color ? f.colors.c_str() : "");
    }

    int fit = 0;
    if (layout_.detail) {
        const int content_h = current_height_px_ - 2 * resolve_px_token("space_md", 10);
        fit = tool_switcher_detail_rows_that_fit(rows, content_h - layout_.rail_h - kToolRailGap,
                                                 body_px, small_px);
    }
    set_int_if_changed(&s_detail_rows_subject, fit);

    // The face: the mounted head by its full name where the cell has room,
    // else the G-code name. With no tools there is no head to name, and an
    // out-of-range active index names none the list holds; "?" keeps the face
    // visibly occupied.
    const int room = layout_.cell_w - 2 * resolve_px_token("space_xs", 4);
    std::string face;
    std::string face_sub;
    ui::HeadFilament face_filament;
    if (heads == 0) {
        // Nothing discovered yet: the face names nothing.
    } else if (active >= 0 && active < heads) {
        const ToolInfo mounted = model.head_tool(active);
        face = tool_text(mounted, ui::ToolLabelForm::Full);
        if (ui::text_width(face.c_str(), body) > room)
            face = short_text(mounted);
        face_sub = head_temp_text(active);
        face_filament = model.filament(active);
    } else if (active == -1) {
        face = lv_tr("Choose head");
        if (ui::text_width(face.c_str(), body) > room)
            face = lv_tr("None");
    } else {
        face = "?";
    }
    ui::copy_string_if_changed(&s_face_label_subject, face.c_str());
    ui::copy_string_if_changed(&s_face_sub_subject, face_sub.c_str());
    set_int_if_changed(&s_face_has_color_subject, face_filament.has_color ? 1 : 0);
    if (face_filament.has_color) {
        set_color_if_changed(&s_face_color_subject, face_filament.color_rgb);
        set_color_if_changed(
            &s_face_band_text_subject,
            lv_color_to_u32(theme_manager_get_readable_on(lv_color_hex(face_filament.color_rgb))) &
                0xFFFFFF);
    }
    ui::copy_string_if_changed(&s_face_colors_subject, face_filament.colors.c_str());
    ui::copy_string_if_changed(&s_face_finish_subject, face_filament.finish.c_str());
    ui::copy_string_if_changed(&s_face_material_subject, face_filament.material.c_str());

    publish_two_line();
}

void ToolSwitcherWidget::publish_head_temp(int i) {
    if (!s_subjects.initialized())
        return;
    auto& model = ui::ToolHeadsModel::instance();
    const int heads = model.head_count();
    if (i < 0 || i >= heads || static_cast<size_t>(i) >= model.sub_pool().size())
        return;
    const int active = model.mounted_head();

    const std::string temp = head_temp_text(i);
    if (label_form_ == ui::ToolLabelForm::Code) {
        ui::copy_string_if_changed(model.sub_pool().at(i), temp.c_str());
    }

    const std::vector<int> order = tool_switcher_detail_order(heads, active);
    const int rows = static_cast<int>(std::min(order.size(), s_detail_text.size()));
    for (int r = 0; r < rows; ++r) {
        if (order[r] != i)
            continue;
        const bool main = r == 0 && i == active;
        ui::copy_string_if_changed(s_detail_text.at(r),
                                   detail_head_text(i, model.head_tool(i), main).c_str());
        set_int_if_changed(s_detail_hot.at(r), head_hot(i) ? 1 : 0);
        break;
    }

    if (i == active) {
        ui::copy_string_if_changed(&s_face_sub_subject, temp.c_str());
    }

    publish_two_line();
}

void ToolSwitcherWidget::publish_two_line() {
    // A second line shows only where the cell is tall enough for both lines
    // over its band, and every shown second line is narrow enough.
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int body_px = line_px(body);
    const int small_px = line_px(small);
    const int room = layout_.cell_w - 2 * resolve_px_token("space_xs", 4);
    const int band_px = lv_subject_get_int(&s_band_h_subject);
    bool two_line = layout_.cell_h - band_px >= body_px + small_px;
    const bool rail =
        layout_.mode == ToolSwitcherMode::RailWithActions || layout_.mode == ToolSwitcherMode::Rail;
    auto fits = [&](const char* text) { return ui::text_width(text, small) <= room; };
    if (rail) {
        const int heads = lv_subject_get_int(&s_rail_heads_subject);
        for (int i = 0; i < heads && two_line; ++i) {
            two_line = fits(lv_subject_get_string(ui::ToolHeadsModel::instance().sub_pool().at(i)));
        }
    } else {
        two_line = two_line && fits(lv_subject_get_string(&s_face_sub_subject));
    }
    set_int_if_changed(&s_two_line_subject, two_line ? 1 : 0);
}

void ToolSwitcherWidget::on_active_tool_changed(int tool_index) {
    // A head fed by several lanes is named for the lane it has mounted, so a
    // change can re-decide the label form as well as move the highlight.
    publish_layout();
    publish_heads();
    spdlog::debug("[ToolSwitcher] Active tool changed to T{}", tool_index);
}

// ============================================================================
// The sheet: every head and Park, raised from the face or the mounted segment
// ============================================================================

void ToolSwitcherWidget::show_tool_picker() {
    if (has_overlay_open() || !parent_screen_ || !widget_obj_) {
        return;
    }

    // Refuse before opening a list in which every entry is a guaranteed-failure
    // dead end.
    const AmsError refusal = tool_change_refusal();
    if (!refusal.success()) {
        spdlog::info("[ToolSwitcher] Picker refused: {}", refusal.technical_msg);
        helix::ui::notify_ams_warning(refusal);
        return;
    }

    refresh_action_support();

    // The card hangs off the widget tile's left edge; ContextMenu keeps it on
    // screen where the grid is wider than the room to the tile's right.
    ui::tool_picker().open(parent_screen_, widget_obj_, helix::ui::ContextMenu::AnchorAlign::Left,
                           ui::ToolPicker::Mode::Mount, [](int head) {
                               if (s_active_instance) {
                                   s_active_instance->handle_tool_selected(head);
                               }
                           });
}

// ============================================================================
// Tool selection with safety gate
// ============================================================================

AmsError ToolSwitcherWidget::tool_change_refusal() const {
    // No backend means a plain Tn / macro path with no firmware macro that could
    // hide a home — the documented argument for passing false here.
    AmsBackend* backend = AmsState::instance().get_backend();
    return helix::ui::tool_change_print_refusal(printer_state_.print_state().get_print_lifecycle(),
                                                backend && backend->filament_ops_self_home());
}

void ToolSwitcherWidget::refresh_print_gating() {
    // Not moves_machine's machine_motion_blocked: that holds through PAUSED,
    // where a head change is offered behind its confirmation.
    const bool blocked =
        !tool_change_refusal().success() || printer_state_.print_state().spool_latch_active();
    set_int_if_changed(&s_change_blocked_subject, blocked ? 1 : 0);
}

ToolSwitcherActions ToolSwitcherWidget::actions_offered() {
    return ui::tool_switcher_actions_offered();
}

void ToolSwitcherWidget::refresh_action_support() {
    const ToolSwitcherActions actions = actions_offered();
    set_int_if_changed(&s_park_supported_subject, actions.park ? 1 : 0);
    set_int_if_changed(&s_feeder_supported_subject, actions.feeder ? 1 : 0);
}

bool ToolSwitcherWidget::refuse_while_spools_on_bed() const {
    if (!printer_state_.print_state().spool_latch_active()) {
        return false;
    }
    spdlog::info("[ToolSwitcher] Refused: spools are on the bed");
    helix::ui::notify_ams_warning(helix::ui::spools_on_bed_refusal());
    return true;
}

bool ToolSwitcherWidget::refuse_machine_action(const char* technical,
                                               const char* while_printing) const {
    if (refuse_while_spools_on_bed()) {
        return true;
    }
    // Disabled while a job holds the machine, paused included (moves_machine).
    // A tap can still land between the job starting and the binding catching
    // up.
    if (job_holds_machine(printer_state_.print_state().get_print_lifecycle())) {
        spdlog::info("[ToolSwitcher] {}", technical);
        helix::ui::notify_ams_warning(
            AmsError(AmsResult::WRONG_STATE, technical, while_printing, ""));
        return true;
    }
    return false;
}

void ToolSwitcherWidget::handle_park_requested() {
    helix::ui::request_tool_park(printer_state_);
}

void ToolSwitcherWidget::handle_feeder_toggled() {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !backend->supports_feeder_control()) {
        return;
    }

    // The gripper is all that holds the filament: opening it under a print
    // drops the strand.
    if (refuse_machine_action("Feeder blocked: print in progress",
                              lv_tr("Cannot open or close the feeder while a print is active"))) {
        return;
    }

    const bool open = !backend->feeder_open();
    spdlog::info("[ToolSwitcher] {} the feeder", open ? "Opening" : "Closing");
    const AmsError err = backend->set_feeder_open(open);
    if (!err.success()) {
        helix::ui::notify_ams_error(err);
    }
}

void ToolSwitcherWidget::handle_tool_selected(int head_index) {
    auto& tool_state = ToolState::instance();

    const auto heads = tool_state.physical_heads();
    if (head_index < 0 || head_index >= static_cast<int>(heads.size())) {
        return;
    }
    // The head's own tool: the mounted one when this head is mounted, so a
    // mounted head is never re-selected through another of its lanes.
    const int tool_index =
        heads[static_cast<size_t>(head_index)].representative_tool(tool_state.active_tool_index());

    // The buttons are greyed by refresh_print_gating(), but a tap can still land
    // in the window between a print starting and the observer firing, so the
    // shared decision refuses here too.
    helix::ui::ToolChangeHooks hooks;
    // The published heads follow ToolState's active-tool subject, so a refused
    // change never moved the highlight. Resync anyway: a backend that got
    // partway before failing leaves the subject as the only truth.
    hooks.on_failed = [] {
        helix::ui::queue_update("ToolSwitcherWidget::resync_after_error", []() {
            if (s_active_instance) {
                s_active_instance->on_active_tool_changed(
                    ToolState::instance().active_tool_index());
            }
        });
    };
    helix::ui::offer_tool_change(printer_state_, tool_index, std::move(hooks));
}

// ============================================================================
// Static XML event callbacks (registered at startup, used in XML if needed)
// ============================================================================

void ToolSwitcherWidget::tool_park_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_park_cb");
    // The picker's Park row closes the picker like any other row.
    if (s_active_instance) {
        s_active_instance->hide_mount_picker();
        s_active_instance->handle_park_requested();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::tool_feeder_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_feeder_cb");
    // The picker stays open, so its feeder button shows the state it reaches.
    if (s_active_instance) {
        s_active_instance->handle_feeder_toggled();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::handle_segment_tapped(int head_index) {
    // The mounted head's segment raises the sheet, where Park is in Rail mode.
    if (head_index == ui::ToolHeadsModel::instance().mounted_head()) {
        show_tool_picker();
        return;
    }
    handle_tool_selected(head_index);
}

void ToolSwitcherWidget::tool_segment_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_segment_cb");
    // XML event user_data is the segment's index as a string.
    const char* index = static_cast<const char*>(lv_event_get_user_data(e));
    if (index) {
        // A picker cell closes the picker like any other choice, in either
        // mode; a rail cell raises it when its head is the mounted one.
        if (auto* picker = helix::ui::ContextMenu::active_as<ui::ToolPicker>()) {
            picker->choose(std::atoi(index));
        } else if (s_active_instance) {
            s_active_instance->handle_segment_tapped(std::atoi(index));
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::tool_face_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_face_cb");
    if (s_active_instance) {
        s_active_instance->show_tool_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
