// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "speed_flow_widget.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_print_tune_overlay.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "tune_controller.h"
#include "value_status.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

using helix::ui::measure_text_px;

namespace {

constexpr int kResetPct = 100;

// The verdict and readings the widget publishes, read by speed_flow_part.xml and
// panel_widget_speed_flow.xml. Module-level because the widget is single-instance and
// registration rides SubjectInitializer's panel-subject phase, so the subjects exist
// before any XML that binds them is parsed.
lv_subject_t s_rung_subject{};
lv_subject_t s_show_measured_subject{};
lv_subject_t s_show_steppers_subject{};
lv_subject_t s_speed_status_subject{};
lv_subject_t s_flow_status_subject{};
lv_subject_t s_speed_text_subject{};
lv_subject_t s_flow_text_subject{};
lv_subject_t s_speed_measured_subject{};
lv_subject_t s_flow_measured_subject{};
char s_speed_text_buf[32];
char s_flow_text_buf[32];
char s_speed_measured_buf[32];
char s_flow_measured_buf[32];
helix::WidgetSubjects s_subjects{"SpeedFlowWidgetSubjects"};
// The instance bound to the live tile. Only it publishes: a transient instance (a
// rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::SpeedFlowWidget> s_tile;

void speed_flow_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Wide),
                               "speed_flow_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_measured_subject, 0, "speed_flow_show_measured", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_steppers_subject, 0, "speed_flow_show_steppers", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_speed_status_subject, 0, "speed_flow_speed_status", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_flow_status_subject, 0, "speed_flow_flow_status", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_speed_text_subject, s_speed_text_buf, "",
                                  "speed_flow_speed_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_flow_text_subject, s_flow_text_buf, "", "speed_flow_flow_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_speed_measured_subject, s_speed_measured_buf, "",
                                  "speed_flow_speed_measured", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_flow_measured_subject, s_flow_measured_buf, "",
                                  "speed_flow_flow_measured", s_subjects);
    });
}

/// The four readings every text is composed from.
struct Readings {
    int speed_pct = 100;
    int flow_pct = 100;
    int live_velocity_mm_s = 0;
    int extruder_velocity_centimm_s = 0;
    float filament_diameter_mm = 1.75f;
};

struct Texts {
    std::string speed;
    std::string flow;
    std::string speed_measured;
    std::string flow_measured;
};

/// The percent sign dropped, for the rungs too narrow to carry it. Measured readings
/// keep their unit: a bare number would not say which axis it measures.
std::string without_percent(const std::string& text) {
    return text.empty() || text.back() != '%' ? text : text.substr(0, text.size() - 1);
}

