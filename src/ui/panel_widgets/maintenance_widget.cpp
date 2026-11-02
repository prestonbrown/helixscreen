// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/maintenance_widget.h"

#include "ui_event_safety.h"
#include "ui_maintenance_overlay.h"
#include "ui_subject_registry.h"

#include "helix_psram_attr.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "src/ui/panel_widgets/maintenance_model.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

using helix::ui::measure_text_px;

namespace {

// The verdict panel_widget_maintenance.xml binds. Module-level because the widget is
// single-instance and the subjects must exist before the XML that binds them is parsed.
HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_total_subject{};
HELIX_PSRAM_BSS lv_subject_t s_ring_d_subject{};
helix::WidgetSubjects s_subjects{"MaintenanceWidgetSubjects"};
helix::LiveTileOwner<helix::MaintenanceWidget> s_tile;

void maintenance_init_subjects() {
    // The meters first: the tile binds their subjects too, and they accrue whether or not
    // the tile is placed.
    helix::maintenance_model::init_subjects();
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Cell),
                               "maintenance_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_total_subject, 0, "maintenance_show_total", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_ring_d_subject, 0, "maintenance_ring_d", s_subjects);
    });
}

using helix::line_px;

/// The widest of the meters' names in @p font, short or full, with "Off" (what the tile
/// says when every meter is off).
int widest_name_px(const lv_font_t* font, bool full_names) {
    int w = measure_text_px(lv_tr("Off"), font);
    for (int i = 0; i < helix::maintenance::kMeterCount; ++i) {
        const std::string name = full_names ? helix::maintenance_model::meter_name(i)
                                            : helix::maintenance_model::meter_short_name(i);
        w = std::max(w, measure_text_px(name, font));
    }
    return w;
}

/// The minimum content box each rung draws in, mirroring panel_widget_maintenance.xml.
/// Hours are budgeted at the longest interval set, so the rung holds while they move and
/// changes only when an interval does.
struct Measured {
    helix::RungNeeds needs;
    int caption_h = 0; ///< the name line under the ring, with the gap above it
    int list_w = 0;    ///< the rows beside the ring at Wide, with the gap before them
};

Measured measure(const helix::ContentChrome& chrome) {
    using helix::rung_box;
    const lv_font_t* xs = theme_manager_get_font("font_xs");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const lv_font_t* body = theme_manager_get_font("font_body");
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    const int beside_gap = theme_manager_get_spacing("space_sm");
    // The ring's stroke (space_xs) on each side plus a hairline of clearance.
    const int ring_chrome = 2 * (theme_manager_get_spacing("space_xs") + row_gap);

    const int longest = helix::maintenance::longest_interval(helix::maintenance_model::state());
    const std::string hours =
        longest > 0 ? helix::maintenance_model::hours_text(longest) : std::string("--");
    const int hours_small_w = measure_text_px(hours, small);
    const int ring_small = std::max(hours_small_w + ring_chrome, line_px(small) + ring_chrome);
    const int ring_body = measure_text_px(hours, body) + ring_chrome;
    const int short_xs_w = widest_name_px(xs, false);
    const int short_small_w = widest_name_px(small, false);
    const int full_small_w = widest_name_px(small, true);
    const int list_w = short_small_w + col_gap + hours_small_w;
    const int list_h = helix::maintenance::kMeterCount * line_px(small) +
                       (helix::maintenance::kMeterCount - 1) * row_gap;
    const int bar_h = theme_manager_get_spacing("space_xs");
    const int full_row_h = line_px(small) + row_gap + bar_h;
    const int full_h = line_px(small) + helix::maintenance::kMeterCount * (row_gap + full_row_h);

    Measured m;
    m.caption_h = row_gap + line_px(xs);
    m.list_w = beside_gap + list_w;
    helix::RungNeeds& needs = m.needs;
    needs.glance = rung_box(ring_small, ring_small);
    needs.strip = rung_box(short_small_w + col_gap + hours_small_w, line_px(small));
    needs.column = rung_box(std::max(ring_small, short_xs_w), ring_small + row_gap + line_px(xs));
    needs.cell = rung_box(std::max(ring_body, short_xs_w), ring_body + row_gap + line_px(xs));
    needs.wide = rung_box(ring_body + beside_gap + list_w, std::max(ring_body, list_h));
    // Full is a size class as well as a content fit: a 2x1 tile can hold the rows, but the
    // rung is for 2x2 and up.
    needs.full = rung_box(
        std::max(full_small_w + col_gap + hours_small_w, helix::widget_size::w_normal() - chrome.w),
        std::max(full_h, helix::widget_size::h_tall() - chrome.h));
    return m;
}

} // namespace

