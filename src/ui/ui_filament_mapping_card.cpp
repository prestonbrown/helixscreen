// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_card.h"

#include "ui_fonts.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "color_utils.h"
#include "filament_mapper.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_start_checks.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace helix::ui {

// Chips are instantiated from ui_xml/components/filament_swatch.xml (dynamic
// count depends on the gcode file). lv_obj_add_event_cb is used on the card
// itself for the modal-open handler as an allowed exception.

// ============================================================================
// Setup
// ============================================================================

void FilamentMappingCard::create(lv_obj_t* card_widget, lv_obj_t* rows_container,
                                 lv_obj_t* warning_container) {
    card_ = card_widget;
    rows_container_ = rows_container;
    warning_container_ = warning_container;

    // Make the entire card tappable. If an on_tap override is set (the print
    // detail view routes the tap to the panel's single open_remap_modal()), fire
    // that; otherwise fall back to the card's own internal mapping modal.
    if (card_) {
        lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            card_,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentMappingCard*>(lv_event_get_user_data(e));
                if (self->on_tap_) {
                    self->on_tap_();
                } else {
                    self->open_mapping_modal();
                }
            },
            LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[FilamentMapping] Card created");
}

// ============================================================================
// Update / visibility
// ============================================================================

void FilamentMappingCard::update(const std::vector<std::string>& gcode_colors,
                                 const std::vector<std::string>& gcode_materials) {
    if (!card_ || !rows_container_) {
        should_show_ = false;
        return;
    }

    // Check if AMS is available
    auto& ams = AmsState::instance();
    if (!ams.is_available()) {
        should_show_ = false;
        return;
    }

    // NO editable-backend gate here. It once existed to avoid a dead control on
    // Snapmaker U1 / ACE, back when a second surface (the print-detail FILAMENTS
    // card) drew the same chips there. That surface is gone, so hiding here would
    // show the user nothing at all. Whether a TAP does anything is a separate
    // question, answered by PrintSelectDetailView::color_card_opens_remap().

    // A dead-control rule survives here even without the editable-backend gate
    // above: with bypass engaged a single-tool print takes its filament from
    // the external spool, and print_start_checks.cpp compares
    // against that spool instead of the lanes (the `any_bypass_active &&
    // print_lane_requirement(...) <= 1` short-circuit). Offering a lane mapping
    // there claims something the print will not do — a K2 Plus user read the
    // chips as "this maps to lane 2", tapped one to confirm it, and started a
    // print that ran on the bypass spool. A genuinely multi-lane print still
    // uses the mapping with bypass on, so this only hides the <= 1 case.
    //
    // print_lane_requirement() is shared with the gate rather than reimplemented
    // here: it prefers the scan's tools_used and falls back to the palette, and
    // a second copy of that precedence would drift into exactly the mismatch
    // this hides.
    if (ams.any_bypass_active() &&
        helix::print_lane_requirement(used_tools_ ? *used_tools_ : std::set<int>{},
                                      gcode_colors.size()) <= 1) {
        should_show_ = false;
        return;
    }

    // Build tool info from file metadata
    tool_info_ = build_tool_info(gcode_colors, gcode_materials);

    if (tool_info_.empty()) {
        should_show_ = false;
        return;
    }

    // Moved from PrintSelectDetailView::swatches_card_visible_for(): on a
    // multi-tool printer any referenced tool is worth showing (lane identity
    // matters); on a single extruder it takes 2+ tools to be a manual-swap
    // multi-colour file rather than an ordinary single-colour print.
    //
    // print_lane_requirement() (shared with the bypass gate above) is reused
    // rather than a hand-written used_tools_->size() : tool_info_.size()
    // fallback — every OTHER reader of used_tools_ in this class treats an
    // empty set as "no answer, show all" (the bypass gate above,
    // apply_used_tools_filter()'s documented contract), and a second,
    // divergent copy of "empty means fall back to the palette count" is
    // exactly the drift this merge exists to remove. An empty set is not a
    // not-yet-computed sentinel either — PrintSelectDetailView persists a
    // successful zero-tool scan as a legitimate single-extruder answer.
    const int ams_slots = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    const bool is_multi_tool_printer =
        helix::ToolState::instance().is_multi_tool() || ams_slots > 1;
    const size_t tool_count = helix::print_lane_requirement(
        used_tools_ ? *used_tools_ : std::set<int>{}, tool_info_.size());
    if (!(is_multi_tool_printer ? tool_count > 0 : tool_count > 1)) {
        should_show_ = false;
        return;
    }

    // Collect available slots from AMS backends (canonical accessor — single
    // source of truth shared with the print detail view's preflight check).
    available_slots_ = AmsState::instance().collect_available_slots();

    // Seed through the shared rule. Reads the EFFECTIVE auto-match predicate, not
    // the raw setting this used to read - correct on every backend the card now
    // shows on, because AmsState::effective_auto_match() itself deliberately
    // overrides the raw setting on backends (Snapmaker U1) where the two differ.
    mappings_ = AmsState::instance().seed_tool_mappings(tool_info_, available_slots_);

    // Restrict to the tools the gcode actually uses. update() rebuilds from the
    // full palette, so re-apply the current set here — a used-tools set pushed
    // before this (later) update() must survive the rebuild. nullopt/empty is a
    // no-op (show all).
    apply_used_tools_filter(tool_info_, mappings_, used_tools_);

    // Build the compact UI
    rebuild_compact_view();

    // Visibility is published via the `filament_mapping_visible` subject by the
    // detail view — see PrintSelectDetailView::publish_card_visibility().
    should_show_ = true;

    spdlog::debug("[FilamentMapping] Updated: {} tools, {} slots, {} mappings", tool_info_.size(),
                  available_slots_.size(), mappings_.size());
}

