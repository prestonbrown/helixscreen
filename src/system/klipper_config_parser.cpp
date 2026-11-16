// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_parser.h"

#include "core/text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>

using helix::system::ConfigKey;
using helix::system::KlipperConfigEditor;

namespace {

// Klipper ends a value at whitespace followed by `#` or `;`; a bare `#` (a hex
// colour, say) stays part of the value.
std::string strip_inline_comment(std::string_view line) {
    for (size_t i = 1; i < line.size(); ++i) {
        if ((line[i] == '#' || line[i] == ';') && (line[i - 1] == ' ' || line[i - 1] == '\t')) {
            line = line.substr(0, i);
            break;
        }
    }
    return std::string(helix::text_io::trim(line));
}

bool is_comment(std::string_view trimmed) {
    return !trimmed.empty() && (trimmed[0] == '#' || trimmed[0] == ';');
}

} // namespace

bool KlipperConfigParser::parse(const std::string& content) {
    content_ = content;
    modified_ = false;
    reindex();
    return true;
}

void KlipperConfigParser::reindex() {
    structure_ = KlipperConfigEditor::parse_structure(content_);
}

const ConfigKey* KlipperConfigParser::find_key(const std::string& section,
                                               const std::string& key) const {
    auto sec_it = structure_.sections.find(section);
    if (sec_it == structure_.sections.end())
        return nullptr;
    const std::string wanted = helix::text_io::to_lower(key);
    const auto& keys = sec_it->second.keys;
    auto it = std::find_if(keys.rbegin(), keys.rend(),
                           [&](const ConfigKey& k) { return k.name == wanted; });
    return it == keys.rend() ? nullptr : &*it;
}

std::string KlipperConfigParser::get(const std::string& section, const std::string& key,
                                     const std::string& default_val) const {
    const ConfigKey* k = find_key(section, key);
    if (!k)
        return default_val;

    std::vector<std::string_view> lines;
    for (std::string_view sv : helix::text_io::lines(content_))
        lines.push_back(sv);

    // First line carries the value after the separator; the rest are indented
    // continuations. Blank lines inside a value stay, comment lines drop out.
    std::string result = strip_inline_comment(k->value);
    for (int i = k->line_number + 1; i <= k->end_line && i < static_cast<int>(lines.size()); ++i) {
        std::string_view trimmed = helix::text_io::trim(lines[i]);
        if (is_comment(trimmed))
            continue;
        if (!result.empty() || i > k->line_number + 1)
            result += '\n';
        result += strip_inline_comment(trimmed);
    }
    return result;
}

bool KlipperConfigParser::get_bool(const std::string& section, const std::string& key,
                                   bool default_val) const {
    std::string val = get(section, key, "");
    if (val.empty())
        return default_val;
    std::string lower = helix::text_io::to_lower(val);
    if (lower == "true" || lower == "yes" || lower == "1")
        return true;
    if (lower == "false" || lower == "no" || lower == "0")
        return false;
    return default_val;
}

float KlipperConfigParser::get_float(const std::string& section, const std::string& key,
                                     float default_val) const {
    std::string val = get(section, key, "");
    if (val.empty())
        return default_val;
    return helix::text_io::parse_leading<float>(val).value_or(default_val);
}

int KlipperConfigParser::get_int(const std::string& section, const std::string& key,
                                 int default_val) const {
    std::string val = get(section, key, "");
    if (val.empty())
        return default_val;
    return helix::text_io::parse_leading<int>(val).value_or(default_val);
}

void KlipperConfigParser::set(const std::string& section, const std::string& key,
                              const std::string& value) {
    modified_ = true;
    if (!has_section(section)) {
        spdlog::warn("KlipperConfigParser: set() on nonexistent section '{}'", section);
        return;
    }

    if (const ConfigKey* k = find_key(section, key)) {
        // Replacing a multi-line value drops its continuation lines.
        auto edited = KlipperConfigEditor::set_value(content_, section, k->name, value);
        if (!edited)
            return;
        std::vector<std::string> out;
        int line_no = 0;
        for (std::string_view sv : helix::text_io::lines(*edited)) {
            if (line_no <= k->line_number || line_no > k->end_line)
                out.emplace_back(sv);
            ++line_no;
        }
        std::string joined;
        for (const auto& l : out)
            joined += l + '\n';
        content_ = std::move(joined);
    } else if (auto added = KlipperConfigEditor::add_key(content_, section, key, value)) {
        content_ = std::move(*added);
    }
    reindex();
}

bool KlipperConfigParser::has_section(const std::string& section) const {
    return structure_.sections.count(section) > 0;
}

std::vector<std::string> KlipperConfigParser::get_sections() const {
    std::vector<const helix::system::ConfigSection*> secs;
    for (const auto& [name, sec] : structure_.sections)
        secs.push_back(&sec);
    std::sort(secs.begin(), secs.end(),
              [](const auto* a, const auto* b) { return a->line_start < b->line_start; });
    std::vector<std::string> result;
    for (const auto* sec : secs)
        result.push_back(sec->name);
    return result;
}

std::vector<std::string>
KlipperConfigParser::get_sections_matching(const std::string& prefix) const {
    std::vector<std::string> result;
    for (const auto& name : get_sections()) {
        if (name == prefix ||
            (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
             name[prefix.size()] == ' ')) {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<std::string> KlipperConfigParser::get_keys(const std::string& section) const {
    std::vector<std::string> result;
    auto sec_it = structure_.sections.find(section);
    if (sec_it == structure_.sections.end())
        return result;
    for (const auto& k : sec_it->second.keys) {
        if (std::find(result.begin(), result.end(), k.name) == result.end())
            result.push_back(k.name);
    }
    return result;
}

std::string KlipperConfigParser::serialize() const {
    return content_;
}

bool KlipperConfigParser::is_modified() const {
    return modified_;
}
