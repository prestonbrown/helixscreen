// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pre_print_options_renderer.h"

#include "ui_utils.h"

#include "core/subject.h"
#include "lvgl.h"
#include "print_start_phase_label.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix::ui {

namespace {

/// Every live renderer, so the tile's XML value_changed callback can find the
/// renderer that owns the tapped tile.
std::vector<PrePrintOptionsRenderer*>& live_renderers() {
    static std::vector<PrePrintOptionsRenderer*> renderers;
    return renderers;
}

/// Translation tags follow the existing convention of using the English
/// label as both key and default value (see `translations/en.yml`). For
/// options without an explicit `label_key` in the database we fall back to a
/// best-effort humanization of the id ("ai_detect" -> "AI Detect"). New
/// options should declare `label_key` directly to bypass this path.
std::string humanize_id(const std::string& id) {
    std::string out;
    out.reserve(id.size());

    bool capitalize_next = true;
    for (char c : id) {
        if (c == '_' || c == '-') {
            out.push_back(' ');
            capitalize_next = true;
            continue;
        }
        if (capitalize_next) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            capitalize_next = false;
        } else {
            out.push_back(c);
        }
    }

    // Common acronyms: don't lowercase the rest of these even when not first.
    // (We only need the ones that show up in our id space.)
    auto fix_acronym = [&](const std::string& word, const std::string& upper) {
        std::size_t pos = 0;
        while ((pos = out.find(word, pos)) != std::string::npos) {
            // Word-boundary check: previous char is start or space; next char is end or space.
            bool left_ok = (pos == 0) || (out[pos - 1] == ' ');
            bool right_ok = (pos + word.size() == out.size()) || (out[pos + word.size()] == ' ');
            if (left_ok && right_ok) {
                out.replace(pos, word.size(), upper);
                pos += upper.size();
            } else {
                pos += word.size();
            }
        }
    };
    fix_acronym("Ai", "AI");
    fix_acronym("Qgl", "QGL");
    fix_acronym("Pa", "PA");
    fix_acronym("Z tilt", "Z-tilt");

    return out;
}

} // namespace

PrePrintOptionsRenderer::PrePrintOptionsRenderer() {
    live_renderers().push_back(this);
}

PrePrintOptionsRenderer::~PrePrintOptionsRenderer() {
    // state_pool_ is reclaimed after this body, resetting every subject and so
    // uninstalling the binding of any tile still standing.
    clear();
    auto& live = live_renderers();
    live.erase(std::remove(live.begin(), live.end(), this), live.end());
}

std::string PrePrintOptionsRenderer::label_key_for(const PrePrintOption& opt) {
    // First preference: explicit i18n key from the DB.
    if (!opt.label_key.empty()) {
        return opt.label_key;
    }

    // Second preference: a toggle for a pre-print phase reads as that phase
    // does everywhere else. The bed mesh row names its variant instead: whether
    // the mesh is adaptive is set by PrinterState::apply_dynamic_options.
    if (opt.id == "bed_mesh") {
        return opt.adaptive_active ? "Adaptive Bed Mesh" : "Auto Bed Mesh";
    }
    if (const auto phase = helix::print_start_phase_of_option(opt.id)) {
        return helix::print_start_phase_step_key(*phase);
    }
    if (opt.id == "timelapse") {
        return "Record Timelapse";
    }

    // Fallback: humanize the id. Run through lv_tr (in label_for()) in case a
    // translator added an entry under the humanized form.
    return humanize_id(opt.id);
}

std::string PrePrintOptionsRenderer::label_for(const PrePrintOption& opt) {
    return std::string(lv_tr(label_key_for(opt).c_str()));
}

std::string PrePrintOptionsRenderer::default_icon_for(const std::string& id) {
    if (id == "bed_mesh") {
        return "grid_large";
    }
    if (id == "flow_calibrate") {
        return "gauge";
    }
    if (id == "timelapse" || id == "u1_timelapse") {
        return "camera_timer";
    }
    if (id == "ai_detect") {
        return "robot";
    }
    if (id == "qgl" || id == "z_tilt") {
        return "spirit_level";
    }
    if (id == helix::kHeatSoakOptionId) {
        return "hourglass";
    }
    return "tune";
}

void PrePrintOptionsRenderer::populate(lv_obj_t* container, const PrePrintOptionSet& option_set,
                                       const VisibilitySubjectLookup& visibility_lookup,
                                       OnToggleCallback on_toggle) {
    if (!container) {
        spdlog::warn("[PrePrintOptionsRenderer] populate() called with null container");
        return;
    }

    // clear() also drops on_toggle_, so the caller's callback is assigned
    // after it; assigning before clear() would leave a populate() with no
    // toggle callback.
    clear();
    safe_clean_children(container);

    on_toggle_ = std::move(on_toggle);
    lv_xml_register_event_cb(nullptr, "on_pre_print_option_toggled", on_toggle_value_changed);

    if (option_set.options.empty()) {
        spdlog::debug("[PrePrintOptionsRenderer] Empty option set — container left empty");
        return;
    }

    // Flat list of rows in (category, order) sort order. Categories serve as
    // a sort key only — no subheaders rendered. The single PRINT OPTIONS card
    // header in print_file_detail.xml acts as the section title.
    state_pool_.ensure_size(option_set.options.size());
    for (const auto& opt : option_set.options) {
        make_row(container, opt, visibility_lookup);
    }

    spdlog::debug("[PrePrintOptionsRenderer] Populated {} option row(s)", rows_.size());
}

