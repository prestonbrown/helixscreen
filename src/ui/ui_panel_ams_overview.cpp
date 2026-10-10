// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_ams_overview.h"

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_environment_overlay.h"
#include "ui_ams_lane_bar.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_external_spool_menu.h"
#include "ui_filament_path_canvas.h"
#include "ui_nav.h"
#include "ui_overlay_qr_scanner.h"
#include "ui_panel_ams.h"
#include "ui_panel_common.h"
#include "ui_spool_canvas.h"
#include "ui_system_path_canvas.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "ams_unit_pages.h"
#include "app_globals.h"
#include "color_utils.h"
#include "data_root_resolver.h"
#include "display_numbering.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "observer_factory.h"
#include "overlay_base.h"
#include "printer_detector.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

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

/// Zoom animation duration (ms) for detail view transitions
static constexpr uint32_t DETAIL_ZOOM_DURATION_MS = 200;

/// Zoom animation start scale (25% = 64/256)
static constexpr int32_t DETAIL_ZOOM_SCALE_MIN = 64;

/// Zoom animation end scale (100% = 256/256)
static constexpr int32_t DETAIL_ZOOM_SCALE_MAX = 256;

// Global instance pointer for XML callback access (used by back button and animation callbacks)
static std::atomic<AmsOverviewPanel*> g_overview_panel_instance{nullptr};

/// Set a label to "N slots" / "N lanes" text, with null-safety
static void set_slot_count_label(lv_obj_t* label, helix::ui::LaneNoun noun, int slot_count) {
    if (!label) {
        return;
    }
    lv_label_set_text(label, helix::ui::lane_count_label(noun, slot_count).c_str());
}

/// The measured width one mini bar gets for this container and lane count.
/// Both create_mini_bars() and the update-path rebuild check recompute it, so
/// the two can never disagree about when a rebuild is due.
static int32_t measured_bar_width(lv_obj_t* bars_container, int slot_count) {
    lv_obj_update_layout(bars_container);
    int32_t container_width = lv_obj_get_content_width(bars_container);
    if (container_width <= 0) {
        container_width = 80; // Fallback if layout not yet calculated
    }
    int32_t gap = theme_manager_get_spacing("space_xxs");
    return ams_draw::calc_bar_width(container_width, slot_count, gap, MINI_BAR_MIN_WIDTH_PX,
                                    MINI_BAR_MAX_WIDTH_PX);
}

// ============================================================================
// Construction
// ============================================================================

AmsOverviewPanel::AmsOverviewPanel(PrinterState& printer_state, IMoonrakerAPI* api)
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

        // Observe slots_version to auto-refresh when slot data changes. In the unit view the
        // per-slot observers drive the visual state (color, pulse, highlight); what the view
        // has to react to is a change in which pages exist or what a page shows. In the
        // overview it is the unit cards that refresh.
        using helix::ui::observe;
        slots_version_observer_ = observe<int>(
            AmsState::instance().get_slots_version_subject(), this,
            [](AmsOverviewPanel* self, int) {
                self->on_state_changed("AmsOverviewPanel::refresh_units");
            },
            AmsState::instance().get_subjects_lifetime());

        // Observe current_slot to reactively update lane highlights when the active
        // slot changes (e.g., slot selected without load/unload).
        current_slot_observer_ = observe<int>(
            AmsState::instance().get_current_slot_subject(), this,
            [](AmsOverviewPanel* self, int) {
                self->on_state_changed("AmsOverviewPanel::refresh_units/slot");
            },
            AmsState::instance().get_subjects_lifetime());

        // Which units' dryers run decides the drying glyph on the hub's off-page stubs.
        dryer_version_observer_ = observe<int>(
            AmsState::instance().get_units_dryer_version_subject(), this,
            [](AmsOverviewPanel* self, int) {
                if (self->open_ && self->panel_ && self->unit_view_active_)
                    self->sync_pages(/*reopen=*/false);
            },
            AmsState::instance().get_subjects_lifetime());

        // The dots are built from this count; their pitch follows it.
        page_count_observer_ = observe<int>(
            AmsState::instance().get_ams_page_count_subject(), this,
            [](AmsOverviewPanel* self, int) { self->layout_page_dots(); },
            AmsState::instance().get_subjects_lifetime());

        // Observe external spool color changes to reactively update bypass display.
        // NOTE: set_external_spool_info() calls lv_subject_set_int() directly (not via
        // ui_queue_update) which is safe because all current callers are on the LVGL thread.
        // If callers from background threads are added, those must use ui_queue_update().
        external_spool_observer_ = observe<int>(
            AmsState::instance().get_external_spool_color_subject(), this,
            [](AmsOverviewPanel* self, int /*color_int*/) {
                // Delegate to existing refresh helper which reads full spool info
                self->refresh_bypass_display();
            },
            AmsState::instance().get_subjects_lifetime());

        // Engaging bypass changes no slot, so neither the path refresh nor the
        // external-spool observer above fires for it. The ring needs its own.
        bypass_active_observer_ = observe<int>(
            AmsState::instance().get_bypass_active_subject(), this,
            [](AmsOverviewPanel* self, int /*active*/) { self->refresh_bypass_display(); },
            AmsState::instance().get_subjects_lifetime());
    });
}

void AmsOverviewPanel::on_state_changed(const char* tag) {
    if (!open_ || !panel_)
        return;
    if (unit_view_active_) {
        sync_pages(/*reopen=*/false);
        return;
    }
    // Defer rebuild (#80) AND use safe_clean_children in refresh_units callees (#776):
    // object_lifetime_.defer moves work off the observer callback's stack, and
    // safe_clean_children schedules child deletion via lv_obj_delete_async so sync
    // lv_obj_clean() can't corrupt LVGL's event linked list.
    if (!units_rebuild_pending_) {
        units_rebuild_pending_ = true;
        object_lifetime_.defer(tag, [this]() {
            units_rebuild_pending_ = false;
            if (panel_ && cards_row_ && !unit_view_active_)
                refresh_units();
        });
    }
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
    cards_row_ = helix::ui::find_required(panel_, "unit_cards_row", get_name());
    if (!cards_row_) {
        return;
    }
    lv_obj_add_event_cb(cards_row_, &AmsOverviewPanel::on_cards_row_scrolled, LV_EVENT_SCROLL,
                        this);

    // Find system path area and create path canvas widget
    system_path_area_ = helix::ui::find_required(panel_, "system_path_area", get_name());
    if (system_path_area_) {
        system_path_ = ui_system_path_canvas_create(system_path_area_);
        if (system_path_) {
            lv_obj_set_size(system_path_, LV_PCT(100), LV_PCT(100));
            spdlog::debug("[{}] Created system path canvas", get_name());

            bypass_widgets_ = helix::ui::bypass_spool_create(
                system_path_area_, &AmsOverviewPanel::on_bypass_spool_clicked, this);
            // SIZE_CHANGED only — listening to DRAW events would invalidate
            // during render and trip lv_inv_area assertions in LVGL 9.
            lv_obj_add_event_cb(system_path_, &AmsOverviewPanel::on_system_path_size_changed,
                                LV_EVENT_SIZE_CHANGED, this);
        }
    }

    // Find the unit view's widgets
    detail_container_ = helix::ui::find_required(panel_, "unit_detail_container", get_name());
    lv_obj_t* detail_unit = helix::ui::find_required(panel_, "detail_unit_detail", get_name());
    detail_widgets_ = ams_detail_find_widgets(detail_unit);
    detail_path_canvas_ = helix::ui::find_required(panel_, "detail_path_canvas", get_name());
    path_container_ = helix::ui::find_required(panel_, "detail_path_container", get_name());
    prev_button_ = helix::ui::find_required(panel_, "page_prev_button", get_name());
    next_button_ = helix::ui::find_required(panel_, "page_next_button", get_name());
    page_dots_ = helix::ui::find_required(panel_, "page_dots", get_name());

    if (detail_container_) {
        // A horizontal swipe anywhere over the unit pages it; gestures have no declarative
        // form. Only the unit view listens, so a swipe over the overview pages nothing.
        lv_obj_add_event_cb(detail_container_, &AmsOverviewPanel::on_unit_view_gesture,
                            LV_EVENT_GESTURE, this);
    }
    if (detail_path_canvas_) {
        // The arrows follow the hub box, and the bypass spool follows the merge point:
        // both are measured from the canvas, so both wait for its size.
        // SIZE_CHANGED only — listening to DRAW events would invalidate during render
        // and trip lv_inv_area assertions in LVGL 9.
        lv_obj_add_event_cb(detail_path_canvas_, &AmsOverviewPanel::on_path_layout_changed,
                            LV_EVENT_SIZE_CHANGED, this);
    }
    if (page_dots_) {
        lv_obj_add_event_cb(page_dots_, &AmsOverviewPanel::on_page_dots_resized,
                            LV_EVENT_SIZE_CHANGED, this);
        lv_obj_add_event_cb(page_dots_, &AmsOverviewPanel::on_page_dots_resized,
                            LV_EVENT_CHILD_CHANGED, this);
    }
    if (path_container_) {
        page_bypass_widgets_ = helix::ui::bypass_spool_create(
            path_container_, &AmsOverviewPanel::on_bypass_spool_clicked, this);
    }

    // Store global instance for callback access (back button + animation callbacks)
    g_overview_panel_instance.store(this);

    // Set up the shared sidebar component
    sidebar_ = helix::ui::AmsOperationSidebar::attach(printer_state_, panel_);

    // Initial population from backend state
    refresh_units();

    spdlog::debug("[{}] Setup complete!", get_name());
}

