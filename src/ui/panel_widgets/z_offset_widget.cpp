// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_widget.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_print_tune_overlay.h"
#include "ui_z_offset_indicator.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "value_status.h"
#include "widget_subjects.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

using helix::ui::measure_text_px;

namespace {

// The verdict and readings the widget publishes, read by panel_widget_z_offset.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
lv_subject_t s_rung_subject{};
lv_subject_t s_show_save_subject{};
lv_subject_t s_show_steppers_subject{};
lv_subject_t s_show_tool_label_subject{};
lv_subject_t s_status_subject{};
lv_subject_t s_value_text_subject{};
lv_subject_t s_caption_text_subject{};
lv_subject_t s_step_text_subject{};
lv_subject_t s_tool_text_subject{};
char s_value_text_buf[32];
char s_caption_text_buf[48];
char s_step_text_buf[16];
char s_tool_text_buf[32];
helix::WidgetSubjects s_subjects{"ZOffsetWidgetSubjects"};
// The instance bound to the live tile. Only it publishes and acts: a transient
// instance (a fits_at probe, a rebuild's outgoing widget) must not overwrite the
// tile's verdict.
helix::LiveTileOwner<helix::ZOffsetWidget> s_tile;

void z_offset_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Wide),
                               "z_offset_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_save_subject, 0, "z_offset_show_save", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_steppers_subject, 0, "z_offset_show_steppers", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_tool_label_subject, 0, "z_offset_show_tool_label",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_status_subject, 0, "z_offset_status", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_value_text_subject, s_value_text_buf, "", "z_offset_value_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_caption_text_subject, s_caption_text_buf, "",
                                  "z_offset_caption_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_step_text_subject, s_step_text_buf, "", "z_offset_step_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_tool_text_subject, s_tool_text_buf, "", "z_offset_tool_text",
                                  s_subjects);
    });
}

/// The readings every text is composed from.
struct Readings {
    std::optional<int> microns; ///< nullopt: a tool changer's tool has reported nothing
    int pending_delta_microns = 0;
    bool tool_changer = false;
};

/// The caption under the value: the unit, or the unsaved session delta. A tool
/// changer's value is a tool's own offset, which the machine-wide delta says nothing
/// about, so it keeps the unit.
std::string compose_caption(const Readings& r) {
    if (r.tool_changer || r.pending_delta_microns == 0)
        return lv_tr("mm Z-offset");
    char delta[24];
    std::snprintf(delta, sizeof(delta), "%+.3f", r.pending_delta_microns / 1000.0);
    char buf[48];
    std::snprintf(buf, sizeof(buf), lv_tr("%s unsaved"), delta);
    return buf;
}

std::string step_text(int idx) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%g", helix::zoffset::kZStepAmountsMm[idx]);
    return buf;
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* body;
    const lv_font_t* heading;
    const lv_font_t* icon;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_small"),
            theme_manager_get_font("font_body"), theme_manager_get_font("font_heading"),
            theme_manager_get_font("icon_font_xs")};
}

using helix::line_px;

