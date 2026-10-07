// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_macros.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lap_log.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_edit_logic.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using helix::ui::find_required;

MacrosPanel::~MacrosPanel() {
    deinit_subjects();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void MacrosPanel::init_subjects() {
    {
        // Legacy status subject (XML-bound; kept for macro_panel.xml).
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, status_buf_, "macros_status",
                                  subjects_);
        // Scalar subjects that drive the reactive repeat + edit-mode chrome.
        // Registered BEFORE the XML is created (subject-init-order rule). The
        // five per-row pools self-manage their own subject lifetime and are NOT
        // registered here.
        UI_MANAGED_SUBJECT_INT(macro_row_count_, 0, "macro_row_count", subjects_);
        UI_MANAGED_SUBJECT_INT(macro_edit_mode_, 0, "macro_edit_mode", subjects_);
        UI_MANAGED_SUBJECT_INT(macros_edit_save_hidden_, 1, "macros_edit_save_hidden", subjects_);

        // Self-register cleanup so subjects deinit before lv_deinit().
        // Peek instead of calling get_global_macros_panel(): this
        // callback runs from StaticSubjectRegistry::deinit_all(), which is
        // sequenced AFTER StaticPanelRegistry::destroy_all() has already
        // destroyed the panel. The auto-creating getter would build a
        // replacement whose destructor then runs during static destruction,
        // with LVGL and spdlog already gone.
        StaticSubjectRegistry::instance().register_deinit("MacrosPanel", []() {
            if (auto* panel = helix::lazy_global_if_exists<MacrosPanel>()) {
                panel->deinit_subjects();
            }
        });
    }
}

void MacrosPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void MacrosPanel::register_callbacks() {
    // Row identity comes from the event_cb user_data ("$row_index" string).
    register_xml_callbacks({
        {"on_macro_row_clicked",
         [](lv_event_t* e) {
             if (auto i = helix::ui::event_user_int(e))
                 get_global_macros_panel().handle_row_clicked(static_cast<size_t>(*i));
         }},
        {"on_macro_card_long_press",
         [](lv_event_t*) { get_global_macros_panel().handle_long_press(); }},
        {"on_macro_defaults_clicked",
         [](lv_event_t* e) {
             if (auto i = helix::ui::event_user_int(e))
                 get_global_macros_panel().handle_defaults_clicked(static_cast<size_t>(*i));
         }},
        {"on_macros_edit_save",
         [](lv_event_t*) { get_global_macros_panel().exit_edit_mode(true); }},
        {"on_macros_back_clicked",
         [](lv_event_t*) {
             auto& self = get_global_macros_panel();
             if (self.edit_mode_) {
                 self.exit_edit_mode(false); // discard pending changes, stay on panel
             } else {
                 helix::nav::go_back(); // normal Back: close the overlay
             }
         }},
    });
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* MacrosPanel::create(lv_obj_t* parent) {
    // Reset the row count BEFORE building the XML so the freshly-created
    // <repeat> starts at zero rows. on_ui_destroyed() reclaims the pools
    // (unregistering macro_name_<i> etc.), so a stale non-zero count would let
    // the repeat build rows bound to now-unregistered subjects; starting at 0
    // and then setting the real count in rebuild_rows() forces a clean build.
    lv_subject_set_int(&macro_row_count_, 0);

    helix::LapLog laps("MacrosPanel");
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    laps.lap("create overlay");
    ui_alive_ = true;

    // Cache the scrollable rows container so edit-mode transitions can reset
    // scroll position (see enter_edit_mode()/exit_edit_mode()).
    scroll_container_ = find_required(overlay_root_, "macro_list", get_name());

    // Rebuild reactively as macros arrive. When opened at startup (e.g.
    // `--test -p macros`) the panel is created before the queued
    // `api->hardware() = snapshot` runs, so macros() is momentarily empty.
    // nav_buttons_enabled flips to 1 only after hardware is populated on the
    // main thread, so observing it re-runs rebuild_rows() once real macros
    // exist (this also covers reconnect / printer switch).
    nav_enabled_observer_ = helix::ui::observe<int>(
        get_printer_state().network_state().get_nav_buttons_enabled_subject(), this,
        [](MacrosPanel* self, int) {
            // Macros may have just been populated; rebuild only if they changed.
            if (self->refresh_macros()) {
                self->rebuild_rows();
            }
        },
        get_printer_state().get_subjects_lifetime());

    refresh_macros();
    rebuild_rows();
    laps.lap("build rows");

    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void MacrosPanel::on_activate() {
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // create() built the rows for this open (the panel is destroyed on close), so only
    // macros discovered since then need a rebuild. Deferred (#80): on_activate() fires
    // inside overlay_slide_out_complete_cb() while LVGL is still in the animation tick.
    lifetime_.defer("MacrosPanel::rebuild", [this]() {
        if (refresh_macros()) {
            rebuild_rows();
        }
    });
}

void MacrosPanel::on_deactivating(DeactivateReason) {
    spdlog::debug("[{}] on_deactivating()", get_name());

    // Leaving the panel (back button / nav-away) discards any unsaved edit-mode
    // changes: only the header Save persists pending_hidden_.
    if (edit_mode_) {
        exit_edit_mode(false);
    }

    // Call base class (invalidates lifetime_)
}

void MacrosPanel::on_ui_destroyed() {
    // overlay_root_ and all its children have been async-deleted. Drop the
    // discovery observer and reclaim the five row pools so their name-registered
    // subjects are unregistered + freed while LVGL is still live (reclaim runs
    // synchronously here, before the async row deletion tick).
    ui_alive_ = false;
    scroll_container_ = nullptr;
    nav_enabled_observer_.reset();

    name_pool_.reclaim();
    desc_pool_.reclaim();
    visible_pool_.reclaim();
    desc_hidden_pool_.reclaim();
    chevron_hidden_pool_.reclaim();
    defaults_hidden_pool_.reclaim();
}

// ============================================================================
// Row model
// ============================================================================

bool MacrosPanel::refresh_macros() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        // No API (early boot, or a unit test that pre-set all_macros_). Leave
        // the current list intact rather than clobbering it to empty.
        return false;
    }
    const auto& macros = api->hardware().macros();
    std::vector<std::string> fresh(macros.begin(), macros.end());
    std::sort(fresh.begin(), fresh.end());
    if (fresh == all_macros_) {
        return false;
    }
    all_macros_ = std::move(fresh);
    return true;
}

