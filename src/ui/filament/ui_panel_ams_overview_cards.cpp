// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// AmsOverviewPanel's unit cards and tiles and the system path they anchor.

#include "ui_ams_lane_bar.h"
#include "ui_ams_slot_layout.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_panel_ams_overview.h"
#include "ui_system_path_canvas.h"
#include "ui_unit_art.h"
#include "ui_utils.h"
#include "ui_widget_ref.h"

#include "ams_backend.h"
#include "ams_dryer.h"
#include "ams_state.h"
#include "ams_types.h"
#include "ams_unit_pages.h"
#include "display_numbering.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"
#include "printer_detector.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

using namespace helix;

/// Minimum bar width for mini slot bars (prevents invisible bars)
static constexpr int32_t kMiniBarMinWidthPx = 6;

/// Maximum bar width for mini slot bars
static constexpr int32_t kMiniBarMaxWidthPx = 14;

/// Height of each mini slot bar (decorative, no need for responsive scaling)
static constexpr int32_t kMiniBarHeightPx = 40;

int32_t helix::ui::overview_peek_card_width(int32_t row_width, int32_t gap,
                                            const std::vector<int32_t>& min_widths) {
    if (min_widths.empty())
        return 0;
    const int count = static_cast<int>(min_widths.size());
    int32_t needed = (count - 1) * gap;
    int32_t floor_width = 0;
    for (int32_t w : min_widths) {
        needed += w;
        floor_width = LV_MAX(floor_width, w);
    }
    if (needed <= row_width)
        return 0;

    // k whole cards and gaps, then two fifths of the next: width = (row - k*gap) / (k + 2/5).
    // The most cards that still clear every card's floor.
    for (int k = count - 1; k >= 1; --k) {
        const int32_t width = (row_width - k * gap) * 5 / (5 * k + 2);
        if (width >= floor_width)
            return width;
    }
    return floor_width;
}

// overflow_visible only widens the clip by the extended draw size, and the readout hangs past
// the tile's corner by this much.
static void tile_readout_ext_draw_cb(lv_event_t* e) {
    lv_event_set_ext_draw_size(e, theme_manager_get_spacing("space_lg"));
}

/// Set a label to "N slots" / "N lanes" text, with null-safety
static void set_slot_count_label(lv_obj_t* label, helix::ui::LaneNoun noun, int slot_count) {
    if (!label) {
        return;
    }
    lv_label_set_text(label, helix::ui::lane_count_label(noun, slot_count).c_str());
}

/// A tile's art stands this tall against its width: the drawings run close to 4:3.
static constexpr int32_t kTileArtHeightPct = 75;

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
    return ams_draw::calc_bar_width(container_width, slot_count, gap, kMiniBarMinWidthPx,
                                    kMiniBarMaxWidthPx);
}

/// One bit per lane of @p unit, set when that lane's spool lives upstream of
/// the unit itself: a head an ACE feeds draws the chain's end, and its spool
/// box is the routed bay's, not a second one on the heads unit.
static uint32_t upstream_lane_mask(const AmsUnit& unit) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return 0;
    }
    uint32_t mask = 0;
    const int lanes = static_cast<int>(unit.slots.size());
    for (int s = 0; s < lanes && s < 32; ++s) {
        if (backend->upstream_slot(unit.first_slot_global_index + s).has_value()) {
            mask |= (1u << s);
        }
    }
    return mask;
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

    const auto snapshot = AmsState::instance().system_info();
    const AmsSystemInfo& info = *snapshot;
    int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());

    int new_unit_count = static_cast<int>(info.units.size());
    int old_unit_count = static_cast<int>(unit_cards_.size());

    // A tile and a card are different components, so a unit that gains or loses its art
    // rebuilds the row just as a change in the unit count does.
    bool art_changed = false;
    for (const auto& uc : unit_cards_) {
        art_changed |= uc.has_art != unit_has_art(info, uc.unit_index);
    }

    if (new_unit_count != old_unit_count || art_changed) {
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

    // Art flattens at its widget's size, so the row is sized first.
    layout_unit_row();
    for (auto& uc : unit_cards_) {
        if (uc.unit_index >= 0 && uc.unit_index < new_unit_count)
            apply_card_art(uc, info.units[uc.unit_index], info);
    }

    // Update system path visualization
    refresh_system_path(info, current_slot);
}

bool AmsOverviewPanel::unit_has_art(const AmsSystemInfo& info, int unit_index) {
    return helix::ui::unit_art_available(info, unit_index,
                                         helix::ui::unit_reports_climate(unit_index));
}

