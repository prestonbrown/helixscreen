// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "active_spool_widget.h"

#include "ui_ams_edit_follow_up.h"
#include "ui_ams_edit_overlay.h"
#include "ui_color_picker.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_spool_canvas.h"
#include "ui_tile_rung.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "active_material_provider.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/observer_factory.h"
#include "filament_display_name.h"
#include "i_moonraker_api.h"
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

// The compact form: a spool one line of the rung's icon face square, over one
// caption line whose text is either "Active Spool" or "No Spool". The caption is
// budgeted whatever show_widget_labels says, because "No Spool" draws regardless.
ActiveSpoolWidget::ActiveSpoolWidget(IMoonrakerAPI* api)
    : TiledPanelWidget("active_spool", TileSizing::Content{"", "", "Active Spool", false, "",
                                                           /*label_always_drawn=*/true,
                                                           TileSizing::IconBox::Square}),
      api_(api) {
    // Registered before the manager parses the component, which drops a
    // binding whose subject is missing at parse time.
    UI_MANAGED_SUBJECT_INT(wide_subject_, 0, "active_spool_wide", subjects_);
    UI_MANAGED_SUBJECT_INT(loaded_subject_, 0, "active_spool_loaded", subjects_);
}

ActiveSpoolWidget::~ActiveSpoolWidget() {
    detach();
}

void ActiveSpoolWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (!widget_obj_)
        return;

    // Register click handler via per-callback user_data
    auto* btn = lv_obj_find_by_name(widget_obj_, "spoolman_btn");
    if (btn) {
        lv_obj_add_event_cb(btn, clicked_cb, LV_EVENT_CLICKED, this);
    }

    // Cache element pointers
    spool_compact_ = lv_obj_find_by_name(widget_obj_, "spool_compact");
    spool_wide_ = lv_obj_find_by_name(widget_obj_, "spool_wide");
    material_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_material");
    brand_color_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_brand_color");
    weight_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_weight");

    // Observe spool changes from all sources
    auto token = lifetime_.token();

    // External spool changes
    spool_color_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [token](ActiveSpoolWidget* self, int /*color*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // AMS backend active slot changes
    current_slot_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_current_slot_subject(), this,
        [token](ActiveSpoolWidget* self, int /*slot*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // AMS slot info changes (material/color edits)
    slots_version_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_slots_version_subject(), this,
        [token](ActiveSpoolWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    lv_subject_set_int(&wide_subject_, is_wide_ ? 1 : 0);
    resize_spool_canvases();

    // Initial display update
    update_spool_display();

    spdlog::debug("[ActiveSpoolWidget] Attached");
}

void ActiveSpoolWidget::detach() {
    lifetime_.invalidate();

    spool_color_observer_.reset();
    current_slot_observer_.reset();
    slots_version_observer_.reset();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    spool_compact_ = nullptr;
    spool_wide_ = nullptr;
    material_label_ = nullptr;
    brand_color_label_ = nullptr;
    weight_label_ = nullptr;

    spdlog::debug("[ActiveSpoolWidget] Detached");
}

void ActiveSpoolWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                        int height_px) {
    sizing_.measure_and_publish(width_px, height_px);
    const bool wide = is_wide_at(width_px, height_px);
    const bool mode_changed = wide != is_wide_;
    is_wide_ = wide;
    lv_subject_set_int(&wide_subject_, wide ? 1 : 0);

    if (!widget_obj_)
        return;

    resize_spool_canvases();
    if (mode_changed) {
        // Refresh display for the now-visible elements
        update_spool_display();
    }
    spdlog::debug("[ActiveSpoolWidget] on_size_changed {}x{} -> {}", width_px, height_px,
                  wide ? "wide" : "compact");
}

int ActiveSpoolWidget::wide_spool_edge() {
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_xl");
    return icon_font ? lv_font_get_line_height(icon_font) : 48;
}

bool ActiveSpoolWidget::is_wide_at(int width_px, int height_px) {
    return width_px >= widget_size::w_normal() && height_px >= wide_spool_edge();
}

void ActiveSpoolWidget::resize_spool_canvases() {
    // The wide row keeps the tier's largest icon edge; the compact spool is
    // one line of the icon face the tile's rung names, the square TileSizing
    // measured.
    if (spool_wide_)
        ui_spool_canvas_set_size(spool_wide_, wide_spool_edge());
    if (spool_compact_) {
        const ui::TileFace face = ui::tile_rung_face(ui::TileLadder::Icon, sizing_.icon_rung());
        if (face.font)
            ui_spool_canvas_set_size(spool_compact_, face.px(lv_font_get_line_height(face.font)));
    }
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
            fill_level = LV_CLAMP(0.0f, fill_level, 1.0f);
        } else {
            fill_level = 1.0f; // No weight data -- show full
        }
    }

    // Determine which spool canvas is active based on current mode
    lv_obj_t* active_spool = is_wide_ ? spool_wide_ : spool_compact_;

    // Update the active spool canvas
    if (active_spool) {
        if (has_spool && source_slot)
            helix::ui::spool_canvas_set_filament(active_spool, active->color_rgb,
                                                 source_slot->multi_color_hexes,
                                                 helix::ui::swatch_finish_of(*source_slot));
        else
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
            // A tag, not lv_tr() text: the label then re-translates itself.
            if (is_wide_) {
                lv_label_set_translation_tag(material_label_, "No Spool");
            } else {
                lv_label_set_text(material_label_, "");
            }
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

    lv_subject_set_int(&loaded_subject_, has_spool ? 1 : 0);
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
                    if (err.success() || err.partially_applied) {
                        helix::ui::offer_edit_follow_up(current, original, result.slot_info);
                    }
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
