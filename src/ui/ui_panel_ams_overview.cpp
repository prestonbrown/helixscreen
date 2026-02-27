// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_ams_overview.h"

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_filament_path_canvas.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_panel_common.h"
#include "ui_spool_canvas.h"
#include "ui_system_path_canvas.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

using namespace helix;

// ============================================================================
// Layout Constants
// ============================================================================

/// Minimum bar width for mini slot bars (prevents invisible bars)
static constexpr int32_t MINI_BAR_MIN_WIDTH_PX = 6;

/// Maximum bar width for mini slot bars
static constexpr int32_t MINI_BAR_MAX_WIDTH_PX = 14;

/// Height of each mini slot bar (decorative, no need for responsive scaling)
static constexpr int32_t MINI_BAR_HEIGHT_PX = 40;

/// Border radius for bar corners
static constexpr int32_t MINI_BAR_RADIUS_PX = 4;

/// Zoom animation duration (ms) for detail view transitions
static constexpr uint32_t DETAIL_ZOOM_DURATION_MS = 200;

/// Zoom animation start scale (25% = 64/256)
static constexpr int32_t DETAIL_ZOOM_SCALE_MIN = 64;

/// Zoom animation end scale (100% = 256/256)
static constexpr int32_t DETAIL_ZOOM_SCALE_MAX = 256;

// Global instance pointer for XML callback access (used by back button and animation callbacks)
static std::atomic<AmsOverviewPanel*> g_overview_panel_instance{nullptr};

/// Set a label to "N slots" text, with null-safety
static void set_slot_count_label(lv_obj_t* label, int slot_count) {
    if (!label) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d slots", slot_count);
    lv_label_set_text(label, buf);
}

// ============================================================================
// Construction
// ============================================================================

AmsOverviewPanel::AmsOverviewPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::debug("[AMS Overview] Constructed");
}

// ============================================================================
// PanelBase Interface
// ============================================================================

void AmsOverviewPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // AmsState handles all subject registration centrally.
        // Overview panel reuses existing AMS subjects (slots_version, etc.)
        AmsState::instance().init_subjects(true);

        // Observe slots_version to auto-refresh when slot data changes.
        // In detail mode, per-slot observers handle visual updates (color, pulse,
        // highlight) automatically — we only need to react to structural changes
        // (slot count changed) or refresh the overview cards.
        using helix::ui::observe_int_sync;
        slots_version_observer_ =
            observe_int_sync<AmsOverviewPanel>(AmsState::instance().get_slots_version_subject(),
                                               this, [](AmsOverviewPanel* self, int) {
                                                   if (!self->panel_)
                                                       return;
                                                   if (self->detail_unit_index_ >= 0) {
                                                       // In detail mode — only rebuild slots if
                                                       // count changed. Per-slot observers drive
                                                       // all visual state (color, pulse, etc.)
                                                       self->refresh_detail_if_needed();
                                                   } else {
                                                       self->refresh_units();
                                                   }
                                               });

        // Observe external spool color changes to reactively update bypass display.
        // NOTE: set_external_spool_info() calls lv_subject_set_int() directly (not via
        // ui_queue_update) which is safe because all current callers are on the LVGL thread.
        // If callers from background threads are added, those must use ui_queue_update().
        using helix::ui::observe_int_sync;
        external_spool_observer_ = observe_int_sync<AmsOverviewPanel>(
            AmsState::instance().get_external_spool_color_subject(), this,
            [](AmsOverviewPanel* self, int /*color_int*/) {
                // Delegate to existing refresh helper which reads full spool info
                self->refresh_bypass_display();
            });
    });
}

void AmsOverviewPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::debug("[{}] Setting up...", get_name());

    // Standard overlay panel setup (header bar, responsive padding)
    ui_overlay_panel_setup_standard(panel_, parent_screen_, "overlay_header", "overview_content");

    // Find the unit cards row container from XML
    cards_row_ = lv_obj_find_by_name(panel_, "unit_cards_row");
    if (!cards_row_) {
        spdlog::error("[{}] Could not find 'unit_cards_row' in XML", get_name());
        return;
    }

    // Find system path area and create path canvas widget
    system_path_area_ = lv_obj_find_by_name(panel_, "system_path_area");
    if (system_path_area_) {
        system_path_ = ui_system_path_canvas_create(system_path_area_);
        if (system_path_) {
            lv_obj_set_size(system_path_, LV_PCT(100), LV_PCT(100));
            if (PrinterDetector::is_voron_printer()) {
                ui_system_path_canvas_set_faceted_toolhead(system_path_, true);
            }
            spdlog::debug("[{}] Created system path canvas", get_name());
        }
    }

    // Find detail view containers
    detail_container_ = lv_obj_find_by_name(panel_, "unit_detail_container");
    lv_obj_t* detail_unit = lv_obj_find_by_name(panel_, "detail_unit_detail");
    detail_widgets_ = ams_detail_find_widgets(detail_unit);
    detail_path_canvas_ = lv_obj_find_by_name(panel_, "detail_path_canvas");

    // Store global instance for callback access (back button + animation callbacks)
    g_overview_panel_instance.store(this);

    // Set up the shared sidebar component
    sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(printer_state_, api_);
    sidebar_->setup(panel_);
    sidebar_->init_observers();

    // Initial population from backend state
    refresh_units();

    spdlog::debug("[{}] Setup complete!", get_name());
}

