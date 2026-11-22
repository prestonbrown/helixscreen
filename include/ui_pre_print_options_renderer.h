// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "helix/xml/indexed_subject_pool.h"
#include "pre_print_option.h"

#include <functional>
#include <lvgl.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @file ui_pre_print_options_renderer.h
 * @brief Renders the per-print toggle list on the print-detail panel.
 *
 * Owns the per-option state: one int subject per option in the active
 * printer's `PrePrintOptionSet`, `pre_print_option_on_<i>` for row i. Builds a
 * 2-column grid of `option_tile` pills, each checked while its subject reads 1:
 * icon + 2-line label, filled with the primary tint and showing a check in
 * place of the icon when checked. A tap flips the subject. Categories are used
 * as a sort key only; no sub-headers are emitted (the surrounding "PRINT
 * OPTIONS" card header in `print_file_detail.xml` provides the section title).
 *
 * Visibility per row may be bound to a caller-supplied subject via the
 * `VisibilitySubjectLookup` callback — returning a non-null subject hides the
 * row when it reads 0. The detail view passes a lookup that always returns
 * nullptr, so its declared options are always visible.
 *
 * The renderer does NOT decide visibility itself — the caller passes the
 * lookup function.
 *
 * ## Subject lifecycle
 *
 * The state subjects live in a grow-only `IndexedSubjectPool` the renderer
 * owns, so a tile never outlives the subject it binds: the pool is reclaimed
 * only with the renderer, which resets every subject and so uninstalls any
 * binding a still-standing tile holds. `populate()` reuses the slots and
 * re-registers their names, so a second live renderer (a test's own beside
 * the app's) never leaves this one's tiles bound to the other's subjects.
 */
class PrePrintOptionsRenderer {
  public:
    /**
     * @brief Optional lookup for an option's row visibility.
     *
     * Returning nullptr leaves the row visible unconditionally — used for
     * options whose visibility is purely driven by the option set being
     * present in the database (e.g. timelapse or framework-only options).
     * A plugin- or macro-gated option can return its visibility subject here.
     */
    using VisibilitySubjectLookup = std::function<lv_subject_t*(const std::string& id)>;

    /**
     * @brief Callback invoked when a tile toggles. Receives the option id
     *        and the new state (1 = enabled / checked).
     */
    using OnToggleCallback = std::function<void(const std::string& id, int new_state)>;

    PrePrintOptionsRenderer();
    ~PrePrintOptionsRenderer();

    PrePrintOptionsRenderer(const PrePrintOptionsRenderer&) = delete;
    PrePrintOptionsRenderer& operator=(const PrePrintOptionsRenderer&) = delete;
    PrePrintOptionsRenderer(PrePrintOptionsRenderer&&) = delete;
    PrePrintOptionsRenderer& operator=(PrePrintOptionsRenderer&&) = delete;

    /**
     * @brief Replace the contents of `container` with rows for `option_set`.
     *
     * Existing children of `container` are deleted via `safe_clean_children`
     * before new rows are added. If the option set is empty, the container is
     * left empty (no rows, no headers).
     *
     * Per-option state subjects are initialized to the option's
     * `default_enabled` value. Re-calling `populate()` resets state to
     * defaults — caller must persist any user toggles externally if needed.
     *
     * @param container Parent widget that will hold the rows
     * @param option_set Options to render (sorted by category/order on input)
     * @param visibility_lookup Callback returning the can_show_* subject for
     *        each id, or nullptr to skip visibility binding for that option
     * @param on_toggle Callback fired when any tile changes state
     */
    void populate(lv_obj_t* container, const PrePrintOptionSet& option_set,
                  const VisibilitySubjectLookup& visibility_lookup, OnToggleCallback on_toggle);

    /**
     * @brief Drop all rows (e.g. on panel close / printer change).
     */
    void clear();

    /**
     * @brief Read the current toggle state for `id`.
     *
     * @return The subject value (0 or 1) for the named option, or
     *         `default_if_missing` when the option is not currently rendered.
     */
    [[nodiscard]] int get_state(const std::string& id, int default_if_missing = 0) const;

    /**
     * @brief Set toggle state for `id`. No-op if id is not present.
     *
     * @note This updates the underlying subject (and so the tile's checked
     *       state, which binds it), but does NOT invoke the
     *       `OnToggleCallback`. Only a tap on the tile reaches the toggle
     *       callback.
     *       Callers that need both the model update and the side-effect must
     *       invoke the side-effect themselves.
     */
    void set_state(const std::string& id, int new_state);

    /**
     * @brief Number of option rows currently rendered. Excludes sub-headers.
     */
    [[nodiscard]] size_t row_count() const {
        return rows_.size();
    }

    /**
     * @brief List the ids of the rendered option rows in display order.
     */
    [[nodiscard]] std::vector<std::string> rendered_ids() const;

    /**
     * @brief Look up the row widget for `id`. Returns nullptr if not present.
     *        Test/diagnostic helper.
     */
    [[nodiscard]] lv_obj_t* get_row(const std::string& id) const;

    /**
     * @brief Look up the tile widget for `id`: the checkable object itself.
     *        Returns nullptr if not present. Test/diagnostic helper.
     */
    [[nodiscard]] lv_obj_t* get_toggle(const std::string& id) const;

    /// Look up the i18n string for an option's label, falling back to a
    /// humanized version of the id when no `label_key` is set in the DB.
    /// Public because the option label is the user's name for the feature
    /// wherever it is referenced, not just on the toggle row —
    /// PrintPreparationManager uses it to name the features a dropped
    /// modification affects.
    static std::string label_for(const PrePrintOption& opt);

    /// The tile icon for an option id (`grid_large` for bed_mesh,
    /// `camera_timer` for timelapse, `tune` for anything unlisted). The
    /// database's `PrePrintOption::icon` is not read: no entry sets it and
    /// its stored form does not match these icon names.
    static std::string default_icon_for(const std::string& id);

  private:
    friend class PrePrintOptionsRendererTestAccess;

    struct OptionRow {
        std::string id;
        lv_obj_t* row = nullptr;
        lv_obj_t* toggle_widget = nullptr;
        /// This row's slot in state_pool_.
        lv_subject_t* state_subject = nullptr;
    };

    /// Resolve the untranslated i18n key for an option's label — either the
    /// DB's `label_key`, a hardcoded English default for the legacy
    /// mechanical/quality toggles, or a humanized version of the id. Shared
    /// by `label_for()` (translated text) and `make_row()` (which also needs
    /// the raw key for the XML row's `label_tag`, so language changes can
    /// re-resolve the label via `lv_label_set_translation_tag`).
    static std::string label_key_for(const PrePrintOption& opt);

    void make_row(lv_obj_t* container, const PrePrintOption& opt,
                  const VisibilitySubjectLookup& visibility_lookup);

    /// The tiles' XML clicked callback: finds the live renderer that owns the
    /// tile and flips its state.
    static void on_toggle_value_changed(lv_event_t* e);

    /// pre_print_option_on_<i>: 1 while row i's option is on.
    helix::xml::IndexedSubjectPool state_pool_{"pre_print_option_on",
                                               helix::xml::IndexedSubjectPool::Type::Int};
    std::vector<OptionRow> rows_;
    OnToggleCallback on_toggle_;
};

} // namespace helix::ui