std::set<std::string> MacrosPanel::seed_default_hidden() const {
    auto& sm = helix::SettingsManager::instance();
    return helix::macros::compute_effective_hidden(all_macros_, sm.hidden_macros_key_exists(),
                                                   sm.get_hidden_macros());
}

void MacrosPanel::rebuild_rows() {
    if (!ui_alive_) {
        // A deferred/observer rebuild may fire after the overlay UI was torn
        // down (singleton outlives its widgets). The pools are reclaimed; do
        // not re-grow them for a repeat that no longer exists.
        spdlog::debug("[{}] rebuild_rows() skipped — overlay UI not alive", get_name());
        return;
    }

    // Edit mode shows ALL macros (incl. _*); normal mode filters out the
    // effective hidden set.
    const std::set<std::string> hidden = seed_default_hidden();
    displayed_ = edit_mode_ ? all_macros_ : helix::macros::filter_visible(all_macros_, hidden);

    const size_t n = displayed_.size();

    // Grow all six pools before setting any values (grow-only within session).
    name_pool_.ensure_size(n);
    desc_pool_.ensure_size(n);
    visible_pool_.ensure_size(n);
    desc_hidden_pool_.ensure_size(n);
    chevron_hidden_pool_.ensure_size(n);
    defaults_hidden_pool_.ensure_size(n);

    helix::LapLog laps("MacrosPanel");
    // Populate every pool BEFORE publishing the count, so the repeat binds to
    // already-populated subjects (no first-frame flash).
    for (size_t i = 0; i < n; ++i) {
        const std::string& macro = displayed_[i];
        std::string display_name = prettify_macro_name(macro);
        auto cached = helix::MacroParamCache::instance().get(macro);
        const bool has_desc = !cached.description.empty();
        const bool no_params = (cached.knowledge == helix::MacroParamKnowledge::KNOWN_NO_PARAMS);
        const bool has_params = (cached.knowledge == helix::MacroParamKnowledge::KNOWN_PARAMS);
        const bool is_hidden = pending_hidden_.count(macro) > 0;

        const auto rv = helix::macros::compute_row_values(edit_mode_, is_hidden, has_desc,
                                                          no_params, has_params);

        name_pool_.set_string(i, display_name);
        desc_pool_.set_string(i, cached.description);
        visible_pool_.set_int(i, rv.visible);
        desc_hidden_pool_.set_int(i, rv.desc_hidden);
        chevron_hidden_pool_.set_int(i, rv.chevron_hidden);
        defaults_hidden_pool_.set_int(i, rv.defaults_hidden);
    }

    laps.lap("fill row subjects");
    lv_subject_set_int(&macro_row_count_, static_cast<int>(n));
    laps.lap("create rows");

    spdlog::info("[{}] rebuild_rows: {} displayed ({} discovered, edit={})", get_name(), n,
                 all_macros_.size(), edit_mode_);
}