void AmsOverviewPanel::on_activate() {
    // Reset coalescing flag to prevent stale state from a previous deactivation
    units_rebuild_pending_ = false;

    spdlog::debug("[{}] Activated - syncing from backend", get_name());

    const bool opening = !open_;
    open_ = true;
    if (!sidebar_) {
        sidebar_ = helix::ui::AmsOperationSidebar::attach(printer_state_, panel_);
    }

    AmsState::instance().sync_from_backend();

    // The closed panel's card observers skipped every change; rebuild once.
    if (opening && cards_row_ && !unit_view_active_) {
        refresh_units();
    }

    if (sidebar_)
        sidebar_->sync_from_state();

    // Coming back from an overlay keeps the page it was on.
    if (unit_view_active_) {
        sync_pages(/*reopen=*/false);
    }
}

void AmsOverviewPanel::run_close() {
    if (auto* p = helix::lazy_global_if_exists<AmsOverviewPanel>()) {
        p->on_closed();
    }
}

void AmsOverviewPanel::on_closed() {
    // A close callback can run after a reopen has already pushed the panel again.
    // Its own callback was consumed by that run, so arm the next close again.
    if (panel_ && (helix::nav::is_in_stack(panel_) || helix::nav::is_push_pending(panel_))) {
        helix::nav::on_close(panel_, &AmsOverviewPanel::run_close);
        return;
    }
    open_ = false;
    sidebar_.reset();
    // The next open starts at the overview.
    if (unit_view_active_) {
        reset_to_overview();
    }
    if (detail_path_canvas_) {
        helix::ui::filament_path_canvas_release_buffer(detail_path_canvas_);
    }
}

bool AmsOverviewPanel::rebuild() {
    if (!panel_ || helix::nav::is_showing(panel_)) {
        return false;
    }
    destroy_ams_overview_panel_ui();
    return true;
}

void AmsOverviewPanel::on_deactivating(DeactivateReason) {
    spdlog::debug("[{}] Deactivated", get_name());

    // Dismiss context menu if open
    if (context_menu_ && context_menu_->is_visible()) {
        context_menu_->hide();
    }
}

// ============================================================================
// Unit Card Management
// ============================================================================

void AmsOverviewPanel::refresh_units() {
    if (!cards_row_) {
        return;
    }
    ++units_refreshes_;

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
        create_unit_cards(info, backend->lane_noun());
    } else {
        // Same number of units - update existing cards in place. unit_cards_ is in
        // DISPLAY order (see create_unit_cards), so each card names its own unit.
        for (auto& uc : unit_cards_) {
            if (uc.unit_index >= 0 && uc.unit_index < new_unit_count)
                update_unit_card(uc, info.units[uc.unit_index], backend->lane_noun());
        }
    }

    // Update system path visualization
    refresh_system_path(info, current_slot);
}