void AmsOverviewPanel::on_activate() {
    spdlog::debug("[{}] Activated - syncing from backend", get_name());

    AmsState::instance().sync_from_backend();

    if (sidebar_)
        sidebar_->sync_from_state();

    if (detail_unit_index_ >= 0) {
        // Re-entering while in detail mode — refresh the detail slots
        show_unit_detail(detail_unit_index_);
    } else {
        refresh_units();
    }
}

void AmsOverviewPanel::on_deactivate() {
    spdlog::debug("[{}] Deactivated", get_name());

    // Reset to overview mode so next open starts at the cards view
    if (detail_unit_index_ >= 0) {
        show_overview();
    }
}

// ============================================================================
// Unit Card Management
// ============================================================================

void AmsOverviewPanel::refresh_units() {
    if (!cards_row_) {
        return;
    }

    // Overview shows units from the active backend. Multi-unit support handles
    // backends with multiple physical units (e.g., 2x Box Turtle on one AFC system).
    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::debug("[{}] No backend available", get_name());
        return;
    }

    AmsSystemInfo info = backend->get_system_info();
    int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());

    int new_unit_count = static_cast<int>(info.units.size());
    int old_unit_count = static_cast<int>(unit_cards_.size());

    if (new_unit_count != old_unit_count) {
        // Unit count changed - rebuild all cards
        spdlog::debug("[{}] Unit count changed {} -> {}, rebuilding cards", get_name(),
                      old_unit_count, new_unit_count);
        create_unit_cards(info);
    } else {
        // Same number of units - update existing cards in place
        for (int i = 0; i < new_unit_count; ++i) {
            update_unit_card(unit_cards_[i], info.units[i], current_slot);
        }
    }

    // Update system path visualization
    refresh_system_path(info, current_slot);
}