/// Everything the labels show at `rung`, composed once for the widget to render and to
/// measure, so the ladder never decides on widths the labels do not draw.
/// `value_is_measured` puts the measured readings where the percentages go.
Texts compose_texts(const Readings& r, helix::SizeRung rung, bool value_is_measured) {
    using helix::SizeRung;
    const auto percent =
        helix::tune::status_speed_flow_text(false, r.speed_pct, r.flow_pct, r.live_velocity_mm_s,
                                            r.extruder_velocity_centimm_s, r.filament_diameter_mm);
    const auto measured =
        helix::tune::status_speed_flow_text(true, r.speed_pct, r.flow_pct, r.live_velocity_mm_s,
                                            r.extruder_velocity_centimm_s, r.filament_diameter_mm);
    Texts t;
    t.speed = value_is_measured ? measured.speed : percent.speed;
    t.flow = value_is_measured ? measured.flow : percent.flow;
    t.speed_measured = measured.speed;
    t.flow_measured = measured.flow;
    if (!value_is_measured && (rung == SizeRung::Strip || rung == SizeRung::Column)) {
        t.speed = without_percent(t.speed);
        t.flow = without_percent(t.flow);
    }
    return t;
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

int widest(const std::string& a, const std::string& b, const lv_font_t* font) {
    return std::max(measure_text_px(a.c_str(), font), measure_text_px(b.c_str(), font));
}

/// The minimum content box each rung draws in, for the state being decided. Mirrors
/// speed_flow_part.xml: spacing is the XML's pad_row (space_xxs) and pad_column
/// (space_xs); the gap between the two axes is the body's, which the content-box
/// chrome already holds back.
helix::RungNeeds measure_needs(const Readings& worst, bool printing, bool measured_values) {
    using helix::rung_box;
    using helix::SizeRung;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    // The small rungs hold one short number under a glyph; they get the least margin.
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    const int step_px = theme_manager_get_spacing("button_height_sm");

    const int icon_w =
        std::max(measure_text_px(helix::ui::icon::lookup_codepoint("speed"), f.icon),
                 measure_text_px(helix::ui::icon::lookup_codepoint("water"), f.icon));
    const int icon_h = line_px(f.icon);
    const int small_h = line_px(f.small);
    const int xs_h = line_px(f.xs);
    const int body_h = line_px(f.body);
    const int heading_h = line_px(f.heading);

    const auto texts_at = [&](SizeRung rung) {
        return compose_texts(worst, rung,
                             measured_values &&
                                 static_cast<int>(rung) < static_cast<int>(SizeRung::Cell));
    };
    const std::string speed_label = lv_tr("Speed");
    const std::string flow_label = lv_tr("Flow");

    helix::RungNeeds needs;

    {
        const Texts t = texts_at(SizeRung::Glance);
        needs.glance = rung_box(std::max(icon_w, measure_text_px(t.speed.c_str(), f.xs)) + tight,
                                icon_h + xs_h + 1);
    }
    {
        const Texts t = texts_at(SizeRung::Strip);
        const int block_w = icon_w + col_gap + widest(t.speed, t.flow, f.xs);
        needs.strip = rung_box(2 * block_w + tight, std::max(icon_h, xs_h) + 1);
    }
    {
        const Texts t = texts_at(SizeRung::Column);
        const int block_w = std::max(icon_w, widest(t.speed, t.flow, f.xs));
        needs.column = rung_box(block_w + tight, 2 * (icon_h + xs_h) + 1);
    }
    {
        const Texts t = texts_at(SizeRung::Cell);
        const int row_w = icon_w + col_gap + widest(t.speed, t.flow, f.body);
        const int under_w = printing ? widest(t.speed_measured, t.flow_measured, f.xs) : 0;
        const int part_h = std::max(icon_h, body_h) + (printing ? row_gap + xs_h : 0);
        needs.cell = rung_box(std::max(row_w, under_w) + comfort, 2 * part_h + 1);
    }
    {
        const Texts t = texts_at(SizeRung::Wide);
        const int label_w = widest(speed_label, flow_label, f.body);
        const int row_w =
            label_w + col_gap + widest(t.speed, t.flow, f.body) +
            (printing ? col_gap + widest(t.speed_measured, t.flow_measured, f.small) : 0);
        needs.wide = rung_box(row_w + comfort, 2 * body_h + 1);
    }
    {
        const Texts t = texts_at(SizeRung::Full);
        const int label_w = widest(speed_label, flow_label, f.small);
        const int header_w =
            label_w + (printing ? col_gap + widest(t.speed_measured, t.flow_measured, f.small) : 0);
        const int value_w = widest(t.speed, t.flow, f.heading);
        const int value_row_w = value_w + (printing ? 2 * (step_px + col_gap) : 0);
        const int value_row_h = printing ? std::max(step_px, heading_h) : heading_h;
        needs.full = rung_box(std::max(header_w, value_row_w) + comfort,
                              2 * (small_h + row_gap + value_row_h) + 1);
    }
    return needs;
}

} // namespace

namespace helix {

struct SpeedFlowWidget::LiveState {
    Readings readings;
    bool printing = false;
    bool physical = false;
};

SpeedFlowWidget::SpeedFlowWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

SpeedFlowWidget::~SpeedFlowWidget() {
    detach();
}

SpeedFlowWidget::LiveState SpeedFlowWidget::read_live() const {
    auto& motion = printer_state_.motion_state();
    LiveState s;
    s.readings.speed_pct = lv_subject_get_int(motion.get_speed_factor_subject());
    s.readings.flow_pct = lv_subject_get_int(motion.get_flow_factor_subject());
    s.readings.live_velocity_mm_s = lv_subject_get_int(motion.get_live_velocity_subject());
    s.readings.extruder_velocity_centimm_s =
        lv_subject_get_int(motion.get_live_extruder_velocity_subject());
    s.readings.filament_diameter_mm = printer_state_.get_discovery().filament_diameter_mm();
    s.printing = lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) != 0;
    s.physical = DisplaySettingsManager::instance().get_speed_flow_physical_units();
    return s;
}