void AmsOverviewPanel::create_unit_cards(const AmsSystemInfo& info, helix::ui::LaneNoun noun) {
    if (!cards_row_) {
        return;
    }

    // Flush pending layout so LVGL doesn't reference children we're about to
    // destroy (use-after-free in layout_update_core, issue #711). Called from
    // refresh_units under the slots_version observer — safe_clean_children
    // escapes UpdateQueue::process_pending() so sync deletion can't corrupt
    // LVGL's event linked list (#776).
    lv_obj_update_layout(cards_row_);
    helix::ui::safe_clean_children(cards_row_);
    unit_cards_.clear();

    const int unit_count = static_cast<int>(info.units.size());
    if (unit_count > AmsState::MAX_UNITS) {
        // One line naming the cap, instead of seven XML parser warnings per
        // excess card. Those cards still render their slots; only the
        // temperature/humidity badge is unavailable.
        spdlog::warn("[{}] Backend reports {} units but only {} have environment subjects - "
                     "unit {} onward render without the temp/humidity badge",
                     get_name(), unit_count, AmsState::MAX_UNITS, AmsState::MAX_UNITS + 1);
    }

    // Cards run in nozzle order (SystemToolLayout::display_order).
    const std::vector<int> order =
        ams_draw::compute_system_tool_layout(info, AmsState::instance().get_backend())
            .display_order;

    for (int slot = 0; slot < unit_count; ++slot) {
        const int i = order[slot];
        const AmsUnit& unit = info.units[i];
        UnitCard uc;
        uc.unit_index = i;

        // Create card from XML component — all static styling is declarative.
        // Fully-expanded per-unit subject names (kept alive across lv_xml_create) so
        // each card binds to its own unit's environment-indicator subjects. Units
        // past MAX_UNITS get the always-off placeholders — AmsState owns which is
        // which, since it owns the cap and the registrations.
        const AmsState::EnvIndicatorSubjectNames s = AmsState::env_indicator_subject_names(i);
        const std::string absent = AmsState::unit_absent_subject_name(i);
        const std::string disconnected = AmsState::unit_disconnected_subject_name(i);
        const char* attrs[] = {"absent",
                               absent.c_str(),
                               "disconnected",
                               disconnected.c_str(),
                               "temp_text",
                               s.temp_text.c_str(),
                               "humidity_text",
                               s.humidity_text.c_str(),
                               "humidity_status",
                               s.humidity_status.c_str(),
                               "humidity_visible",
                               s.humidity_visible.c_str(),
                               "visible",
                               s.visible.c_str(),
                               "drying_active",
                               s.drying_active.c_str(),
                               "drying_text",
                               s.drying_text.c_str(),
                               nullptr,
                               nullptr};
        uc.card = static_cast<lv_obj_t*>(lv_xml_create(cards_row_, "ams_unit_card", attrs));
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
        uc.logo_image = helix::ui::find_required(uc.card, "unit_logo", get_name());
        uc.name_label = helix::ui::find_required(uc.card, "unit_name", get_name());
        uc.bars_container = helix::ui::find_required(uc.card, "bars_container", get_name());
        uc.slot_count_label = helix::ui::find_required(uc.card, "slot_count", get_name());

        // Stamp the unit index on the environment indicator so its click handler
        // knows which unit's overlay to open.
        if (lv_obj_t* ind = helix::ui::find_required(uc.card, "env_indicator", get_name())) {
            lv_obj_set_user_data(ind, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        }

        // Set logo image based on AMS system type
        ams_draw::apply_logo(uc.logo_image, unit, info);

        // Set dynamic content only — unit name and slot count vary per unit
        uc.display_name = ams_draw::get_unit_display_name(unit, i);
        if (uc.name_label) {
            lv_label_set_text(uc.name_label, uc.display_name.c_str());
        }

        set_slot_count_label(uc.slot_count_label, noun, unit.slot_count);

        // Create the mini bars for this unit (dynamic — slot count varies)
        create_mini_bars(uc, unit);

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

void AmsOverviewPanel::update_unit_card(UnitCard& card, const AmsUnit& unit,
                                        helix::ui::LaneNoun noun) {
    if (!card.card) {
        return;
    }

    // Update name label. Keep display_name in step - it is the only untruncated
    // copy, and publish_cards_compact() measures it.
    card.display_name = ams_draw::get_unit_display_name(unit, card.unit_index);
    if (card.name_label) {
        lv_label_set_text(card.name_label, card.display_name.c_str());
    }

    // Rebuild the mini bars only when a geometry input changed (lane count or
    // the measured width). Color, fill, active and error repaint in place
    // through the per-slot subjects each ams_lane_bar observes.
    // Flush pending layout first — deferred callbacks can run between layout
    // passes, and cleaning children while LVGL still references them causes
    // use-after-free in layout_update_core (issue #711). Called from refresh_units
    // under the slots_version observer — safe_clean_children escapes
    // UpdateQueue::process_pending() so sync deletion can't corrupt LVGL's
    // event linked list (#776).
    if (card.bars_container) {
        lv_obj_update_layout(card.bars_container);
        int slot_count = static_cast<int>(unit.slots.size());
        if (helix::ui::lane_bars_stale(card.bars_built,
                                       {unit.first_slot_global_index, slot_count,
                                        measured_bar_width(card.bars_container, slot_count)})) {
            helix::ui::safe_clean_children(card.bars_container);
            create_mini_bars(card, unit);
        }
    }

    // Update slot count
    set_slot_count_label(card.slot_count_label, noun, unit.slot_count);

    // Update error badge visibility and color
    if (card.error_badge) {
        bool animate = DisplaySettingsManager::instance().get_animations_enabled();
        auto worst = ams_draw::worst_unit_severity(unit);
        ams_draw::update_error_badge(card.error_badge, unit.has_any_error(), worst, animate);
    }
}

void AmsOverviewPanel::create_mini_bars(UnitCard& card, const AmsUnit& unit) {
    if (!card.bars_container) {
        return;
    }

    int slot_count = static_cast<int>(unit.slots.size());
    if (slot_count <= 0) {
        card.bars_built = {unit.first_slot_global_index, 0, 0};
        return;
    }

    // Bar width is measured to fit the container (rule 8's measured-layout
    // exception keeps it in C++); everything else - state, fill, color, the
    // active and error decorations - flows from AmsState's per-slot subjects
    // inside ams_lane_bar, so the bars repaint in place instead of being
    // rebuilt on every refresh.
    int32_t bar_width = measured_bar_width(card.bars_container, slot_count);
    card.bars_built = {unit.first_slot_global_index, slot_count, bar_width};

    helix::ui::ams_lane_bar_create_range(card.bars_container, unit.first_slot_global_index,
                                         slot_count, bar_width, MINI_BAR_HEIGHT_PX);
}

// ============================================================================
// System Path
// ============================================================================

void AmsOverviewPanel::push_unit_anchors(bool relayout) {
    if (!system_path_ || !cards_row_)
        return;

    // Only the refresh path needs a layout flush (the cards were just created).
    // During LV_EVENT_SCROLL the coordinates already carry the new scroll offset,
    // and forcing a relayout mid-scroll would re-enter LVGL's layout pass on every
    // scroll step.
    if (relayout)
        lv_obj_update_layout(cards_row_);

    lv_area_t path_coords;
    lv_obj_get_coords(system_path_, &path_coords);

    int32_t narrowest = LV_COORD_MAX;
    for (const auto& uc : unit_cards_) {
        if (!uc.card || uc.unit_index < 0)
            continue;
        lv_area_t card_coords;
        lv_obj_get_coords(uc.card, &card_coords);
        int32_t center_x = (card_coords.x1 + card_coords.x2) / 2 - path_coords.x1;
        ui_system_path_canvas_set_unit_x(system_path_, uc.unit_index, center_x);
        narrowest = LV_MIN(narrowest, lv_area_get_width(&card_coords));
    }

    if (narrowest != LV_COORD_MAX) {
        publish_cards_compact(narrowest);
    }
}

// Decide whether the unit cards have to shed decoration to stay readable, and
// publish it for ams_unit_card.xml to bind against.
//
// This is measured rather than derived from a token or a breakpoint on purpose:
// card width is (row width / unit count), so a 5-unit rig on a large screen is
// tighter than a 2-unit rig on a small one, and neither input alone predicts it.
// DECLARATIVE_OK: computing the DATA in C++ is the rule - the appearance change
// itself stays in XML, bound to this subject.
void AmsOverviewPanel::publish_cards_compact(int32_t narrowest_card_w) {
    // Content width the card has left after its own padding.
    const int32_t content_w = narrowest_card_w - 2 * theme_manager_get_spacing("space_sm");

    // Widest unit name that has to fit, and the logo's declared size.
    //
    // NONE of these inputs depend on whether the logo is currently shown - that
    // is deliberate. Measuring the logo widget would make the rule feed back on
    // its own output (hide it, the row gets roomier, unhide it, repeat), so the
    // logo contributes its STYLE width, which LVGL keeps while hidden, and the
    // card width comes from row-width/unit-count, which the logo does not affect.
    int32_t widest_name = 0;
    int32_t logo_w = 0;
    for (const auto& uc : unit_cards_) {
        if (uc.name_label && !uc.display_name.empty()) {
            const lv_font_t* font = lv_obj_get_style_text_font(uc.name_label, LV_PART_MAIN);
            if (font) {
                lv_point_t size;
                // Measure the STORED name, never lv_label_get_text(): long_mode=dots
                // rewrites the label's buffer with the ellipsized string, so reading
                // it back measures the truncation and always reports a fit.
                lv_text_get_size(&size, uc.display_name.c_str(), font, 0, 0, LV_COORD_MAX,
                                 LV_TEXT_FLAG_NONE);
                widest_name = LV_MAX(widest_name, size.x);
            }
        }
        if (uc.logo_image && logo_w == 0) {
            logo_w = lv_obj_get_style_width(uc.logo_image, LV_PART_MAIN);
        }
    }

    // Hide the logo exactly when the name cannot be read beside it. If the name
    // does not fit even without the logo, hiding still buys the title its widest
    // possible ellipsis.
    const int32_t needed = logo_w + theme_manager_get_spacing("space_xs") + widest_name;
    const bool compact = (widest_name > 0) && (content_w < needed);

    lv_subject_t* subj = AmsState::instance().get_cards_compact_subject();
    if (subj && lv_subject_get_int(subj) != (compact ? 1 : 0)) {
        lv_subject_set_int(subj, compact ? 1 : 0);
        spdlog::debug("[{}] Unit cards {}: content {}px, logo {} + name {} = {}px needed",
                      get_name(), compact ? "COMPACT (logo hidden)" : "full", content_w, logo_w,
                      widest_name, needed);
    }
}

// The card row scrolls independently of the path canvas below it, so the stem
// anchors have to be re-sampled on every scroll - without this they keep
// pointing at where each card used to be (visible as connector lines that stay
// put while the cards slide under them).
void AmsOverviewPanel::on_cards_row_scrolled(lv_event_t* e) {
    // DECLARATIVE_OK: LV_EVENT_SCROLL has no declarative equivalent, and the
    // value being recomputed is measured pixel geometry.
    auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e));
    if (self)
        self->push_unit_anchors(/*relayout=*/false);
}

void AmsOverviewPanel::refresh_system_path(const AmsSystemInfo& info, int current_slot) {
    if (!system_path_)
        return;

    int unit_count = static_cast<int>(info.units.size());
    ui_system_path_canvas_set_unit_count(system_path_, unit_count);

    push_unit_anchors(/*relayout=*/true);

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

    // Set bypass path state (canvas draws the connecting lines only)
    bool bypass_active = info.supports_bypass && (current_slot == -2);
    uint32_t bypass_color = 0x888888; // Default gray when no external spool assigned
    auto ext_spool = AmsState::instance().get_external_spool_info();
    if (ext_spool) {
        bypass_color = ext_spool->color_rgb;
    }
    ui_system_path_canvas_set_bypass(
        system_path_, helix::ui::bypass_node_visible_for(AmsState::instance().get_backend()),
        bypass_active, bypass_color);

    // Drive the shared BypassSpoolWidgets overlay from current state. On AFC the
    // whole node disappears while bypass is disengaged — AFC reports a virtual
    // bypass whether or not one exists, so leaving it on the path advertised a
    // bypass the machine does not have (#1229).
    const bool show_bypass = helix::ui::bypass_node_visible_for(AmsState::instance().get_backend());
    if (bypass_widgets_.valid()) {
        helix::ui::bypass_spool_set_visible(bypass_widgets_, show_bypass);
        if (show_bypass) {
            bypass_spool_set_has_spool(bypass_widgets_, ext_spool.has_value());
            bypass_spool_set_color(bypass_widgets_, bypass_color);
            bypass_spool_set_material(bypass_widgets_, (ext_spool && !ext_spool->material.empty())
                                                           ? ext_spool->material.c_str()
                                                           : "");
            update_bypass_widgets_position();
        }
    }

    // Compute physical tool layout (handles HUB units with unique per-lane mapped_tools)
    auto* backend = AmsState::instance().get_backend();
    auto tool_layout = ams_draw::compute_system_tool_layout(info, backend);

    // Set per-unit hub sensor states, topology, and tool routing
    for (int i = 0; i < unit_count && i < static_cast<int>(info.units.size()); ++i) {
        const auto& unit = info.units[i];
        ui_system_path_canvas_set_unit_hub_sensor(system_path_, i, unit.has_hub_sensor,
                                                  unit.hub_sensor_triggered);

        // The unit's furthest-loaded lane colors its route when it is not the
        // active unit.
        PathSegment lane_seg = PathSegment::NONE;
        uint32_t lane_color = 0;
        if (backend) {
            for (int s = 0; s < unit.slot_count; ++s) {
                const int global = unit.first_slot_global_index + s;
                const PathSegment seg = backend->get_slot_filament_segment(global);
                if (seg > lane_seg) {
                    lane_seg = seg;
                    lane_color = backend->get_slot_info(global).color_rgb;
                }
            }
        }
        ui_system_path_canvas_set_unit_lane(system_path_, i, static_cast<int>(lane_seg),
                                            lane_color);

        PathTopology topo = unit.topology;
        if (backend) {
            topo = backend->get_unit_topology(i);
        }
        ui_system_path_canvas_set_unit_topology(system_path_, i, static_cast<int>(topo));
        helix::ui::ui_system_path_canvas_set_unit_absent(system_path_, i, unit.absent);

        if (i < static_cast<int>(tool_layout.units.size())) {
            const auto& utl = tool_layout.units[i];
            ui_system_path_canvas_set_unit_tools(system_path_, i, utl.tool_count,
                                                 utl.first_physical_tool);

            // The unit's hub and the buffer under it: the box the unit view shows
            // for the hub's first unit. Units on one hub get one group number.
            const int buffer_unit = helix::ui::overview_buffer_unit(tool_layout, i);
            helix::ui::BufferBoxState buffer;
            if (buffer_unit >= 0) {
                buffer = helix::ui::ams_detail_buffer_box(info, buffer_unit);
            }
            helix::ui::ui_system_path_canvas_set_unit_hub(
                system_path_, i, helix::ui::overview_hub_group(tool_layout, i), buffer.present,
                buffer.fault, buffer.label);
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

    // Toolhead badges. With extruder identity these read "E<n>" and name the
    // physical extruder; without it they fall back to the legacy AFC lane-alias
    // "T<n>" labels. compute_tool_badge_labels() owns that decision.
    {
        const auto badges =
            ams_draw::compute_tool_badge_labels(tool_layout, info, current_slot, active_tool);
        if (!badges.numbers.empty()) {
            ui_system_path_canvas_set_tool_label_prefix(system_path_, badges.prefix);
            ui_system_path_canvas_set_tool_virtual_numbers(system_path_, badges.numbers.data(),
                                                           static_cast<int>(badges.numbers.size()));
        }
    }

    // The buffer on the output line (single toolhead): the unit view's box for the
    // system reading.
    {
        const helix::ui::BufferBoxState box = helix::ui::ams_detail_buffer_box(info, -1);
        helix::ui::ui_system_path_canvas_set_buffer(system_path_, box.present, box.fault,
                                                    box.label);
    }

    // Filament reach, error and the toolhead sensor
    {
        ui_system_path_canvas_set_filament_segment(
            system_path_,
            lv_subject_get_int(AmsState::instance().get_path_filament_segment_subject()));
        ui_system_path_canvas_set_error_segment(
            system_path_, backend ? static_cast<int>(backend->infer_error_segment()) : 0);

        bool has_toolhead = std::any_of(info.units.begin(), info.units.end(),
                                        [](const AmsUnit& u) { return u.has_toolhead_sensor; });
        ui_system_path_canvas_set_toolhead_sensor(system_path_, has_toolhead);
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

    spdlog::info("[AMS Overview] Unit {} clicked - showing its page", unit_index);

    // Zoom into the unit view inline (swaps left column content, no overlay push)
    self->show_unit_view(unit_index);

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
// Unit view (one unit per page)
// ============================================================================

namespace {

/// A unit's identity across rebuilds: its name, or its first slot when it has none. The
/// position in the backend's list is not stable, and neither is the slot a unit starts at
/// when a backend that sorts its units gains one.
std::string unit_key(const AmsUnit& unit) {
    return unit.name.empty() ? "#" + std::to_string(unit.first_slot_global_index) : unit.name;
}

/// What makes two page lists the same screen: the same units in the same order, grouped the
/// same way. The drying flags are not part of it; they only repaint the stubs.
bool same_page_structure(const std::vector<helix::ui::UnitPage>& a,
                         const std::vector<std::string>& units_a,
                         const std::vector<helix::ui::UnitPage>& b,
                         const std::vector<std::string>& units_b) {
    if (units_a != units_b || a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].group != b[i].group || a[i].same_hub_before != b[i].same_hub_before ||
            a[i].same_hub_after != b[i].same_hub_after)
            return false;
    }
    return true;
}

} // namespace

void AmsOverviewPanel::sync_pages(bool reopen, int focus_unit) {
    if (!panel_ || !unit_view_active_)
        return;
    ++pages_refreshes_;

    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        show_overview();
        return;
    }

    const AmsSystemInfo info = backend->get_system_info();
    auto pages = helix::ui::build_unit_pages(info, [&](int pos) {
        const DryerInfo dryer = backend->get_dryer_info(info.units[pos].unit_index);
        return dryer.supported && dryer.active;
    });
    if (pages.empty()) {
        show_overview();
        return;
    }
    std::vector<std::string> units;
    units.reserve(pages.size());
    for (const auto& p : pages)
        units.push_back(unit_key(info.units[p.unit_index]));

    int target = -1;
    if (focus_unit >= 0) {
        for (size_t i = 0; i < pages.size(); ++i) {
            if (pages[i].unit_index == focus_unit)
                target = static_cast<int>(i);
        }
    }
    if (target < 0 && reopen) {
        target = helix::ui::initial_unit_page(pages, info);
    } else if (target < 0) {
        // The unit on screen stays on screen while it still has a page; otherwise the page
        // number holds, clamped into the pages that remain.
        for (size_t i = 0; i < units.size(); ++i) {
            if (units[i] == shown_unit_key_)
                target = static_cast<int>(i);
        }
        if (target < 0)
            target = std::clamp(page_, 0, static_cast<int>(pages.size()) - 1);
    }

    const bool restructured = !same_page_structure(pages_, page_units_, pages, units);
    const bool moved = target != page_;
    pages_ = std::move(pages);
    page_units_ = std::move(units);
    page_ = target;

    const bool new_unit = page_units_[page_] != shown_unit_key_;
    if (reopen || restructured || moved || new_unit) {
        spdlog::debug("[{}] Pages {} (page {}, {} units)", get_name(),
                      reopen ? "opened" : "changed", page_, pages_.size());
    }
    // An open lays the spool box out again: it was last measured at whatever size the panel
    // had when it was hidden.
    show_current_page(info, /*relayout=*/reopen);
}

void AmsOverviewPanel::show_current_page(const AmsSystemInfo& info, bool relayout) {
    if (!panel_ || page_ < 0 || page_ >= static_cast<int>(pages_.size()))
        return;

    const helix::ui::UnitPage& page = pages_[page_];
    if (page.unit_index < 0 || page.unit_index >= static_cast<int>(info.units.size()))
        return;
    const AmsUnit& unit = info.units[page.unit_index];

    const std::string key = unit_key(unit);
    const bool new_unit = key != shown_unit_key_;
    shown_unit_key_ = key;
    shown_unit_pos_ = page.unit_index;

    auto& ams = AmsState::instance();
    ams.set_unit_page(static_cast<int>(pages_.size()), page_);
    // The unit's own number, not its position: that is what the viewed-unit flag and the
    // environment mirror are keyed on.
    ams.set_viewed_unit(unit.unit_index);
    ams.set_unit_page_header(ams_draw::get_unit_display_name(unit, page.unit_index),
                             ams_draw::unit_logo_path(unit, info));

    if (relayout || new_unit || detail_slot_count_ != std::min(unit.slot_count, MAX_DETAIL_SLOTS)) {
        create_detail_slots(info, page.unit_index);
    }
    helix::ui::ams_detail_sync_slot_states(detail_slot_widgets_, detail_slot_count_);

    if (detail_path_canvas_) {
        // The hub box is sized for the widest unit of this hub, at the lane pitch that
        // unit's slot row has, so it holds still as the pages go by.
        int hub_lanes = 0;
        for (const auto& other : pages_) {
            if (other.group == page.group && other.unit_index < static_cast<int>(info.units.size()))
                hub_lanes = std::max(hub_lanes, info.units[other.unit_index].slot_count);
        }
        int32_t lane_pitch = 0;
        if (hub_lanes > 0 && detail_widgets_.slot_grid) {
            lv_obj_t* slot_area = lv_obj_get_parent(detail_widgets_.slot_grid);
            lv_obj_update_layout(slot_area);
            const AmsSlotLayout widest =
                helix::ui::ams_detail_slot_layout(lv_obj_get_content_width(slot_area), hub_lanes);
            lane_pitch = widest.slot_width - widest.overlap;
        }
        ui_filament_path_canvas_set_fixed_hub(detail_path_canvas_, hub_lanes, lane_pitch);
        ui_filament_path_canvas_set_offpage_units(detail_path_canvas_, page.same_hub_before,
                                                  page.drying_before, page.same_hub_after,
                                                  page.drying_after);
    }
    update_path_canvas();
    layout_paging_controls();
    scroll_current_dot_into_view();
}

void AmsOverviewPanel::step_page(int delta) {
    const int next = page_ + delta;
    if (!unit_view_active_ || page_ < 0 || next < 0 || next >= static_cast<int>(pages_.size()))
        return;
    page_ = next;
    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;
    show_current_page(backend->get_system_info(), /*relayout=*/false);
}

bool AmsOverviewPanel::next_page() {
    const int before = page_;
    step_page(+1);
    return page_ != before;
}

bool AmsOverviewPanel::prev_page() {
    const int before = page_;
    step_page(-1);
    return page_ != before;
}

void AmsOverviewPanel::show_unit_view(int unit_index) {
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
    // unit_cards_ is in display order, which is not unit order (create_unit_cards
    // sorts by nozzle), so find the card that names this unit rather than
    // indexing - indexing zoomed the wrong card open.
    lv_area_t card_coords = {};
    auto card_it =
        std::find_if(unit_cards_.begin(), unit_cards_.end(), [unit_index](const UnitCard& c) {
            return c.unit_index == unit_index && c.card != nullptr;
        });
    if (card_it != unit_cards_.end()) {
        lv_obj_update_layout(card_it->card);
        lv_obj_get_coords(card_it->card, &card_coords);
    }

    spdlog::info("[{}] Showing the page of unit {} ({})", get_name(), unit_index,
                 info.units[unit_index].name);

    // Swap visibility: the overview's elements hide, the unit view shows. Its widgets are
    // laid out before the page measures them.
    unit_view_active_ = true;
    AmsState::instance().set_unit_view_active(true);
    lv_obj_update_layout(detail_container_);
    sync_pages(/*reopen=*/true, unit_index);
    if (!unit_view_active_)
        return;

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

    // The page no longer follows the backend from here on, even while the zoom-out plays.
    unit_view_active_ = false;
    AmsState::instance().set_viewed_unit(-1);

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
            // Reset transform properties for next use
            lv_obj_set_style_transform_scale(container, DETAIL_ZOOM_SCALE_MAX, LV_PART_MAIN);
            lv_obj_set_style_opa(container, LV_OPA_COVER, LV_PART_MAIN);

            // Show overview elements (use global instance since lambda has no 'this')
            AmsOverviewPanel* self = g_overview_panel_instance.load();
            if (self) {
                self->reset_to_overview();
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
        reset_to_overview();
    }
}

void AmsOverviewPanel::reset_to_overview() {
    // Zoom-out has finished or is skipped; whatever else was in flight is moot.
    unit_view_active_ = false;
    if (detail_container_) {
        lv_obj_set_style_transform_scale(detail_container_, DETAIL_ZOOM_SCALE_MAX, LV_PART_MAIN);
        lv_obj_set_style_opa(detail_container_, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (context_menu_ && context_menu_->is_visible()) {
        context_menu_->hide();
    }

    auto& ams = AmsState::instance();
    ams.set_viewed_unit(-1);
    ams.set_unit_view_active(false);
    ams.set_unit_page(0, 0);
    ams.set_unit_page_header("", nullptr);

    destroy_detail_slots();
    pages_.clear();
    page_units_.clear();
    page_ = -1;
    shown_unit_key_.clear();
    shown_unit_pos_ = -1;

    refresh_units();
}

void AmsOverviewPanel::create_detail_slots(const AmsSystemInfo& info, int unit_pos) {
    destroy_detail_slots();

    // The environment readout is keyed on the unit's own number.
    ams_detail_pre_show_env_indicator(detail_widgets_, info.units[unit_pos].unit_index);

    auto result = ams_detail_create_slots(detail_widgets_, detail_slot_widgets_, MAX_DETAIL_SLOTS,
                                          unit_pos, on_detail_slot_clicked, this);

    detail_slot_count_ = result.slot_count;

    ams_detail_update_labels(detail_widgets_, detail_slot_widgets_, result.slot_count,
                             result.layout);
    ams_detail_update_badges(detail_widgets_, detail_slot_widgets_, result.slot_count,
                             result.layout);
    ams_detail_update_tray(detail_widgets_, detail_slot_widgets_, result.slot_count, unit_pos);
    helix::ui::ams_detail_sync_lane_entry(detail_path_canvas_, detail_widgets_.slot_grid);

    spdlog::debug("[{}] Created {} detail slots via shared helpers", get_name(), result.slot_count);
}

void AmsOverviewPanel::destroy_detail_slots() {
    // The canvas caches the slots' spool containers and reads them whenever it lays out,
    // including while the next unit's slots are being created.
    if (detail_path_canvas_) {
        ui_filament_path_canvas_set_slot_grid(detail_path_canvas_, nullptr);
    }
    ams_detail_destroy_slots(detail_widgets_, detail_slot_widgets_, detail_slot_count_);
}

void AmsOverviewPanel::update_path_canvas() {
    if (!detail_path_canvas_ || shown_unit_pos_ < 0)
        return;
    // The whole path: lanes, hub, buffer, toolhead and bypass, as AmsPanel draws it.
    ams_detail_setup_path_canvas(detail_path_canvas_, detail_widgets_.slot_grid, shown_unit_pos_);
    refresh_page_bypass();
}

void AmsOverviewPanel::layout_paging_controls() {
    if (!path_container_ || !detail_path_canvas_ || !prev_button_ || !next_button_)
        return;

    lv_obj_update_layout(path_container_);
    const int32_t hit = lv_obj_get_width(prev_button_);
    const int32_t gap = theme_manager_get_spacing("space_xs");
    lv_area_t content;
    lv_obj_get_content_coords(path_container_, &content);
    lv_area_t canvas;
    lv_obj_get_coords(detail_path_canvas_, &canvas);

    // Level with the hub box, which only the canvas knows; the middle of the canvas until
    // it has drawn one. On a screen so narrow that the box leaves no room for an arrow
    // beside it, the arrows sit just below it instead, either side of the trunk.
    int32_t center_y = (canvas.y1 + canvas.y2) / 2;
    bool beside_hub = true;
    lv_area_t hub;
    if (ui_filament_path_canvas_get_hub_box(detail_path_canvas_, &hub)) {
        beside_hub = hub.x1 - canvas.x1 >= hit + gap && canvas.x2 - hub.x2 >= hit + gap;
        center_y = beside_hub ? (hub.y1 + hub.y2) / 2 : hub.y2 + gap + hit / 2;
    }
    // The canvas keeps its stubs and their labels clear of arrows standing at their level.
    ui_filament_path_canvas_set_edge_reserve(detail_path_canvas_, beside_hub ? hit + gap : 0,
                                             center_y - hit / 2, center_y + hit / 2);

    const int32_t y = center_y - hit / 2 - content.y1;
    lv_obj_set_pos(prev_button_, 0, y);
    lv_obj_set_pos(next_button_, lv_area_get_width(&content) - hit, y);
}

void AmsOverviewPanel::layout_page_dots() {
    if (!page_dots_)
        return;
    const int n = lv_subject_get_int(AmsState::instance().get_ams_page_count_subject());
    if (n < 2)
        return;

    // The nominal pitch, closing toward a floor of space_xxs between dots as pages are added
    // so the row keeps fitting. Past the floor the row stays at that pitch and scrolls to
    // keep the current page's dot in view.
    const int32_t dot = theme_manager_get_spacing("ams_page_dot_size");
    const int32_t nominal = theme_manager_get_spacing("space_sm");
    const int32_t floor_gap = theme_manager_get_spacing("space_xxs");
    const int32_t room = lv_obj_get_content_width(page_dots_) - n * dot;
    const int32_t gap = std::clamp(room / (n - 1), floor_gap, nominal);
    if (lv_obj_get_style_pad_column(page_dots_, LV_PART_MAIN) != gap)
        lv_obj_set_style_pad_column(page_dots_, gap, LV_PART_MAIN);
}

void AmsOverviewPanel::scroll_current_dot_into_view() {
    if (!page_dots_ || page_ < 0)
        return;
    if (lv_obj_t* dot = lv_obj_get_child(page_dots_, page_))
        lv_obj_scroll_to_view(dot, LV_ANIM_OFF);
}

void AmsOverviewPanel::on_unit_view_gesture(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AMS Overview] on_unit_view_gesture");

    auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (!self || !indev)
        return;

    // Swipe left brings the next page, swipe right the previous; where that arrow is
    // hidden the swipe goes nowhere. The release that follows a swipe is not a tap, so the
    // spool it began on is not clicked.
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        self->next_page();
        lv_indev_wait_release(indev);
    } else if (dir == LV_DIR_RIGHT) {
        self->prev_page();
        lv_indev_wait_release(indev);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsOverviewPanel::on_path_layout_changed(lv_event_t* e) {
    if (auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e))) {
        self->layout_paging_controls();
        self->update_page_bypass_widgets_position();
    }
}

void AmsOverviewPanel::on_page_dots_resized(lv_event_t* e) {
    if (auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e))) {
        self->layout_page_dots();
        self->scroll_current_dot_into_view();
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void AmsOverviewPanel::clear_panel_reference() {
    open_ = false;
    // Cancel animations and dismiss menus while widget pointers are still valid
    if (detail_container_) {
        lv_anim_delete(detail_container_, nullptr);
    }
    if (context_menu_) {
        context_menu_->hide();
    }

    // Destroy detail slot widgets BEFORE the parent tree deletion. A zoom-out animation
    // cancelled above never reaches its completion, so its slots would otherwise survive:
    // their DELETE handlers release() observers instead of properly removing them, leaving
    // dangling entries in AmsState subjects. Explicitly destroying them here ensures proper
    // cleanup.
    destroy_detail_slots();

    // Clear observer guards before clearing widget pointers
    slots_version_observer_.reset();
    current_slot_observer_.reset();
    external_spool_observer_.reset();
    bypass_active_observer_.reset();
    dryer_version_observer_.reset();
    page_count_observer_.reset();

    // Clean up sidebar before clearing panel references
    sidebar_.reset();

    // Clear global instance pointer
    g_overview_panel_instance.store(nullptr);

    // Clear widget references
    detach_widget_hooks();
    system_path_ = nullptr;
    system_path_area_ = nullptr;
    panel_ = nullptr;
    parent_screen_ = nullptr;
    cards_row_ = nullptr;
    unit_cards_.clear();

    // Clear unit view state
    detail_container_ = nullptr;
    path_container_ = nullptr;
    prev_button_ = nullptr;
    next_button_ = nullptr;
    page_dots_ = nullptr;
    bypass_widgets_ = {};
    page_bypass_widgets_ = {};
    detail_widgets_ = AmsDetailWidgets{};
    detail_path_canvas_ = nullptr;
    unit_view_active_ = false;
    detail_slot_count_ = 0;
    std::fill(std::begin(detail_slot_widgets_), std::end(detail_slot_widgets_), nullptr);
    pages_.clear();
    page_units_.clear();
    page_ = -1;
    shown_unit_key_.clear();
    shown_unit_pos_ = -1;

    // Reset subjects_initialized_ so observers are recreated on next access
    subjects_initialized_ = false;
}

// ============================================================================
// Global Instance
// ============================================================================

static lv_obj_t* s_ams_overview_panel_obj = nullptr;
// Theme generation the cached tree was built under.
static int s_ams_overview_theme_gen = 0;

// Lazy registration flag for XML component
static bool s_overview_registered = false;

static void ensure_overview_registered() {
    if (s_overview_registered) {
        return;
    }

    spdlog::info("[AMS Overview] Lazy-registering XML component");

    // Register sidebar callbacks before component registration
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    // Tool text observers initialized in ui_ams_current_tool_init() at startup.

    // Register context-aware back button callback for header
    // Unit view: return to overview. Overview mode: close the overlay.
    lv_xml_register_event_cb(nullptr, "on_ams_overview_back_clicked", [](lv_event_t* e) {
        LV_UNUSED(e);
        AmsOverviewPanel* panel = g_overview_panel_instance.load();
        if (panel && panel->is_in_unit_view()) {
            panel->show_overview();
        } else {
            helix::nav::go_back();
        }
    });
    lv_xml_register_event_cb(nullptr, "on_ams_page_prev_clicked", [](lv_event_t* e) {
        LV_UNUSED(e);
        if (auto* panel = helix::lazy_global_if_exists<AmsOverviewPanel>())
            panel->prev_page();
    });
    lv_xml_register_event_cb(nullptr, "on_ams_page_next_clicked", [](lv_event_t* e) {
        LV_UNUSED(e);
        if (auto* panel = helix::lazy_global_if_exists<AmsOverviewPanel>())
            panel->next_page();
    });

    // Register the environment-indicator badge (component + click callback).
    // ui_panel_ams.cpp registers the same name for the single-unit AmsPanel path,
    // but when the multi-unit overview is the first (or only) AMS entry point in a
    // run, that registration never runs — each ams_unit_card's
    // <ams_environment_indicator event_cb="on_env_indicator_clicked"> would
    // otherwise fail to bind (lv_xml_get_event_cb: "no event was found"). The
    // shared helper's process-lifetime guard registers the callback and component
    // exactly once, before create_unit_cards() parses ams_unit_card.xml.
    helix::ui::ensure_ams_env_indicator_registered();

    // Register canvas widgets
    ui_system_path_canvas_register();
    ui_filament_path_canvas_register();

    // Register AMS slot widgets for the unit view
    // (safe to call multiple times — each register function has an internal guard)
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_ams_lane_bar_register();

    // Register the XML components (dependencies must be registered before overview panel)
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_unit_detail.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_loaded_card.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_context_menu.xml").c_str());
    // ams_unit_card.xml nests <ams_environment_indicator>, which is already
    // registered above via ensure_ams_env_indicator_registered().
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_unit_card.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_sidebar.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_overview_panel.xml").c_str());

    s_overview_registered = true;
    spdlog::debug("[AMS Overview] XML registration complete");
}

// The shared sequence lives in helix::ui::teardown_overlay_ui(); this site
// differs from OverlayBase::destroy_overlay_ui() only in the two things a
// site is allowed to differ in:
//   1. DetachSubtree deletion — the root contains grid/flex layouts whose
//      grid_update/flex_update must be structurally unable to race teardown
//      (#983).
//   2. clear_panel_reference() as the before-delete hook — it destroys the
//      sidebar and context-menu sub-objects that own widgets in this subtree,
//      so it must run while the tree is still attached.
void destroy_ams_overview_panel_ui() {
    helix::ui::teardown_overlay_ui(
        s_ams_overview_panel_obj, "AmsOverviewPanel", helix::ui::TeardownDelete::DetachSubtree,
        nullptr, helix::ui::TeardownHooks::before([]() {
            if (auto* panel = helix::lazy_global_if_exists<AmsOverviewPanel>()) {
                panel->clear_panel_reference();
            }
        }));
}

AmsOverviewPanel& get_global_ams_overview_panel() {
    AmsOverviewPanel& panel = helix::lazy_global_with_teardown<AmsOverviewPanel>(
        "AmsOverviewPanel", destroy_ams_overview_panel_ui, get_printer_state(),
        get_moonraker_api());

    const int theme_gen = lv_subject_get_int(theme_manager_get_changed_subject());
    if (s_ams_overview_panel_obj && s_ams_overview_theme_gen != theme_gen &&
        !helix::nav::is_showing(s_ams_overview_panel_obj)) {
        destroy_ams_overview_panel_ui();
    }

    // Lazy create the panel UI if not yet created
    if (!s_ams_overview_panel_obj) {
        s_ams_overview_theme_gen = theme_gen;
        ensure_overview_registered();

        // Initialize AmsState subjects BEFORE XML creation so bindings work
        AmsState::instance().init_subjects(true);
        // A new tree opens on the overview, whatever a dropped one left in the subjects.
        AmsState::instance().set_unit_view_active(false);
        AmsState::instance().set_unit_page(0, 0);

        // Create the panel on the active screen
        lv_obj_t* screen = lv_scr_act();
        s_ams_overview_panel_obj = helix::ui::create_xml_hidden(screen, "ams_overview_panel");

        if (s_ams_overview_panel_obj) {
            // Initialize panel observers
            if (!panel.are_subjects_initialized()) {
                panel.init_subjects();
            }

            // Setup the panel
            panel.setup(s_ams_overview_panel_obj, screen);
            lv_obj_add_flag(s_ams_overview_panel_obj, LV_OBJ_FLAG_HIDDEN);

            // Kept alive between opens, like the AMS panel; open_ams_overview_panel()
            // registers the overlay and its close callback on every open.
            helix::nav::register_overlay(s_ams_overview_panel_obj, &panel);

            spdlog::info("[AMS Overview] Lazy-created panel UI");
        } else {
            spdlog::error("[AMS Overview] Failed to create panel from XML");
        }
    }

    return panel;
}

// ============================================================================
// Slot Context Menu (unit view)
// ============================================================================

void AmsOverviewPanel::handle_detail_slot_tap(int global_slot_index, lv_point_t click_pt) {
    spdlog::info("[{}] Detail slot {} tapped", get_name(), global_slot_index);

    // Find the local widget for positioning the menu
    if (shown_unit_pos_ < 0)
        return;

    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    AmsSystemInfo info = backend->get_system_info();
    if (shown_unit_pos_ >= static_cast<int>(info.units.size()))
        return;

    const auto& unit = info.units[shown_unit_pos_];
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

    if (!context_menu_) {
        context_menu_ = std::make_unique<helix::ui::AmsContextMenu>();
    }

    helix::ui::SlotMenuHost host;
    host.parent_screen = parent_screen_;
    host.api = api_;
    host.log_tag = "[AmsOverview]";
    host.path_canvas = [this] { return detail_path_canvas_; };
    host.sidebar = [this] { return sidebar_.get(); };
    helix::ui::open_slot_context_menu(*context_menu_, host, slot_index, near_widget, click_pt);
}

// ============================================================================
// Bypass Spool Interaction
// ============================================================================

void AmsOverviewPanel::on_bypass_spool_clicked(lv_event_t* e) {
    if (auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e))) {
        self->handle_bypass_click();
    }
}