void AmsOverviewPanel::create_unit_cards(const AmsSystemInfo& info) {
    if (!cards_row_) {
        return;
    }

    // Remove old card widgets
    lv_obj_clean(cards_row_);
    unit_cards_.clear();

    int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());

    for (int i = 0; i < static_cast<int>(info.units.size()); ++i) {
        const AmsUnit& unit = info.units[i];
        UnitCard uc;
        uc.unit_index = i;

        // Create card from XML component — all static styling is declarative
        uc.card = static_cast<lv_obj_t*>(lv_xml_create(cards_row_, "ams_unit_card", nullptr));
        if (!uc.card) {
            spdlog::error("[{}] Failed to create ams_unit_card XML for unit {}", get_name(), i);
            continue;
        }

        // Flex grow so cards share available width equally
        lv_obj_set_flex_grow(uc.card, 1);

        // Store unit index for click handler
        // NOTE: lv_obj_add_event_cb used here (not XML event_cb) because each dynamically
        // created card needs per-instance user_data (unit index) that XML bindings can't provide.
        lv_obj_set_user_data(uc.card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(uc.card, on_unit_card_clicked, LV_EVENT_CLICKED, this);

        // Find child widgets declared in XML
        uc.logo_image = lv_obj_find_by_name(uc.card, "unit_logo");
        uc.name_label = lv_obj_find_by_name(uc.card, "unit_name");
        uc.bars_container = lv_obj_find_by_name(uc.card, "bars_container");
        uc.slot_count_label = lv_obj_find_by_name(uc.card, "slot_count");

        // Set logo image based on AMS system type
        ams_draw::apply_logo(uc.logo_image, unit, info);

        // Set dynamic content only — unit name and slot count vary per unit
        if (uc.name_label) {
            lv_label_set_text(uc.name_label, ams_draw::get_unit_display_name(unit, i).c_str());
        }

        set_slot_count_label(uc.slot_count_label, unit.slot_count);

        // Create the mini bars for this unit (dynamic — slot count varies)
        create_mini_bars(uc, unit, current_slot);

        // Create error badge (top-right of card, initially hidden)
        uc.error_badge = ams_draw::create_error_badge(uc.card, 12);
        lv_obj_set_align(uc.error_badge, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_style_translate_x(uc.error_badge, -4, LV_PART_MAIN);
        lv_obj_set_style_translate_y(uc.error_badge, 4, LV_PART_MAIN);

        {
            bool animate = DisplaySettingsManager::instance().get_animations_enabled();
            auto worst = ams_draw::worst_unit_severity(unit);
            ams_draw::update_error_badge(uc.error_badge, unit.has_any_error(), worst, animate);
        }

        unit_cards_.push_back(uc);
    }

    spdlog::debug("[{}] Created {} unit cards from XML (bypass={})", get_name(),
                  static_cast<int>(unit_cards_.size()), info.supports_bypass);
}

void AmsOverviewPanel::update_unit_card(UnitCard& card, const AmsUnit& unit, int current_slot) {
    if (!card.card) {
        return;
    }

    // Update name label
    if (card.name_label) {
        lv_label_set_text(card.name_label,
                          ams_draw::get_unit_display_name(unit, card.unit_index).c_str());
    }

    // Rebuild mini bars (slot colors/status may have changed)
    if (card.bars_container) {
        lv_obj_clean(card.bars_container);
        create_mini_bars(card, unit, current_slot);
    }

    // Update slot count
    set_slot_count_label(card.slot_count_label, unit.slot_count);

    // Update error badge visibility and color
    if (card.error_badge) {
        bool animate = DisplaySettingsManager::instance().get_animations_enabled();
        auto worst = ams_draw::worst_unit_severity(unit);
        ams_draw::update_error_badge(card.error_badge, unit.has_any_error(), worst, animate);
    }
}

void AmsOverviewPanel::create_mini_bars(UnitCard& card, const AmsUnit& unit, int current_slot) {
    if (!card.bars_container) {
        return;
    }

    int slot_count = static_cast<int>(unit.slots.size());
    if (slot_count <= 0) {
        return;
    }

    // Calculate bar width to fit within bars_container
    lv_obj_update_layout(card.bars_container);
    int32_t container_width = lv_obj_get_content_width(card.bars_container);
    if (container_width <= 0) {
        container_width = 80; // Fallback if layout not yet calculated
    }
    int32_t gap = theme_manager_get_spacing("space_xxs");
    int32_t bar_width = ams_draw::calc_bar_width(container_width, slot_count, gap,
                                                 MINI_BAR_MIN_WIDTH_PX, MINI_BAR_MAX_WIDTH_PX);

    for (int s = 0; s < slot_count; ++s) {
        const SlotInfo& slot = unit.slots[s];
        int global_idx = unit.first_slot_global_index + s;
        bool is_loaded = (global_idx == current_slot);

        auto col = ams_draw::create_slot_column(card.bars_container, bar_width, MINI_BAR_HEIGHT_PX,
                                                MINI_BAR_RADIUS_PX);

        ams_draw::BarStyleParams params;
        params.color_rgb = slot.color_rgb;
        params.fill_pct = ams_draw::fill_percent_from_slot(slot);
        params.is_present = slot.is_present();
        params.is_loaded = is_loaded;
        params.has_error = (slot.status == SlotStatus::BLOCKED || slot.error.has_value());
        params.severity = slot.error.has_value() ? slot.error->severity : SlotError::INFO;

        ams_draw::style_slot_bar(col, params, MINI_BAR_RADIUS_PX);
    }
}

// ============================================================================
// System Path
// ============================================================================

void AmsOverviewPanel::refresh_system_path(const AmsSystemInfo& info, int current_slot) {
    if (!system_path_)
        return;

    int unit_count = static_cast<int>(info.units.size());
    ui_system_path_canvas_set_unit_count(system_path_, unit_count);

    // Calculate and set X positions based on unit card positions
    // Force layout so we can get accurate card positions
    if (cards_row_) {
        lv_obj_update_layout(cards_row_);
    }

    for (int i = 0; i < unit_count && i < static_cast<int>(unit_cards_.size()); ++i) {
        if (unit_cards_[i].card) {
            // Get card center X relative to the system path widget
            lv_obj_update_layout(unit_cards_[i].card);
            lv_area_t card_coords;
            lv_obj_get_coords(unit_cards_[i].card, &card_coords);

            lv_area_t path_coords;
            lv_obj_get_coords(system_path_, &path_coords);
            int32_t card_center_x = (card_coords.x1 + card_coords.x2) / 2 - path_coords.x1;
            ui_system_path_canvas_set_unit_x(system_path_, i, card_center_x);
        }
    }

    // Set active unit based on current slot
    int active_unit = info.get_active_unit_index();
    ui_system_path_canvas_set_active_unit(system_path_, active_unit);

    // Set filament color from active slot
    if (current_slot >= 0) {
        const SlotInfo* slot = info.get_slot_global(current_slot);
        if (slot) {
            ui_system_path_canvas_set_active_color(system_path_, slot->color_rgb);
        }
    }

    // Set whether filament is fully loaded
    ui_system_path_canvas_set_filament_loaded(system_path_, info.filament_loaded);

    // Set bypass path state (bypass is drawn inside the canvas, no card needed)
    bool bypass_active = info.supports_bypass && (current_slot == -2);
    uint32_t bypass_color = 0x888888; // Default gray when no external spool assigned
    auto ext_spool = AmsState::instance().get_external_spool_info();
    if (ext_spool) {
        bypass_color = ext_spool->color_rgb;
    }
    ui_system_path_canvas_set_bypass(system_path_, info.supports_bypass, bypass_active,
                                     bypass_color);

    // Set whether an external spool is assigned (controls filled vs hollow spool box)
    ui_system_path_canvas_set_bypass_has_spool(system_path_, ext_spool.has_value());

    // Register bypass click callback (safe to call repeatedly — just updates the stored cb)
    ui_system_path_canvas_set_bypass_callback(system_path_, on_bypass_spool_clicked, this);

    // Compute physical tool layout (handles HUB units with unique per-lane mapped_tools)
    auto* backend = AmsState::instance().get_backend();
    auto tool_layout = ams_draw::compute_system_tool_layout(info, backend);

    // Set per-unit hub sensor states, topology, and tool routing
    for (int i = 0; i < unit_count && i < static_cast<int>(info.units.size()); ++i) {
        const auto& unit = info.units[i];
        ui_system_path_canvas_set_unit_hub_sensor(system_path_, i, unit.has_hub_sensor,
                                                  unit.hub_sensor_triggered);

        PathTopology topo = unit.topology;
        if (backend) {
            topo = backend->get_unit_topology(i);
        }
        ui_system_path_canvas_set_unit_topology(system_path_, i, static_cast<int>(topo));

        if (i < static_cast<int>(tool_layout.units.size())) {
            const auto& utl = tool_layout.units[i];
            ui_system_path_canvas_set_unit_tools(system_path_, i, utl.tool_count,
                                                 utl.first_physical_tool);
        }
    }

    // Translate active slot's virtual tool number to physical nozzle index
    int active_tool = -1;
    if (current_slot >= 0) {
        const SlotInfo* active_slot = info.get_slot_global(current_slot);
        if (active_slot && active_slot->mapped_tool >= 0) {
            auto it = tool_layout.virtual_to_physical.find(active_slot->mapped_tool);
            if (it != tool_layout.virtual_to_physical.end()) {
                active_tool = it->second;
            }
        }
    }

    ui_system_path_canvas_set_total_tools(system_path_, tool_layout.total_physical_tools);
    ui_system_path_canvas_set_active_tool(system_path_, active_tool);
    ui_system_path_canvas_set_current_tool(system_path_, info.current_tool);

    // Set virtual tool labels for badge display.
    // For HUB units with an active slot, override the static hub_tool_label with the
    // actual virtual tool number (e.g., show "T6" when AMS_1 slot 3 is loaded, not "T4").
    if (!tool_layout.physical_to_virtual_label.empty()) {
        auto labels = tool_layout.physical_to_virtual_label; // mutable copy
        if (active_tool >= 0 && active_tool < static_cast<int>(labels.size()) &&
            current_slot >= 0) {
            const SlotInfo* active_slot_info = info.get_slot_global(current_slot);
            if (active_slot_info && active_slot_info->mapped_tool >= 0) {
                labels[active_tool] = active_slot_info->mapped_tool;
            }
        }
        ui_system_path_canvas_set_tool_virtual_numbers(system_path_, labels.data(),
                                                       static_cast<int>(labels.size()));
    }

    // Set toolhead sensor state
    {
        auto segment = static_cast<PathSegment>(
            lv_subject_get_int(AmsState::instance().get_path_filament_segment_subject()));
        bool toolhead_triggered = (segment >= PathSegment::TOOLHEAD);

        bool has_toolhead = std::any_of(info.units.begin(), info.units.end(),
                                        [](const AmsUnit& u) { return u.has_toolhead_sensor; });
        ui_system_path_canvas_set_toolhead_sensor(system_path_, has_toolhead, toolhead_triggered);
    }

    // Status text now shown in shared sidebar component (ams_sidebar.xml)
    // No longer drawn on the canvas to avoid duplication

    ui_system_path_canvas_refresh(system_path_);
}

// ============================================================================
// Event Handling
// ============================================================================

void AmsOverviewPanel::on_unit_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AMS Overview] on_unit_card_clicked");

    auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e));
    if (!self) {
        spdlog::warn("[AMS Overview] Card clicked but panel instance is null");
        return;
    }

    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int unit_index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));

    spdlog::info("[AMS Overview] Unit {} clicked - showing inline detail", unit_index);

    // Show detail view inline (swaps left column content, no overlay push)
    self->show_unit_detail(unit_index);

    LVGL_SAFE_EVENT_CB_END();
}

