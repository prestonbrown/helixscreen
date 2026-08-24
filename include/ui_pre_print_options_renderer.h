// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

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
 * Owns the per-option `lv_subject_t` state (one int subject per option in the
 * active printer's `PrePrintOptionSet`). Builds a flat row list — one row per
 * option, label on the left and `ui_switch` on the right. Categories are used
 * as a sort key only; no sub-headers are emitted (the surrounding "PRINT
 * OPTIONS" card header in `print_file_detail.xml` provides the section title).
 *
 * Visibility per row may be bound to a caller-supplied subject via the
 * `VisibilitySubjectLookup` callback — returning a non-null subject hides the
 * row when it reads 0. The current detail-view caller passes a lookup that
 * always returns nullptr (declared options are always visible) but the hook
 * remains available for future plugin-/macro-gated options.
 *
 * The renderer does NOT decide visibility itself — the caller passes the
 * lookup function.
 *
 * ## Subject lifecycle
 *
 * Per-option state subjects are heap-allocated `lv_subject_t` instances owned
 * by `OptionRow::state_subject`. They are paired one-to-one with their row
 * widget. The pairing means subject and observers always die together inside
 * `clear()` / the destructor, so no `SubjectLifetime` token is needed: when a
 * row's owning subject is deinited, all of that subject's observers are
 * uninstalled atomically. There is no scenario where the subject can outlive
 * its observers (or vice-versa) since both are stored in the same `OptionRow`
 * struct.
 *
 * ## clear() lifetime contract
 *
 * `clear()` deinits every state subject (which uninstalls observers from the
 * row widgets) and then drops the row vector. It runs in three contexts, each
 * with different widget liveness:
 *
 *   1. From `populate()` rebuild path — `clear()` runs BEFORE
 *      `safe_clean_children`. Widgets are still alive at deinit time, so
 *      `lv_subject_deinit` walks each observer's widget and removes the
 *      `LV_EVENT_DELETE` cleanup callback cleanly.
 *
 *   2. From `OverlayBase::on_ui_destroyed()` (panel close) — widgets are
 *      still alive per OverlayBase's contract (deferred delete on next tick).
 *      Same path as above.
 *
 *   3. From the renderer destructor at static teardown — widgets may already
 *      be gone (LVGL deleted them earlier). In that case, observers were
 *      auto-removed when the widgets were deleted, so the subject's observer
 *      list is already empty and `lv_subject_deinit` is a no-op cleanup that
 *      still frees the subject's internal lists.
 */
class PrePrintOptionsRenderer {
  public:
    /**
     * @brief Optional lookup for an option's row visibility.
     *
     * Returning nullptr leaves the row visible unconditionally — used for
     * options whose visibility is purely driven by the option set being
     * present in the database (e.g. timelapse or framework-only options).
     * Future plugin-/macro-gated options can plug a per-option visibility
     * subject in here.
     */
    using VisibilitySubjectLookup = std::function<lv_subject_t*(const std::string& id)>;

    /**
     * @brief Callback invoked when a switch toggles. Receives the option id
     *        and the new state (1 = enabled / checked).
     */
    using OnToggleCallback = std::function<void(const std::string& id, int new_state)>;

    PrePrintOptionsRenderer() = default;
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
     * @param on_toggle Callback fired when any switch changes state
     */
    void populate(lv_obj_t* container, const PrePrintOptionSet& option_set,
                  const VisibilitySubjectLookup& visibility_lookup, OnToggleCallback on_toggle);

    /**
     * @brief Drop all rows and subjects (e.g. on panel close / printer change).
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
     * @note This updates the underlying subject (and propagates to the
     *       switch's checked state via the observer wiring), but does NOT
     *       invoke the `OnToggleCallback`. Only user-driven changes that fire
     *       `LV_EVENT_VALUE_CHANGED` on the switch reach the toggle callback.
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
     * @brief Look up the switch widget for `id`. Returns nullptr if not
     *        present. Test/diagnostic helper.
     */
    [[nodiscard]] lv_obj_t* get_switch(const std::string& id) const;

    /// Look up the i18n string for an option's label, falling back to a
    /// humanized version of the id when no `label_key` is set in the DB.
    /// Public because the option label is the user's name for the feature
    /// wherever it is referenced, not just on the toggle row —
    /// PrintPreparationManager uses it to name the features a dropped
    /// modification affects.
    static std::string label_for(const PrePrintOption& opt);

  private:
    friend class PrePrintOptionsRendererTestAccess;

    struct OptionRow {
        std::string id;
        lv_obj_t* row = nullptr;
        lv_obj_t* switch_widget = nullptr;
        std::unique_ptr<lv_subject_t> state_subject;
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

    static void on_switch_value_changed(lv_event_t* e);

    std::vector<OptionRow> rows_;
    OnToggleCallback on_toggle_;
};

} // namespace helix::ui