namespace helix {

void register_maintenance_widget() {
    register_widget_factory(
        "maintenance", [](const std::string&) { return std::make_unique<MaintenanceWidget>(); });
    register_widget_subjects("maintenance", maintenance_init_subjects);
    lv_xml_register_event_cb(nullptr, "maintenance_clicked_cb",
                             MaintenanceWidget::maintenance_clicked_cb);
}

MaintenanceWidget::MaintenanceWidget() = default;

MaintenanceWidget::~MaintenanceWidget() {
    detach();
}

MaintenanceTileVerdict MaintenanceWidget::decide(int width_px, int height_px) const {
    // The root's padding, as panel_widget_maintenance.xml sets it.
    const ContentChrome chrome = content_padding_or_estimate(root(), "space_sm", "space_xxs");
    const lv_font_t* xs = theme_manager_get_font("font_xs");
    MaintenanceTileLayoutInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    const Measured m = measure(chrome);
    in.needs = m.needs;
    in.caption_h = m.caption_h;
    in.list_w = m.list_w;
    in.total_line_w = measure_text_px(maintenance_model::total_line(), xs);
    in.total_line_h = line_px(xs) + theme_manager_get_spacing("space_xxs");
    return decide_maintenance_tile_layout(in);
}

void MaintenanceWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    // A language switch and every meter change arrive as a version bump: the names and
    // the total line are what the decision measures.
    version_observer_ = ui::observe<int>(
        maintenance_model::version_subject(), this,
        [](MaintenanceWidget* self, int) { self->refresh(); }, maintenance_model::lifetime());

    // Instances are recycled across rebuilds and the size callback may not follow.
    refresh();
    spdlog::debug("[MaintenanceWidget] Attached");
}

void MaintenanceWidget::detach() {
    version_observer_.reset();
    uninstall_delete_hook();
    parent_screen_ = nullptr;
    if (MaintenanceWidget* displaced = s_tile.release(this))
        displaced->refresh();
}

void MaintenanceWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: drop pointers only.
    parent_screen_ = nullptr;
}

void MaintenanceWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                        int height_px) {
    granted_w_ = width_px;
    granted_h_ = height_px;
    refresh();
}

bool MaintenanceWidget::fits_at(int width_px, int height_px) const {
    return decide(width_px, height_px).fits;
}

void MaintenanceWidget::refresh() {
    if (granted_w_ <= 0 || granted_h_ <= 0 || !s_tile.is_owner(this))
        return;
    const MaintenanceTileVerdict v = decide(granted_w_, granted_h_);
    ui::set_int_if_changed(&s_rung_subject, static_cast<int>(v.rung));
    ui::set_int_if_changed(&s_show_total_subject, v.show_total ? 1 : 0);
    ui::set_int_if_changed(&s_ring_d_subject, v.ring_d);
    spdlog::trace("[MaintenanceWidget] {}x{} rung={} total={} ring={}", granted_w_, granted_h_,
                  static_cast<int>(v.rung), v.show_total, v.ring_d);
}

void MaintenanceWidget::maintenance_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceWidget] maintenance_clicked_cb");
    if (auto* self = panel_widget_from_event<MaintenanceWidget>(e)) {
        self->record_interaction();
        if (self->parent_screen_)
            get_maintenance_overlay().show(self->parent_screen_);
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
