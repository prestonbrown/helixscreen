// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_defaults.h"

#include "config.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {

/// Settings leaf holding every record, keyed by lowercase macro name.
/// Config::df() scopes it to the active printer.
std::string store_leaf() {
    return Config::get_instance()->df() + "macros/param_defaults";
}

/// Lowercase key for a macro name, matching MacroParamCache's normalisation.
std::string store_key(const std::string& macro_name) {
    std::string key = macro_name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return key;
}

/// Read the whole record table. Malformed data reads as absent, never throws:
/// one hand-edited settings.json must not take macro running down.
json read_record_table() {
    try {
        return Config::get_instance()->get<json>(store_leaf(), json::object());
    } catch (const std::exception& e) {
        spdlog::warn("[MacroParamDefaults] {} malformed, ignoring: {}", store_leaf(), e.what());
        return json::object();
    }
}

void write_record_table(const json& table) {
    Config* config = Config::get_instance();
    config->set<json>(store_leaf(), table);
    config->save();
}

} // namespace

MacroParamDefaults& MacroParamDefaults::instance() {
    static MacroParamDefaults inst;
    return inst;
}

MacroParamDefaultRecord MacroParamDefaults::get(const std::string& macro_name) const {
    MacroParamDefaultRecord record;
    const json node = read_record_table().value(store_key(macro_name), json());
    if (!node.is_object()) {
        return record;
    }
    if (node.contains("values") && node["values"].is_object()) {
        for (auto it = node["values"].begin(); it != node["values"].end(); ++it) {
            if (it.value().is_string()) {
                record.values[it.key()] = it.value().get<std::string>();
            }
        }
    }
    record.ask_for_params = node.value("ask", true);
    return record;
}

void MacroParamDefaults::set(const std::string& macro_name, const MacroParamDefaultRecord& record) {
    MacroParamDefaultRecord cleaned = record;
    for (auto it = cleaned.values.begin(); it != cleaned.values.end();) {
        // An empty value means "use the macro's own default" — storing it would
        // shadow nothing and prefill the prompt with blanks.
        it = it->second.empty() ? cleaned.values.erase(it) : std::next(it);
    }
    // A record with nothing to apply and still asking is today's behavior; the
    // presence of a record must mean something changed.
    if (cleaned.values.empty() && cleaned.ask_for_params) {
        clear(macro_name);
        return;
    }

    json node = json::object();
    node["values"] = cleaned.values;
    if (!cleaned.ask_for_params) {
        node["ask"] = false;
    }

    json table = read_record_table();
    table[store_key(macro_name)] = node;
    write_record_table(table);
}

void MacroParamDefaults::clear(const std::string& macro_name) {
    json table = read_record_table();
    if (!table.is_object() || !table.contains(store_key(macro_name))) {
        return;
    }
    table.erase(store_key(macro_name));
    write_record_table(table);
}

} // namespace helix
