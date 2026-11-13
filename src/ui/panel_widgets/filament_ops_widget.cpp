// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_ops_widget.h"

#include "ui_event_safety.h"
#include "ui_nav.h"
#include "ui_panel_filament.h"

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_filament_ops_widget() {
    register_widget_factory("filament_ops", [](const std::string& instance_id) {
        return std::make_unique<FilamentOpsWidget>(instance_id);
    });

    lv_xml_register_event_cb(nullptr, "filament_ops_widget_clicked_cb",
                             FilamentOpsWidget::clicked_cb);
}

void open_filament_controls() {
    if (jump_to_full_page(kFilamentOpsFunction)) {
        return;
    }
    helix::nav::request_panel(PanelId::Filament);
}

FilamentOpsWidget::FilamentOpsWidget(const std::string& instance_id)
    : TiledPanelWidget(instance_id, TileSizing::Content{"", "", "Filament", false}),
      instance_id_(instance_id),
      full_page_(sizing_, kFilamentOpsFunction,
                 []() -> FullPageContent& { return get_global_filament_panel(); }) {}

FilamentOpsWidget::~FilamentOpsWidget() {
    detach();
}

void FilamentOpsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    full_page_.attach(widget_obj);
    install_delete_hook(widget_obj);
}

void FilamentOpsWidget::detach() {
    uninstall_delete_hook();
    full_page_.detach();
}

void FilamentOpsWidget::on_hooked_root_deleted() {
    full_page_.on_tile_deleted();
}

void FilamentOpsWidget::on_activate() {
    full_page_.activate();
}

void FilamentOpsWidget::on_deactivate() {
    full_page_.deactivate();
}

void FilamentOpsWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    TiledPanelWidget::on_size_changed(colspan, rowspan, width_px, height_px);
    full_page_.set_placement(fills_extra_page(), page_index());
}

void FilamentOpsWidget::handle_click() {
    // A full page's tile root is the page itself; a tap on its frame is not a
    // request to open the controls it already shows.
    if (full_page_.is_full_page()) {
        return;
    }
    open_filament_controls();
}

void FilamentOpsWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentOpsWidget] clicked_cb");
    auto* self = panel_widget_from_event<FilamentOpsWidget>(e);
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