void AmsOverviewPanel::on_detail_slot_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AMS Overview] on_detail_slot_clicked");

    auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }

    // Capture click point from the input device while event is still active
    lv_point_t click_pt = {0, 0};
    lv_indev_t* indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &click_pt);
    }

    // Use current_target (widget callback was registered on) not target (originally clicked child)
    lv_obj_t* slot = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto global_index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(slot)));
    self->handle_detail_slot_tap(global_index, click_pt);

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Detail View (inline unit zoom)
// ============================================================================

void AmsOverviewPanel::refresh_detail_if_needed() {
    if (detail_unit_index_ < 0 || !panel_)
        return;

    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    AmsSystemInfo info = backend->get_system_info();
    if (detail_unit_index_ >= static_cast<int>(info.units.size()))
        return;

    const AmsUnit& unit = info.units[detail_unit_index_];
    int new_slot_count = static_cast<int>(unit.slots.size());

    if (new_slot_count != detail_slot_count_) {
        // Structural change — rebuild slots (no animation restart)
        spdlog::debug("[{}] Detail slot count changed {} -> {}, rebuilding", get_name(),
                      detail_slot_count_, new_slot_count);
        create_detail_slots(unit);
        update_detail_header(unit, info);
    }

    // Always update path canvas — segment/action changes need to propagate
    // even when slot count hasn't changed (e.g., load/unload animations)
    setup_detail_path_canvas(unit, info);
}

