// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "helix/xml/indexed_subject_pool.h"
#include "lvgl.h"
#include "macro_param_modal.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

/**
 * @file ui_panel_macros.h
 * @brief Klipper macro execution panel
 *
 * Displays discovered Klipper macros as a reactive, declarative row list and
 * allows single-tap execution. A long-press enters edit mode, where each row
 * gains a visibility checkbox; the header Save button persists the per-printer
 * hidden-macro set.
 *
 * ## Declarative row list
 * The row widgets are built by an XML `<repeat count="macro_row_count">` in
 * `macro_panel.xml`; C++ NEVER creates or cleans row widgets. The panel drives
 * only subjects: five per-row IndexedSubjectPools (name/desc/visible/
 * desc_hidden/chevron_hidden) plus the scalar `macro_row_count`,
 * `macro_edit_mode`, and `macros_edit_save_hidden`. Setting the pools then the
 * count lets the repeat build rows bound to already-populated subjects.
 *
 * ## Pools (grow-only within a session, reclaimed on close)
 * The five pools grow via ensure_size() as the visible list grows and are all
 * reclaim()ed in on_ui_destroyed() so their name-registered subjects are
 * unregistered and freed while LVGL is still live.
 */
class MacrosPanel : public OverlayBase {
  public:
    MacrosPanel();
    ~MacrosPanel() override;

    // === OverlayBase interface ===
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Macros";
    }

    // === Lifecycle hooks ===
    void on_activate() override;
    void on_deactivate() override;

  protected:
    /**
     * @brief Null cached state and reclaim row subject pools after teardown.
     *
     * OverlayBase::destroy_overlay_ui() async-deletes overlay_root_ and then
     * calls this hook. MacrosPanel is a singleton that outlives its widgets, so
     * this drops the discovery observer and reclaims all five pools (unregister
     * + deinit + free) so a re-open starts from a clean subject registry.
     */
    void on_ui_destroyed() override;

  public:
    // === Public API ===
    lv_obj_t* get_panel() const {
        return overlay_root_;
    }

    /**
     * @brief XML event callbacks (registered globally via
     * lv_xml_register_event_cb). Route to the singleton via the global accessor.
     */
    static void on_macro_row_clicked(lv_event_t* e);     ///< tap: toggle (edit) or run
    static void on_macro_card_long_press(lv_event_t* e); ///< long-press: enter edit mode
    static void on_macros_edit_save(lv_event_t* e);      ///< header Save: persist + exit edit
    static void
    on_macros_back_clicked(lv_event_t* e); ///< header Back: exit edit mode, else pop overlay

  private:
    friend struct MacrosPanelTestAccess;

    // === Edit-mode model ===
    /**
     * @brief Refresh all_macros_ from the discovered hardware (sorted, incl.
     * `_`-prefixed). No-op when no IMoonrakerAPI is available (leaves the
     * current list intact — used for both mock and reconnect timing).
     */
    void refresh_macros();

    /**
     * @brief Recompute displayed_ from all_macros_, size + populate the five
     * pools, then publish macro_row_count. Populate-before-count so the repeat
     * binds to already-set subjects (no first-frame flash).
     */
    void rebuild_rows();

    /// Enter edit mode: seed pending_hidden_, show checkboxes + Save, rebuild.
    void enter_edit_mode();

    /// Exit edit mode. When @p save, persist pending_hidden_ via SettingsManager.
    void exit_edit_mode(bool save);

    /// Reset the row list's scroll position to the top (deferred). Called after
    /// enter/exit edit-mode rebuilds so the mode-change row-count swap never
    /// leaves the list scrolled to the bottom.
    void scroll_list_to_top();

    /// Edit-mode row tap: flip pending_hidden_ membership and the visible pool.
    void toggle_row(size_t display_index);

    /**
     * @brief The effective hidden set: the saved set, or the first-run seed
     * (`_`-prefixed macros) when the per-printer key does not yet exist.
     */
    std::set<std::string> seed_default_hidden() const;

    // === Run path (unchanged) ===
    void execute_macro(const std::string& macro_name);
    void fetch_params_and_execute(const std::string& macro_name);
    void fetch_params_and_run(const std::string& macro_name);
    void execute_with_params(const std::string& macro_name, const helix::MacroParamResult& result);
    static std::string prettify_macro_name(const std::string& name);

    // === State ===
    std::vector<std::string> all_macros_;  ///< sorted discovered macros (incl. _*)
    std::set<std::string> pending_hidden_; ///< in-flight edit-mode hidden set
    std::vector<std::string> displayed_;   ///< macros currently rendered (row order)
    bool edit_mode_ = false;               ///< true while in edit mode
    bool ui_alive_ = false;                ///< true between create() and on_ui_destroyed()
    lv_obj_t* scroll_container_ = nullptr; ///< "macro_list" — reset to top on edit-mode transitions

    // Flags
    bool callbacks_registered_ = false;

    // === Per-row subject pools (grow-only; reclaimed on close) ===
    helix::xml::IndexedSubjectPool name_pool_{"macro_name",
                                              helix::xml::IndexedSubjectPool::Type::String};
    // Descriptions can exceed the 64-char default.
    helix::xml::IndexedSubjectPool desc_pool_{"macro_desc",
                                              helix::xml::IndexedSubjectPool::Type::String, 256};
    helix::xml::IndexedSubjectPool visible_pool_{"macro_visible",
                                                 helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool desc_hidden_pool_{"macro_desc_hidden",
                                                     helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool chevron_hidden_pool_{"macro_chevron_hidden",
                                                        helix::xml::IndexedSubjectPool::Type::Int};

    // Macro parameter modal and dangerous macro confirmation
    helix::MacroParamModal param_modal_;
    std::string pending_dangerous_macro_; ///< Macro awaiting danger confirmation
    std::string pending_run_macro_;       ///< Macro awaiting generic run confirmation

    // === Scalar subjects (registered in init_subjects; NOT reclaimed) ===
    SubjectManager subjects_;
    char status_buf_[64] = {};
    lv_subject_t status_subject_{};          ///< "macros_status" (legacy, XML-bound)
    lv_subject_t macro_row_count_{};         ///< "macro_row_count" (drives the repeat)
    lv_subject_t macro_edit_mode_{};         ///< "macro_edit_mode" (0/1)
    lv_subject_t macros_edit_save_hidden_{}; ///< "macros_edit_save_hidden" (1 = Save hidden)

    /// Re-runs rebuild_rows() once macros become available (late discovery /
    /// reconnect / printer switch). nav_buttons_enabled flips to 1 only after
    /// hardware is populated on the main thread.
    ObserverGuard nav_enabled_observer_;
};

/**
 * @brief Get the global MacrosPanel instance
 *
 * Creates the instance on first call. Used by static callbacks.
 *
 * @return Reference to singleton MacrosPanel
 */
MacrosPanel& get_global_macros_panel();
