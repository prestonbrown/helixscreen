// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_modal.h"

#include "ui_event_safety.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

using namespace helix;

std::string helix::macro_param_placeholder(const MacroParam& param) {
    if (param.default_kind == MacroDefaultKind::Expression) {
        // The value exists only on the printer, and the template source that
        // computes it is not something the user could type back in.
        return lv_tr("Printer default");
    }
    return param.default_value.empty() ? param.name : param.default_value;
}

MacroParamModal* MacroParamModal::s_active_instance_ = nullptr;

namespace {

/// Save mode (1) re-titles the dialog, swaps Run for Save and reveals the
/// "Ask for parameters" toggle; run mode (0) hides all of that.
lv_subject_t s_save_mode_subject{};
/// The "Ask for parameters" toggle: 1 = prompt with these values prefilled,
/// 0 = run with them without asking.
lv_subject_t s_ask_subject{};
bool s_modal_subjects_registered = false;
SubjectManager s_subjects;

/// XML subjects for the modal's save mode. Idempotent; deinit is self-registered
/// with StaticSubjectRegistry so a rebuilt component scope never outlives them.
void register_modal_subjects() {
    if (s_modal_subjects_registered)
        return;
    lv_subject_init_int(&s_save_mode_subject, 0);
    lv_subject_init_int(&s_ask_subject, 1);
    s_subjects.publish("macro_param_modal_save_mode", &s_save_mode_subject);
    s_subjects.publish("macro_param_modal_ask", &s_ask_subject);
    StaticSubjectRegistry::instance().register_deinit("MacroParamModalSubjects", []() {
        s_subjects.deinit_all();
        s_modal_subjects_registered = false;
    });
    s_modal_subjects_registered = true;
}

} // namespace

void MacroParamModal::show_for_macro(lv_obj_t* parent, const std::string& macro_name,
                                     const std::vector<MacroParam>& params,
                                     MacroExecuteCallback on_execute,
                                     const std::map<std::string, std::string>& prefill) {
    macro_name_ = macro_name;
    params_ = params;
    prefill_ = prefill;
    on_execute_ = std::move(on_execute);
    raw_mode_ = false;
    save_mode_ = false;
    register_modal_subjects();
    lv_subject_set_int(&s_save_mode_subject, 0);
    show_common(parent);
}

void MacroParamModal::show_for_unknown_params(lv_obj_t* parent, const std::string& macro_name,
                                              MacroExecuteCallback on_execute) {
    macro_name_ = macro_name;
    params_.clear();
    prefill_.clear();
    on_execute_ = std::move(on_execute);
    raw_mode_ = true;
    save_mode_ = false;
    register_modal_subjects();
    lv_subject_set_int(&s_save_mode_subject, 0);
    show_common(parent);
}

void MacroParamModal::show_for_defaults(lv_obj_t* parent, const std::string& macro_name,
                                        const std::vector<MacroParam>& params,
                                        const MacroParamDefaultRecord& record,
                                        MacroParamSaveCallback on_save) {
    macro_name_ = macro_name;
    params_ = params;
    prefill_ = record.values;
    on_execute_ = nullptr;
    on_save_ = std::move(on_save);
    raw_mode_ = false;
    save_mode_ = true;
    register_modal_subjects();
    lv_subject_set_int(&s_save_mode_subject, 1);
    lv_subject_set_int(&s_ask_subject, record.ask_for_params ? 1 : 0);
    show_common(parent);
}

void MacroParamModal::show_common(lv_obj_t* parent) {
    textareas_.clear();
    raw_textarea_ = nullptr;

    // Register callbacks before showing (idempotent)
    lv_xml_register_event_cb(nullptr, "macro_param_modal_run_cb", MacroParamModal::run_cb);
    lv_xml_register_event_cb(nullptr, "macro_param_modal_cancel_cb", MacroParamModal::cancel_cb);
    lv_xml_register_event_cb(nullptr, "macro_param_modal_save_cb", MacroParamModal::save_cb);
    lv_xml_register_event_cb(nullptr, "macro_param_modal_ask_cb", MacroParamModal::ask_toggled_cb);

    if (!show(parent)) {
        spdlog::error("[MacroParamModal] Failed to show modal{}", raw_mode_ ? " (raw mode)" : "");
        raw_mode_ = false;
        return;
    }

    s_active_instance_ = this;
}

void MacroParamModal::on_show() {
    // Set subtitle to macro name
    lv_obj_t* subtitle = find_widget("modal_subtitle");
    if (subtitle) {
        lv_label_set_text(subtitle, macro_name_.c_str());
    }

    populate_param_fields();
}

void MacroParamModal::on_ok() {
    if (save_mode_) {
        on_save_clicked();
        return;
    }
    if (on_execute_) {
        on_execute_(collect_values());
    }
    dismiss();
}