void AmsOverviewPanel::apply_card_art(UnitCard& card, const AmsUnit& unit,
                                      const AmsSystemInfo& info) {
    if (card.has_art) {
        helix::ui::unit_art_set_unit(card.art, info, card.unit_index,
                                     helix::ui::unit_reports_climate(card.unit_index));
    } else if (card.logo_image) {
        ams_draw::apply_logo(card.logo_image, unit, info);
    }
}

// DECLARATIVE_OK: every width and height here comes from the measured row; the tokens that
// bound them are the styles this reads back.
void AmsOverviewPanel::layout_unit_row() {
    if (!cards_row_ || unit_cards_.empty())
        return;
    lv_obj_update_layout(cards_row_);
    const int32_t row_width = lv_obj_get_content_width(cards_row_);
    if (row_width <= 0)
        return;
    laid_out_row_width_ = row_width;

    std::vector<int32_t> min_widths;
    for (const auto& uc : unit_cards_) {
        if (uc.card)
            min_widths.push_back(lv_obj_get_style_min_width(uc.card, LV_PART_MAIN));
    }
    const int32_t peek_width = helix::ui::overview_peek_card_width(
        row_width, lv_obj_get_style_pad_column(cards_row_, LV_PART_MAIN), min_widths);

    for (const auto& uc : unit_cards_) {
        if (!uc.card)
            continue;
        if (uc.compact_width > 0) {
            lv_obj_set_flex_grow(uc.card, 0);
            lv_obj_set_width(uc.card, uc.compact_width);
        } else if (peek_width > 0) {
            lv_obj_set_flex_grow(uc.card, 0);
            lv_obj_set_width(uc.card, peek_width);
        } else {
            lv_obj_remove_local_style_prop(uc.card, LV_STYLE_WIDTH, LV_PART_MAIN);
            lv_obj_set_flex_grow(uc.card, 1);
        }
    }
    lv_obj_update_layout(cards_row_);

    for (const auto& uc : unit_cards_) {
        if (uc.has_art && uc.art_stage)
            lv_obj_set_height(uc.art_stage, lv_obj_get_width(uc.card) * kTileArtHeightPct / 100);
    }
    lv_obj_update_layout(cards_row_);
}