void AmsOverviewPanel::handle_bypass_click() {
    helix::ui::show_external_spool_menu(
        parent_screen_, unit_view_active_ ? detail_path_canvas_ : system_path_, context_menu_,
        helix::ui::sidebar_external_spool_hooks(sidebar_.get()));
}

void AmsOverviewPanel::refresh_bypass_display() {
    refresh_system_bypass();
    refresh_page_bypass();
}

void AmsOverviewPanel::refresh_page_bypass() {
    if (!page_bypass_widgets_.valid() || !unit_view_active_) {
        return;
    }
    helix::ui::bypass_spool_sync_from_state(page_bypass_widgets_);
    // The canvas draws the tube to the spool; its own state follows the same subjects.
    if (detail_path_canvas_ && shown_unit_pos_ >= 0) {
        auto ext = AmsState::instance().get_external_spool_info();
        const bool node = helix::ui::bypass_node_visible_for(AmsState::instance().get_backend());
        ui_filament_path_canvas_set_bypass_has_spool(detail_path_canvas_, ext.has_value() && node);
        if (ext && node)
            ui_filament_path_canvas_set_bypass_color(detail_path_canvas_, ext->color_rgb);
        ui_filament_path_canvas_set_bypass_active(
            detail_path_canvas_,
            node && lv_subject_get_int(AmsState::instance().get_bypass_active_subject()) != 0);
        ui_filament_path_canvas_refresh(detail_path_canvas_);
    }
    update_page_bypass_widgets_position();
}

