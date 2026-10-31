// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "macro_param_defaults.h"
#include "macro_params.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace helix {

struct MacroParamModalTestAccess; // test-only friend (tests/test_helpers/)

/// The hint an empty field for @p param shows: its literal default, a translated
/// "Printer default" when the macro computes its own, or the parameter name when
/// it has no default.
[[nodiscard]] std::string macro_param_placeholder(const MacroParam& param);

/// Callback invoked when user confirms macro execution with parameters
using MacroExecuteCallback = std::function<void(const MacroParamResult& result)>;

/// Callback invoked when the user saves a macro's default parameters
using MacroParamSaveCallback = std::function<void(const MacroParamDefaultRecord& record)>;

/// Modal dialog that prompts for macro parameter values before execution.
/// Dynamically creates labeled textarea fields for each detected parameter.
class MacroParamModal : public Modal {
  public:
    MacroParamModal() = default;
    ~MacroParamModal() override;

    const char* get_name() const override {
        return "Macro Parameters";
    }
    const char* component_name() const override {
        return "macro_param_modal";
    }

    /// Show the modal for a specific macro with its detected parameters.
    /// @param parent Parent object (usually lv_screen_active())
    /// @param macro_name Display name for the subtitle
    /// @param params Detected parameters with defaults
    /// @param on_execute Called when user clicks Run with collected values
    /// @param prefill Values typed into the fields of the parameters they name. They
    ///        are sent on Run unless the user clears them.
    void show_for_macro(lv_obj_t* parent, const std::string& macro_name,
                        const std::vector<MacroParam>& params, MacroExecuteCallback on_execute,
                        const std::map<std::string, std::string>& prefill = {});

    /// Show the modal for a macro with unknown parameters (raw text input).
    /// @param parent Parent object (usually lv_screen_active())
    /// @param macro_name Display name for the subtitle
    /// @param on_execute Called when user clicks Run with parsed KEY=VALUE pairs
    void show_for_unknown_params(lv_obj_t* parent, const std::string& macro_name,
                                 MacroExecuteCallback on_execute);

    /// Show the modal in save mode: same field list as show_for_macro(),
    /// prefilled with @p record's values, titled "Default Parameters", primary
    /// button "Save", plus an "Ask for parameters" toggle below the fields.
    /// Saving hands the filled record to @p on_save; the macro is NOT run.
    /// KNOWN_PARAMS macros only - there is no field list to save otherwise.
    void show_for_defaults(lv_obj_t* parent, const std::string& macro_name,
                           const std::vector<MacroParam>& params,
                           const MacroParamDefaultRecord& record, MacroParamSaveCallback on_save);

    // Static callbacks for button wiring
    static void run_cb(lv_event_t* e);
    static void cancel_cb(lv_event_t* e);
    static void save_cb(lv_event_t* e);
    static void ask_toggled_cb(lv_event_t* e);

  protected:
    void on_show() override;
    void on_ok() override;
    void on_cancel() override;
    /// Every close, answered or not, drops the widget pointers and the static
    /// instance pointer: the widgets are about to be deleted.
    void on_hide() override;

  private:
    friend struct MacroParamModalTestAccess;

    std::string macro_name_;
    std::vector<MacroParam> params_;
    std::map<std::string, std::string> prefill_; ///< Initial field text, by parameter name
    MacroExecuteCallback on_execute_;
    MacroParamSaveCallback on_save_; ///< Save-mode primary action; runs nothing.
    /// textareas_[i] is params_[i]'s field, nullptr when it could not be built.
    std::vector<lv_obj_t*> textareas_;
    bool raw_mode_ = false;            ///< True when showing raw text input (UNKNOWN macros)
    lv_obj_t* raw_textarea_ = nullptr; ///< Textarea for raw param input
    bool save_mode_ = false;           ///< True when editing saved defaults, not running.

    void show_common(lv_obj_t* parent);
    void dismiss();
    void populate_param_fields();
    MacroParamResult collect_values() const;
    void on_save_clicked();

    static MacroParamModal* s_active_instance_;
};

} // namespace helix