SpeedFlowVerdict SpeedFlowWidget::decide(int width_px, int height_px, bool printing,
                                         bool physical) const {
    // The tree's own padding where it exists; before it does (the load path asks
    // fits_at then) the root's padding on each side. The body's gap is charged whether
    // or not Glance hides the second axis, so the verdict does not follow the last one.
    const ContentChrome chrome = content_chrome_or_estimate(
        root(), "space_xxs", "space_xxs", "space_xxs", "space_xxs", BranchGap::Always);
    SpeedFlowInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.printing = printing;
    in.physical_units = physical;
    // Widths are budgeted for the widest readings the labels can show, so the rung does
    // not change as the numbers do.
    Readings widest_readings;
    widest_readings.speed_pct = tune::kSpeedMaxPct;
    widest_readings.flow_pct = tune::kFlowMaxPct;
    widest_readings.live_velocity_mm_s = 300;
    widest_readings.extruder_velocity_centimm_s = 3000;
    widest_readings.filament_diameter_mm = printer_state_.get_discovery().filament_diameter_mm();
    in.needs = measure_needs(widest_readings, printing, false);
    if (printing && physical) {
        in.measured_needs = measure_needs(widest_readings, printing, true);
    }
    const auto& n = in.needs;
    spdlog::trace("[SpeedFlowWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} printing={} physical={}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, printing, physical);
    const auto& m = in.measured_needs;
    spdlog::trace("[SpeedFlowWidget] measured needs g{}x{} s{}x{} c{}x{}", m.glance.w, m.glance.h,
                  m.strip.w, m.strip.h, m.column.w, m.column.h);
    return decide_speed_flow_layout(in);
}

void SpeedFlowWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    auto& motion = printer_state_.motion_state();
    const auto lifetime = printer_state_.get_subjects_lifetime();
    const auto token = lifetime_.token();
    const auto on_change = [token](SpeedFlowWidget* self, int) {
        if (token.expired())
            return;
        self->on_live_changed();
    };
    speed_observer_ =
        ui::observe<int>(motion.get_speed_factor_subject(), this, on_change, lifetime);
    flow_observer_ = ui::observe<int>(motion.get_flow_factor_subject(), this, on_change, lifetime);
    // Velocity ticks arrive several times a second and only matter while a measured
    // reading is on screen.
    const auto on_velocity = [token](SpeedFlowWidget* self, int) {
        if (token.expired())
            return;
        if (self->verdict_.show_measured || self->verdict_.value_is_measured)
            self->publish_texts();
    };
    velocity_observer_ =
        ui::observe<int>(motion.get_live_velocity_subject(), this, on_velocity, lifetime);
    extruder_velocity_observer_ =
        ui::observe<int>(motion.get_live_extruder_velocity_subject(), this, on_velocity, lifetime);
    print_active_observer_ = ui::observe<int>(
        printer_state_.print_state().get_print_active_subject(), this, on_change, lifetime);
    units_observer_ = ui::observe<int>(
        DisplaySettingsManager::instance().subject_speed_flow_physical_units(), this, on_change,
        DisplaySettingsManager::instance().get_subjects_lifetime());

    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish_texts();
}

void SpeedFlowWidget::detach() {
    lifetime_.invalidate();
    speed_observer_.reset();
    flow_observer_.reset();
    velocity_observer_.reset();
    extruder_velocity_observer_.reset();
    print_active_observer_.reset();
    units_observer_.reset();
    if (SpeedFlowWidget* displaced = s_tile.release(this)) {
        displaced->relayout_for_granted_size();
        displaced->publish_texts();
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void SpeedFlowWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                      int height_px) {
    const LiveState live = read_live();
    verdict_ = decide(width_px, height_px, live.printing, live.physical);
    decided_printing_ = live.printing;
    decided_physical_ = live.physical;
    if (!s_tile.is_owner(this))
        return;

    lv_subject_set_int(&s_rung_subject, static_cast<int>(verdict_.rung));
    lv_subject_set_int(&s_show_measured_subject, verdict_.show_measured ? 1 : 0);
    lv_subject_set_int(&s_show_steppers_subject, verdict_.show_steppers ? 1 : 0);
    publish_texts();

    spdlog::debug("[SpeedFlowWidget] on_size_changed {}x{} rung={} measured={} replaced={} "
                  "steppers={} fits={} printing={} physical={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.show_measured,
                  verdict_.value_is_measured, verdict_.show_steppers, verdict_.fits, live.printing,
                  live.physical);
}

bool SpeedFlowWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content in either state. The
    // physical-units setting never refuses one: the measured readings only take the
    // percentages' place where they fit.
    return decide(width_px, height_px, false, false).fits &&
           decide(width_px, height_px, true, false).fits;
}