void AmsOverviewPanel::refresh_system_bypass() {
    if (!system_path_) {
        return;
    }

    auto ext_spool = AmsState::instance().get_external_spool_info();

    if (ext_spool) {
        // Preserve current bypass active state, update color from spool
        auto* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsSystemInfo info = backend->get_system_info();
            int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());
            bool bypass_active = info.supports_bypass && (current_slot == -2);
            ui_system_path_canvas_set_bypass(system_path_,
                                             helix::ui::bypass_node_visible_for(backend),
                                             bypass_active, ext_spool->color_rgb);
        }
    }

    // Same rule as the refresh path above (#1229).
    const bool show_bypass2 =
        helix::ui::bypass_node_visible_for(AmsState::instance().get_backend());
    if (bypass_widgets_.valid()) {
        helix::ui::bypass_spool_set_visible(bypass_widgets_, show_bypass2);
        bypass_spool_set_has_spool(bypass_widgets_, show_bypass2 && ext_spool.has_value());
        if (show_bypass2 && ext_spool) {
            bypass_spool_set_color(bypass_widgets_, ext_spool->color_rgb);
        } else {
            bypass_spool_set_color(bypass_widgets_, 0x888888);
        }
        bypass_spool_set_material(bypass_widgets_,
                                  (show_bypass2 && ext_spool && !ext_spool->material.empty())
                                      ? ext_spool->material.c_str()
                                      : "");
        // Same ring a lane slot wears when it is the active node.
        helix::ui::bypass_spool_set_active(
            bypass_widgets_,
            show_bypass2 &&
                lv_subject_get_int(AmsState::instance().get_bypass_active_subject()) != 0);
        update_bypass_widgets_position();
    }

    ui_system_path_canvas_refresh(system_path_);
}