void MacrosPanel::enter_edit_mode() {
    if (edit_mode_) {
        return;
    }
    refresh_macros();
    pending_hidden_ = seed_default_hidden();
    edit_mode_ = true;
    lv_subject_set_int(&macro_edit_mode_, 1);
    lv_subject_set_int(&macros_edit_save_hidden_, 0); // show Save
    rebuild_rows();
    scroll_list_to_top();
    spdlog::info("[{}] Entered edit mode ({} hidden seeded)", get_name(), pending_hidden_.size());
}

void MacrosPanel::exit_edit_mode(bool save) {
    if (!edit_mode_) {
        return;
    }
    if (save) {
        helix::SettingsManager::instance().set_hidden_macros(
            std::vector<std::string>(pending_hidden_.begin(), pending_hidden_.end()));
        spdlog::info("[{}] Saved {} hidden macros", get_name(), pending_hidden_.size());
    }
    edit_mode_ = false;
    lv_subject_set_int(&macro_edit_mode_, 0);
    lv_subject_set_int(&macros_edit_save_hidden_, 1); // hide Save
    rebuild_rows();
    scroll_list_to_top();
}

void MacrosPanel::scroll_list_to_top() {
    // The row-count change on rebuild_rows() drives a <repeat> rebuild that
    // leaves the scrollable list scrolled to the bottom. Reset to top after
    // the mode-change rebuild. Rows are created synchronously when
    // macro_row_count is set, but layout may still be pending, so defer to
    // the next main-thread tick (lifetime_.defer is safe here — main thread,
    // `this`/singleton stays valid).
    if (scroll_container_) {
        lifetime_.defer("MacrosPanel::scroll_top", [this]() {
            if (scroll_container_) {
                lv_obj_scroll_to_y(scroll_container_, 0, LV_ANIM_OFF);
            }
        });
    }
}

void MacrosPanel::toggle_row(size_t display_index) {
    if (display_index >= displayed_.size()) {
        return;
    }
    const std::string& macro = displayed_[display_index];
    if (pending_hidden_.count(macro)) {
        pending_hidden_.erase(macro);
    } else {
        pending_hidden_.insert(macro);
    }
    // Reactive: flip only this row's visibility int (no full rebuild).
    visible_pool_.set_int(display_index, pending_hidden_.count(macro) ? 0 : 1);
}

std::string MacrosPanel::prettify_macro_name(const std::string& name) {
    return helix::get_display_name(name, helix::DeviceType::MACRO);
}

// ============================================================================
// Run path
// ============================================================================

void MacrosPanel::fetch_params_and_execute(const std::string& macro_name) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No IMoonrakerAPI available - cannot fetch params", get_name());
        return;
    }

    helix::MacroRunRequest req;
    req.dangerous = helix::is_dangerous_macro(macro_name, get_printer_state().get_discovery());
    req.confirm_plain_run =
        helix::SafetySettingsManager::instance().get_macro_require_confirmation();

    // For dangerous macros, show confirmation before doing anything else
    if (helix::decide_macro_run(helix::MacroParamCache::instance().get(macro_name), req).action ==
        helix::MacroRunAction::ConfirmDangerous) {
        spdlog::warn("[{}] Dangerous macro requested: {}", get_name(), macro_name);

        // Store pending macro name for the confirmation callbacks; on_dismiss
        // clears it too, so a backdrop tap or ESC cannot strand it.
        pending_dangerous_macro_ = macro_name;

        std::string msg = fmt::format(lv_tr("{} may cause unintended changes. Are you sure?"),
                                      prettify_macro_name(macro_name));
        helix::ui::ConfirmOptions opts;
        opts.on_cancel = [this] {
            pending_dangerous_macro_.clear();
            spdlog::debug("[MacrosPanel] Dangerous macro cancelled");
        };
        opts.on_dismiss = [this] { pending_dangerous_macro_.clear(); };
        opts.owner_token = lifetime_.token();
        helix::ui::modal_confirm(
            lv_tr("Run Dangerous Macro?"), msg.c_str(), ModalSeverity::Warning, lv_tr("Run"),
            [this] {
                std::string macro = pending_dangerous_macro_;
                pending_dangerous_macro_.clear();
                fetch_params_and_run(macro);
            },
            opts);
        return;
    }

    fetch_params_and_run(macro_name);
}