void AmsOverviewPanel::on_cards_row_resized(lv_event_t* e) {
    // DECLARATIVE_OK: LV_EVENT_SIZE_CHANGED has no declarative equivalent. The row sizes its
    // cards from its own width, and that cannot run inside the layout pass that reports it.
    auto* self = static_cast<AmsOverviewPanel*>(lv_event_get_user_data(e));
    if (!self || !self->cards_row_ || self->unit_view_active_)
        return;
    const int32_t width = lv_obj_get_content_width(self->cards_row_);
    if (width > 0 && width != self->laid_out_row_width_)
        self->on_state_changed("[AmsOverviewPanel] cards row resized");
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
    if (unit_count > AmsState::kMaxUnits) {
        // One line naming the cap, instead of seven XML parser warnings per
        // excess card. Those cards still render their slots; only the
        // temperature/humidity badge is unavailable.
        spdlog::warn("[{}] Backend reports {} units but only {} have environment subjects - "
                     "unit {} onward render without the temp/humidity badge",
                     get_name(), unit_count, AmsState::kMaxUnits, AmsState::kMaxUnits + 1);
    }

    // Cards run in nozzle order (SystemToolLayout::display_order).
    const std::vector<int> order =
        ams_draw::compute_system_tool_layout(info, AmsState::instance().get_backend())
            .display_order;

    // Beside drawn units, a unit with no drawing is a compact tile of its lane bars.
    const bool any_art = std::any_of(order.begin(), order.begin() + unit_count,
                                     [&](int u) { return unit_has_art(info, u); });

    for (int slot = 0; slot < unit_count; ++slot) {
        const int i = order[slot];
        const AmsUnit& unit = info.units[i];
        UnitCard uc;
        uc.unit_index = i;

        // Create card from XML component — all static styling is declarative.
        // Fully-expanded per-unit subject names (kept alive across lv_xml_create) so
        // each card binds to its own unit's environment-indicator subjects. Units
        // past kMaxUnits get the always-off placeholders — AmsState owns which is
        // which, since it owns the cap and the registrations.
        const AmsState::EnvIndicatorSubjectNames s = AmsState::env_indicator_subject_names(i);
        const std::string absent = AmsState::unit_absent_subject_name(i);
        const std::string disconnected = AmsState::unit_disconnected_subject_name(i);
        const std::string logo_color = AmsState::unit_logo_color_subject_name(i);
        uc.has_art = unit_has_art(info, i);
        const bool as_tile = uc.has_art || any_art;
        // A tile takes the card's name: the tour and ctl recipes find a unit by it.
        const char* attrs[] = {"absent",
                               absent.c_str(),
                               as_tile ? "name" : "logo_color",
                               as_tile ? "ams_unit_card" : logo_color.c_str(),
                               "disconnected",
                               disconnected.c_str(),
                               "temp_text",
                               s.temp_text.c_str(),
                               "humidity_text",
                               s.humidity_text.c_str(),
                               "humidity",
                               s.humidity.c_str(),
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
        uc.card = static_cast<lv_obj_t*>(
            lv_xml_create(cards_row_, as_tile ? "ams_unit_tile" : "ams_unit_card", attrs));
        if (!uc.card) {
            spdlog::error("[{}] Failed to create ams_unit_card XML for unit {}", get_name(), i);
            continue;
        }

        // Flex grow so cards share available width equally; a bars tile stays compact.
        if (uc.has_art || !as_tile)
            lv_obj_set_flex_grow(uc.card, 1);
        else {
            lv_obj_set_style_min_width(uc.card, 0, LV_PART_MAIN);
            uc.compact_width = theme_manager_get_spacing("ams_tile_art_max_h") * 3 / 2;
            lv_obj_set_width(uc.card, uc.compact_width);
        }

        // Store unit index for click handler
        // NOTE: lv_obj_add_event_cb used here (not XML event_cb) because each dynamically
        // created card needs per-instance user_data (unit index) that XML bindings can't provide.
        lv_obj_set_user_data(uc.card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(uc.card, on_unit_card_clicked, LV_EVENT_CLICKED, this);

        // Find child widgets declared in XML
        // required-names: ams_unit_tile ams_unit_card
        uc.name_label = helix::ui::find_required(uc.card, "unit_name", get_name());
        if (as_tile) {
            // required-names: ams_unit_tile
            lv_obj_t* stage = helix::ui::find_required(uc.card, "tile_stage", get_name());
            uc.art_stage = uc.has_art ? stage : nullptr;
            for (lv_obj_t* clip : {uc.card, stage}) {
                // DECLARATIVE_OK: extended draw size has no XML form
                lv_obj_add_event_cb(clip, tile_readout_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE,
                                    nullptr);
                lv_obj_refresh_ext_draw_size(clip);
            }
            if (uc.has_art) {
                // required-names: ams_unit_tile
                uc.art = helix::ui::find_required(uc.card, "unit_art", get_name());
            } else {
                // required-names: ams_unit_tile
                uc.bars_container = helix::ui::find_required(uc.card, "bars_container", get_name());
            }
        } else {
            // required-names: ams_unit_card
            uc.logo_image = helix::ui::find_required(uc.card, "unit_logo", get_name());
            // required-names: ams_unit_card
            uc.bars_container = helix::ui::find_required(uc.card, "bars_container", get_name());
            // required-names: ams_unit_card
            uc.slot_count_label = helix::ui::find_required(uc.card, "slot_count", get_name());
        }

        // Stamp the unit index on the environment indicator so its click handler
        // knows which unit's overlay to open.
        // required-names: ams_unit_tile ams_unit_card
        if (lv_obj_t* ind = helix::ui::find_required(uc.card, "env_indicator", get_name())) {
            lv_obj_set_user_data(ind, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        }

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
                                        measured_bar_width(card.bars_container, slot_count),
                                        upstream_lane_mask(unit)})) {
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
        card.bars_built = {unit.first_slot_global_index, 0, 0, 0};
        return;
    }

    // Bar width is measured to fit the container (rule 8's measured-layout
    // exception keeps it in C++); everything else - state, fill, color, the
    // active and error decorations - flows from AmsState's per-slot subjects
    // inside ams_lane_bar, so the bars repaint in place instead of being
    // rebuilt on every refresh.
    int32_t bar_width = measured_bar_width(card.bars_container, slot_count);
    const uint32_t mask = upstream_lane_mask(unit);
    card.bars_built = {unit.first_slot_global_index, slot_count, bar_width, mask};

    helix::ui::ams_lane_bar_create_range(card.bars_container, unit.first_slot_global_index,
                                         slot_count, bar_width, kMiniBarHeightPx, mask);
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
        if (!uc.has_art) // a tile has no logo to shed
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
    // ams_unit_card.xml pads the card's left and right with space_xs.
    const int32_t content_w = narrowest_card_w - 2 * theme_manager_get_spacing("space_xs");

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
        if (uc.has_art)
            continue;
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
            if (ext_spool)
                bypass_spool_set_filament(bypass_widgets_, *ext_spool);
            else
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
            const auto& routed = info.units[i].lane_is_hub_routed;
            ui_system_path_canvas_set_unit_tools(
                system_path_, i, utl.tool_count, utl.first_physical_tool,
                static_cast<int>(std::count(routed.begin(), routed.end(), true)));

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