void AmsOverviewPanel::on_system_path_size_changed(lv_event_t* e) {
    if (auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e)))
        self->update_bypass_widgets_position();
}

void AmsOverviewPanel::detach_widget_hooks() {
    // These widgets can outlive this panel; their callbacks must not.
    helix::ui::remove_event_cb_if_alive(cards_row_, &AmsOverviewPanel::on_cards_row_scrolled, this);
    helix::ui::remove_event_cb_if_alive(system_path_,
                                        &AmsOverviewPanel::on_system_path_size_changed, this);
    helix::ui::remove_event_cb_if_alive(detail_container_, &AmsOverviewPanel::on_unit_view_gesture,
                                        this);
    helix::ui::remove_event_cb_if_alive(detail_path_canvas_,
                                        &AmsOverviewPanel::on_path_layout_changed, this);
    helix::ui::remove_event_cb_if_alive(page_dots_, &AmsOverviewPanel::on_page_dots_resized, this);
}

AmsOverviewPanel::~AmsOverviewPanel() {
    detach_widget_hooks();
}

void AmsOverviewPanel::update_bypass_widgets_position() {
    if (!bypass_widgets_.valid() || !system_path_ || !system_path_area_) {
        return;
    }
    int32_t abs_cx = 0;
    int32_t abs_cy = 0;
    if (!ui_system_path_canvas_get_bypass_merge_pos(system_path_, &abs_cx, &abs_cy)) {
        return;
    }
    lv_obj_update_layout(system_path_area_);
    lv_area_t parent_abs;
    lv_obj_get_content_coords(system_path_area_, &parent_abs);
    helix::ui::bypass_spool_set_position(bypass_widgets_, abs_cx - parent_abs.x1,
                                         abs_cy - parent_abs.y1);
}

