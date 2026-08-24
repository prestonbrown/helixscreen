// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_card.h"

#include "ui_fonts.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "color_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_start_checks.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace helix::ui {

// Pills are instantiated from ui_xml/components/filament_mapping_pill.xml
// (dynamic count depends on the gcode file). lv_obj_add_event_cb is used on
// the card itself for the modal-open handler as an allowed exception.

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

    // Hide on backends with no editable tool mapping (Snapmaker U1, ACE).
    // Without this, users can open the modal and pick mappings that the
    // print-start path then warns away — dead control. The print-start
    // warning toast stays as a safety net.
    bool any_editable = false;
    for (int i = 0, n = ams.backend_count(); i < n; ++i) {
        auto* backend = ams.get_backend(i);
        if (!backend) {
            continue;
        }
        auto caps = backend->get_tool_mapping_capabilities();
        if (caps.supported && caps.editable) {
            any_editable = true;
            break;
        }
    }
    if (!any_editable) {
        should_show_ = false;
        return;
    }

    // Same dead-control rule as the check above, extended to the case that
    // actually bit a user: with bypass engaged a single-tool print takes its
    // filament from the external spool, and print_start_checks.cpp compares
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

    // Collect available slots from AMS backends (canonical accessor — single
    // source of truth shared with the print detail view's preflight check).
    available_slots_ = AmsState::instance().collect_available_slots();

    // Compute mappings based on user preference
    if (SettingsManager::instance().get_auto_color_map()) {
        // Color matching: clear firmware mappings so they don't override color matches
        auto slots_for_matching = available_slots_;
        for (auto& s : slots_for_matching) {
            s.current_tool_mapping = -1;
        }
        mappings_ = helix::FilamentMapper::compute_defaults(tool_info_, slots_for_matching);
    } else {
        // Positional assignment (T0→slot 0, T1→slot 1, etc.)
        mappings_ = helix::FilamentMapper::use_current_assignments(tool_info_, available_slots_);
    }

    // Restrict to the tools the gcode actually uses. update() rebuilds from the
    // full palette, so re-apply the current set here — a used-tools set pushed
    // before this (later) update() must survive the rebuild. nullopt/empty is a
    // no-op (show all).
    apply_used_tools_filter(tool_info_, mappings_, used_tools_);

    // Build the compact UI
    rebuild_compact_view();

    // Visibility is published via the `filament_mapping_visible` subject by the
    // detail view — see PrintSelectDetailView::publish_mapping_visibility().
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
                       std::to_string(t.material.size()) + ":" + t.material + "|";
    }
    for (const auto& m : mappings_) {
        fingerprint += std::to_string(m.tool_index) + ">" + std::to_string(m.mapped_slot) + ":" +
                       std::to_string(m.mapped_backend) + (m.is_auto ? "a" : "m") + "|";
    }
    for (const auto& s : available_slots_) {
        fingerprint += std::to_string(s.backend_index) + "." + std::to_string(s.slot_index) + "=" +
                       std::to_string(s.color_rgb) + (s.is_empty ? "e" : "f") + "|";
    }
    if (fingerprint == last_render_fingerprint_ && lv_obj_get_child_count(rows_container_) > 0) {
        return;
    }
    last_render_fingerprint_ = std::move(fingerprint);

    helix::ui::safe_clean_children(rows_container_);

    // Pill layout, sizing, padding, fonts all live in
    // ui_xml/components/filament_mapping_pill.xml — tune visuals without
    // rebuilding. C++ only supplies per-pill dynamic data: colors, Tx label,
    // and the empty-slot warning variant.
    lv_obj_set_flex_flow(rows_container_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_flex_cross_place(rows_container_, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(rows_container_, theme_manager_get_spacing("space_xs"), 0);

    size_t count = std::min(mappings_.size(), tool_info_.size());
    bool multi_tool = count > 1;
    // Cap visible pills so a file with many tools doesn't flood the right
    // column. Beyond the cap, the remaining tools are summarized in a single
    // "+N" overflow pill that fills the final grid cell (tap the card to see
    // and edit the full mapping).
    constexpr size_t MAX_VISIBLE_PILLS = 6;
    size_t visible = count;
    bool overflow = count > MAX_VISIBLE_PILLS;
    if (overflow) {
        visible = MAX_VISIBLE_PILLS - 1; // leave space for the overflow pill
    }
    for (size_t i = 0; i < visible; ++i) {
        const auto& mapping = mappings_[i];
        const auto& tool = tool_info_[i];

        auto* pill = static_cast<lv_obj_t*>(
            lv_xml_create(rows_container_, "filament_mapping_pill", nullptr));
        if (!pill) {
            continue;
        }
        // Target two pills per row (2x2 grid for four-tool prints). Slightly
        // under 50% so the inter-pill gap doesn't force wrapping.
        lv_obj_set_width(pill, lv_pct(48));

        // G-code color dot and Tx label (only shown for multi-tool files).
        lv_color_t gcode_color = lv_color_hex(tool.color_rgb);
        if (auto* gcode_dot = lv_obj_find_by_name(pill, "gcode_dot")) {
            lv_obj_set_style_bg_color(gcode_dot, gcode_color, 0);
        }
        if (auto* tool_lbl = lv_obj_find_by_name(pill, "tool_label")) {
            if (multi_tool) {
                lv_label_set_text_fmt(tool_lbl, "T%d", tool.tool_index);
                lv_obj_set_style_text_color(tool_lbl, theme_manager_get_contrast_color(gcode_color),
                                            0);
                lv_obj_remove_flag(tool_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Slot color dot — resolve mapped slot; empty slots render as a
        // transparent circle with a warning-colored border.
        uint32_t slot_color = 0x808080;
        bool slot_empty = false;
        if (!mapping.is_auto && mapping.mapped_slot >= 0) {
            for (const auto& s : available_slots_) {
                if (s.slot_index == mapping.mapped_slot &&
                    s.backend_index == mapping.mapped_backend) {
                    slot_color = s.color_rgb;
                    slot_empty = s.is_empty;
                    break;
                }
            }
        }
        if (auto* slot_dot = lv_obj_find_by_name(pill, "slot_dot")) {
            if (slot_empty) {
                lv_obj_set_style_bg_opa(slot_dot, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(slot_dot, 2, 0);
                lv_obj_set_style_border_color(slot_dot, theme_manager_get_color("warning"), 0);
                lv_obj_set_style_border_opa(slot_dot, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(slot_dot, lv_color_hex(slot_color), 0);
            }
        }
    }

    if (overflow) {
        if (auto* more = static_cast<lv_obj_t*>(
                lv_xml_create(rows_container_, "filament_mapping_more_pill", nullptr))) {
            lv_obj_set_width(more, lv_pct(48));
            if (auto* lbl = lv_obj_find_by_name(more, "count_label")) {
                lv_label_set_text_fmt(lbl, "+%zu", count - visible);
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
    mapping_modal_.set_on_mappings_updated([this](auto mappings) {
        mappings_ = std::move(mappings);
        rebuild_compact_view();
        if (on_mappings_changed_) {
            on_mappings_changed_();
        }
    });
    mapping_modal_.show(lv_screen_active());
}

// ============================================================================
// Data collection
// ============================================================================

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

        // Parse color
        if (i < colors.size() && !colors[i].empty()) {
            auto parsed = helix::parse_hex_color(colors[i]);
            tool.color_rgb = parsed.value_or(0x808080);
        } else {
            tool.color_rgb = 0x808080;
        }

        // Material
        if (i < materials.size()) {
            tool.material = materials[i];
        }

        tools.push_back(std::move(tool));
    }

    return tools;
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
