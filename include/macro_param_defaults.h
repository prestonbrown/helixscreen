// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <string>

namespace helix {

/** One macro's saved parameter defaults plus its "Ask for parameters" toggle. */
struct MacroParamDefaultRecord {
    /// Saved KEY -> value strings. Keys match MacroParam's uppercase spelling;
    /// empty values mean "the macro's own default" and are never stored.
    std::map<std::string, std::string> values;
    /// False = run with `values` without prompting. True (or no record at all)
    /// = today's behavior: values prefill the prompt, they never skip it.
    bool ask_for_params = true;
};

/**
 * @brief Per-printer store of saved macro parameter defaults.
 *
 * Records live under the ACTIVE printer's settings subtree (Config::df() +
 * "macros/param_defaults"), keyed by the macro's lowercase name the same way
 * MacroParamCache keys its cache, so a printer switch never leaks another
 * printer's values. Missing record = today's behavior: empty values, ask on.
 *
 * An empty record that still asks is indistinguishable from no record and is
 * not stored at all.
 */
class MacroParamDefaults {
  public:
    static MacroParamDefaults& instance();

    MacroParamDefaultRecord get(const std::string& macro_name) const;
    void set(const std::string& macro_name, const MacroParamDefaultRecord& record);
    void clear(const std::string& macro_name);

  private:
    MacroParamDefaults() = default;
};

} // namespace helix