void AmsOverviewPanel::update_page_bypass_widgets_position() {
    if (!page_bypass_widgets_.valid() || !detail_path_canvas_ || !path_container_) {
        return;
    }
    int32_t abs_cx = 0;
    int32_t abs_cy = 0;
    if (!ui_filament_path_canvas_get_bypass_merge_pos(detail_path_canvas_, &abs_cx, &abs_cy)) {
        return;
    }
    lv_obj_update_layout(path_container_);
    lv_area_t parent_abs;
    lv_obj_get_content_coords(path_container_, &parent_abs);
    helix::ui::bypass_spool_set_position(page_bypass_widgets_, abs_cx - parent_abs.x1,
                                         abs_cy - parent_abs.y1);
}

// ============================================================================
// Multi-unit Navigation
// ============================================================================

namespace {

void drop_hidden_ams_panel_ui() {
    const AmsPanel* detail = get_existing_ams_panel();
    if (detail && detail->get_panel() && !helix::nav::is_showing(detail->get_panel())) {
        destroy_ams_panel_ui();
    }
}

void drop_hidden_overview_ui() {
    if (s_ams_overview_panel_obj && !helix::nav::is_showing(s_ams_overview_panel_obj)) {
        destroy_ams_overview_panel_ui();
    }
}

// Critical memory pressure drops the hidden cached AMS trees, which the small-RAM
// printers (AD5M/AD5X) need back. Linux only in practice: the monitor is never
// started on the ESP32.
void ensure_ams_pressure_responder() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    helix::MemoryMonitor::instance().add_pressure_responder([](helix::MemoryPressureLevel level) {
        if (level < helix::MemoryPressureLevel::critical) {
            return;
        }
        helix::ui::queue_update("AMS::drop_hidden_ui", []() {
            drop_hidden_ams_panel_ui();
            drop_hidden_overview_ui();
        });
    });
}

