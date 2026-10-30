// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quick_action_buttons.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "format_utils.h"
#include "led/led_controller.h"
#include "lvgl.h"
#include "macro_executor.h"
#include "macro_param_defaults.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "panel_widgets/led_widget.h"
#include "printer_state.h"
#include "safety_settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix {

using helix::ui::observe;

QuickActionButtons::QuickActionButtons() = default;

QuickActionButtons::~QuickActionButtons() {
    release_widgets();
}

void QuickActionButtons::init_subjects(SubjectManager& subjects) {
    // Literal names, one per slot: the registry keys on the string it is given.
    UI_MANAGED_SUBJECT_INT(visible_[0], 0, "macro_1_visible", subjects);
    UI_MANAGED_SUBJECT_INT(visible_[1], 0, "macro_2_visible", subjects);
    UI_MANAGED_SUBJECT_INT(visible_[2], 0, "macro_3_visible", subjects);
    UI_MANAGED_SUBJECT_INT(visible_[3], 0, "macro_4_visible", subjects);
    UI_MANAGED_SUBJECT_INT(available_[0], 0, "macro_1_available", subjects);
    UI_MANAGED_SUBJECT_INT(available_[1], 0, "macro_2_available", subjects);
    UI_MANAGED_SUBJECT_INT(available_[2], 0, "macro_3_available", subjects);
    UI_MANAGED_SUBJECT_INT(available_[3], 0, "macro_4_available", subjects);
    UI_MANAGED_SUBJECT_STRING_N(name_[0], name_buf_[0].data(), name_buf_[0].size(), "",
                                "macro_1_name", subjects);
    UI_MANAGED_SUBJECT_STRING_N(name_[1], name_buf_[1].data(), name_buf_[1].size(), "",
                                "macro_2_name", subjects);
    UI_MANAGED_SUBJECT_STRING_N(name_[2], name_buf_[2].data(), name_buf_[2].size(), "",
                                "macro_3_name", subjects);
    UI_MANAGED_SUBJECT_STRING_N(name_[3], name_buf_[3].data(), name_buf_[3].size(), "",
                                "macro_4_name", subjects);
    UI_MANAGED_SUBJECT_INT(light_[0], 0, "macro_1_light", subjects);
    UI_MANAGED_SUBJECT_INT(light_[1], 0, "macro_2_light", subjects);
    UI_MANAGED_SUBJECT_INT(light_[2], 0, "macro_3_light", subjects);
    UI_MANAGED_SUBJECT_INT(light_[3], 0, "macro_4_light", subjects);
    UI_MANAGED_SUBJECT_INT(header_visible_, 1, "macro_header_visible", subjects);
}

void QuickActionButtons::setup(lv_obj_t* panel, lv_obj_t* parent_screen,
                               PrinterState& printer_state, IMoonrakerAPI* api) {
    reload();

    // attach() finds light_icon inside the cell by name; the XML wires the click.
    for (size_t i = 0; i < led_widgets_.size(); ++i) {
        const std::string slot = "macro_" + std::to_string(i + 1);
        const std::string cell = slot + "_light_cell";
        if (lv_obj_t* led_cell = lv_obj_find_by_name(panel, cell.c_str())) {
            led_widgets_[i] = std::make_unique<LedWidget>("controls_" + slot, printer_state, api);
            led_widgets_[i]->attach_tile(led_cell, parent_screen);
        }
    }

    // Which macros a printer defines is not fixed for the life of a session: a
    // Klipper restart or a config change re-runs discovery, and StandardMacros
    // re-resolves every slot against the new list. Sampling once left a button
    // enabled for a macro that had gone away (and hidden for one that had
    // arrived) until the panel next deactivated.
    macros_version_observer_ = observe<int>(
        StandardMacros::instance().get_macros_version_subject(), this,
        [](QuickActionButtons* self, int /* version */) { self->refresh(); },
        StandardMacros::instance().get_subjects_lifetime());

    // The light toggle's slot depends on whether an LED is controllable, which
    // discovery settles after setup.
    led_controllable_observer_ = observe<int>(
        led::LedController::instance().get_led_controllable_subject(), this,
        [](QuickActionButtons* self, int /* controllable */) { self->refresh(); },
        led::LedController::instance().get_subjects_lifetime());
}

void QuickActionButtons::release_widgets() {
    // Detach while LVGL is still valid (the dtor of each calls detach(), but do
    // it explicitly first so observers go before subjects).
    for (auto& w : led_widgets_) {
        w.reset();
    }
}

void QuickActionButtons::reload() {
    load_config();
    refresh();
}

void QuickActionButtons::load_config() {
    stored_ = read_stored_quick_slots();
    for (size_t i = 0; i < slots_.size(); ++i) {
        const std::string& name = stored_.value[i];
        slots_[i] = name.empty() || name == kQuickSlotLight ? std::nullopt
                                                            : StandardMacros::slot_from_name(name);
    }
}

