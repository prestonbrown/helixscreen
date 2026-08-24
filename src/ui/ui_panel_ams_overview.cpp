// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_ams_overview.h"

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_environment_overlay.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_external_spool_menu.h"
#include "ui_filament_path_canvas.h"
#include "ui_nav_manager.h"
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
#include "app_globals.h"
#include "color_utils.h"
#include "data_root_resolver.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <numeric>
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

/**
 * @brief Is this lane the one firmware considers seated and loaded?
 *
 * SINGLE SOURCE OF TRUTH for the active-lane highlight: the per-slot
 * active-loaded subject (AmsBackend::slot_is_actively_loaded(i)) — the same read
 * apply_current_slot_highlight() makes in ui_ams_slot.cpp. The mini bars used to
 * ask `global_idx == current_slot`, which disagreed with the slot widgets on the
 * very same panel: an idle unload left the bar outlined after the detail view's
 * glow had cleared, and on AFC/CFS a current_slot that names a lane the per-slot
 * parse disagrees with (#1194) put the highlight on two different lanes.
 *
 * The bars are rebuilt wholesale on every refresh (see create_mini_bars), and
 * AmsState bumps slots_version on every slot_active_loaded delta, so the panel's
 * existing slots_version observer is what keeps this live — no per-slot observer,
 * which would race the rebuild (#705/#776).
 */
static bool slot_is_active_loaded(int global_slot_index) {
    lv_subject_t* subject = AmsState::instance().get_slot_active_loaded_subject(global_slot_index);
    return subject && lv_subject_get_int(subject) != 0;
}

/// Set a label to "N slots" text, with null-safety
static void set_slot_count_label(lv_obj_t* label, int slot_count) {
    if (!label) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), lv_tr("%d slots"), slot_count);
    lv_label_set_text(label, buf);
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

        // Observe slots_version to auto-refresh when slot data changes.
        // In detail mode, per-slot observers handle visual updates (color, pulse,
        // highlight) automatically — we only need to react to structural changes
        // (slot count changed) or refresh the overview cards.
        using helix::ui::observe_int_sync;
        slots_version_observer_ = observe_int_sync<AmsOverviewPanel>(
            AmsState::instance().get_slots_version_subject(), this,
            [](AmsOverviewPanel* self, int) {
                if (!self->panel_)
                    return;
                if (self->detail_unit_index_ >= 0) {
                    // In detail mode — only rebuild slots if
                    // count changed. Per-slot observers drive
                    // all visual state (color, pulse, etc.)
                    self->refresh_detail_if_needed();
                    return;
                }
                // Defer rebuild (#80) AND use safe_clean_children
                // in refresh_units callees (#776): lifetime_.defer
                // moves work off the observer callback's stack, and
                // safe_clean_children schedules child deletion via
                // lv_obj_delete_async so sync lv_obj_clean() can't
                // corrupt LVGL's event linked list.
                if (!self->units_rebuild_pending_) {
                    self->units_rebuild_pending_ = true;
                    self->lifetime_.defer("AmsOverviewPanel::refresh_units", [self]() {
                        self->units_rebuild_pending_ = false;
                        if (self->panel_ && self->cards_row_ && self->detail_unit_index_ < 0)
                            self->refresh_units();
                    });
                }
            });

        // Observe current_slot to reactively update lane highlights when the active
        // slot changes (e.g., slot selected without load/unload).
        current_slot_observer_ = observe_int_sync<AmsOverviewPanel>(
            AmsState::instance().get_current_slot_subject(), this, [](AmsOverviewPanel* self, int) {
                if (!self->panel_)
                    return;
                if (self->detail_unit_index_ >= 0) {
                    self->refresh_detail_if_needed();
                    return;
                }
                if (!self->units_rebuild_pending_) {
                    self->units_rebuild_pending_ = true;
                    self->lifetime_.defer("AmsOverviewPanel::refresh_units/slot", [self]() {
                        self->units_rebuild_pending_ = false;
                        if (self->panel_ && self->cards_row_ && self->detail_unit_index_ < 0)
                            self->refresh_units();
                    });
                }
            });

        // Observe external spool color changes to reactively update bypass display.
        // NOTE: set_external_spool_info() calls lv_subject_set_int() directly (not via
        // ui_queue_update) which is safe because all current callers are on the LVGL thread.
        // If callers from background threads are added, those must use ui_queue_update().
        external_spool_observer_ = observe_int_sync<AmsOverviewPanel>(
            AmsState::instance().get_external_spool_color_subject(), this,
            [](AmsOverviewPanel* self, int /*color_int*/) {
                // Delegate to existing refresh helper which reads full spool info
                self->refresh_bypass_display();
            },
            AmsState::instance().get_subjects_lifetime());
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
    lv_obj_add_event_cb(cards_row_, &AmsOverviewPanel::on_cards_row_scrolled, LV_EVENT_SCROLL,
                        this);

    // Find system path area and create path canvas widget
    system_path_area_ = lv_obj_find_by_name(panel_, "system_path_area");
    if (system_path_area_) {
        system_path_ = ui_system_path_canvas_create(system_path_area_);
        if (system_path_) {
            lv_obj_set_size(system_path_, LV_PCT(100), LV_PCT(100));
            spdlog::debug("[{}] Created system path canvas", get_name());

            bypass_widgets_ = helix::ui::bypass_spool_create(
                system_path_area_, &AmsOverviewPanel::on_bypass_spool_clicked, this);
            // SIZE_CHANGED only — listening to DRAW events would invalidate
            // during render and trip lv_inv_area assertions in LVGL 9.
            lv_obj_add_event_cb(
                system_path_,
                [](lv_event_t* e) {
                    auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e));
                    if (self) {
                        self->update_bypass_widgets_position();
                    }
                },
                LV_EVENT_SIZE_CHANGED, this);
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
    sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(printer_state_);
    sidebar_->setup(panel_);
    sidebar_->init_observers();

    // Initial population from backend state
    refresh_units();

    spdlog::debug("[{}] Setup complete!", get_name());
}