// The panels outlive a close, so a reopen usually has nothing to build; only a
// first open (or one after the tree was dropped) waits under the loading pill.
void open_ams_overlay(bool built, const std::function<void()>& open) {
    ensure_ams_pressure_responder();
    if (built) {
        open();
    } else {
        helix::nav::build_under_loading_pill(open);
    }
}

// Close callbacks fire once, so every open registers its own.
void push_ams_overlay(lv_obj_t* root, IPanelLifecycle* lifecycle,
                      helix::OverlayCloseCallback on_closed) {
    helix::nav::register_overlay(root, lifecycle);
    helix::nav::on_close(root, std::move(on_closed));
    helix::nav::push_overlay(root);
}

void open_ams_overview_panel(int units) {
    drop_hidden_ams_panel_ui();
    open_ams_overlay(s_ams_overview_panel_obj != nullptr, [units]() {
        spdlog::info("[AMS] Multi-unit setup ({} units) - showing overview", units);
        auto& overview = get_global_ams_overview_panel();
        if (lv_obj_t* panel = overview.get_panel()) {
            push_ams_overlay(panel, &overview, &AmsOverviewPanel::run_close);
        }
    });
}

} // namespace

void helix::ui::open_ams_detail_panel() {
    drop_hidden_overview_ui();
    const AmsPanel* existing = get_existing_ams_panel();
    open_ams_overlay(existing && existing->get_panel(), []() {
        spdlog::info("[AMS] Single-unit setup - showing detail panel directly");
        auto& detail = get_global_ams_panel();
        if (lv_obj_t* panel = detail.get_panel()) {
            push_ams_overlay(panel, &detail, &AmsPanel::run_close);
        }
    });
}

void navigate_to_ams_panel() {
    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[AMS] navigate_to_ams_panel called with no backend");
        return;
    }

    // Only one of the two panels serves a given unit count; opening one drops the
    // other's hidden tree rather than holding both.
    const AmsSystemInfo info = backend->get_system_info();
    if (info.is_multi_unit()) {
        open_ams_overview_panel(info.unit_count());
    } else {
        helix::ui::open_ams_detail_panel();
    }
}