void MacrosPanel::fetch_params_and_run(const std::string& macro_name) {
    auto cached = helix::MacroParamCache::instance().get(macro_name);

    // Reached straight from a row tap, or from the dangerous-macro dialog's
    // Run button - where the dangerous flag recomputes true and suppresses the
    // plain-run confirm below, so the macro is never confirmed twice.
    helix::MacroRunRequest req;
    req.dangerous = helix::is_dangerous_macro(macro_name, get_printer_state().get_discovery());
    req.dangerous_confirmed = true;
    req.confirm_plain_run =
        helix::SafetySettingsManager::instance().get_macro_require_confirmation();

    // Saved defaults: ask off runs (or confirms a run) with the saved values;
    // ask on prefills the param modal with them. Either way a missing record
    // behaves exactly as before.
    const helix::MacroParamDefaultRecord defaults =
        helix::MacroParamDefaults::instance().get(macro_name);
    req.prompt_for_params = defaults.ask_for_params;
    req.saved_values = defaults.values;

    const helix::MacroRunDecision decision = helix::decide_macro_run(cached, req);

    if (decision.action == helix::MacroRunAction::Run) {
        execute_with_params(macro_name,
                            helix::macro_param_result_from_values(cached.params, decision.params));
        return;
    }

    if (decision.action == helix::MacroRunAction::ConfirmRun) {
        pending_run_macro_ = macro_name;
        const helix::MacroParamResult params =
            helix::macro_param_result_from_values(cached.params, decision.params);
        std::string msg = fmt::format(lv_tr("Run {}?"), prettify_macro_name(macro_name));
        helix::ui::ConfirmOptions opts;
        opts.on_cancel = [this] { pending_run_macro_.clear(); };
        opts.on_dismiss = [this] { pending_run_macro_.clear(); };
        opts.owner_token = lifetime_.token();
        helix::ui::modal_confirm(
            lv_tr("Run Macro?"), msg.c_str(), ModalSeverity::Info, lv_tr("Run"),
            [this, params] {
                std::string macro = pending_run_macro_;
                pending_run_macro_.clear();
                execute_with_params(macro, params);
            },
            opts);
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::warn("[{}] No active screen - cannot show param modal for '{}'", get_name(),
                     macro_name);
        return;
    }

    auto token = lifetime_.token();
    std::string name = macro_name;
    auto on_result = [this, token, name](const helix::MacroParamResult& result) {
        if (token.expired())
            return;
        execute_with_params(name, result);
    };

    if (decision.action == helix::MacroRunAction::Prompt) {
        param_modal_.show_for_macro(screen, macro_name, cached.params, on_result, decision.params);
    } else {
        param_modal_.show_for_unknown_params(screen, macro_name, on_result);
    }
}

void MacrosPanel::execute_with_params(const std::string& macro_name,
                                      const helix::MacroParamResult& result) {
    IMoonrakerAPI* api = get_moonraker_api();
    helix::execute_macro_gcode(api, macro_name, result, "[MacrosPanel]",
                               get_printer_state().get_discovery());
}

// ============================================================================
// Event Handlers
// ============================================================================

void MacrosPanel::handle_row_clicked(size_t i) {
    if (i >= displayed_.size()) {
        return;
    }
    if (edit_mode_) {
        toggle_row(i);
    } else {
        fetch_params_and_execute(displayed_[i]);
    }
}

void MacrosPanel::handle_long_press() {
    if (edit_mode_) {
        return;
    }
    // Minimal scroll-suppression: LVGL fires LONG_PRESSED on hold duration
    // alone, so a hold during a scroll drag would falsely enter edit mode.
    // The macro list has no arcs/sliders, so a scroll-object check is
    // sufficient (cf. HomePanel::should_suppress_edit_mode, which also
    // guards arc/slider drags - not needed here).
    lv_indev_t* indev = lv_indev_active();
    if (indev && lv_indev_get_scroll_obj(indev)) {
        return;
    }
    // Cancel the in-progress press so the row's click (run macro) does not
    // fire on release now that we're switching into edit mode.
    if (indev) {
        lv_indev_reset(indev, nullptr);
    }
    enter_edit_mode();
}

void MacrosPanel::handle_defaults_clicked(size_t i) {
    if (i >= displayed_.size()) {
        return;
    }
    const std::string& macro = displayed_[i];

    auto cached = helix::MacroParamCache::instance().get(macro);
    if (cached.knowledge != helix::MacroParamKnowledge::KNOWN_PARAMS) {
        // No declared parameter list to edit. The button is hidden for these
        // rows; this is the belt for a stale row index after a rebuild.
        spdlog::debug("[{}] No saved-defaults editor for '{}' (knowledge != KNOWN_PARAMS)",
                      get_name(), macro);
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::warn("[{}] No active screen for the defaults editor of '{}'", get_name(), macro);
        return;
    }

    auto token = lifetime_.token();
    std::string name = macro;
    param_modal_.show_for_defaults(screen, name, cached.params,
                                   helix::MacroParamDefaults::instance().get(name),
                                   [token, name](const helix::MacroParamDefaultRecord& record) {
                                       if (token.expired())
                                           return;
                                       helix::MacroParamDefaults::instance().set(name, record);
                                   });
}
