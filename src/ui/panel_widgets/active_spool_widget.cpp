// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "active_spool_widget.h"

#include "ui_ams_edit_overlay.h"
#include "ui_color_picker.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_spool_canvas.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "active_material_provider.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_display_name.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "spoolman_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <optional>

namespace helix {

void register_active_spool_widget() {
    register_widget_factory("active_spool", [](const std::string&) {
        auto* api = PanelWidgetManager::instance().shared_resource<IMoonrakerAPI>();
        return std::make_unique<ActiveSpoolWidget>(api);
    });
}

ActiveSpoolWidget::ActiveSpoolWidget(IMoonrakerAPI* api) : api_(api) {}

ActiveSpoolWidget::~ActiveSpoolWidget() {
    detach();
}

void ActiveSpoolWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (!widget_obj_)
        return;

    lv_obj_set_user_data(widget_obj_, this);

    // Register click handler via per-callback user_data
    auto* btn = lv_obj_find_by_name(widget_obj_, "spoolman_btn");
    if (btn) {
        lv_obj_add_event_cb(btn, clicked_cb, LV_EVENT_CLICKED, this);
    }

    // Cache element pointers
    spool_compact_ = lv_obj_find_by_name(widget_obj_, "spool_compact");
    wide_layout_ = lv_obj_find_by_name(widget_obj_, "spoolman_wide_layout");
    spool_wide_ = lv_obj_find_by_name(widget_obj_, "spool_wide");
    material_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_material");
    brand_color_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_brand_color");
    weight_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_weight");
    no_spool_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_no_spool_label");

    // Observe spool changes from all sources
    auto token = lifetime_.token();

