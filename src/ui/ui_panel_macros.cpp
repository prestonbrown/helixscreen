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
#include "ui_virtual_list.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lap_log.h"
#include "lvgl.h"
#include "macro_edit_logic.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "observer_factory.h"
#include "preprint_skip_wrappers.h"
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
#include <map>
#include <tuple>

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
        // per-slot pools self-manage their own subject lifetime and are NOT
        // registered here.
        UI_MANAGED_SUBJECT_INT(macro_row_count_, 0, "macro_row_count", subjects_);
        UI_MANAGED_SUBJECT_INT(macro_slot_count_, 0, "macro_slot_count", subjects_);
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
    // It names the row's slot; the slot says which macro it shows right now.
    register_xml_callbacks({
        {"on_macro_row_clicked",
         [](lv_event_t* e) {
             auto& self = get_global_macros_panel();
             if (auto i = helix::ui::event_user_int(e))
                 self.handle_row_clicked(self.item_in_slot(static_cast<size_t>(*i)));
         }},
        {"on_macro_card_long_press",
         [](lv_event_t* e) {
             // A hold that enters edit mode is not also a tap that runs the macro.
             if (get_global_macros_panel().handle_long_press())
                 helix::ui::end_press(e);
         }},
        {"on_macro_defaults_clicked",
         [](lv_event_t* e) {
             auto& self = get_global_macros_panel();
             if (auto i = helix::ui::event_user_int(e))
                 self.handle_defaults_clicked(self.item_in_slot(static_cast<size_t>(*i)));
         }},
        {"on_macro_list_scroll",
         [](lv_event_t*) { get_global_macros_panel().update_visible(false); }},
        {"on_macro_list_resized",
         [](lv_event_t*) {
             // Row heights follow the list width and the slot count its height, and
             // measuring needs a layout pass, which cannot nest inside the one
             // reporting this resize.
             auto& self = get_global_macros_panel();
             self.lifetime_.defer("MacrosPanel::relayout", [&self]() {
                 if (!self.scroll_container_) {
                     return;
                 }
                 if (lv_obj_get_width(self.scroll_container_) != self.measured_width_ ||
                     lv_obj_get_content_height(self.scroll_container_) !=
                         self.measured_viewport_h_) {
                     self.layout_rows();
                 } else {
                     self.update_visible(false);
                 }
             });
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
    // Reset the slot count BEFORE building the XML so the freshly-created
    // <repeat> starts at zero rows. on_ui_destroyed() reclaims the pools
    // (unregistering macro_name_<i> etc.), so a stale non-zero count would let
    // the repeat build rows bound to now-unregistered subjects; starting at 0
    // and growing it in layout_rows() forces a clean build.
    lv_subject_set_int(&macro_row_count_, 0);
    lv_subject_set_int(&macro_slot_count_, 0);
    slots_.clear();
    slot_items_.clear();
    row_tops_.clear();
    shown_first_ = shown_last_ = -1;
    last_leading_ = last_trailing_ = -1;
    measured_width_ = measured_viewport_h_ = -1;

    helix::LapLog laps("MacrosPanel");
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    laps.lap("create overlay");
    ui_alive_ = true;

    scroll_container_ = find_required(overlay_root_, "macro_list", get_name());
    rows_container_ = find_required(overlay_root_, "rows_container", get_name());
    leading_spacer_ = find_required(overlay_root_, "leading_spacer", get_name());
    trailing_spacer_ = find_required(overlay_root_, "trailing_spacer", get_name());

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
    // discovery observer and reclaim the row slot pools so their name-registered
    // subjects are unregistered + freed while LVGL is still live (reclaim runs
    // synchronously here, before the async row deletion tick).
    ui_alive_ = false;
    scroll_container_ = nullptr;
    rows_container_ = nullptr;
    leading_spacer_ = nullptr;
    trailing_spacer_ = nullptr;
    slots_.clear();
    slot_items_.clear();
    nav_enabled_observer_.reset();

    name_pool_.reclaim();
    desc_pool_.reclaim();
    visible_pool_.reclaim();
    desc_hidden_pool_.reclaim();
    chevron_hidden_pool_.reclaim();
    defaults_hidden_pool_.reclaim();
    slot_hidden_pool_.reclaim();
}

// ============================================================================
// Row model
// ============================================================================

bool MacrosPanel::refresh_macros() {
    helix::IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        // No API (early boot, or a unit test that pre-set all_macros_). Leave
        // the current list intact rather than clobbering it to empty.
        return false;
    }
    const auto& hw = api->hardware();
    std::vector<std::string> fresh = helix::macros::panel_macros(
        hw.macros(), helix::skip_wrappers::wrapped_commands(hw.skip_active()));
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

    lv_subject_set_int(&macro_row_count_, static_cast<int>(displayed_.size()));
    layout_rows();

    spdlog::info("[{}] rebuild_rows: {} displayed ({} discovered, edit={})", get_name(),
                 displayed_.size(), all_macros_.size(), edit_mode_);
}

helix::macros::RowValues MacrosPanel::row_values(size_t item) const {
    const std::string& macro = displayed_[item];
    const auto cached = helix::MacroParamCache::instance().get(macro);
    return helix::macros::compute_row_values(
        edit_mode_, pending_hidden_.count(macro) > 0, !cached.description.empty(),
        cached.knowledge == helix::MacroParamKnowledge::KNOWN_NO_PARAMS,
        helix::macro_takes_saved_defaults(cached.knowledge));
}

void MacrosPanel::fill_slot(size_t slot, size_t item) {
    const auto rv = row_values(item);
    name_pool_.set_string(slot, prettify_macro_name(displayed_[item]));
    desc_pool_.set_string(slot,
                          helix::MacroParamCache::instance().get(displayed_[item]).description);
    visible_pool_.set_int(slot, rv.visible);
    desc_hidden_pool_.set_int(slot, rv.desc_hidden);
    chevron_hidden_pool_.set_int(slot, rv.chevron_hidden);
    defaults_hidden_pool_.set_int(slot, rv.defaults_hidden);
    slot_hidden_pool_.set_int(slot, 0);
}

size_t MacrosPanel::item_in_slot(size_t slot) const {
    if (slot >= slot_items_.size() || slot_items_[slot] < 0) {
        return SIZE_MAX;
    }
    return static_cast<size_t>(slot_items_[slot]);
}

void MacrosPanel::layout_rows() {
    if (!rows_container_) {
        return;
    }
    // Every slot is about to be rebound, so a press held on one would release onto
    // a different macro.
    helix::ui::reset_input_within(rows_container_);

    helix::LapLog laps("MacrosPanel");
    const size_t n = displayed_.size();
    const auto grow_slots = [this](size_t count) {
        name_pool_.ensure_size(count);
        desc_pool_.ensure_size(count);
        visible_pool_.ensure_size(count);
        desc_hidden_pool_.ensure_size(count);
        chevron_hidden_pool_.ensure_size(count);
        defaults_hidden_pool_.ensure_size(count);
        slot_hidden_pool_.ensure_size(count);
        for (size_t s = slots_.size(); s < count; s++) {
            slot_hidden_pool_.set_int(s, 1);
        }
        // The repeat rebuilds every slot for the new count.
        lv_subject_set_int(&macro_slot_count_, static_cast<int>(count));
        slots_.clear();
        for (size_t s = 0; s < count; s++) {
            slots_.push_back(find_required(rows_container_, fmt::format("macro_slot_{}", s).c_str(),
                                           get_name()));
        }
        slot_items_.assign(count, -1);
    };
    if (n > 0 && slots_.empty()) {
        grow_slots(1);
    }

    // A row's height follows only what it shows besides its one-line name, so rows
    // sharing a description and the same optional parts are measured once.
    const int gap = slots_.empty() ? 0 : lv_obj_get_style_margin_bottom(slots_[0], LV_PART_MAIN);
    std::map<std::tuple<std::string, int, int, int>, int> measured;
    row_tops_.assign(1, 0);
    int min_h = INT32_MAX, max_h = 0;
    for (size_t i = 0; i < n; i++) {
        const auto rv = row_values(i);
        auto key =
            std::make_tuple(helix::MacroParamCache::instance().get(displayed_[i]).description,
                            rv.desc_hidden, rv.defaults_hidden, rv.chevron_hidden);
        auto it = measured.find(key);
        if (it == measured.end()) {
            fill_slot(0, i);
            lv_obj_update_layout(slots_[0]);
            it = measured.emplace(std::move(key), lv_obj_get_height(slots_[0])).first;
        }
        min_h = std::min(min_h, it->second);
        max_h = std::max(max_h, it->second);
        row_tops_.push_back(row_tops_.back() + it->second + gap);
    }
    laps.lap("measure rows");

    if (n > 0) {
        // The most rows a window can hold: a viewport of the shortest rows below a
        // partly scrolled-off tallest one, plus the overscan at each end.
        const int viewport = lv_obj_get_content_height(scroll_container_);
        const size_t needed =
            std::min(n, static_cast<size_t>((viewport + max_h + gap) / std::max(1, min_h + gap) +
                                            1 + 2 * kBufferRows));
        if (needed > slots_.size()) {
            grow_slots(needed);
        }
    }
    std::fill(slot_items_.begin(), slot_items_.end(), -1);
    update_visible(true);
    // A list that shrank can leave the scroll position past its new end; clamp it and
    // show the rows that brings into view.
    lv_obj_update_layout(scroll_container_);
    lv_obj_readjust_scroll(scroll_container_, LV_ANIM_OFF);
    update_visible(false);
    measured_width_ = lv_obj_get_width(scroll_container_);
    measured_viewport_h_ = lv_obj_get_content_height(scroll_container_);
    laps.lap("show rows");
}

void MacrosPanel::update_visible(bool refill) {
    if (!rows_container_ || !scroll_container_) {
        return;
    }
    const auto w = helix::ui::compute_window(lv_obj_get_scroll_y(scroll_container_),
                                             lv_obj_get_content_height(scroll_container_),
                                             row_tops_, kBufferRows);
    if (!refill && w.first == shown_first_ && w.last == shown_last_) {
        return;
    }
    shown_first_ = w.first;
    shown_last_ = w.last;
    helix::ui::sync_list_spacers(rows_container_, leading_spacer_, trailing_spacer_, w,
                                 last_leading_, last_trailing_);
    helix::ui::show_window(
        rows_container_, slot_items_, w.first, w.last, refill,
        [this](size_t slot) { return slots_[slot]; },
        [this](size_t slot, ssize_t item) { fill_slot(slot, static_cast<size_t>(item)); },
        [this](size_t slot) { slot_hidden_pool_.set_int(slot, 1); });
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
    if (scroll_container_) {
        lv_obj_scroll_to_y(scroll_container_, 0, LV_ANIM_OFF);
        update_visible(false);
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
    // Reactive: flip only the visibility int of the slot showing it (no rebuild).
    for (size_t slot = 0; slot < slot_items_.size(); slot++) {
        if (slot_items_[slot] == static_cast<ssize_t>(display_index)) {
            visible_pool_.set_int(slot, row_values(display_index).visible);
        }
    }
}

std::string MacrosPanel::prettify_macro_name(const std::string& name) {
    return helix::get_display_name(name, helix::DeviceType::MACRO);
}

// ============================================================================
// Run path
// ============================================================================

void MacrosPanel::fetch_params_and_execute(const std::string& macro_name) {
    helix::IMoonrakerAPI* api = get_moonraker_api();
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
        param_modal_.show_for_unknown_params(screen, macro_name, on_result, decision.params);
    }
}

void MacrosPanel::execute_with_params(const std::string& macro_name,
                                      const helix::MacroParamResult& result) {
    helix::IMoonrakerAPI* api = get_moonraker_api();
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

bool MacrosPanel::handle_long_press() {
    if (edit_mode_) {
        return false;
    }
    // Minimal scroll-suppression: LVGL fires LONG_PRESSED on hold duration
    // alone, so a hold during a scroll drag would falsely enter edit mode.
    // The macro list has no arcs/sliders, so a scroll-object check is
    // sufficient (cf. HomePanel::should_suppress_edit_mode, which also
    // guards arc/slider drags - not needed here).
    lv_indev_t* indev = lv_indev_active();
    if (indev && lv_indev_get_scroll_obj(indev)) {
        return false;
    }
    enter_edit_mode();
    return true;
}

void MacrosPanel::handle_defaults_clicked(size_t i) {
    if (i >= displayed_.size()) {
        return;
    }
    const std::string& macro = displayed_[i];

    auto cached = helix::MacroParamCache::instance().get(macro);
    if (!helix::macro_takes_saved_defaults(cached.knowledge)) {
        // The macro takes no parameters. The button is hidden for these rows;
        // this is the belt for a stale row index after a rebuild.
        spdlog::debug("[{}] No saved-defaults editor for '{}' (no parameters)", get_name(), macro);
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