void AmsOverviewPanel::show_unit_detail(int unit_index) {
    if (!panel_ || !detail_container_ || !cards_row_)
        return;

    // Cancel any in-flight zoom animations to prevent race conditions
    lv_anim_delete(detail_container_, nullptr);

    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    AmsSystemInfo info = backend->get_system_info();
    if (unit_index < 0 || unit_index >= static_cast<int>(info.units.size()))
        return;

    // Capture clicked card's screen center BEFORE hiding overview elements
    lv_area_t card_coords = {};
    if (unit_index < static_cast<int>(unit_cards_.size()) && unit_cards_[unit_index].card) {
        lv_obj_update_layout(unit_cards_[unit_index].card);
        lv_obj_get_coords(unit_cards_[unit_index].card, &card_coords);
    }

    detail_unit_index_ = unit_index;
    const AmsUnit& unit = info.units[unit_index];

    spdlog::info("[{}] Showing detail for unit {} ({})", get_name(), unit_index, unit.name);

    // Update detail header (logo + name)
    update_detail_header(unit, info);

    // Create slot widgets for this unit
    create_detail_slots(unit);

    // Configure path canvas for this unit's filament routing
    setup_detail_path_canvas(unit, info);

    // Swap visibility: hide overview elements, show detail
    lv_obj_add_flag(cards_row_, LV_OBJ_FLAG_HIDDEN);
    if (system_path_area_)
        lv_obj_add_flag(system_path_area_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);

    // Zoom-in animation (scale + fade) — gated on animations setting
    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        // Set transform pivot to the clicked card's center relative to detail container
        lv_obj_update_layout(detail_container_);
        lv_area_t detail_coords;
        lv_obj_get_coords(detail_container_, &detail_coords);
        int32_t pivot_x = (card_coords.x1 + card_coords.x2) / 2 - detail_coords.x1;
        int32_t pivot_y = (card_coords.y1 + card_coords.y2) / 2 - detail_coords.y1;
        lv_obj_set_style_transform_pivot_x(detail_container_, pivot_x, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(detail_container_, pivot_y, LV_PART_MAIN);

        // Start small and transparent
        lv_obj_set_style_transform_scale(detail_container_, DETAIL_ZOOM_SCALE_MIN, LV_PART_MAIN);
        lv_obj_set_style_opa(detail_container_, LV_OPA_TRANSP, LV_PART_MAIN);

        // Scale animation
        lv_anim_t scale_anim;
        lv_anim_init(&scale_anim);
        lv_anim_set_var(&scale_anim, detail_container_);
        lv_anim_set_values(&scale_anim, DETAIL_ZOOM_SCALE_MIN, DETAIL_ZOOM_SCALE_MAX);
        lv_anim_set_duration(&scale_anim, DETAIL_ZOOM_DURATION_MS);
        lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        lv_anim_start(&scale_anim);

        // Fade animation
        lv_anim_t fade_anim;
        lv_anim_init(&fade_anim);
        lv_anim_set_var(&fade_anim, detail_container_);
        lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade_anim, DETAIL_ZOOM_DURATION_MS);
        lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        lv_anim_start(&fade_anim);
    } else {
        // No animation — ensure final state
        lv_obj_set_style_transform_scale(detail_container_, DETAIL_ZOOM_SCALE_MAX, LV_PART_MAIN);
        lv_obj_set_style_opa(detail_container_, LV_OPA_COVER, LV_PART_MAIN);
    }
}

void AmsOverviewPanel::show_overview() {
    if (!panel_ || !detail_container_ || !cards_row_)
        return;

    // Cancel any in-flight zoom animations to prevent race conditions
    lv_anim_delete(detail_container_, nullptr);

    // Dismiss context menu if open
    if (context_menu_ && context_menu_->is_visible()) {
        context_menu_->hide();
    }

    spdlog::info("[{}] Returning to overview mode", get_name());

    detail_unit_index_ = -1;

    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        // Zoom-out animation: scale down + fade out, then swap visibility
        // Transform pivot is still set from the zoom-in (card center position)
        lv_anim_t scale_anim;
        lv_anim_init(&scale_anim);
        lv_anim_set_var(&scale_anim, detail_container_);
        lv_anim_set_values(&scale_anim, DETAIL_ZOOM_SCALE_MAX, DETAIL_ZOOM_SCALE_MIN);
        lv_anim_set_duration(&scale_anim, DETAIL_ZOOM_DURATION_MS);
        lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        // On complete: swap visibility and clean up
        lv_anim_set_completed_cb(&scale_anim, [](lv_anim_t* anim) {
            auto* container = static_cast<lv_obj_t*>(anim->var);
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
            // Reset transform properties for next use
            lv_obj_set_style_transform_scale(container, DETAIL_ZOOM_SCALE_MAX, LV_PART_MAIN);
            lv_obj_set_style_opa(container, LV_OPA_COVER, LV_PART_MAIN);

            // Show overview elements (use global instance since lambda has no 'this')
            AmsOverviewPanel* self = g_overview_panel_instance.load();
            if (self) {
                self->destroy_detail_slots();
                if (self->cards_row_)
                    lv_obj_remove_flag(self->cards_row_, LV_OBJ_FLAG_HIDDEN);
                if (self->system_path_area_)
                    lv_obj_remove_flag(self->system_path_area_, LV_OBJ_FLAG_HIDDEN);
                self->refresh_units();
            }
        });
        lv_anim_start(&scale_anim);

        // Fade animation
        lv_anim_t fade_anim;
        lv_anim_init(&fade_anim);
        lv_anim_set_var(&fade_anim, detail_container_);
        lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&fade_anim, DETAIL_ZOOM_DURATION_MS);
        lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        lv_anim_start(&fade_anim);
    } else {
        // No animation — instant swap
        destroy_detail_slots();
        lv_obj_remove_flag(cards_row_, LV_OBJ_FLAG_HIDDEN);
        if (system_path_area_)
            lv_obj_remove_flag(system_path_area_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
        refresh_units();
    }
}