void QuickActionButtons::update_button(StandardMacros& macros,
                                       const std::optional<StandardMacroSlot>& slot, size_t index) {
    lv_subject_t& visible = visible_[index];
    lv_subject_t& available = available_[index];
    const int button_num = static_cast<int>(index + 1);

    if (!slot) {
        lv_subject_set_int(&visible, 0);
        lv_subject_set_int(&available, 0);
        return;
    }

    const auto& info = macros.get(*slot);

    if (!info.is_empty()) {
        lv_subject_set_int(&visible, 1);
        lv_subject_set_int(&available, 1);
        lv_subject_copy_string(&name_[index], info.translated_name());
        spdlog::trace("[QuickActionButtons] Macro {}: '{}' → {}", button_num, info.display_name,
                      info.get_macro());
        return;
    }

    if (info.has_missing_macro()) {
        // The user assigned this slot and the printer does not answer for it (a
        // preset can seed a macro name the machine never defined, and a Klipper
        // config change can retire one). Keep the button where they left it and
        // grey it out: a button that disappears reads as a bug in the screen,
        // while a disabled one points at the assignment that needs fixing.
        lv_subject_set_int(&visible, 1);
        lv_subject_set_int(&available, 0);
        lv_subject_copy_string(&name_[index], info.translated_name());
        spdlog::debug("[QuickActionButtons] Macro {} slot '{}' disabled: '{}' is not defined on "
                      "this printer",
                      button_num, info.slot_name, info.missing_macro);
        return;
    }

    lv_subject_set_int(&visible, 0);
    lv_subject_set_int(&available, 0);
    spdlog::trace("[QuickActionButtons] Macro {} slot '{}' is empty, hiding button", button_num,
                  info.slot_name);
}

void QuickActionButtons::refresh() {
    auto& macros = StandardMacros::instance();
    const auto kinds = resolve_current_quick_slots(stored_);

    for (size_t i = 0; i < slots_.size(); ++i) {
        const bool light = kinds[i] == QuickSlotKind::Light;
        lv_subject_set_int(&light_[i], light ? 1 : 0);
        if (kinds[i] == QuickSlotKind::Macro) {
            update_button(macros, slots_[i], i);
        } else {
            lv_subject_set_int(&visible_[i], 0);
            lv_subject_set_int(&available_[i], 0);
        }
    }

    // Hide the Quick Actions header when row 2 shows anything, to save space
    const bool row2_visible = kinds[2] != QuickSlotKind::Empty || kinds[3] != QuickSlotKind::Empty;
    lv_subject_set_int(&header_visible_, row2_visible ? 0 : 1);
}

void QuickActionButtons::execute(size_t index, IMoonrakerAPI* api, const LifetimeToken& owner) {
    if (index >= slots_.size()) {
        spdlog::warn("[QuickActionButtons] Invalid macro index: {}", index);
        return;
    }

    const auto& slot = slots_[index];
    if (!slot) {
        spdlog::debug("[QuickActionButtons] Macro {} clicked but no slot configured",
                      static_cast<int>(index + 1));
        return;
    }

    // Backstop for the XML disabled binding. LVGL suppresses CLICKED on a
    // LV_STATE_DISABLED object, so this normally cannot be reached from touch —
    // but execute() is also the entry point for the remote-control server
    // and any future caller, and dispatching a macro the printer does not define
    // is exactly the silent failure this gate exists to stop.
    const auto& gate_info = StandardMacros::instance().get(*slot);
    if (gate_info.is_empty() && gate_info.has_missing_macro()) {
        spdlog::warn("[QuickActionButtons] Macro {} slot '{}' names '{}', which this printer does "
                     "not define",
                     static_cast<int>(index + 1), gate_info.slot_name, gate_info.missing_macro);
        NOTIFY_WARNING(lv_tr("{} is not set up on this printer"), gate_info.translated_name());
        return;
    }

    // Quick buttons never raise the param modal, so the decision weighs the
    // Safety setting and the macro's saved defaults: a saved record rides along
    // on the run, filtered to the declared names.
    const auto& info = StandardMacros::instance().get(*slot);
    const CachedMacroInfo cached = MacroParamCache::instance().get(info.get_macro());
    MacroRunRequest run_req;
    run_req.prompt_for_params = false;
    run_req.confirm_plain_run = SafetySettingsManager::instance().get_macro_require_confirmation();
    run_req.saved_values = MacroParamDefaults::instance().get(info.get_macro()).values;
    const MacroRunDecision decision = decide_macro_run(cached, run_req);
    if (decision.action != MacroRunAction::ConfirmRun) {
        run(index, api, decision.params);
        return;
    }

    std::string msg = fmt::format(lv_tr("Run {}?"), info.translated_name());
    ui::ConfirmOptions opts;
    opts.on_dismiss = [this] { run_confirmation_dialog_.release(); };
    opts.owner_token = owner;
    run_confirmation_dialog_ = ui::modal_confirm(
        lv_tr("Run Macro?"), msg.c_str(), ModalSeverity::Info, lv_tr("Run"),
        [this, index, api, params = decision.params] {
            run_confirmation_dialog_.release(); // the dialog closes itself
            run(index, api, params);
        },
        opts);
}

void QuickActionButtons::run(size_t index, IMoonrakerAPI* api,
                             const std::map<std::string, std::string>& params) {
    if (index >= slots_.size() || !slots_[index]) {
        return;
    }
    const auto& slot = *slots_[index];

    const auto& info = StandardMacros::instance().get(slot);
    spdlog::debug("[QuickActionButtons] Macro {} clicked, executing slot '{}' → {}", index + 1,
                  info.slot_name, info.get_macro());

    NOTIFY_INFO(lv_tr("Running {}..."), info.translated_name());
    if (!StandardMacros::instance().execute(
            slot, api, params,
            [name = std::string(info.translated_name())]() {
                NOTIFY_SUCCESS(lv_tr("{} complete"), name);
            },
            [](const MoonrakerError& err) {
                helix::ui::notify_error_tr(TR_NOOP("Macro failed: {}"), err);
            })) {
        NOTIFY_WARNING(lv_tr("{} macro not configured"), info.translated_name());
    }
}

} // namespace helix