void PrePrintOptionsRenderer::clear() {
    // The pool keeps its subjects for the next populate(); a tile still
    // standing keeps its binding until LVGL deletes it.
    rows_.clear();
    on_toggle_ = nullptr;
}

int PrePrintOptionsRenderer::get_state(const std::string& id, int default_if_missing) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    if (it == rows_.end() || !it->state_subject) {
        return default_if_missing;
    }
    return lv_subject_get_int(it->state_subject);
}

void PrePrintOptionsRenderer::set_state(const std::string& id, int new_state) {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    if (it == rows_.end() || !it->state_subject) {
        return;
    }
    lv_subject_set_int(it->state_subject, new_state);
}

std::vector<std::string> PrePrintOptionsRenderer::rendered_ids() const {
    std::vector<std::string> ids;
    ids.reserve(rows_.size());
    for (const auto& r : rows_) {
        ids.push_back(r.id);
    }
    return ids;
}

lv_obj_t* PrePrintOptionsRenderer::get_row(const std::string& id) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    return (it != rows_.end()) ? it->row : nullptr;
}

lv_obj_t* PrePrintOptionsRenderer::get_toggle(const std::string& id) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    return (it != rows_.end()) ? it->toggle_widget : nullptr;
}

void PrePrintOptionsRenderer::make_row(lv_obj_t* container, const PrePrintOption& opt,
                                       const VisibilitySubjectLookup& visibility_lookup) {
    OptionRow row;
    row.id = opt.id;

    // populate() sized the pool for every option, so this row's slot exists.
    const size_t slot = rows_.size();
    row.state_subject = state_pool_.at(slot);
    state_pool_.set_int(slot, opt.default_enabled ? 1 : 0);
    const std::string checked_subject = "pre_print_option_on_" + std::to_string(slot);

    // The tile itself, built from the option_tile XML component
    // (ui_xml/components/option_tile.xml) rather than hand-assembled here, so
    // the appearance can't drift from the XML-authored widgets. The tile is
    // the row: the toggled object and the container are one widget, so
    // there is no child lookup.
    //
    // label_tag carries the untranslated i18n key (matching label_for()'s
    // lv_tr() call) so the label can be re-resolved by
    // lv_label_set_translation_tag() on a language change — passing "" here
    // would make lv_translation_get() fall back to returning the tag itself
    // ("") and blank the label out.
    std::string label_key = label_key_for(opt);
    std::string text = std::string(lv_tr(label_key.c_str()));
    std::string icon = default_icon_for(opt.id);
    const char* attrs[] = {"label",
                           text.c_str(),
                           "label_tag",
                           label_key.c_str(),
                           "icon",
                           icon.c_str(),
                           "checked_subject",
                           checked_subject.c_str(),
                           "callback",
                           "on_pre_print_option_toggled",
                           nullptr};
    lv_obj_t* row_obj = static_cast<lv_obj_t*>(lv_xml_create(container, "option_tile", attrs));
    if (!row_obj) {
        spdlog::error("[PrePrintOptionsRenderer] lv_xml_create('option_tile') returned "
                      "NULL for '{}'",
                      opt.id);
        return;
    }
    lv_obj_set_name(row_obj, ("option_tile_" + opt.id).c_str());
    row.row = row_obj;
    row.toggle_widget = row_obj;

    // Visibility binding — `bind_flag_if_eq subject="..." flag="hidden" ref_value="0"`.
    if (visibility_lookup) {
        if (lv_subject_t* vis = visibility_lookup(opt.id)) {
            lv_subject_add_observer_obj(
                vis,
                [](lv_observer_t* observer, lv_subject_t* subject) {
                    auto* widget = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
                    if (lv_subject_get_int(subject) == 0) {
                        lv_obj_set_hidden(widget, true);
                    } else {
                        lv_obj_set_hidden(widget, false);
                    }
                },
                row.row, nullptr);
        }
    }

    rows_.push_back(std::move(row));
}

void PrePrintOptionsRenderer::on_toggle_value_changed(lv_event_t* e) {
    auto* tile = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    for (PrePrintOptionsRenderer* renderer : live_renderers()) {
        auto it = std::find_if(renderer->rows_.begin(), renderer->rows_.end(),
                               [tile](const OptionRow& r) { return r.row == tile; });
        if (it == renderer->rows_.end()) {
            continue;
        }
        // Copied: on_toggle_ may repopulate, which drops the row.
        const std::string id = it->id;
        const int new_state = lv_subject_get_int(it->state_subject) == 1 ? 0 : 1;
        renderer->set_state(id, new_state);
        if (renderer->on_toggle_) {
            renderer->on_toggle_(id, new_state);
        }
        return;
    }
}

} // namespace helix::ui