void AmsOverviewPanel::update_detail_header(const AmsUnit& unit, const AmsSystemInfo& info) {
    // Update logo
    lv_obj_t* logo = lv_obj_find_by_name(panel_, "detail_logo");
    ams_draw::apply_logo(logo, unit, info);

    // Update name
    lv_obj_t* name = lv_obj_find_by_name(panel_, "detail_unit_name");
    if (name) {
        lv_label_set_text(name, ams_draw::get_unit_display_name(unit, detail_unit_index_).c_str());
    }
}

void AmsOverviewPanel::create_detail_slots(const AmsUnit& unit) {
    ams_detail_destroy_slots(detail_widgets_, detail_slot_widgets_, detail_slot_count_);

    // Find unit index from backend
    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    AmsSystemInfo info = backend->get_system_info();
    int unit_index = -1;
    for (int i = 0; i < static_cast<int>(info.units.size()); ++i) {
        if (info.units[i].first_slot_global_index == unit.first_slot_global_index) {
            unit_index = i;
            break;
        }
    }

    auto result = ams_detail_create_slots(detail_widgets_, detail_slot_widgets_, MAX_DETAIL_SLOTS,
                                          unit_index, on_detail_slot_clicked, this);

    detail_slot_count_ = result.slot_count;

    ams_detail_update_labels(detail_widgets_, detail_slot_widgets_, result.slot_count,
                             result.layout);
    ams_detail_update_badges(detail_widgets_, detail_slot_widgets_, result.slot_count,
                             result.layout);
    ams_detail_update_tray(detail_widgets_);

    spdlog::debug("[{}] Created {} detail slots via shared helpers", get_name(), result.slot_count);
}

void AmsOverviewPanel::destroy_detail_slots() {
    ams_detail_destroy_slots(detail_widgets_, detail_slot_widgets_, detail_slot_count_);
}

void AmsOverviewPanel::setup_detail_path_canvas(const AmsUnit& unit, const AmsSystemInfo& info) {
    if (!detail_path_canvas_)
        return;

    // Find unit index
    int unit_index = -1;
    for (int i = 0; i < static_cast<int>(info.units.size()); ++i) {
        if (info.units[i].first_slot_global_index == unit.first_slot_global_index) {
            unit_index = i;
            break;
        }
    }

    ams_detail_setup_path_canvas(detail_path_canvas_, detail_widgets_.slot_grid, unit_index,
                                 true /* hub_only */);
}

// ============================================================================
// Cleanup
// ============================================================================

void AmsOverviewPanel::clear_panel_reference() {
    // Cancel animations and dismiss menus while widget pointers are still valid
    if (detail_container_) {
        lv_anim_delete(detail_container_, nullptr);
    }
    if (context_menu_) {
        context_menu_->hide();
    }

    // Destroy detail slot widgets BEFORE the parent tree deletion.
    // When on_deactivate() → show_overview() starts an animation whose completed
    // callback calls destroy_detail_slots(), and we cancel that animation above,
    // the slots are never cleaned up. Their DELETE handlers release() observers
    // instead of properly removing them, leaving dangling entries in AmsState
    // subjects. Explicitly destroying them here ensures proper cleanup.
    destroy_detail_slots();

    // Clear observer guards before clearing widget pointers
    slots_version_observer_.reset();
    external_spool_observer_.reset();

    // Clean up sidebar before clearing panel references
    sidebar_.reset();

    // Clear global instance pointer
    g_overview_panel_instance.store(nullptr);

    // Clear widget references
    system_path_ = nullptr;
    system_path_area_ = nullptr;
    panel_ = nullptr;
    parent_screen_ = nullptr;
    cards_row_ = nullptr;
    unit_cards_.clear();

    // Clear detail view state
    detail_container_ = nullptr;
    detail_widgets_ = AmsDetailWidgets{};
    detail_path_canvas_ = nullptr;
    detail_unit_index_ = -1;
    detail_slot_count_ = 0;
    std::fill(std::begin(detail_slot_widgets_), std::end(detail_slot_widgets_), nullptr);

    // Reset subjects_initialized_ so observers are recreated on next access
    subjects_initialized_ = false;
}

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<AmsOverviewPanel> g_ams_overview_panel;
static lv_obj_t* s_ams_overview_panel_obj = nullptr;

// Lazy registration flag for XML component
static bool s_overview_registered = false;

static void ensure_overview_registered() {
    if (s_overview_registered) {
        return;
    }

    spdlog::info("[AMS Overview] Lazy-registering XML component");

    // Register sidebar and dryer card callbacks before component registration
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    helix::ui::AmsDryerCard::register_callbacks_static();

    // Register back button callback for detail view
    lv_xml_register_event_cb(nullptr, "on_ams_overview_back_clicked", [](lv_event_t* e) {
        LV_UNUSED(e);
        AmsOverviewPanel* panel = g_overview_panel_instance.load();
        if (panel) {
            panel->show_overview();
        }
    });

    // Register canvas widgets
    ui_system_path_canvas_register();
    ui_filament_path_canvas_register();

    // Register AMS slot widgets for inline detail view
    // (safe to call multiple times — each register function has an internal guard)
    ui_spool_canvas_register();
    ui_ams_slot_register();

    // Register the XML components (dependencies must be registered before overview panel)
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_unit_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_dryer_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/dryer_presets_modal.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_overview_panel.xml");

    s_overview_registered = true;
    spdlog::debug("[AMS Overview] XML registration complete");
}

