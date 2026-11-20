// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xml_attribute_validator.h"

#include "spdlog/spdlog.h"

#include <regex>

namespace xml_validator {

std::unordered_set<std::string> extract_attributes_from_parser(const std::string& file_content,
                                                               const std::string& widget_name) {
    std::unordered_set<std::string> attrs;

    if (file_content.empty()) {
        return attrs;
    }

    // Pattern 1: Direct lv_streq calls - lv_streq("attr_name", name) or lv_streq("attr_name", ...)
    // Captures the first string argument as an attribute name
    std::regex lv_streq_regex(R"regex(lv_streq\s*\(\s*"([^"]+)")regex");
    auto streq_begin =
        std::sregex_iterator(file_content.begin(), file_content.end(), lv_streq_regex);
    auto streq_end = std::sregex_iterator();

    for (auto it = streq_begin; it != streq_end; ++it) {
        std::string attr = (*it)[1].str();
        attrs.insert(attr);
        spdlog::trace("[xml_validator] {} - found lv_streq attr: {}", widget_name, attr);
    }

    // Pattern 2: lv_xml_get_value_of calls - lv_xml_get_value_of(attrs, "attr_name")
    // Captures the second string argument as an attribute name
    std::regex get_value_regex(R"regex(lv_xml_get_value_of\s*\([^,]+,\s*"([^"]+)")regex");
    auto get_value_begin =
        std::sregex_iterator(file_content.begin(), file_content.end(), get_value_regex);
    auto get_value_end = std::sregex_iterator();

    for (auto it = get_value_begin; it != get_value_end; ++it) {
        std::string attr = (*it)[1].str();
        attrs.insert(attr);
        spdlog::trace("[xml_validator] {} - found lv_xml_get_value_of attr: {}", widget_name, attr);
    }

    // Pattern 3: SET_STYLE_IF macro - SET_STYLE_IF(prop, value)
    // Extracts "style_" + prop as an attribute name
    std::regex set_style_regex(R"regex(SET_STYLE_IF\s*\(\s*(\w+)\s*,)regex");
    auto set_style_begin =
        std::sregex_iterator(file_content.begin(), file_content.end(), set_style_regex);
    auto set_style_end = std::sregex_iterator();

    for (auto it = set_style_begin; it != set_style_end; ++it) {
        std::string prop = (*it)[1].str();
        std::string attr = "style_" + prop;
        attrs.insert(attr);
        spdlog::trace("[xml_validator] {} - found SET_STYLE_IF attr: {}", widget_name, attr);
    }

    // Pattern 4: strcmp calls - strcmp(name, "attr_name") == 0 or strcmp(attrs[i], "attr_name") ==
    // 0 Captures the second string argument as an attribute name
    std::regex strcmp_regex(
        R"regex(strcmp\s*\(\s*\w+(?:\[\w+\])?\s*,\s*"([^"]+)"\s*\)\s*==\s*0)regex");
    auto strcmp_begin =
        std::sregex_iterator(file_content.begin(), file_content.end(), strcmp_regex);
    auto strcmp_end = std::sregex_iterator();

    for (auto it = strcmp_begin; it != strcmp_end; ++it) {
        std::string attr = (*it)[1].str();
        attrs.insert(attr);
        spdlog::trace("[xml_validator] {} - found strcmp attr: {}", widget_name, attr);
    }

    spdlog::debug("[xml_validator] Extracted {} attributes from {}", attrs.size(), widget_name);
    return attrs;
}

std::vector<std::pair<std::string, std::string>>
extract_widget_registration(const std::string& file_content) {
    std::vector<std::pair<std::string, std::string>> registrations;

    if (file_content.empty()) {
        return registrations;
    }

    // Pattern: lv_xml_register_widget("widget_name", create_fn, apply_fn)
    // Captures widget_name and apply_fn_name
    std::regex register_regex(
        R"regex(lv_xml_register_widget\s*\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*\))regex");
    auto reg_begin = std::sregex_iterator(file_content.begin(), file_content.end(), register_regex);
    auto reg_end = std::sregex_iterator();

    for (auto it = reg_begin; it != reg_end; ++it) {
        std::string widget_name = (*it)[1].str();
        std::string apply_fn = (*it)[3].str();
        registrations.emplace_back(widget_name, apply_fn);
        spdlog::trace("[xml_validator] Found registration: {} -> {}", widget_name, apply_fn);
    }

    spdlog::debug("[xml_validator] Found {} widget registrations", registrations.size());
    return registrations;
}

ComponentInfo extract_component_props(const std::string& xml_content) {
    ComponentInfo info;

    if (xml_content.empty()) {
        return info;
    }
    // Every <!-- ... --> removed, so markup quoted in a comment (a usage example)
    // is never read as the component's own.
    const std::string markup = [&xml_content] {
        std::string out;
        out.reserve(xml_content.size());
        size_t pos = 0;
        while (pos < xml_content.size()) {
            const size_t open = xml_content.find("<!--", pos);
            if (open == std::string::npos) {
                out.append(xml_content, pos, std::string::npos);
                break;
            }
            out.append(xml_content, pos, open - pos);
            const size_t close = xml_content.find("-->", open + 4);
            if (close == std::string::npos)
                break;
            pos = close + 3;
        }
        return out;
    }();

    // Check if root element is <component> - if not, return empty
    std::regex component_regex(R"regex(<component\b)regex");
    if (!std::regex_search(markup, component_regex)) {
        spdlog::trace("[xml_validator] Not a component XML (no <component> root)");
        return info;
    }

    // Find <view extends="widget_name"> or just <view> (defaults to "lv_obj")
    std::regex view_extends_regex(R"regex(<view\s+extends\s*=\s*"([^"]+)")regex");
    std::smatch extends_match;
    if (std::regex_search(markup, extends_match, view_extends_regex)) {
        info.extends = extends_match[1].str();
        spdlog::trace("[xml_validator] Component extends: {}", info.extends);
    } else {
        // Check if there's a <view> without extends
        std::regex view_regex(R"regex(<view\b)regex");
        if (std::regex_search(markup, view_regex)) {
            info.extends = "lv_obj";
            spdlog::trace("[xml_validator] Component extends lv_obj (default)");
        }
    }

    // Find all <prop name="prop_name" occurrences
    std::regex prop_regex(R"regex(<prop\s+name\s*=\s*"([^"]+)")regex");
    auto prop_begin = std::sregex_iterator(markup.begin(), markup.end(), prop_regex);
    auto prop_end = std::sregex_iterator();

    for (auto it = prop_begin; it != prop_end; ++it) {
        std::string prop_name = (*it)[1].str();
        info.props.insert(prop_name);
        spdlog::trace("[xml_validator] Found prop: {}", prop_name);
    }

    spdlog::debug("[xml_validator] Extracted {} props from component", info.props.size());
    return info;
}

std::unordered_map<std::string, std::unordered_set<std::string>>
build_inheritance_tree(const WidgetDatabase& db) {
    std::unordered_map<std::string, std::unordered_set<std::string>> result;

    if (db.widget_attrs.empty()) {
        return result;
    }

    // For each widget, compute the full set of valid attributes by walking up the inheritance chain
    for (const auto& [widget_name, direct_attrs] : db.widget_attrs) {
        std::unordered_set<std::string> full_attrs = direct_attrs;

        // Walk up the inheritance chain. A component whose chain loops back on
        // itself stops at the repeat rather than walking it forever.
        std::string current = widget_name;
        std::unordered_set<std::string> visited{widget_name};
        while (true) {
            auto inherit_it = db.inheritance.find(current);
            if (inherit_it == db.inheritance.end() || inherit_it->second.empty()) {
                // No parent or reached top of hierarchy
                break;
            }

            std::string parent = inherit_it->second;
            if (!visited.insert(parent).second) {
                spdlog::warn("[xml_validator] {} has an inheritance cycle through {}", widget_name,
                             parent);
                break;
            }

            // Check if parent has attributes defined
            auto parent_attrs_it = db.widget_attrs.find(parent);
            if (parent_attrs_it == db.widget_attrs.end()) {
                // Parent not found in database - stop here
                spdlog::trace("[xml_validator] {} inherits from {} but parent not in database",
                              current, parent);
                break;
            }

            // Merge parent attributes
            for (const auto& attr : parent_attrs_it->second) {
                full_attrs.insert(attr);
            }

            spdlog::trace("[xml_validator] {} inherits {} attrs from {}", widget_name,
                          parent_attrs_it->second.size(), parent);

            // Move up to the next parent
            current = parent;
        }

        result[widget_name] = std::move(full_attrs);
        spdlog::trace("[xml_validator] {} has {} total attributes", widget_name,
                      result[widget_name].size());
    }

    spdlog::debug("[xml_validator] Built inheritance tree for {} widgets", result.size());
    return result;
}

} // namespace xml_validator
