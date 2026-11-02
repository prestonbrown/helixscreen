// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pre_print_options_renderer.h"

#include "ui_utils.h"

#include "lvgl.h"
#include "print_start_phase_label.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix::ui {

namespace {

/// Per-tile event-callback context. Stored as the user_data of the tile
/// widget so the dispatcher can recover both the renderer and the option id
/// at event time. Owned by the tile's `LV_EVENT_DELETE` callback (set up
/// alongside the value-changed callback) so it never leaks.
struct ToggleUserData {
    PrePrintOptionsRenderer* renderer;
    std::string id;
};

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

PrePrintOptionsRenderer::~PrePrintOptionsRenderer() {
    // clear() walks each row and explicitly deinits its subject. Even when
    // the container has already been deleted (its observers gone), calling
    // lv_subject_deinit on a subject with no observers is a no-op cleanup
    // that frees its internal lists. Skipping this would leak the
    // subject->subs_ll backing on every detail-view destroy cycle.
    clear();
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
    return "tune";
}

void PrePrintOptionsRenderer::populate(lv_obj_t* container, const PrePrintOptionSet& option_set,
                                       const VisibilitySubjectLookup& visibility_lookup,
                                       OnToggleCallback on_toggle) {
    if (!container) {
        spdlog::warn("[PrePrintOptionsRenderer] populate() called with null container");
        return;
    }

    // Order matters: deinit subjects (observers attached to live widgets) BEFORE
    // we ask LVGL to delete those widgets. If we deleted widgets first,
    // `safe_clean_children`'s widget cleanup would later fire each observer's
    // LV_EVENT_DELETE handler, and that handler calls `lv_observer_remove` on
    // the still-live subject — except in our case the subject is about to
    // disappear and our deinit walk would race the widget-side cleanup. By
    // deiniting subjects first, we walk each subject's observer list and
    // uninstall the per-observer state from the (still-alive) widget, so the
    // subsequent widget delete has nothing to do for those observers. The
    // alternative ordering would risk `lv_observer_remove` touching a freed
    // subject's `subs_ll` (UAF).
    //
    // clear() also drops on_toggle_, so the caller's callback is assigned
    // after it; assigning before clear() would leave a populate() with no
    // toggle callback.
    clear();
    safe_clean_children(container);

    on_toggle_ = std::move(on_toggle);

    if (option_set.options.empty()) {
        spdlog::debug("[PrePrintOptionsRenderer] Empty option set — container left empty");
        return;
    }

    // Flat list of rows in (category, order) sort order. Categories serve as
    // a sort key only — no subheaders rendered. The single PRINT OPTIONS card
    // header in print_file_detail.xml acts as the section title.
    for (const auto& opt : option_set.options) {
        make_row(container, opt, visibility_lookup);
    }

    spdlog::debug("[PrePrintOptionsRenderer] Populated {} option row(s)", rows_.size());
}

void PrePrintOptionsRenderer::clear() {
    // Each OptionRow owns its lv_subject_t via unique_ptr; we must call
    // lv_subject_deinit explicitly before the unique_ptr drops the
    // allocation, otherwise the subject's observer linked-list backing
    // leaks. Observers themselves are owned by the row widgets that were
    // already deleted (or are being deleted by safe_clean_children); LVGL
    // auto-removes observers on widget delete, so by the time we get here
    // the subject's observer list is already empty.
    for (auto& row : rows_) {
        if (row.state_subject) {
            lv_subject_deinit(row.state_subject.get());
        }
    }
    rows_.clear();
    on_toggle_ = nullptr;
}

int PrePrintOptionsRenderer::get_state(const std::string& id, int default_if_missing) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    if (it == rows_.end() || !it->state_subject) {
        return default_if_missing;
    }
    return lv_subject_get_int(it->state_subject.get());
}