void destroy_ams_overview_panel_ui() {
    if (s_ams_overview_panel_obj) {
        spdlog::info("[AMS Overview] Destroying panel UI to free memory");

        // Drain deferred observer callbacks while all pointers are still valid.
        // observe_int_sync queues lambdas via queue_update() that capture raw
        // panel/sidebar pointers. If subject changes fired observers between
        // the last timer tick and now, those lambdas are pending. Processing
        // them here prevents use-after-free when the panel is destroyed below.
        helix::ui::UpdateQueue::instance().drain();

        NavigationManager::instance().unregister_overlay_close_callback(s_ams_overview_panel_obj);
        NavigationManager::instance().unregister_overlay_instance(s_ams_overview_panel_obj);

        if (g_ams_overview_panel) {
            g_ams_overview_panel->clear_panel_reference();
        }

        helix::ui::safe_delete(s_ams_overview_panel_obj);
    }
}

AmsOverviewPanel& get_global_ams_overview_panel() {
    if (!g_ams_overview_panel) {
        g_ams_overview_panel =
            std::make_unique<AmsOverviewPanel>(get_printer_state(), get_moonraker_api());
        StaticPanelRegistry::instance().register_destroy("AmsOverviewPanel",
                                                         []() { g_ams_overview_panel.reset(); });
    }

    // Lazy create the panel UI if not yet created
    if (!s_ams_overview_panel_obj && g_ams_overview_panel) {
        ensure_overview_registered();

        // Initialize AmsState subjects BEFORE XML creation so bindings work
        AmsState::instance().init_subjects(true);

        // Create the panel on the active screen
        lv_obj_t* screen = lv_scr_act();
        s_ams_overview_panel_obj =
            static_cast<lv_obj_t*>(lv_xml_create(screen, "ams_overview_panel", nullptr));

        if (s_ams_overview_panel_obj) {
            // Initialize panel observers
            if (!g_ams_overview_panel->are_subjects_initialized()) {
                g_ams_overview_panel->init_subjects();
            }

            // Setup the panel
            g_ams_overview_panel->setup(s_ams_overview_panel_obj, screen);
            lv_obj_add_flag(s_ams_overview_panel_obj, LV_OBJ_FLAG_HIDDEN);

            // Register overlay instance for lifecycle management
            NavigationManager::instance().register_overlay_instance(s_ams_overview_panel_obj,
                                                                    g_ams_overview_panel.get());

            // Register close callback to destroy UI when overlay is closed
            NavigationManager::instance().register_overlay_close_callback(
                s_ams_overview_panel_obj, []() { destroy_ams_overview_panel_ui(); });

            spdlog::info("[AMS Overview] Lazy-created panel UI with close callback");
        } else {
            spdlog::error("[AMS Overview] Failed to create panel from XML");
        }
    }

    return *g_ams_overview_panel;
}

// ============================================================================
// Slot Context Menu (detail view)
// ============================================================================

void AmsOverviewPanel::handle_detail_slot_tap(int global_slot_index, lv_point_t click_pt) {
    spdlog::info("[{}] Detail slot {} tapped", get_name(), global_slot_index);

    // Find the local widget for positioning the menu
    if (detail_unit_index_ < 0)
        return;

    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    AmsSystemInfo info = backend->get_system_info();
    if (detail_unit_index_ >= static_cast<int>(info.units.size()))
        return;

    const auto& unit = info.units[detail_unit_index_];
    int local_index = global_slot_index - unit.first_slot_global_index;

    if (local_index < 0 || local_index >= detail_slot_count_)
        return;

    lv_obj_t* slot_widget = detail_slot_widgets_[local_index];
    if (!slot_widget)
        return;

    show_detail_context_menu(global_slot_index, slot_widget, click_pt);
}