void MacroParamModal::on_save_clicked() {
    if (on_save_) {
        // Every field the user filled becomes a saved value, whatever side of
        // the params/variables split it would run on; the run path re-splits
        // by the declared shapes when it sends them.
        const MacroParamResult collected = collect_values();
        MacroParamDefaultRecord record;
        record.values = collected.params;
        record.values.insert(collected.variables.begin(), collected.variables.end());
        record.ask_for_params = lv_subject_get_int(&s_ask_subject) != 0;
        on_save_(record);
    }
    dismiss();
}

void MacroParamModal::on_cancel() {
    dismiss();
}

void MacroParamModal::dismiss() {
    raw_mode_ = false;
    raw_textarea_ = nullptr;
    textareas_.clear(); // Clear before hide() -- widgets are about to be deleted
    s_active_instance_ = nullptr;
    hide();
}

void MacroParamModal::populate_param_fields() {
    lv_obj_t* param_list = find_widget("param_list");
    if (!param_list) {
        spdlog::error("[MacroParamModal] param_list container not found");
        return;
    }

    textareas_.clear();

    if (raw_mode_) {
        const char* attrs[] = {"label",       lv_tr("Parameters"),
                               "placeholder", lv_tr("e.g. NAME=my_var VALUE=123"),
                               nullptr,       nullptr};
        lv_obj_t* field = static_cast<lv_obj_t*>(lv_xml_create(param_list, "form_field", attrs));
        if (field) {
            raw_textarea_ = lv_obj_find_by_name(field, "field_input");
        }
        spdlog::debug("[MacroParamModal] Created raw param field for {}", macro_name_);
        return;
    }

    for (const auto& param : params_) {
        // Prettify: lowercase with first letter capitalized
        std::string display_name = param.name;
        display_name = helix::text_io::to_lower(display_name);
        if (!display_name.empty()) {
            display_name[0] = static_cast<char>(::toupper(display_name[0]));
        }

        // The hint an empty field shows; an empty field leaves the macro its own default.
        const std::string placeholder = macro_param_placeholder(param);

        // Create form_field component (label + themed text_input with keyboard wiring)
        const char* attrs[] = {
            "label", display_name.c_str(), "placeholder", placeholder.c_str(), nullptr, nullptr};
        lv_obj_t* field = static_cast<lv_obj_t*>(lv_xml_create(param_list, "form_field", attrs));
        if (!field) {
            spdlog::warn("[MacroParamModal] Failed to create form_field for {}", param.name);
            // An empty slot keeps every later field at its parameter's index;
            // collect_values() skips it.
            textareas_.push_back(nullptr);
            continue;
        }

        lv_obj_t* textarea = lv_obj_find_by_name(field, "field_input");
        // A prefill is the field's text, so Run sends it unless the user clears it.
        if (auto it = prefill_.find(param.name); textarea && it != prefill_.end()) {
            lv_textarea_set_text(textarea, it->second.c_str());
        }

        textareas_.push_back(textarea);
    }

    spdlog::debug("[MacroParamModal] Created {} param fields for {}", params_.size(), macro_name_);
}

MacroParamResult MacroParamModal::collect_values() const {
    MacroParamResult result;

    if (raw_mode_ && raw_textarea_) {
        const char* text = lv_textarea_get_text(raw_textarea_);
        if (text && text[0] != '\0') {
            result.params = parse_raw_macro_params(text);
        }
        return result;
    }

    for (size_t i = 0; i < params_.size() && i < textareas_.size(); ++i) {
        if (!textareas_[i]) {
            continue;
        }
        const char* text = lv_textarea_get_text(textareas_[i]);
        if (text && text[0] != '\0') {
            if (params_[i].is_variable) {
                result.variables[params_[i].name] = text;
            } else {
                result.params[params_[i].name] = text;
            }
        }
    }

    return result;
}

// Static callbacks
void MacroParamModal::run_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroParamModal] run_cb");
    (void)e;
    if (s_active_instance_) {
        s_active_instance_->on_ok();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void MacroParamModal::cancel_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroParamModal] cancel_cb");
    (void)e;
    if (s_active_instance_) {
        s_active_instance_->on_cancel();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void MacroParamModal::save_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroParamModal] save_cb");
    (void)e;
    if (s_active_instance_) {
        s_active_instance_->on_save_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void MacroParamModal::ask_toggled_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroParamModal] ask_toggled_cb");
    lv_obj_t* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (toggle) {
        lv_subject_set_int(&s_ask_subject, lv_obj_has_state(toggle, LV_STATE_CHECKED) ? 1 : 0);
    }
    LVGL_SAFE_EVENT_CB_END();
}