void PrePrintOptionsRenderer::set_state(const std::string& id, int new_state) {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    if (it == rows_.end() || !it->state_subject) {
        return;
    }
    lv_subject_set_int(it->state_subject.get(), new_state);
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

    // Heap-allocate the state subject so it survives container rebuilds where
    // OptionRow values may be moved around.
    row.state_subject = std::make_unique<lv_subject_t>();
    lv_subject_init_int(row.state_subject.get(), opt.default_enabled ? 1 : 0);

    // The tile itself, built from the option_tile XML component
    // (ui_xml/components/option_tile.xml) rather than hand-assembled here, so
    // the appearance can't drift from the XML-authored widgets. The tile is
    // the row: the checkable object and the container are one widget, so
    // there is no child lookup.
    //
    // `callback` is deliberately left unset (its XML "" default): callback=""
    // resolves to the no-op event cb registered for the empty name
    // (xml_registration.cpp), leaving the real handler to be wired
    // imperatively via lv_obj_add_event_cb below.
    //
    // label_tag carries the untranslated i18n key (matching label_for()'s
    // lv_tr() call) so the label can be re-resolved by
    // lv_label_set_translation_tag() on a language change — passing "" here
    // would make lv_translation_get() fall back to returning the tag itself
    // ("") and blank the label out.
    std::string label_key = label_key_for(opt);
    std::string text = std::string(lv_tr(label_key.c_str()));
    std::string icon = default_icon_for(opt.id);
    const char* attrs[] = {"label", text.c_str(), "label_tag", label_key.c_str(),
                           "icon",  icon.c_str(), nullptr};
    lv_obj_t* row_obj = static_cast<lv_obj_t*>(lv_xml_create(container, "option_tile", attrs));
    if (!row_obj) {
        spdlog::error("[PrePrintOptionsRenderer] lv_xml_create('option_tile') returned "
                      "NULL for '{}'",
                      opt.id);
        // Subject was init'd above but never handed to a tile; deinit
        // now or its observer-list backing leaks (see ~PrePrintOptionsRenderer).
        lv_subject_deinit(row.state_subject.get());
        return;
    }
    lv_obj_set_name(row_obj, ("option_tile_" + opt.id).c_str());
    row.row = row_obj;

    if (opt.default_enabled) {
        lv_obj_add_state(row_obj, LV_STATE_CHECKED);
    }
    row.toggle_widget = row_obj;

    // Bind tile checked-state to the per-option state subject. We do this
    // imperatively rather than via the XML <bind_state_if_eq> path because
    // the tile is created dynamically.
    lv_subject_add_observer_obj(
        row.state_subject.get(),
        [](lv_observer_t* observer, lv_subject_t* subject) {
            auto* widget = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
            if (lv_subject_get_int(subject) == 1) {
                lv_obj_add_state(widget, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(widget, LV_STATE_CHECKED);
            }
        },
        row_obj, nullptr);

    // Visibility binding — `bind_flag_if_eq subject="..." flag="hidden" ref_value="0"`.
    if (visibility_lookup) {
        if (lv_subject_t* vis = visibility_lookup(opt.id)) {
            lv_subject_add_observer_obj(
                vis,
                [](lv_observer_t* observer, lv_subject_t* subject) {
                    auto* widget = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
                    if (lv_subject_get_int(subject) == 0) {
                        lv_obj_add_flag(widget, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_remove_flag(widget, LV_OBJ_FLAG_HIDDEN);
                    }
                },
                row.row, nullptr);
        }
    }

    // Wire up the value-changed callback. ToggleUserData is heap-allocated
    // and freed in the LV_EVENT_DELETE handler so it lives exactly as long
    // as the tile widget.
    auto* user_data = new ToggleUserData{this, opt.id};
    lv_obj_add_event_cb(row_obj, on_toggle_value_changed, LV_EVENT_VALUE_CHANGED, user_data);
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            // Free the heap-allocated user data when the widget is deleted.
            // LV_EVENT_DELETE callbacks fire exactly once.
            auto* data = static_cast<ToggleUserData*>(lv_event_get_user_data(e));
            delete data;
        },
        LV_EVENT_DELETE, user_data);

    rows_.push_back(std::move(row));
}

void PrePrintOptionsRenderer::on_toggle_value_changed(lv_event_t* e) {
    // user_data lifetime is guaranteed by the LV_EVENT_DELETE handler, which
    // LVGL fires last among a widget's event callbacks. VALUE_CHANGED can
    // never fire after `data` is freed because deletion runs first only on
    // the LV_EVENT_DELETE path, which then frees `data` once.
    auto* data = static_cast<ToggleUserData*>(lv_event_get_user_data(e));
    if (!data || !data->renderer) {
        return;
    }
    auto* tile = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int new_state = lv_obj_has_state(tile, LV_STATE_CHECKED) ? 1 : 0;
    data->renderer->set_state(data->id, new_state);
    if (data->renderer->on_toggle_) {
        data->renderer->on_toggle_(data->id, new_state);
    }
}

} // namespace helix::ui