void SpeedFlowWidget::publish_texts() {
    if (!s_tile.is_owner(this))
        return;
    const LiveState live = read_live();
    const Texts t = compose_texts(live.readings, verdict_.rung, verdict_.value_is_measured);
    const auto set = [](lv_subject_t* subject, const std::string& text) {
        if (text != lv_subject_get_string(subject))
            lv_subject_copy_string(subject, text.c_str());
    };
    set(&s_speed_text_subject, t.speed);
    set(&s_flow_text_subject, t.flow);
    set(&s_speed_measured_subject, t.speed_measured);
    set(&s_flow_measured_subject, t.flow_measured);
    lv_subject_set_int(&s_speed_status_subject,
                       static_cast<int>(status::override_status(live.readings.speed_pct)));
    lv_subject_set_int(&s_flow_status_subject,
                       static_cast<int>(status::override_status(live.readings.flow_pct)));
}

void SpeedFlowWidget::on_live_changed() {
    const LiveState live = read_live();
    if (live.printing != decided_printing_ || live.physical != decided_physical_) {
        relayout_for_granted_size();
    }
    publish_texts();
}

void SpeedFlowWidget::open_tune_overlay() {
    get_print_tune_overlay().show(parent_screen_, get_moonraker_api(), printer_state_);
}

void SpeedFlowWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpeedFlowWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<SpeedFlowWidget>(e)) {
        self->record_interaction();
        self->open_tune_overlay();
    }
    LVGL_SAFE_EVENT_CB_END();
}

namespace {

/// Steppers and long-press only act while the Full rung shows them, so a stray
/// long-press on the read-only value does nothing and its release still opens
/// the Tune overlay. A reset ends the press, so the lift opens nothing.
bool steppers_shown() {
    return lv_subject_get_int(&s_show_steppers_subject) != 0;
}

} // namespace

void register_speed_flow_widget() {
    register_widget_factory("speed_flow", [](const std::string&) {
        return std::make_unique<SpeedFlowWidget>(get_printer_state());
    });
    register_widget_subjects("speed_flow", speed_flow_init_subjects);

    lv_xml_register_event_cb(nullptr, "speed_flow_clicked_cb", SpeedFlowWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "speed_flow_speed_step_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[SpeedFlowWidget] speed_step_cb");
        // A stepper tap is not a tap on the tile, which opens the Tune overlay.
        lv_event_stop_bubbling(e);
        if (auto delta = ui::event_user_int(e)) {
            const int current =
                lv_subject_get_int(get_printer_state().motion_state().get_speed_factor_subject());
            tune::set_speed_percent(get_moonraker_api(), current + *delta);
        }
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "speed_flow_flow_step_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[SpeedFlowWidget] flow_step_cb");
        // A stepper tap is not a tap on the tile, which opens the Tune overlay.
        lv_event_stop_bubbling(e);
        if (auto delta = ui::event_user_int(e)) {
            const int current =
                lv_subject_get_int(get_printer_state().motion_state().get_flow_factor_subject());
            tune::set_flow_percent(get_moonraker_api(), current + *delta);
        }
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "speed_flow_speed_reset_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[SpeedFlowWidget] speed_reset_cb");
        lv_event_stop_bubbling(e);
        if (steppers_shown()) {
            ui::end_press(e);
            tune::set_speed_percent(get_moonraker_api(), kResetPct);
        }
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "speed_flow_flow_reset_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[SpeedFlowWidget] flow_reset_cb");
        lv_event_stop_bubbling(e);
        if (steppers_shown()) {
            ui::end_press(e);
            tune::set_flow_percent(get_moonraker_api(), kResetPct);
        }
        LVGL_SAFE_EVENT_CB_END();
    });
}

} // namespace helix