void FilamentMappingCard::refresh_slot_data() {
    if (!card_ || !rows_container_) {
        return;
    }
    if (!AmsState::instance().is_available()) {
        return;
    }
    // Refresh loaded colors + presence only; mappings_ and tool_info_ untouched.
    available_slots_ = AmsState::instance().collect_available_slots();
    rebuild_compact_view();
}

void FilamentMappingCard::set_used_tools(std::optional<std::set<int>> used) {
    used_tools_ = std::move(used);
    // Compact the card's current tool_info_/mappings_ in lockstep. Mappings-
    // preserving (no recompute) — mirrors refresh_slot_data. nullopt/empty is a
    // no-op (show all). Does NOT touch the detail view's full-palette copies
    // (current_filament_colors_/materials) — only the card's own vectors.
    apply_used_tools_filter(tool_info_, mappings_, used_tools_);
    rebuild_compact_view();
}

bool FilamentMappingCard::has_mismatch() const {
    return has_any_mismatch();
}

void FilamentMappingCard::on_ui_destroyed() {
    card_ = nullptr;
    rows_container_ = nullptr;
    warning_container_ = nullptr;
    // The widgets this fingerprint described are gone — a recycled card must
    // fully re-render, not early-return against a stale render.
    last_render_fingerprint_.clear();
}

// ============================================================================
// Compact swatch pair view
// ============================================================================

