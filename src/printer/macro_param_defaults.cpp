// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_defaults.h"

#include "config.h"
#include "core/text_io.h"
#include "json_utils.h"

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
    key = helix::text_io::to_lower(key);
    return key;
}

/// Read the whole record table. Malformed data reads as absent, never throws:
/// one hand-edited settings.json must not take macro running down. A table
/// that is not an object reads as empty so the next set() replaces it.
json read_record_table() {
    json table = Config::get_instance()->get<json>(store_leaf(), json::object());
    if (!table.is_object()) {
        spdlog::warn("[MacroParamDefaults] {} is not an object, ignoring", store_leaf());
        return json::object();
    }
    return table;
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
    const json table = read_record_table();
    const std::string key = store_key(macro_name);
    const json* stored = helix::json_util::find_member(table, key.c_str());
    const json node = stored ? *stored : json();
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
    if (const auto ask = node.find("ask"); ask != node.end() && ask->is_boolean()) {
        record.ask_for_params = ask->get<bool>();
    }
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