/// The minimum content box each rung draws in. Mirrors panel_widget_z_offset.xml:
/// spacing is the XML's pad_row (space_xxs) and pad_column (space_xs); the gap
/// between the rows of the Full rung is the body's.
helix::RungNeeds measure_needs(bool tool_changer) {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    // The small rungs hold one short number under a glyph; they get the least margin.
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    // A box measured exactly renders as an overlap by a pixel.
    constexpr int kSlackPx = 1;
    const int button_px = theme_manager_get_spacing("button_height_sm");

    const int icon_w = measure_text_px(helix::ui::icon::lookup_codepoint("axis_z_arrow"), f.icon);
    const int icon_h = line_px(f.icon);
    const int xs_h = line_px(f.xs);
    const int small_h = line_px(f.small);
    const int body_h = line_px(f.body);
    const int heading_h = line_px(f.heading);

    // The widest values the labels can show, so the rung does not change as the
    // numbers do.
    const std::string wide2 = helix::format_z_offset_value(-8888, 2);
    const std::string wide3 = helix::format_z_offset_value(-8888, 3);
    Readings worst;
    worst.pending_delta_microns = -888;
    const std::string unit_caption = compose_caption({});
    const std::string delta_caption = compose_caption(worst);
    const int caption_w =
        std::max(measure_text_px(unit_caption, f.xs), measure_text_px(delta_caption, f.xs));
    // The indicator draws proportionally and survives shrinking; this is the least
    // width at which its tick labels still read.
    const int indicator_w = 3 * theme_manager_get_spacing("space_xl");

    helix::RungNeeds needs;
    needs.glance =
        rung_box(std::max(icon_w, measure_text_px(wide2, f.xs)) + tight, icon_h + xs_h + kSlackPx);
    needs.strip = rung_box(icon_w + col_gap + measure_text_px(wide3, f.xs) + tight,
                           std::max(icon_h, xs_h) + kSlackPx);
    // A tall half cell has the height for the unit under the value, which a short one
    // does not; that is what separates Column from Glance.
    needs.column = rung_box(std::max(icon_w, measure_text_px(wide3, f.xs)) + tight,
                            icon_h + 2 * xs_h + 2 * row_gap + comfort + kSlackPx);

    const int cell_row_w = icon_w + col_gap + measure_text_px(wide3, f.body);
    const int cell_w = std::max(cell_row_w, caption_w);
    const int cell_h = std::max(icon_h, body_h) + row_gap + xs_h;
    // Height gets the same margin: a 1x0.5 tile is one pixel short of this box, and
    // must stay the Strip.
    needs.cell = rung_box(cell_w + comfort, cell_h + comfort + kSlackPx);

    needs.wide = rung_box(cell_w + col_gap + indicator_w + col_gap + button_px + comfort,
                          std::max(cell_h, button_px) + comfort + kSlackPx);

    const int full_row_w = icon_w + col_gap + measure_text_px(wide3, f.heading);
    const std::string tool_label = helix::ui::lane_label(helix::ui::active_tool_noun(), 88);
    const int block_w =
        std::max({full_row_w, caption_w, tool_changer ? measure_text_px(tool_label, f.small) : 0});
    const int block_h =
        std::max(icon_h, heading_h) + row_gap + xs_h + (tool_changer ? row_gap + small_h : 0);
    const int stepper_w =
        2 * button_px + measure_text_px(step_text(0), f.small) + 2 * col_gap + comfort;
    needs.full = rung_box(std::max(block_w + col_gap + indicator_w, stepper_w) + comfort,
                          block_h + 2 * (row_gap + button_px) + kSlackPx);
    return needs;
}

} // namespace