void AmsOverviewPanel::on_activate() {
    // Reset coalescing flag to prevent stale state from a previous deactivation
    units_rebuild_pending_ = false;

    spdlog::debug("[{}] Activated - syncing from backend", get_name());

    AmsState::instance().sync_from_backend();

    if (sidebar_)
        sidebar_->sync_from_state();

    if (detail_unit_index_ >= 0) {
        // Re-entering while in detail mode — refresh the detail slots
        show_unit_detail(detail_unit_index_);
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
        // Same number of units - update existing cards in place. unit_cards_ is in
        // DISPLAY order (see create_unit_cards), so each card names its own unit.
        for (auto& uc : unit_cards_) {
            if (uc.unit_index >= 0 && uc.unit_index < new_unit_count)
                update_unit_card(uc, info.units[uc.unit_index]);
        }
    }

    // Update system path visualization
    refresh_system_path(info, current_slot);
}

void AmsOverviewPanel::create_unit_cards(const AmsSystemInfo& info) {
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

    // Lay the cards out in nozzle order, not backend order.
    //
    // Which unit a backend lists first has nothing to do with which toolhead it
    // feeds: a Claymore wired to e0 can be last in the list while e0's nozzle is
    // the leftmost of four. Left in backend order, its connector ran diagonally
    // across every other unit's. Sorting by the unit's first physical nozzle
    // makes the card row monotonic with the toolhead row, so the lanes below only
    // cross where the plumbing genuinely does - and it parks units that share a
    // nozzle (a Box Turtle and a Claymore both on e0) side by side.
    //
    // stable_sort, so a system whose order is already monotonic (every one-unit
    // rig, and most multi-unit ones) is left exactly as it was.
    std::vector<int> order(unit_count);
    std::iota(order.begin(), order.end(), 0);
    {
        auto layout =
            ams_draw::compute_system_tool_layout(info, AmsState::instance().get_backend());
        auto first_nozzle = [&layout](int unit) {
            return (unit < static_cast<int>(layout.units.size()))
                       ? layout.units[unit].first_physical_tool
                       : unit;
        };
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return first_nozzle(a) < first_nozzle(b); });
    }

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
        const char* attrs[] = {"temp_text",
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
        uc.logo_image = lv_obj_find_by_name(uc.card, "unit_logo");
        uc.name_label = lv_obj_find_by_name(uc.card, "unit_name");
        uc.bars_container = lv_obj_find_by_name(uc.card, "bars_container");
        uc.slot_count_label = lv_obj_find_by_name(uc.card, "slot_count");

        // Stamp the unit index on the environment indicator so its click handler
        // knows which unit's overlay to open.
        if (lv_obj_t* ind = lv_obj_find_by_name(uc.card, "env_indicator")) {
            lv_obj_set_user_data(ind, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        }

        // Set logo image based on AMS system type
        ams_draw::apply_logo(uc.logo_image, unit, info);

        // Set dynamic content only — unit name and slot count vary per unit
        uc.display_name = ams_draw::get_unit_display_name(unit, i);
        if (uc.name_label) {
            lv_label_set_text(uc.name_label, uc.display_name.c_str());
        }

        set_slot_count_label(uc.slot_count_label, unit.slot_count);

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

void AmsOverviewPanel::update_unit_card(UnitCard& card, const AmsUnit& unit) {
    if (!card.card) {
        return;
    }

    // Update name label. Keep display_name in step - it is the only untruncated
    // copy, and publish_cards_compact() measures it.
    card.display_name = ams_draw::get_unit_display_name(unit, card.unit_index);
    if (card.name_label) {
        lv_label_set_text(card.name_label, card.display_name.c_str());
    }

    // Rebuild mini bars (slot colors/status may have changed).
    // Flush pending layout first — deferred callbacks can run between layout
    // passes, and cleaning children while LVGL still references them causes
    // use-after-free in layout_update_core (issue #711). Called from refresh_units
    // under the slots_version observer — safe_clean_children escapes
    // UpdateQueue::process_pending() so sync deletion can't corrupt LVGL's
    // event linked list (#776).
    if (card.bars_container) {
        lv_obj_update_layout(card.bars_container);
        helix::ui::safe_clean_children(card.bars_container);
        create_mini_bars(card, unit);
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

void AmsOverviewPanel::create_mini_bars(UnitCard& card, const AmsUnit& unit) {
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
        bool is_loaded = slot_is_active_loaded(global_idx);

        auto col = ams_draw::create_slot_column(card.bars_container, bar_width, MINI_BAR_HEIGHT_PX,
                                                MINI_BAR_RADIUS_PX);

        ams_draw::BarStyleParams params;
        params.color_rgb = slot.color_rgb;
        // These mini bars are rebuilt wholesale (safe_clean_children) on every
        // overview refresh, so fill is read from this snapshot rather than via a
        // per-slot fill subject observer — an observer would race the rebuild
        // (#705/#776). Semantics stay unified: fill_percent_from_slot uses
        // SlotInfo::display_fill_pct. -1 = "no data" → render an empty bar (0)
        // rather than a phantom fill; style_slot_bar hides the fill anyway for a
        // non-present lane.
        int fp = ams_draw::fill_percent_from_slot(slot);
        params.fill_pct = fp < 0 ? 0 : fp;
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

    // Restore header to overview mode: show title, hide detail elements
    lv_obj_t* title = lv_obj_find_by_name(panel_, "header_title");
    if (title)
        lv_obj_remove_flag(title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* logo = lv_obj_find_by_name(panel_, "detail_logo");
    if (logo)
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* name_label = lv_obj_find_by_name(panel_, "detail_unit_name");
    if (name_label)
        lv_obj_add_flag(name_label, LV_OBJ_FLAG_HIDDEN);

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
    // Hide overview title, show detail elements in main header
    lv_obj_t* title = lv_obj_find_by_name(panel_, "header_title");
    if (title)
        lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);

    // Update and show logo
    lv_obj_t* logo = lv_obj_find_by_name(panel_, "detail_logo");
    if (logo) {
        ams_draw::apply_logo(logo, unit, info);
        lv_obj_remove_flag(logo, LV_OBJ_FLAG_HIDDEN);
    }

    // Update and show name
    lv_obj_t* name = lv_obj_find_by_name(panel_, "detail_unit_name");
    if (name) {
        lv_label_set_text(name, ams_draw::get_unit_display_name(unit, detail_unit_index_).c_str());
        lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
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

    // Pre-show environment indicator so flex layout accounts for its width
    ams_detail_pre_show_env_indicator(detail_widgets_, detail_unit_index_);

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
    current_slot_observer_.reset();
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

    // Register sidebar callbacks before component registration
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    // Tool text observers initialized in ui_ams_current_tool_init() at startup.

    // Register context-aware back button callback for header
    // Detail mode: return to overview. Overview mode: close the overlay.
    lv_xml_register_event_cb(nullptr, "on_ams_overview_back_clicked", [](lv_event_t* e) {
        LV_UNUSED(e);
        AmsOverviewPanel* panel = g_overview_panel_instance.load();
        if (panel && panel->is_in_detail_mode()) {
            panel->show_overview();
        } else {
            NavigationManager::instance().go_back();
        }
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

    // Register AMS slot widgets for inline detail view
    // (safe to call multiple times — each register function has an internal guard)
    ui_spool_canvas_register();
    ui_ams_slot_register();

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

// NOTE: this deliberately does NOT call OverlayBase::destroy_overlay_ui().
// AmsOverviewPanel derives from PanelBase, not OverlayBase — the two are
// sibling IPanelLifecycle implementations, so the helper is not reachable from
// here (there is no overlay_root_ and no on_ui_destroyed() on this hierarchy).
// The sequence below mirrors the helper's, with two required deviations:
//   1. clear_panel_reference() runs BEFORE deletion (the helper's
//      on_ui_destroyed() runs after), because it destroys sub-objects that own
//      widgets in this subtree.
//   2. safe_delete_subtree() instead of safe_delete_deferred() — see #983.
void destroy_ams_overview_panel_ui() {
    if (s_ams_overview_panel_obj) {
        spdlog::info("[AMS Overview] Destroying panel UI to free memory");

        // Drain deferred observer callbacks while all pointers are still valid.
        // observe_int_sync queues lambdas via queue_update() that capture raw
        // panel/sidebar pointers. If subject changes fired observers between
        // the last timer tick and now, those lambdas are pending. Processing
        // them here prevents use-after-free when the panel is destroyed below.
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();

        NavigationManager::instance().unregister_overlay_close_callback(s_ams_overview_panel_obj);
        NavigationManager::instance().unregister_overlay_instance(s_ams_overview_panel_obj);

        // Breadcrumb the destroy so crashes in the close path can be pinned to
        // which overlay was being torn down. Pairs with the "overlay+" crumb on
        // push, and matches OverlayBase::destroy_overlay_ui().
        crash_handler::breadcrumb::note("ovrl_dst", "AmsOverviewPanel");

        if (g_ams_overview_panel) {
            g_ams_overview_panel->clear_panel_reference();
        }

        // safe_delete_subtree (not safe_delete) because the AMS overview panel
        // root contains grid/flex layouts whose grid_update/flex_update could
        // race teardown if an ancestor relayouts during this drain. Detaching
        // the subtree off-tree before deferred deletion makes that race
        // structurally impossible (#983). Null the pointer ourselves — the
        // subtree helper takes a raw pointer and does not null its argument.
        helix::ui::safe_delete_subtree(s_ams_overview_panel_obj);
        s_ams_overview_panel_obj = nullptr;
    }
}

AmsOverviewPanel& get_global_ams_overview_panel() {
    if (!g_ams_overview_panel) {
        g_ams_overview_panel =
            std::make_unique<AmsOverviewPanel>(get_printer_state(), get_moonraker_api());
        StaticPanelRegistry::instance().register_destroy("AmsOverviewPanel", []() {
            destroy_ams_overview_panel_ui();
            g_ams_overview_panel.reset();
        });
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

    context_menu_->set_action_callback([this](helix::ui::AmsContextMenu::MenuAction action,
                                              int slot) {
        AmsBackend* backend = AmsState::instance().get_backend();

        // EJECT / RECOVER_POSITION / SELECT_GATE / CHECK_GATE / CLEAR_SPOOL are
        // identical in both AMS panels — see ams_dispatch_backend_action().
        if (helix::ui::ams_dispatch_backend_action(action, slot, detail_path_canvas_)) {
            return;
        }

        switch (action) {
        case helix::ui::AmsContextMenu::MenuAction::LOAD:
            if (sidebar_) {
                sidebar_->handle_load_with_preheat(slot);
            }
            break;

        case helix::ui::AmsContextMenu::MenuAction::UNLOAD:
            // Route through the sidebar so start_operation(UNLOAD) builds the
            // correct stepper (mirrors how LOAD routes via handle_load_with_preheat).
            if (sidebar_) {
                sidebar_->handle_unload(slot);
            } else if (backend) {
                AmsError error = backend->unload_filament(slot);
                if (error.result != AmsResult::SUCCESS) {
                    helix::ui::notify_ams_error(error);
                }
            } else {
                NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
                return;
            }
            break;

        case helix::ui::AmsContextMenu::MenuAction::EDIT:
            show_edit_modal(slot);
            break;

        case helix::ui::AmsContextMenu::MenuAction::SPOOLMAN:
            show_edit_modal(slot, /*open_on_picker=*/true);
            break;

        case helix::ui::AmsContextMenu::MenuAction::SCAN_QR: {
#if HELIX_HAS_CAMERA
            spdlog::info("[AmsOverview] SCAN_QR action for slot {}", slot);
            auto& scanner = helix::ui::get_qr_scanner_overlay();
            scanner.show(parent_screen_, slot, [slot](const SpoolInfo& spool) {
                AmsBackend* be = AmsState::instance().get_backend();
                if (!be)
                    return;

                // Capture the pre-edit slot BEFORE the commit — its unlink arm
                // (clear the server active spool) needs the old link.
                SlotInfo original = be->get_slot_info(slot);
                SlotInfo applied = original;
                apply_spool_to_slot(applied, spool);
                AmsError err = AmsState::instance().commit_slot_edit(slot, original, applied);
                if (!err.success()) {
                    helix::ui::notify_ams_error(err);
                    return;
                }
                spdlog::info("[AmsOverview] QR scan assigned spool #{} to slot {}", spool.id, slot);
            });
#endif // HELIX_HAS_CAMERA
            break;
        }

        case helix::ui::AmsContextMenu::MenuAction::CANCELLED:
        default:
            break;
        }
    });

    // Determine whether to offer Unload for this slot. Decoupled from the
    // display LOADED status so a runout that clears the head sensor doesn't
    // disable Unload on the firmware's active slot (#995).
    bool is_loaded = false;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        is_loaded = backend->can_unload_from_toolhead(slot_index);
    }

    context_menu_->set_click_point(click_pt);
    context_menu_->show_near_widget(parent_screen_, slot_index, near_widget, is_loaded, backend);
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
        parent_screen_, system_path_, context_menu_,
        [this](bool open_on_picker) { show_edit_modal(-2, open_on_picker); });
}

void AmsOverviewPanel::refresh_bypass_display() {
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
        update_bypass_widgets_position();
    }

    ui_system_path_canvas_refresh(system_path_);
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

void AmsOverviewPanel::show_edit_modal(int slot_index, bool open_on_picker) {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show slot editor - no parent screen", get_name());
        return;
    }

    auto& editor = helix::ui::get_ams_edit_overlay();

    // External spool (bypass/direct) - not managed by backend
    if (slot_index == -2) {
        auto ext = AmsState::instance().get_external_spool_info();
        SlotInfo initial_info = ext.value_or(SlotInfo{});
        initial_info.slot_index = -2;
        initial_info.global_index = -2;

        editor.show_for_slot(
            parent_screen_, -2, initial_info, api_,
            [](const helix::ui::AmsEditOverlay::EditResult& result) {
                if (result.saved) {
                    AmsState::instance().commit_external_spool_edit(result.slot_info);
                    // bypass display update handled reactively by external_spool_observer_
                    NOTIFY_INFO(lv_tr("External spool updated"));
                }
            },
            open_on_picker);
        return;
    }

    // Regular AMS slot
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    SlotInfo initial_info = backend->get_slot_info(slot_index);

    editor.show_for_slot(
        parent_screen_, slot_index, initial_info, api_,
        [](const helix::ui::AmsEditOverlay::EditResult& result) {
            if (result.saved && result.slot_index >= 0) {
                AmsBackend* backend = AmsState::instance().get_backend();
                if (backend) {
                    // Capture the pre-edit slot BEFORE the commit — its unlink
                    // arm (clear the server active spool) needs the old link.
                    SlotInfo original = backend->get_slot_info(result.slot_index);
                    AmsError err = AmsState::instance().commit_slot_edit(
                        result.slot_index, original, result.slot_info);
                    if (!err.success()) {
                        helix::ui::notify_ams_error(err);
                        return;
                    }
                    NOTIFY_INFO(lv_tr("Slot {} updated"), result.slot_index + 1);
                }
            }
        },
        open_on_picker);
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
            // Re-register before push: switch_to_panel_impl() clears
            // overlay_instances_ on navbar switches (keeping only the
            // persistent map), so a cached panel re-opened after a navbar tap
            // loses its lifecycle registration. Idempotent (keyed by widget).
            NavigationManager::instance().register_overlay_instance(panel, &overview);
            NavigationManager::instance().push_overlay(panel);
        }
    } else {
        // Single-unit (or no units): go directly to detail panel
        spdlog::info("[AMS] Single-unit setup - showing detail panel directly");
        auto& detail = get_global_ams_panel();
        lv_obj_t* panel = detail.get_panel();
        if (panel) {
            // Re-register before push (see multi-unit branch above): cached
            // panel re-opened after a navbar switch loses its registration.
            NavigationManager::instance().register_overlay_instance(panel, &detail);
            NavigationManager::instance().push_overlay(panel);
        }
    }
}