void FilamentMappingCard::rebuild_compact_view() {
    if (!rows_container_) {
        return;
    }

    // [L081] freeze+drain handles UpdateQueue concurrency, but LVGL's own
    // event-dispatch loop (modal on_mappings_updated → here) is the other
    // batch we have to escape. safe_clean_children async-deletes via LVGL.
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    // drain() runs whatever was already queued, and NavigationManager::go_back()
    // is fully deferred — so a pop queued before we got here executes right on
    // that line. Popping the print-detail overlay reaches on_ui_destroyed(),
    // which nulls rows_container_ underneath us. The check above is stale from
    // this point on; every use below must come after a fresh read (#1221).
    if (!rows_container_) {
        spdlog::debug("[FilamentMapping] Container destroyed during drain — skipping rebuild");
        return;
    }

    // Idempotent render: identical (tools, mappings, slot state) + existing
    // children => nothing visible changed => skip the destroy/recreate. Kills
    // the late "gray -> real" rebuild when AMS resync data arrives after the
    // panel opens. MUST stay below the post-drain null check above: a
    // container destroyed during the drain returns before this, and no render
    // happened, so no fingerprint is written either.
    //
    // material is the only free-form string in the encoding (every other field
    // is numeric or a single flag char), so it is the only one that can smuggle
    // the ':'/'|' separators: one tool with material "A|1:0:B" and the two tools
    // {0,0,"A"},{1,0,"B"} both render "0:0:A|1:0:B|" in the tool section. The
    // mappings section below happens to break the tie today (it re-encodes the
    // tool count, and mappings_ is built parallel to tool_info_), so this is
    // hardening rather than a live skipped rebuild — but nothing enforces that
    // redundancy. Length-prefixing as <len>':'<bytes> makes the tool section
    // unambiguous on its own, for any material text.
    std::string fingerprint;
    fingerprint.reserve(128);
    for (const auto& t : tool_info_) {
        fingerprint += std::to_string(t.tool_index) + ":" + std::to_string(t.color_rgb) + ":" +
                       (t.color_known ? "k" : "u") + ":" + std::to_string(t.material.size()) + ":" +
                       t.material + "|";
    }
    for (const auto& m : mappings_) {
        fingerprint += std::to_string(m.tool_index) + ">" + std::to_string(m.mapped_slot) + ":" +
                       std::to_string(m.mapped_backend) + (m.is_auto ? "a" : "m") + "|";
    }
    for (const auto& s : available_slots_) {
        fingerprint += std::to_string(s.backend_index) + "." + std::to_string(s.slot_index) + "=" +
                       std::to_string(s.color_rgb) + (s.is_empty ? "e" : "f") + "|";
    }

    // ONE row, and what runs past its right edge is clipped, not wrapped:
    // filament_mapping_rows is flex_flow="row" at a fixed height with
    // scrollable="false". So cap the chips at what actually fits and say so with
    // a "+N" pill - dropping the rest silently would hide lanes the print uses.
    // n chips occupy n*CHIP_W + (n-1)*gap, hence the (+ gap) on both sides.
    const int32_t gap = theme_manager_get_spacing("space_xs");
    const int32_t avail = lv_obj_get_content_width(rows_container_);
    const size_t capacity = chips_that_fit(avail, gap);

    // Capacity is an input to the render, so it has to be an input to the
    // fingerprint. A render that lands before layout has settled measures a
    // zero-width row and takes the MIN_VISIBLE_CHIPS fallback; without this the
    // wrong answer would be PERMANENT, because every later rebuild carrying the
    // same data early-returns below and on_ui_destroyed() is the only other thing
    // that clears the fingerprint. Encode the capacity rather than the raw width:
    // a width change that does not change how many chips fit changes nothing on
    // screen and should not cost a rebuild.
    fingerprint += "c" + std::to_string(capacity);

    if (fingerprint == last_render_fingerprint_ && lv_obj_get_child_count(rows_container_) > 0) {
        return;
    }
    last_render_fingerprint_ = std::move(fingerprint);

    helix::ui::safe_clean_children(rows_container_);

    // Chip layout, sizing, padding, fonts all live in
    // ui_xml/components/filament_swatch.xml — tune visuals without rebuilding.
    // C++ only supplies per-chip dynamic data: the two band colours, the Tx and
    // lane labels, the divider colour, and the empty-slot warning variant.
    lv_obj_set_flex_flow(rows_container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(rows_container_, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(rows_container_, gap, 0);

    // The "+N" pill takes one of the slots rather than being drawn past the edge,
    // so overflowing costs a chip.
    const size_t tool_count = tool_info_.size();
    const bool overflow = tool_count > capacity;
    const size_t visible = overflow ? capacity - 1 : tool_count;
    spdlog::debug("[FilamentMapping] Row {}px fits {} chip(s); {} tool(s) -> {} shown{}", avail,
                  capacity, tool_count, visible, overflow ? " + overflow pill" : "");

    const bool multi_tool = tool_count > 1;
    const lv_color_t neutral = theme_manager_get_color("text_muted");

    for (size_t i = 0; i < visible; ++i) {
        const auto& tool = tool_info_[i];
        auto* chip =
            static_cast<lv_obj_t*>(lv_xml_create(rows_container_, "filament_swatch", nullptr));
        if (!chip) {
            continue;
        }
        // Fix the chip width in code: a numeric width on a component <view> root
        // is not honoured by lv_xml_create (only "content"/"%"), and the band
        // labels use flex_grow (which contributes 0 to content width), so without
        // this the whole chip collapses to 0.
        lv_obj_set_width(chip, CHIP_WIDTH); // DECLARATIVE_OK: lv_xml_create width limitation

        // mappings_ is built parallel to tool_info_ and compacted in lockstep by
        // apply_used_tools_filter, so index i is this tool's mapping. Fall back to
        // a default mapping rather than indexing past the end if that ever drifts.
        const ToolMapping mapping = (i < mappings_.size()) ? mappings_[i] : ToolMapping{};
        const helix::AvailableSlot* const resolved =
            helix::FilamentMapper::resolve_mapped_slot(mapping, available_slots_);

        // Every mutation below is per-item payload on a C++-generated collection:
        // the card builds one chip per used tool from runtime data, so there is no
        // XML instance per tool to hang a bind on.

        // Colour mismatch surrounds the WHOLE chip (color_mismatch style, under
        // selector user_2 in filament_swatch.xml): the wrong thing is the pairing
        // the two bands represent, not either band on its own. That scope is also
        // what keeps it apart from the empty-lane border below, which stays on
        // bottom_band because "this lane holds nothing" is a fact about the lane.
        //
        // Classified HERE, against the lane just resolved, rather than read from
        // mapping.color_mismatch. refresh_slot_data() replaces available_slots_
        // and rebuilds with mappings_ deliberately untouched, so a cached verdict
        // outlives the lane it judged: swap a spool for one holding the file's
        // colour and the bottom band repaints while the surround stays on. That
        // is the same shape as the stale lane number this chip was built to fix.
        // DECLARATIVE_OK: per-item payload on a C++-generated collection.
        if (resolved &&
            helix::FilamentMapper::classify_mismatches(tool, *resolved).color_mismatch) {
            lv_obj_add_state(chip, LV_STATE_USER_2);
        }

        // TOP band: the gcode file's intended colour for this tool.
        if (auto* top = lv_obj_find_by_name(chip, "top_band")) {
            if (tool.color_known) {
                lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0); // DECLARATIVE_OK: see above
                lv_obj_set_style_bg_color(top, lv_color_hex(tool.color_rgb), 0);
            } else {
                // No fill: painting the neutral stand-in reads as "this file
                // prints in grey", a claim nothing has made. (K2 Plus report.)
                lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0); // DECLARATIVE_OK: see above
            }
            if (auto* tool_lbl = lv_obj_find_by_name(top, "tool_label")) {
                if (multi_tool) {
                    lv_label_set_text_fmt(tool_lbl, "T%d", tool.tool_index);
                    // Contrast is computed against the fill; with no fill there is
                    // nothing to contrast against, so take the normal text colour.
                    lv_obj_set_style_text_color(tool_lbl,
                                                tool.color_known ? theme_manager_get_contrast_color(
                                                                       lv_color_hex(tool.color_rgb))
                                                                 : theme_manager_get_color("text"),
                                                0);
                    lv_obj_remove_flag(tool_lbl, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(tool_lbl, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // BOTTOM band: the present colour of the effective mapped lane.
        const bool slot_empty = resolved && resolved->is_empty;
        const lv_color_t slot_color =
            (resolved && !resolved->is_empty) ? lv_color_hex(resolved->color_rgb) : neutral;
        if (auto* bottom = lv_obj_find_by_name(chip, "bottom_band")) {
            if (slot_empty) {
                // Declarative empty_slot style (warning border, reduced opacity)
                // declared in filament_swatch.xml under selector user_1.
                lv_obj_add_state(bottom, LV_STATE_USER_1);
            } else if (resolved) {
                lv_obj_set_style_bg_color(bottom, slot_color, 0); // DECLARATIVE_OK: see above
            } else {
                // No lane chosen yet: naming one would be a claim the mapping has
                // not made, so the band stays blank.
                lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0); // DECLARATIVE_OK: see above
            }
            if (auto* slot_lbl = lv_obj_find_by_name(bottom, "slot_label")) {
                // resolve_mapped_slot() already found the lane; asking
                // mapped_lane_display_number() would rescan available_slots_ for
                // the same answer, which is the split this task exists to close.
                const int lane_number = resolved ? resolved->local_slot_index + 1 : -1;
                if (lane_number > 0) {
                    lv_label_set_text_fmt(slot_lbl, "%d", lane_number);
                    // An empty lane draws no fill, so there is nothing to contrast
                    // against - take the warning colour that the band border uses.
                    lv_obj_set_style_text_color(slot_lbl,
                                                slot_empty
                                                    ? theme_manager_get_color("warning")
                                                    : theme_manager_get_contrast_color(slot_color),
                                                0);
                    lv_obj_remove_flag(slot_lbl, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(slot_lbl, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // Divider: a colour that reads against BOTH band fills. Blend the two
        // 50/50 and take the contrast of the blend, so the rule stays visible
        // whether the bands are light, dark or mixed.
        if (auto* divider = lv_obj_find_by_name(chip, "divider")) {
            const lv_color_t top_color = tool.color_known ? lv_color_hex(tool.color_rgb) : neutral;
            lv_obj_set_style_bg_color(
                divider,
                theme_manager_get_contrast_color(lv_color_mix(top_color, slot_color, LV_OPA_50)),
                0); // DECLARATIVE_OK: see above
        }
    }

    if (overflow) {
        if (auto* more = static_cast<lv_obj_t*>(
                lv_xml_create(rows_container_, "filament_mapping_more_pill", nullptr))) {
            // Same 40px slot as a chip - the capacity arithmetic above counted it
            // as one - and the row's full height, since the row is fixed-height
            // and the component is content-sized.
            lv_obj_set_width(more, CHIP_WIDTH); // DECLARATIVE_OK: see the chip width above
            lv_obj_set_height(more, lv_pct(100));
            if (auto* lbl = lv_obj_find_by_name(more, "count_label")) {
                const size_t hidden = tool_count - visible;
                lv_label_set_text_fmt(lbl, "+%zu", hidden); // DECLARATIVE_OK: see above
            }
        }
    }

    // Warning icon visibility is handled by XML bind_flag_if_eq on "filament_mismatch" subject
}

bool FilamentMappingCard::has_any_mismatch() const {
    for (const auto& m : mappings_) {
        if (m.material_mismatch) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Color queries
// ============================================================================

std::vector<uint32_t> FilamentMappingCard::get_mapped_colors() const {
    return helix::FilamentMapper::resolve_display_colors(tool_info_, mappings_, available_slots_);
}

// ============================================================================
// Modal interaction
// ============================================================================

void FilamentMappingCard::open_mapping_modal() {
    spdlog::debug("[FilamentMapping] Opening mapping modal");

    mapping_modal_.set_tool_info(tool_info_);
    mapping_modal_.set_available_slots(available_slots_);
    mapping_modal_.set_mappings(mappings_);
    mapping_modal_.set_on_mappings_updated(
        [this](auto mappings) { set_mappings(std::move(mappings)); });
    mapping_modal_.show(lv_screen_active());
}

// ============================================================================
// Data collection
// ============================================================================

std::vector<helix::GcodeToolInfo>
FilamentMappingCard::build_used_tool_info(const std::vector<std::string>& colors,
                                          const std::vector<std::string>& materials,
                                          const std::set<int>& used) {
    if (used.empty()) {
        return {};
    }
    const auto all_tool_info = build_tool_info(colors, materials);

    std::vector<helix::GcodeToolInfo> tools;
    tools.reserve(used.size());
    for (int tool : used) {
        if (tool >= 0 && static_cast<size_t>(tool) < all_tool_info.size()) {
            auto info = all_tool_info[static_cast<size_t>(tool)];
            info.tool_index = tool; // real gcode tool number, not palette ordinal
            tools.push_back(info);
        }
    }
    return tools;
}

std::vector<helix::GcodeToolInfo>
FilamentMappingCard::build_tool_info(const std::vector<std::string>& colors,
                                     const std::vector<std::string>& materials) {
    std::vector<helix::GcodeToolInfo> tools;

    // Use the larger of colors or materials to determine tool count.
    // If both are empty, return empty — the card will be hidden.
    size_t count = std::max(colors.size(), materials.size());
    if (count == 0) {
        return tools;
    }

    for (size_t i = 0; i < count; ++i) {
        helix::GcodeToolInfo tool;
        tool.tool_index = static_cast<int>(i);

        // Parse color. A missing or unparsable entry leaves color_rgb as a
        // NEUTRAL STAND-IN, not an answer: Moonraker omits filament_colors
        // entirely for some slicers (every OrcaSlicer file on a K2 Plus reports
        // filament_type and nothing else), and the palette is backfilled later
        // from the G-code footer or the viewer. Flagging it keeps the stand-in
        // from being read as the file's choice — see GcodeToolInfo::color_known.
        if (i < colors.size() && !colors[i].empty()) {
            auto parsed = helix::parse_hex_color(colors[i]);
            tool.color_rgb = parsed.value_or(0x808080);
            tool.color_known = parsed.has_value();
        } else {
            tool.color_rgb = 0x808080;
            tool.color_known = false;
        }

        // Material
        if (i < materials.size()) {
            tool.material = materials[i];
        }

        tools.push_back(std::move(tool));
    }

    return tools;
}

size_t FilamentMappingCard::chips_that_fit(int32_t content_width, int32_t gap) {
    // Layout has not settled on the very first render, so the width reads 0 (or
    // negative once padding is subtracted). Fall back to the floor rather than
    // to "everything": drawing past the right edge is what silently loses lanes.
    if (content_width <= 0) {
        return MIN_VISIBLE_CHIPS;
    }
    // n chips occupy n*CHIP_WIDTH + (n-1)*gap, hence the (+ gap) on both sides.
    const int32_t n = (content_width + gap) / (CHIP_WIDTH + gap);
    // Always offer one slot: a row too narrow for a single chip still has to show
    // the "+N" pill rather than nothing at all.
    return static_cast<size_t>(std::max<int32_t>(1, n));
}

void FilamentMappingCard::apply_used_tools_filter(std::vector<helix::GcodeToolInfo>& tool_info,
                                                  std::vector<helix::ToolMapping>& mappings,
                                                  const std::optional<std::set<int>>& used) {
    // nullopt OR empty set ⇒ no filter (show all). Safety rule: never blank the
    // card pre-parse, and never hide everything on the headless single-extruder
    // path (where the used set is empty forever).
    if (!used || used->empty()) {
        return;
    }
    const std::set<int>& keep = *used;

    // Filter BOTH vectors independently by their own .tool_index — they are
    // built parallel (mappings_[i].tool_index == tool_info_[i].tool_index), so
    // the same predicate compacts them in lockstep. std::remove_if preserves
    // order; .tool_index is retained (used for the "T%d" label + modal rows).
    tool_info.erase(std::remove_if(tool_info.begin(), tool_info.end(),
                                   [&keep](const helix::GcodeToolInfo& t) {
                                       return keep.count(t.tool_index) == 0;
                                   }),
                    tool_info.end());
    mappings.erase(std::remove_if(mappings.begin(), mappings.end(),
                                  [&keep](const helix::ToolMapping& m) {
                                      return keep.count(m.tool_index) == 0;
                                  }),
                   mappings.end());
}

const helix::GcodeToolInfo*
FilamentMappingCard::find_by_tool_index(const std::vector<helix::GcodeToolInfo>& tool_info,
                                        int tool_index) {
    for (const auto& t : tool_info) {
        if (t.tool_index == tool_index) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace helix::ui
