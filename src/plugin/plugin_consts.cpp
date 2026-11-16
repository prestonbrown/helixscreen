// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_consts.h"

#include "../ui/theme/theme_manager_internal.h"
#include "display_metrics.h"
#include "helix-xml/src/libs/expat/expat.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <cstring>
#include <string_view>

namespace helix::plugin {

namespace {

struct ConstsWalk {
    PluginConsts* out;
    int consts_depth = 0;
};

std::unordered_map<std::string, std::string>* tier_table(PxTierTables& t, std::string_view suffix) {
    if (suffix == "_micro")
        return &t.micro;
    if (suffix == "_tiny")
        return &t.tiny;
    if (suffix == "_small")
        return &t.small;
    if (suffix == "_medium")
        return &t.medium;
    if (suffix == "_large")
        return &t.large;
    if (suffix == "_xlarge")
        return &t.xlarge;
    if (suffix == "_xxlarge")
        return &t.xxlarge;
    return nullptr;
}

/// Splits `ab__w_medium` into its base and table; nullptr when unsuffixed.
std::unordered_map<std::string, std::string>* split_suffix(PxTierTables& t, const std::string& name,
                                                           std::string& base) {
    // Longest first, so _xxlarge is not read as _xlarge or _large.
    for (const char* suffix :
         {"_xxlarge", "_xlarge", "_medium", "_micro", "_small", "_large", "_tiny"}) {
        const size_t n = std::strlen(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0) {
            base = name.substr(0, name.size() - n);
            return tier_table(t, suffix);
        }
    }
    return nullptr;
}

void on_start(void* ud, const XML_Char* el, const XML_Char** attrs) {
    auto& w = *static_cast<ConstsWalk*>(ud);
    if (std::strcmp(el, "consts") == 0) {
        ++w.consts_depth;
        return;
    }
    if (w.consts_depth == 0)
        return;
    std::string name;
    std::string value;
    for (int i = 0; attrs[i]; i += 2) {
        if (std::strcmp(attrs[i], "name") == 0)
            name = attrs[i + 1];
        else if (std::strcmp(attrs[i], "value") == 0)
            value = attrs[i + 1];
    }
    if (name.empty())
        return;
    std::string base;
    if (std::strcmp(el, "px") == 0) {
        if (auto* table = split_suffix(w.out->px, name, base)) {
            (*table)[base] = value;
            return;
        }
    }
    w.out->plain.push_back({el, name, value});
}

void on_end(void* ud, const XML_Char* el) {
    auto& w = *static_cast<ConstsWalk*>(ud);
    if (std::strcmp(el, "consts") == 0 && w.consts_depth > 0)
        --w.consts_depth;
}

} // namespace

PluginConsts collect_plugin_consts(const std::vector<std::string>& xml_files) {
    PluginConsts out;
    for (const auto& xml : xml_files) {
        ConstsWalk w{&out};
        XML_Parser p = XML_ParserCreate(nullptr);
        XML_SetUserData(p, &w);
        XML_SetElementHandler(p, &on_start, &on_end);
        XML_Parse(p, xml.data(), static_cast<int>(xml.size()), XML_TRUE);
        XML_ParserFree(p);
    }
    return out;
}

std::vector<std::string> missing_tier_errors(const PxTierTables& px) {
    std::vector<std::string> errors;
    std::vector<std::string> bases;
    for (const auto* table :
         {&px.micro, &px.tiny, &px.small, &px.medium, &px.large, &px.xlarge, &px.xxlarge}) {
        for (const auto& [base, value] : *table) {
            if (std::find(bases.begin(), bases.end(), base) == bases.end())
                bases.push_back(base);
        }
    }
    for (const auto& base : bases) {
        std::string missing;
        if (!px.small.count(base))
            missing += " _small";
        if (!px.medium.count(base))
            missing += " _medium";
        if (!px.large.count(base))
            missing += " _large";
        if (!missing.empty())
            errors.push_back("px constant '" + base +
                             "' needs the _small/_medium/_large set; missing" + missing);
    }
    return errors;
}

void register_plugin_consts(const PluginConsts& c, lv_display_t* display) {
    lv_xml_component_scope_t* globals = lv_xml_component_get_scope("globals");
    auto put = [globals](const std::string& name, const std::string& value) {
        if (lv_xml_get_const_silent(globals, name.c_str()))
            lv_xml_set_const(globals, name.c_str(), value.c_str());
        else
            lv_xml_register_const(globals, name.c_str(), value.c_str());
    };
    for (const auto& [base, value] : theme_manager_resolve_px_tables(c.px, display, false))
        put(base, value);
    const double scale = DisplayMetrics::active_scale();
    for (const auto& p : c.plain)
        put(p.name,
            p.type == "px" ? theme_detail::scale_px_token(p.name, p.value, scale) : p.value);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
