// SPDX-License-Identifier: GPL-3.0-or-later

#include "material_z_offset.h"

#include "core/text_io.h"
#include "filament_database.h"
#include "moonraker_types.h"
#include "print_cost.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "z_offset_persistence.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "hv/json.hpp"

namespace helix::material_zoffset {
namespace {

using nlohmann::json;

/// The status object for a gcode_macro section.
constexpr const char* kVarsObject = "gcode_macro _START_PRINT_VARS";
constexpr const char* kHelixObject = "gcode_macro _HELIX_MATERIAL";
constexpr const char* kHelixVarPrefix = "helix_zoffset_";
constexpr const char* kHelixMeshPrefix = "helix_";
constexpr const char* kVarsPrefix = "offset_";
constexpr const char* kHelixEnabledVar = "helix_material_offset_enabled";

int to_microns(double mm) {
    return static_cast<int>(std::lround(mm * 1000.0));
}

std::string format_mm(int microns) {
    return fmt::format("{:.3f}", static_cast<double>(microns) / 1000.0);
}

const json* member(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &(*it);
}

bool set_offset(Table& table, const std::string& key, int microns) {
    auto it = table.offsets.find(key);
    if (it != table.offsets.end() && it->second == microns) {
        return false;
    }
    table.offsets[key] = microns;
    return true;
}

bool allowed(std::string_view key, std::string_view extra) {
    return std::all_of(key.begin(), key.end(), [extra](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
               extra.find(c) != std::string_view::npos;
    });
}

// --- _START_PRINT_VARS (K2 Kalico and hand-made copies) ---------------------
//
// The start macro reads `printer["gcode_macro _START_PRINT_VARS"]
// .get("offset_" + MATERIAL|lower, ...offset_default)` and applies it with an
// absolute SET_GCODE_OFFSET Z=. Klipper lower-cases option names, so the
// variable_offset_PETG line reads back as offset_petg. offset_probe in the same
// macro is not a material.

std::optional<std::string> vars_key(std::string_view material) {
    std::string key = text_io::to_lower(text_io::trim(material));
    if (key.empty()) {
        return std::string(kDefaultKey);
    }
    // Anything a config option name and the Jinja lookup both carry intact.
    if (!allowed(key, "+.-")) {
        return std::nullopt;
    }
    return key;
}

bool vars_merge(const json& status, Table& table) {
    const json* vars = member(status, kVarsObject);
    if (!vars || !vars->is_object()) {
        return false;
    }
    bool changed = false;
    for (auto it = vars->begin(); it != vars->end(); ++it) {
        const std::string& name = it.key();
        if (name.rfind(kVarsPrefix, 0) != 0 || !it->is_number()) {
            continue;
        }
        const std::string key = name.substr(std::char_traits<char>::length(kVarsPrefix));
        if (key.empty() || key == "probe") {
            continue;
        }
        changed |= set_offset(table, key, to_microns(it->get<double>()));
    }
    return changed;
}

std::optional<WritePlan> vars_write(const Table& table, const std::string& key, int microns) {
    WritePlan plan;
    plan.edits_config_file = true;
    // SET_GCODE_VARIABLE rejects a variable the section does not declare, so a
    // new material takes effect once Klipper next reads the edited file.
    plan.live_now = table.offsets.count(key) != 0;
    if (plan.live_now) {
        plan.gcode =
            fmt::format("SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE={}{} VALUE={}",
                        kVarsPrefix, key, format_mm(microns));
    }
    return plan;
}

// --- HelixScreen's macro -----------------------------------------------------
//
// HELIX_MATERIAL_OFFSET (assets/config/helix_macros.cfg) reads
// helix_zoffset_<key> from [save_variables], else helix_zoffset_default, and
// loads the helix_<key> mesh profile when it exists. Its Jinja builds the key
// with lower and four replaces; helix_key() is the same rule.

std::optional<std::string> helix_key(std::string_view material) {
    std::string key = text_io::to_lower(text_io::trim(material));
    if (key.empty()) {
        return std::string(kDefaultKey);
    }
    for (char& c : key) {
        if (c == '-' || c == ' ' || c == '+' || c == '.') {
            c = '_';
        }
    }
    if (!allowed(key, "")) {
        return std::nullopt;
    }
    return key;
}

bool helix_merge(const json& status, Table& table) {
    bool changed = false;
    if (const json* sv = member(status, "save_variables")) {
        if (const json* vars = member(*sv, "variables"); vars && vars->is_object()) {
            if (const json* flag = member(*vars, kHelixEnabledVar); flag && flag->is_number()) {
                const bool on = flag->get<double>() != 0.0;
                changed |= table.enabled != on;
                table.enabled = on;
            }
            for (auto it = vars->begin(); it != vars->end(); ++it) {
                const std::string& name = it.key();
                if (name.rfind(kHelixVarPrefix, 0) != 0 || !it->is_number()) {
                    continue;
                }
                const std::string key =
                    name.substr(std::char_traits<char>::length(kHelixVarPrefix));
                if (!key.empty()) {
                    changed |= set_offset(table, key, to_microns(it->get<double>()));
                }
            }
        }
    }
    if (const json* mesh = member(status, "bed_mesh")) {
        if (const json* profiles = member(*mesh, "profiles"); profiles && profiles->is_object()) {
            std::set<std::string> meshes;
            for (auto it = profiles->begin(); it != profiles->end(); ++it) {
                if (it.key().rfind(kHelixMeshPrefix, 0) == 0 &&
                    it.key().size() > std::char_traits<char>::length(kHelixMeshPrefix)) {
                    meshes.insert(
                        it.key().substr(std::char_traits<char>::length(kHelixMeshPrefix)));
                }
            }
            if (meshes != table.meshes) {
                table.meshes = std::move(meshes);
                changed = true;
            }
        }
    }
    if (const json* macro = member(status, kHelixObject)) {
        if (const json* applied = member(*macro, "applied"); applied && applied->is_string()) {
            const std::string value = applied->get<std::string>();
            changed |= value != table.applied;
            table.applied = value;
        }
        if (const json* count = member(*macro, "apply_count"); count && count->is_number()) {
            const int value = count->get<int>();
            changed |= value != table.apply_count;
            table.apply_count = value;
        }
    }
    return changed;
}

std::optional<WritePlan> helix_write(const Table&, const std::string& key, int microns) {
    WritePlan plan;
    plan.gcode = fmt::format("SAVE_VARIABLE VARIABLE={}{} VALUE={}", kHelixVarPrefix, key,
                             format_mm(microns));
    plan.live_now = true;
    return plan;
}

// --- The table ---------------------------------------------------------------

struct Provider {
    const char* name;
    /// Macro whose presence selects the row.
    const char* detect_macro;
    std::vector<std::string> status_objects;
    /// Whether the printer can persist a value through this row.
    bool (*ready)(const PrinterDiscovery& hw);
    std::optional<std::string> (*key)(std::string_view material);
    bool (*merge)(const json& status, Table& table);
    std::optional<WritePlan> (*write)(const Table& table, const std::string& key, int microns);
    /// Section a config edit targets, normalized; nullptr when the row edits no file.
    const char* config_section;
    /// The start macro reports each application through apply_count.
    bool reports_application;
    /// Hint macro for a screen-started print's material; nullptr for none.
    const char* hint_macro;
    /// Prefix of a per-material bed mesh profile; nullptr for none.
    const char* mesh_prefix;
    /// Printer-side save variable the start macro obeys; nullptr where the
    /// macro applies its table on every print.
    const char* enable_var;
};

bool always_ready(const PrinterDiscovery&) {
    return true;
}

bool has_save_variables(const PrinterDiscovery& hw) {
    return hw.has_save_variables();
}

const std::vector<Provider>& providers() {
    // Row order is match priority. A printer whose own start macro keeps the
    // table applies it on every print, so it wins over HelixScreen's macro,
    // which the helper-macro pack installs everywhere.
    static const std::vector<Provider> table = {
        {"_START_PRINT_VARS",
         "_START_PRINT_VARS",
         {kVarsObject},
         &always_ready,
         &vars_key,
         &vars_merge,
         &vars_write,
         "gcode_macro _start_print_vars",
         false,
         nullptr,
         nullptr,
         nullptr},
        {"HelixScreen macro",
         "HELIX_MATERIAL_OFFSET",
         {"save_variables", "bed_mesh", kHelixObject},
         &has_save_variables,
         &helix_key,
         &helix_merge,
         &helix_write,
         nullptr,
         true,
         "_HELIX_MATERIAL",
         kHelixMeshPrefix,
         kHelixEnabledVar},
    };
    return table;
}

const Provider* match(const PrinterDiscovery& hw) {
    // A firmware that keeps the Z offset itself re-applies its own value over
    // ours, and its save path is a different one entirely.
    if (zoffset::firmware_persists_z_offset(hw)) {
        return nullptr;
    }
    for (const auto& p : providers()) {
        if (hw.has_macro(p.detect_macro)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool supported(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->status_objects : std::vector<std::string>{};
}

bool write_ready(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->ready(hw);
}

bool reports_application(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->reports_application;
}

bool start_macro_applies(const PrinterDiscovery& hw, const Table& table) {
    const Provider* p = match(hw);
    if (!p) {
        return false;
    }
    return p->enable_var ? table.enabled.value_or(false) : true;
}

std::string enable_gcode(const PrinterDiscovery& hw, bool on) {
    const Provider* p = match(hw);
    if (!p || !p->enable_var) {
        return {};
    }
    return fmt::format("SAVE_VARIABLE VARIABLE={} VALUE={}", p->enable_var, on ? 1 : 0);
}

bool merge_status(const PrinterDiscovery& hw, const nlohmann::json& status, Table& table) {
    const Provider* p = match(hw);
    return p && p->merge(status, table);
}

std::optional<std::string> material_key(const PrinterDiscovery& hw, std::string_view material) {
    const Provider* p = match(hw);
    return p ? p->key(material) : std::nullopt;
}

std::string display_name(std::string_view key) {
    return text_io::to_upper(key);
}

std::optional<int> applied_offset(const PrinterDiscovery& hw, const Table& table,
                                  std::string_view material) {
    if (auto key = material_key(hw, material)) {
        if (auto it = table.offsets.find(*key); it != table.offsets.end()) {
            return it->second;
        }
    }
    if (auto it = table.offsets.find(kDefaultKey); it != table.offsets.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<WritePlan> write_plan(const PrinterDiscovery& hw, const Table& table,
                                    std::string_view material, int microns) {
    const Provider* p = match(hw);
    if (!p) {
        return std::nullopt;
    }
    auto key = p->key(material);
    if (!key) {
        return std::nullopt;
    }
    return p->write(table, *key, microns);
}

std::string config_section(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->config_section ? p->config_section : std::string{};
}

std::string start_hint_gcode(const PrinterDiscovery& hw, std::string_view material,
                             std::string_view filename) {
    const Provider* p = match(hw);
    if (!p || !p->hint_macro) {
        return {};
    }
    const auto set = [p](std::string_view m, std::string_view f) {
        return fmt::format("SET_GCODE_VARIABLE MACRO={0} VARIABLE=next VALUE='\"{1}\"'\n"
                           "SET_GCODE_VARIABLE MACRO={0} VARIABLE=next_file VALUE='\"{2}\"'",
                           p->hint_macro, m, f);
    };
    // Klipper cuts a command line at '#' or ';', splits it with shlex and
    // reads the value as a Python literal: a name with any of these, or a
    // control character, cannot travel intact, so the hint is cleared.
    const bool file_ok =
        !filename.empty() && std::none_of(filename.begin(), filename.end(), [](char c) {
            return c == '\'' || c == '"' || c == '\\' || c == '#' || c == ';' ||
                   static_cast<unsigned char>(c) < 0x20;
        });
    // The material carries no quotes: the key's character rule guarantees it.
    if (!file_ok || text_io::trim(material).empty() || !p->key(material)) {
        return set("", "");
    }
    return set(text_io::to_upper(text_io::trim(material)), filename);
}

bool stores_mesh(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->mesh_prefix;
}

std::string mesh_profile_name(const PrinterDiscovery& hw, std::string_view material) {
    const Provider* p = match(hw);
    if (!p || !p->mesh_prefix) {
        return {};
    }
    auto key = p->key(material);
    return key ? std::string(p->mesh_prefix) + *key : std::string{};
}

// ============================================================================
// The config file edit
// ============================================================================

namespace {

/// One line of a file: its text without the ending, and the ending itself.
struct Line {
    std::string_view text;
    std::string_view eol;
};

std::vector<Line> split_lines(std::string_view content) {
    std::vector<Line> lines;
    size_t start = 0;
    while (start < content.size()) {
        size_t nl = content.find('\n', start);
        size_t end = nl == std::string_view::npos ? content.size() : nl;
        size_t text_end = end;
        if (nl != std::string_view::npos && end > start && content[end - 1] == '\r') {
            --text_end;
        }
        Line l;
        l.text = content.substr(start, text_end - start);
        l.eol = nl == std::string_view::npos ? std::string_view{}
                                             : content.substr(text_end, nl + 1 - text_end);
        lines.push_back(l);
        start = nl == std::string_view::npos ? content.size() : nl + 1;
    }
    return lines;
}

/// The normalized name of a live section header, or nullopt for any other line.
std::optional<std::string> header_name(std::string_view line) {
    if (line.empty() || line[0] != '[') {
        return std::nullopt;
    }
    const size_t close = line.find(']');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    std::string out;
    bool space = false;
    for (char c : text_io::trim(line.substr(1, close - 1))) {
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool is_continuation(std::string_view line) {
    return !line.empty() && (line[0] == ' ' || line[0] == '\t') && !text_io::trim(line).empty();
}

/// An option line's lower-cased name, or nullopt for comments, blanks,
/// continuations and headers.
std::optional<std::string> option_name(std::string_view line) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#' || line[0] == ';' ||
        line[0] == '[') {
        return std::nullopt;
    }
    const size_t sep = line.find_first_of(":=");
    if (sep == std::string_view::npos) {
        return std::nullopt;
    }
    return text_io::to_lower(text_io::trim(line.substr(0, sep)));
}

/// @p line with its value replaced by @p value, keeping the key, the separator,
/// the spacing after it and any inline comment.
std::string with_value(std::string_view line, const std::string& value) {
    const size_t sep = line.find_first_of(":=");
    size_t v = sep + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) {
        ++v;
    }
    // Klipper's inline comments start at '#' or ';' after whitespace.
    size_t comment = line.size();
    for (size_t i = v; i < line.size(); ++i) {
        if ((line[i] == '#' || line[i] == ';') && i > v &&
            (line[i - 1] == ' ' || line[i - 1] == '\t')) {
            comment = i;
            while (comment > v && (line[comment - 1] == ' ' || line[comment - 1] == '\t')) {
                --comment;
            }
            break;
        }
    }
    std::string out(line.substr(0, sep + 1));
    out += v > sep + 1 ? std::string(line.substr(sep + 1, v - sep - 1)) : std::string(" ");
    out += value;
    out += line.substr(comment);
    return out;
}

} // namespace

int count_sections(std::string_view content, std::string_view section) {
    int n = 0;
    for (const Line& l : split_lines(content)) {
        if (auto name = header_name(l.text); name && *name == section) {
            ++n;
        }
    }
    return n;
}

ConfigEdit edit_config_text(std::string_view content, std::string_view section,
                            std::string_view material, int microns) {
    ConfigEdit result;
    const auto key = vars_key(material);
    if (!key) {
        result.error = fmt::format("'{}' cannot be stored as a config option", material);
        return result;
    }
    const std::string want = std::string("variable_") + kVarsPrefix + *key;

    const std::vector<Line> lines = split_lines(content);
    std::optional<size_t> header;
    int headers = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (auto name = header_name(lines[i].text); name && *name == section) {
            ++headers;
            header = i;
        }
    }
    if (headers != 1) {
        result.error = fmt::format("[{}] appears {} times, expected once", section, headers);
        return result;
    }

    size_t end = *header + 1;
    while (end < lines.size() && !header_name(lines[end].text)) {
        ++end;
    }

    std::vector<size_t> matches;
    size_t anchor = *header;
    for (size_t i = *header + 1; i < end; ++i) {
        auto name = option_name(lines[i].text);
        if (!name) {
            continue;
        }
        if (*name == want) {
            matches.push_back(i);
        }
        if (name->rfind("variable_", 0) == 0) {
            anchor = i;
            while (anchor + 1 < end && is_continuation(lines[anchor + 1].text)) {
                ++anchor;
            }
        }
    }
    if (matches.size() > 1) {
        result.error = fmt::format("{} is set {} times", want, matches.size());
        return result;
    }

    const std::string value = format_mm(microns);
    std::string out;
    out.reserve(content.size() + 48);
    if (matches.size() == 1) {
        const size_t m = matches.front();
        if (m + 1 < end && is_continuation(lines[m + 1].text)) {
            result.error = fmt::format("{} runs onto more than one line", want);
            return result;
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            out += i == m ? with_value(lines[i].text, value) : std::string(lines[i].text);
            out += lines[i].eol;
        }
    } else {
        std::string_view eol = lines[anchor].eol;
        if (eol.empty()) {
            eol = "\n";
            for (const Line& l : lines) {
                if (!l.eol.empty()) {
                    eol = l.eol;
                    break;
                }
            }
        }
        const std::string label = *key == kDefaultKey ? std::string("DEFAULT")
                                                      : text_io::to_upper(text_io::trim(material));
        const std::string added = std::string("variable_") + kVarsPrefix + label + ": " + value;
        for (size_t i = 0; i < lines.size(); ++i) {
            out += lines[i].text;
            if (i != anchor) {
                out += lines[i].eol;
                continue;
            }
            out += eol;
            out += added;
            out += lines[i].eol;
        }
    }
    result.text = std::move(out);
    return result;
}

std::vector<std::string> include_targets(std::string_view content) {
    std::vector<std::string> out;
    for (const Line& l : split_lines(content)) {
        auto name = header_name(l.text);
        if (!name || name->rfind("include ", 0) != 0) {
            continue;
        }
        // The name as written: header_name() lower-cases, and paths are case-sensitive.
        const size_t open = l.text.find_first_not_of(" \t", 1);
        const size_t close = l.text.find(']');
        std::string_view inner = text_io::trim(l.text.substr(open, close - open));
        inner = text_io::trim(inner.substr(std::char_traits<char>::length("include")));
        if (!inner.empty()) {
            out.emplace_back(inner);
        }
    }
    return out;
}

bool include_matches(std::string_view pattern, std::string_view path) {
    if (pattern.empty()) {
        return path.empty();
    }
    if (pattern[0] == '*') {
        for (size_t i = 0; i <= path.size(); ++i) {
            if (include_matches(pattern.substr(1), path.substr(i))) {
                return true;
            }
            if (i < path.size() && path[i] == '/') {
                break;
            }
        }
        return false;
    }
    if (path.empty()) {
        return false;
    }
    if ((pattern[0] == '?' && path[0] != '/') || pattern[0] == path[0]) {
        return include_matches(pattern.substr(1), path.substr(1));
    }
    return false;
}

// ============================================================================
// The primary material
// ============================================================================

std::string primary_material(const std::vector<ToolMaterial>& tools) {
    const ToolMaterial* best = nullptr;
    for (const auto& t : tools) {
        if (t.grams <= 0.0 || text_io::trim(t.material).empty()) {
            continue;
        }
        if (!best || t.grams > best->grams || (t.grams == best->grams && t.tool < best->tool)) {
            best = &t;
        }
    }
    if (best) {
        return std::string(text_io::trim(best->material));
    }
    for (const auto& t : tools) {
        if (!text_io::trim(t.material).empty()) {
            return std::string(text_io::trim(t.material));
        }
    }
    return {};
}

std::string primary_material_for_file(const FileMetadata& metadata,
                                      const std::vector<double>& footer_grams, float diameter_mm) {
    const FilePalette palette = file_palette_from_metadata(metadata);
    auto material_of = [&](int tool) -> std::string {
        const size_t i = tool < 0 ? 0 : static_cast<size_t>(tool);
        if (i < palette.materials.size() && !palette.materials[i].empty()) {
            return palette.materials[i];
        }
        return tool < 0 ? first_filament(metadata.filament_type) : std::string();
    };
    std::vector<ToolMaterial> tools;
    const auto grams = print_cost::file_tool_grams(metadata, material_of(-1), diameter_mm);
    const bool per_tool = std::any_of(grams.begin(), grams.end(),
                                      [](const print_cost::ToolGrams& g) { return g.tool >= 0; });
    if (!per_tool && !footer_grams.empty()) {
        for (size_t i = 0; i < footer_grams.size(); ++i) {
            tools.push_back(
                {static_cast<int>(i), footer_grams[i], material_of(static_cast<int>(i))});
        }
    } else {
        for (const auto& g : grams) {
            tools.push_back({g.tool, g.grams, material_of(g.tool)});
        }
    }
    tools.push_back({-1, 0.0, material_of(-1)}); // no grams at all: the first filament
    return primary_material(tools);
}

std::string first_filament(std::string_view filament_type) {
    return std::string(text_io::trim(filament_type.substr(0, filament_type.find(';'))));
}

int usual_bed_temp(std::string_view material) {
    auto info = filament::find_material(text_io::trim(material));
    return info && info->bed_temp > 0 ? info->bed_temp : -1;
}

std::string closest_material(const std::vector<std::string>& materials, int bed_temp) {
    std::string best;
    int best_diff = std::numeric_limits<int>::max();
    for (const auto& m : materials) {
        const int t = usual_bed_temp(m);
        if (t < 0) {
            continue;
        }
        const int diff = std::abs(t - bed_temp);
        if (diff < best_diff) {
            best_diff = diff;
            best = m;
        }
    }
    return best;
}

} // namespace helix::material_zoffset