void AmsOverviewPanel::show_detail_context_menu(int slot_index, lv_obj_t* near_widget,
                                                lv_point_t click_pt) {
    if (!parent_screen_ || !near_widget)
        return;

    // Create context menu on first use
    if (!context_menu_) {
        context_menu_ = std::make_unique<helix::ui::AmsContextMenu>();
    }

    context_menu_->set_action_callback(
        [this](helix::ui::AmsContextMenu::MenuAction action, int slot) {
            AmsBackend* backend = AmsState::instance().get_backend();

            switch (action) {
            case helix::ui::AmsContextMenu::MenuAction::LOAD:
                if (sidebar_) {
                    sidebar_->handle_load_with_preheat(slot);
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::UNLOAD:
                if (!backend) {
                    NOTIFY_WARNING("AMS not available");
                    return;
                }
                {
                    AmsError error = backend->unload_filament();
                    if (error.result != AmsResult::SUCCESS) {
                        NOTIFY_ERROR("Unload failed: {}", error.user_msg);
                    }
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::EDIT:
                // Navigate to the full AMS panel for edit/spoolman features
                spdlog::info("[{}] Edit requested for slot {} - navigating to AMS panel",
                             get_name(), slot);
                NOTIFY_INFO("Use the AMS detail panel for slot editing");
                break;

            case helix::ui::AmsContextMenu::MenuAction::SPOOLMAN:
                spdlog::info("[{}] Spoolman requested for slot {} - navigating to AMS panel",
                             get_name(), slot);
                NOTIFY_INFO("Use the AMS detail panel for Spoolman assignment");
                break;

            case helix::ui::AmsContextMenu::MenuAction::CANCELLED:
            default:
                break;
            }
        });

    // Check if the slot is loaded
    bool is_loaded = false;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        SlotInfo slot_info = backend->get_slot_info(slot_index);
        is_loaded = (slot_info.status == SlotStatus::LOADED);
    }

    context_menu_->set_click_point(click_pt);
    context_menu_->show_near_widget(parent_screen_, slot_index, near_widget, is_loaded, backend);
}

// ============================================================================
// Bypass Spool Interaction
// ============================================================================

void AmsOverviewPanel::on_bypass_spool_clicked(void* user_data) {
    auto* self = static_cast<AmsOverviewPanel*>(user_data);
    if (self) {
        self->handle_bypass_click();
    }
}

void AmsOverviewPanel::handle_bypass_click() {
    if (!parent_screen_ || !system_path_) {
        return;
    }

    // Capture click point from input device for menu positioning
    lv_point_t click_pt = {0, 0};
    lv_indev_t* indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &click_pt);
    }

    // Create context menu on first use
    if (!context_menu_) {
        context_menu_ = std::make_unique<helix::ui::AmsContextMenu>();
    }

    // Set callback to handle menu actions for external spool
    context_menu_->set_action_callback(
        [this](helix::ui::AmsContextMenu::MenuAction action, int /*slot*/) {
            switch (action) {
            case helix::ui::AmsContextMenu::MenuAction::EDIT:
            case helix::ui::AmsContextMenu::MenuAction::SPOOLMAN:
                show_edit_modal(-2);
                break;

            case helix::ui::AmsContextMenu::MenuAction::CLEAR_SPOOL:
                AmsState::instance().clear_external_spool_info();
                // bypass display update handled reactively by external_spool_observer_
                NOTIFY_INFO("External spool cleared");
                break;

            case helix::ui::AmsContextMenu::MenuAction::CANCELLED:
            default:
                break;
            }
        });

    // Position menu at click point, show for external spool
    context_menu_->set_click_point(click_pt);
    context_menu_->show_for_external_spool(parent_screen_, system_path_);
}

void AmsOverviewPanel::refresh_bypass_display() {
    if (!system_path_) {
        return;
    }

    auto ext_spool = AmsState::instance().get_external_spool_info();
    ui_system_path_canvas_set_bypass_has_spool(system_path_, ext_spool.has_value());

    if (ext_spool) {
        // Preserve current bypass active state, update color from spool
        auto* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsSystemInfo info = backend->get_system_info();
            int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());
            bool bypass_active = info.supports_bypass && (current_slot == -2);
            ui_system_path_canvas_set_bypass(system_path_, info.supports_bypass, bypass_active,
                                             ext_spool->color_rgb);
        }
    }

    ui_system_path_canvas_refresh(system_path_);
}

void AmsOverviewPanel::show_edit_modal(int slot_index) {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show edit modal - no parent screen", get_name());
        return;
    }

    // Create modal on first use (lazy initialization)
    if (!edit_modal_) {
        edit_modal_ = std::make_unique<helix::ui::AmsEditModal>();
    }

    // External spool (bypass/direct) - not managed by backend
    if (slot_index == -2) {
        auto ext = AmsState::instance().get_external_spool_info();
        SlotInfo initial_info = ext.value_or(SlotInfo{});
        initial_info.slot_index = -2;
        initial_info.global_index = -2;

        edit_modal_->set_completion_callback([](const helix::ui::AmsEditModal::EditResult& result) {
            if (result.saved) {
                AmsState::instance().set_external_spool_info(result.slot_info);
                // bypass display update handled reactively by external_spool_observer_
                NOTIFY_INFO("External spool updated");
            }
        });
        edit_modal_->show_for_slot(parent_screen_, -2, initial_info, api_);
        return;
    }

    spdlog::warn("[{}] show_edit_modal called with unsupported slot_index={}", get_name(),
                 slot_index);
}

// ============================================================================
// Multi-unit Navigation
// ============================================================================

void navigate_to_ams_panel() {
    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[AMS] navigate_to_ams_panel called with no backend");
        return;
    }

    AmsSystemInfo info = backend->get_system_info();

    if (info.is_multi_unit()) {
        // Multi-unit: show overview panel
        spdlog::info("[AMS] Multi-unit setup ({} units) - showing overview", info.unit_count());
        auto& overview = get_global_ams_overview_panel();
        lv_obj_t* panel = overview.get_panel();
        if (panel) {
            NavigationManager::instance().push_overlay(panel);
        }
    } else {
        // Single-unit (or no units): go directly to detail panel
        spdlog::info("[AMS] Single-unit setup - showing detail panel directly");
        auto& detail = get_global_ams_panel();
        lv_obj_t* panel = detail.get_panel();
        if (panel) {
            NavigationManager::instance().push_overlay(panel);
        }
    }
}