namespace helix {

struct ZOffsetWidget::LiveState {
    Readings readings;
    bool save_available = false;
    int tool_index = 0;
    std::string tool_label;
};

ZOffsetWidget::ZOffsetWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

ZOffsetWidget::~ZOffsetWidget() {
    detach();
}

ZOffsetWidget::LiveState ZOffsetWidget::read_live() const {
    auto& tools = ToolState::instance();
    LiveState s;
    s.readings.tool_changer =
        lv_subject_get_int(tools.get_per_tool_axis_supported_subject(Axis::Z)) == 1;
    s.tool_index = lv_subject_get_int(tools.get_active_tool_subject());
    if (s.readings.tool_changer) {
        if (tools.tool_offset_known(s.tool_index, Axis::Z)) {
            s.readings.microns =
                static_cast<int>(std::lround(tools.tool_offset_mm(s.tool_index, Axis::Z) * 1000.0));
        }
        const auto* active = tools.active_tool();
        s.tool_label =
            active ? active->display_label : ui::lane_label(ui::active_tool_noun(), s.tool_index);
    } else {
        s.readings.microns = zoffset::displayed_z_offset_microns(printer_state_);
    }
    s.readings.pending_delta_microns = printer_state_.motion_state().get_pending_z_offset_delta();
    s.save_available = zoffset::save_available();
    return s;
}

ZOffsetVerdict ZOffsetWidget::decide(int width_px, int height_px, bool tool_changer) const {
    // The tree's own padding where it exists, and before it does (the load path asks
    // fits_at then) the root's. measure_needs() counts the gaps between the body's parts.
    const ContentChrome chrome = content_padding_or_estimate(root(), "space_xxs", "space_xxs");
    ZOffsetInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.tool_changer = tool_changer;
    in.needs = measure_needs(tool_changer);
    const auto& n = in.needs;
    spdlog::trace("[ZOffsetWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} tool_changer={}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, tool_changer);
    return decide_z_offset_layout(in);
}

void ZOffsetWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    auto& motion = printer_state_.motion_state();
    const auto lifetime = printer_state_.get_subjects_lifetime();
    const auto token = lifetime_.token();
    const auto on_change = [token](ZOffsetWidget* self, int) {
        if (token.expired())
            return;
        self->on_live_changed();
    };
    offset_observer_ =
        ui::observe<int>(motion.get_gcode_z_offset_subject(), this, on_change, lifetime);
    persisted_observer_ =
        ui::observe<int>(motion.get_persisted_z_offset_subject(), this, on_change, lifetime);
    persisted_valid_observer_ =
        ui::observe<int>(motion.get_persisted_z_offset_valid_subject(), this, on_change, lifetime);
    pending_observer_ =
        ui::observe<int>(motion.get_pending_z_offset_delta_subject(), this, on_change, lifetime);
    print_active_observer_ = ui::observe<int>(
        printer_state_.print_state().get_print_active_subject(), this, on_change, lifetime);
    // SubjectInitializer publishes this before any panel XML is created, so it exists
    // here in the app; a missing one means the init order broke.
    if (lv_subject_t* save = zoffset::get_save_available_subject()) {
        save_available_observer_ =
            ui::observe<int>(save, this, on_change, zoffset::get_save_available_lifetime());
    } else {
        spdlog::warn("[ZOffsetWidget] z_offset_save_available is not published; the value "
                     "will not follow unsaved offsets");
    }
    auto& tools = ToolState::instance();
    const auto tools_lifetime = tools.get_subjects_lifetime();
    tools_version_observer_ =
        ui::observe<int>(tools.get_tools_version_subject(), this, on_change, tools_lifetime);
    active_tool_observer_ =
        ui::observe<int>(tools.get_active_tool_subject(), this, on_change, tools_lifetime);
    per_tool_observer_ = ui::observe<int>(tools.get_per_tool_axis_supported_subject(Axis::Z), this,
                                          on_change, tools_lifetime);

    // A run of baby steps is bounded from where the tile began, as the Tune overlay's is
    // from the offset it opened on.
    session_base_mm_ = read_live().readings.microns.value_or(0) / 1000.0;
    indicator_microns_.reset();

    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish(read_live());
}

void ZOffsetWidget::detach() {
    lifetime_.invalidate();
    offset_observer_.reset();
    persisted_observer_.reset();
    persisted_valid_observer_.reset();
    pending_observer_.reset();
    print_active_observer_.reset();
    save_available_observer_.reset();
    tools_version_observer_.reset();
    active_tool_observer_.reset();
    per_tool_observer_.reset();
    if (ZOffsetWidget* displaced = s_tile.release(this)) {
        displaced->indicator_microns_.reset();
        displaced->relayout_for_granted_size();
        displaced->publish(displaced->read_live());
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ZOffsetWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px, int height_px) {
    const LiveState live = read_live();
    const bool tool_changer = live.readings.tool_changer;
    verdict_ = decide(width_px, height_px, tool_changer);
    decided_tool_changer_ = tool_changer;
    if (!s_tile.is_owner(this))
        return;

    lv_subject_set_int(&s_rung_subject, static_cast<int>(verdict_.rung));
    lv_subject_set_int(&s_show_save_subject, verdict_.show_save_row ? 1 : 0);
    lv_subject_set_int(&s_show_steppers_subject, verdict_.show_steppers ? 1 : 0);
    lv_subject_set_int(&s_show_tool_label_subject, verdict_.show_tool_label ? 1 : 0);
    publish(live);

    spdlog::debug("[ZOffsetWidget] on_size_changed {}x{} rung={} decimals={} save={} steppers={} "
                  "tool_label={} fits={} tool_changer={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.decimals,
                  verdict_.show_save_row, verdict_.show_steppers, verdict_.show_tool_label,
                  verdict_.fits, tool_changer);
}

bool ZOffsetWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content. Whether the printer is a
    // tool changer only changes the Full box, so one probe decides.
    return decide(width_px, height_px, false).fits;
}

void ZOffsetWidget::publish(const LiveState& live) {
    if (!s_tile.is_owner(this))
        return;
    const auto set = [](lv_subject_t* subject, const std::string& text) {
        if (text != lv_subject_get_string(subject))
            lv_subject_copy_string(subject, text.c_str());
    };
    set(&s_value_text_subject, format_z_offset_value(live.readings.microns, verdict_.decimals));
    set(&s_caption_text_subject, compose_caption(live.readings));
    set(&s_step_text_subject, step_text(zoffset::persisted_step_index()));
    set(&s_tool_text_subject, live.tool_label);
    lv_subject_set_int(&s_status_subject,
                       static_cast<int>(status::z_offset_status(live.save_available)));

    // A tool that has reported nothing leaves the indicator where it was: 0 would draw
    // a measurement it does not have.
    if (live.readings.microns && indicator_microns_ != live.readings.microns) {
        if (lv_obj_t* indicator = find_indicator()) {
            ui_z_offset_indicator_set_value(indicator, *live.readings.microns);
            indicator_microns_ = live.readings.microns;
        }
    }
}

lv_obj_t* ZOffsetWidget::find_indicator() const {
    return root() ? lv_obj_find_by_name(root(), "z_offset_indicator") : nullptr;
}

void ZOffsetWidget::on_live_changed() {
    const LiveState live = read_live();
    if (live.readings.tool_changer != decided_tool_changer_) {
        relayout_for_granted_size();
    }
    publish(live);
}

void ZOffsetWidget::step(int direction) {
    if (!verdict_.show_steppers || !s_tile.is_owner(this))
        return;
    const double amount = zoffset::kZStepAmountsMm[zoffset::persisted_step_index()];
    const double delta = direction * amount;
    const LiveState live = read_live();
    IMoonrakerAPI* api = get_moonraker_api();
    bool moved = false;
    if (live.readings.tool_changer) {
        moved = zoffset::adjust_tool_offset(api, &printer_state_, live.tool_index, delta);
    } else {
        const double current_mm = live.readings.microns.value_or(0) / 1000.0;
        // Travel is bounded from where a run of steps began: with nothing pending the
        // next step begins a new run.
        if (live.readings.pending_delta_microns == 0)
            session_base_mm_ = current_mm;
        const auto r = zoffset::adjust(api, &printer_state_, session_base_mm_, current_mm, delta);
        moved = !r.clamped_to_noop;
    }
    if (moved) {
        publish(read_live());
        if (lv_obj_t* indicator = find_indicator())
            ui_z_offset_indicator_flash_direction(indicator, direction);
    }
}

void ZOffsetWidget::cycle_step() {
    if (!verdict_.show_steppers || !s_tile.is_owner(this))
        return;
    const int count = static_cast<int>(std::size(zoffset::kZStepAmountsMm));
    zoffset::set_persisted_step_index((zoffset::persisted_step_index() + 1) % count);
    publish(read_live());
}

void ZOffsetWidget::save() {
    if (!verdict_.show_save_row || !s_tile.is_owner(this))
        return;
    const bool blocked =
        lv_subject_get_int(printer_state_.print_state().get_machine_motion_blocked_subject()) != 0;
    if (blocked || !zoffset::save_available())
        return;
    zoffset::save_dirty_offsets_shared();
}

void ZOffsetWidget::open_tune_overlay() {
    get_print_tune_overlay().show(parent_screen_, get_moonraker_api(), printer_state_);
}

void ZOffsetWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ZOffsetWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<ZOffsetWidget>(e)) {
        self->record_interaction();
        self->open_tune_overlay();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void register_z_offset_widget() {
    register_widget_factory("z_offset", [](const std::string&) {
        return std::make_unique<ZOffsetWidget>(get_printer_state());
    });
    register_widget_subjects("z_offset", z_offset_init_subjects);

    lv_xml_register_event_cb(nullptr, "z_offset_clicked_cb", ZOffsetWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "z_offset_step_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ZOffsetWidget] step_cb");
        // A button tap is not a tap on the tile, which opens the Tune overlay.
        lv_event_stop_bubbling(e);
        if (auto direction = ui::event_user_int(e); direction && s_tile.owner())
            s_tile.owner()->step(*direction);
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "z_offset_chip_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ZOffsetWidget] chip_cb");
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->cycle_step();
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "z_offset_save_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ZOffsetWidget] save_cb");
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->save();
        LVGL_SAFE_EVENT_CB_END();
    });
}

} // namespace helix