    // External spool changes
    spool_color_observer_ = helix::ui::observe_int_sync<ActiveSpoolWidget>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [token](ActiveSpoolWidget* self, int /*color*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // AMS backend active slot changes
    current_slot_observer_ = helix::ui::observe_int_sync<ActiveSpoolWidget>(
        AmsState::instance().get_current_slot_subject(), this,
        [token](ActiveSpoolWidget* self, int /*slot*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // AMS slot info changes (material/color edits)
    slots_version_observer_ = helix::ui::observe_int_sync<ActiveSpoolWidget>(
        AmsState::instance().get_slots_version_subject(), this,
        [token](ActiveSpoolWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // Size spool canvases to match responsive icon size
    resize_spool_canvases();

    // Sync layout visibility to the persisted is_wide_ state. Widget instances
    // are recycled across rebuilds (PanelWidgetManager reuse path), but a fresh
    // XML component always starts with wide_layout hidden + spool_compact shown.
    // Without this, a recycled 2x1 instance keeps is_wide_==true, on_size_changed
    // early-returns (wide == is_wide_), and the default-white spool_compact is
    // left visible — the #1109 "static white spool" symptom.
    apply_layout_visibility();

    // Initial display update
    update_spool_display();

    spdlog::debug("[ActiveSpoolWidget] Attached");
}

void ActiveSpoolWidget::detach() {
    lifetime_.invalidate();

    spool_color_observer_.reset();
    current_slot_observer_.reset();
    slots_version_observer_.reset();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    spool_compact_ = nullptr;
    wide_layout_ = nullptr;
    spool_wide_ = nullptr;
    material_label_ = nullptr;
    brand_color_label_ = nullptr;
    weight_label_ = nullptr;
    no_spool_label_ = nullptr;

    spdlog::debug("[ActiveSpoolWidget] Detached");
}

void ActiveSpoolWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                        int /*height_px*/) {
    bool wide = (width_px >= widget_size::W_NORMAL);
    if (wide == is_wide_)
        return;
    is_wide_ = wide;

    if (!widget_obj_)
        return;

    apply_layout_visibility();

    // Refresh display for the now-visible elements
    update_spool_display();

    spdlog::debug("[ActiveSpoolWidget] on_size_changed width_px={} -> {}", width_px,
                  wide ? "wide" : "compact");
}

void ActiveSpoolWidget::apply_layout_visibility() {
    if (is_wide_) {
        // Show wide layout, hide compact spool
        if (wide_layout_)
            lv_obj_remove_flag(wide_layout_, LV_OBJ_FLAG_HIDDEN);
        if (spool_compact_)
            lv_obj_add_flag(spool_compact_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Show compact spool, hide wide layout
        if (wide_layout_)
            lv_obj_add_flag(wide_layout_, LV_OBJ_FLAG_HIDDEN);
        if (spool_compact_)
            lv_obj_remove_flag(spool_compact_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ActiveSpoolWidget::resize_spool_canvases() {
    // Use the responsive icon font that matches #icon_size (the standard widget icon)
    // icon_size resolves to md/lg/xl per breakpoint; icon_font_{size} gives the font
    // We use icon_font_lg which scales: tiny=32, small=48, medium=48, large=48
    // For the spool we want it slightly bigger, matching #icon_size mapping:
    //   tiny/small=md(32), medium=lg(48), large=xl(64)
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_xl");
    int32_t spool_size = icon_font ? lv_font_get_line_height(icon_font) : 48;

    if (spool_compact_)
        ui_spool_canvas_set_size(spool_compact_, spool_size);
    if (spool_wide_)
        ui_spool_canvas_set_size(spool_wide_, spool_size);

    spdlog::debug("[ActiveSpoolWidget] Spool canvas size: {}px (from icon font)", spool_size);
}

void ActiveSpoolWidget::update_spool_display() {
    // Use ActiveMaterialProvider which checks AMS backend first, then external spool
    auto active = helix::get_active_material();
    bool has_spool = active.has_value();

    // Also try to get weight and naming info from the source SlotInfo
    float remaining_weight = 0;
    float total_weight = 0;
    std::optional<SlotInfo> source_slot;

    if (has_spool) {
        // Get the source slot (AMS or external)
        auto& ams = AmsState::instance();
        AmsBackend* backend = ams.get_backend();
        if (backend && backend->is_filament_loaded()) {
            int current = backend->get_current_slot();
            if (current >= 0) {
                source_slot = backend->get_slot_info(current);
            }
        } else {
            source_slot = ams.get_external_spool_info();
        }
        if (source_slot) {
            remaining_weight = source_slot->remaining_weight_g;
            total_weight = source_slot->total_weight_g;
        }
    }

    // Compute fill level and color
    lv_color_t spool_color = lv_color_hex(0x808080); // Gray for no-spool
    float fill_level = 0.0f;

    if (has_spool) {
        spool_color = lv_color_hex(active->color_rgb);
        if (total_weight > 0) {
            fill_level = remaining_weight / total_weight;
            fill_level = LV_CLAMP(fill_level, 0.0f, 1.0f);
        } else {
            fill_level = 1.0f; // No weight data -- show full
        }
    }

    // Determine which spool canvas is active based on current mode
    lv_obj_t* active_spool = is_wide_ ? spool_wide_ : spool_compact_;

    // Update the active spool canvas
    if (active_spool) {
        ui_spool_canvas_set_color(active_spool, spool_color);
        ui_spool_canvas_set_fill_level(active_spool, fill_level);
        // Semi-transparent when no spool assigned
        lv_obj_set_style_opa(active_spool, has_spool ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    // Update text labels (wide mode)
    if (material_label_) {
        if (has_spool) {
            lv_label_set_text(material_label_, active->material_name.c_str());
            lv_obj_set_style_text_align(material_label_, LV_TEXT_ALIGN_LEFT, 0);
        } else {
            lv_label_set_text(material_label_, is_wide_ ? lv_tr("No Spool") : "");
            lv_obj_set_style_text_align(material_label_, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    if (brand_color_label_) {
        std::string brand_color_str;
        if (has_spool && source_slot) {
            // Same precedence as the AMS "currently loaded" card. Spoolman keeps
            // vendor and filament name out of SlotInfo on purpose (see
            // filament_display_name.h), so a Spoolman-linked lane on a backend
            // that reports no brand of its own -- AFC -- can only be named
            // through the identity cache.
            const auto identity = SpoolmanManager::find_identity(source_slot->spoolman_id);
            const auto parts =
                resolve_filament_label_parts(*source_slot, identity ? &*identity : nullptr,
                                             get_color_name_from_hex(source_slot->color_rgb));
            // The material has its own row above this one, so it is joined out.
            brand_color_str = compose_filament_label(parts.brand, parts.name, "");
        }
        lv_label_set_text(brand_color_label_, brand_color_str.c_str());
        if (brand_color_str.empty()) {
            lv_obj_add_flag(brand_color_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(brand_color_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (weight_label_) {
        std::string weight_str;
        if (has_spool && total_weight > 0) {
            weight_str = std::to_string(static_cast<int>(remaining_weight)) + "g / " +
                         std::to_string(static_cast<int>(total_weight)) + "g";
        }
        lv_label_set_text(weight_label_, weight_str.c_str());
        if (weight_str.empty()) {
            lv_obj_add_flag(weight_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(weight_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Show/hide no-spool label (compact mode only -- wide mode uses material_label)
    if (no_spool_label_) {
        if (has_spool || is_wide_) {
            lv_obj_add_flag(no_spool_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(no_spool_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ActiveSpoolWidget::handle_clicked() {
    spdlog::info("[ActiveSpoolWidget] Clicked");

    auto& ams = AmsState::instance();

    // Check if AMS backend has active material
    AmsBackend* backend = ams.get_backend();
    if (backend && backend->is_filament_loaded()) {
        int current = backend->get_current_slot();
        if (current >= 0) {
            spdlog::info("[ActiveSpoolWidget] Opening AMS slot edit for slot {}", current);
            SlotInfo slot = backend->get_slot_info(current);
            helix::ui::get_ams_edit_overlay().show_for_slot(
                parent_screen_, current, slot, api_,
                [current](const helix::ui::AmsEditOverlay::EditResult& result) {
                    if (!result.saved) {
                        return;
                    }
                    AmsBackend* be = AmsState::instance().get_backend();
                    if (!be) {
                        return;
                    }
                    // Capture the pre-edit slot BEFORE the commit — its unlink
                    // arm (clear the server active spool) needs the old link.
                    SlotInfo original = be->get_slot_info(current);
                    AmsError err =
                        AmsState::instance().commit_slot_edit(current, original, result.slot_info);
                    if (!err.success()) {
                        helix::ui::notify_ams_error(err);
                    }
                });
            return;
        }
    }

    // External spool or no spool -- open external spool edit modal
    open_external_spool_edit();
}

void ActiveSpoolWidget::open_external_spool_edit() {
    spdlog::info("[ActiveSpoolWidget] Opening external spool editor");

    auto ext = AmsState::instance().get_external_spool_info();
    SlotInfo initial_info = ext.value_or(SlotInfo{});
    initial_info.slot_index = -2;
    initial_info.global_index = -2;

    helix::ui::get_ams_edit_overlay().show_for_slot(
        parent_screen_, -2, initial_info, api_,
        [](const helix::ui::AmsEditOverlay::EditResult& result) {
            if (result.saved) {
                // Owns S1 (server active-spool sync) + the S5 emptiness
                // predicate + settings persist/erase.
                AmsState::instance().commit_external_spool_edit(result.slot_info);
            }
        });
}

void ActiveSpoolWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ActiveSpoolWidget] clicked_cb");
    auto* self = static_cast<ActiveSpoolWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
