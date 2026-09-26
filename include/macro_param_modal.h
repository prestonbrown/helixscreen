// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "macro_param_defaults.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace helix {

struct MacroParamModalTestAccess; // test-only friend (tests/test_helpers/)

/// What a parameter's |default(...) filter holds.
enum class MacroDefaultKind {
    Absent,     ///< No |default(...) filter
    Literal,    ///< A number, a quoted string, or true/false/none: usable as the value itself
    Expression, ///< Evaluated by Klipper when the macro runs (a variable lookup, arithmetic)
};

/// Parsed macro parameter with optional default value
struct MacroParam {
    std::string name; ///< Parameter name (uppercase, e.g., "EXTRUDER_TEMP")
    /// Text inside |default(...): a literal with its quotes stripped, an expression
    /// verbatim. Empty if none.
    std::string default_value;
    bool is_variable = false; ///< True for Klipper variable_* fields (SET_GCODE_VARIABLE)
    MacroDefaultKind default_kind = MacroDefaultKind::Absent;
};

/// Parse macro parameters from a Klipper gcode_macro template.
/// Detects params.NAME, params['NAME'], params["NAME"] references and
/// extracts |default(VALUE) when present. Deduplicates by name.
[[nodiscard]] std::vector<MacroParam> parse_macro_params(const std::string& gcode_template);

/// The hint an empty field for @p param shows: its literal default, a translated
/// "Printer default" when the macro computes its own, or the parameter name when
/// it has no default.
[[nodiscard]] std::string macro_param_placeholder(const MacroParam& param);

/// Parse raw "KEY=VALUE KEY2=VALUE2" text into a parameter map.
/// Keys are uppercased to match Klipper convention.
[[nodiscard]] std::map<std::string, std::string>
parse_raw_macro_params(const std::string& raw_text);

/// Result from macro parameter modal: inline params and variable overrides
struct MacroParamResult {
    std::map<std::string, std::string> params;    ///< Inline params (MACRO KEY=VALUE)
    std::map<std::string, std::string> variables; ///< Variable overrides (SET_GCODE_VARIABLE)
};

/// Callback invoked when user confirms macro execution with parameters
using MacroExecuteCallback = std::function<void(const MacroParamResult& result)>;

/// Callback invoked when the user saves a macro's default parameters
using MacroParamSaveCallback = std::function<void(const MacroParamDefaultRecord& record)>;

/// Modal dialog that prompts for macro parameter values before execution.
/// Dynamically creates labeled textarea fields for each detected parameter.
class MacroParamModal : public Modal {
  public:
    MacroParamModal() = default;
    ~MacroParamModal() override = default;

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
