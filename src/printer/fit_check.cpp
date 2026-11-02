// SPDX-License-Identifier: GPL-3.0-or-later
#include "fit_check.h"

#include "gcode_wait_line.h"
#include "json_utils.h"
#include "lvgl.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace helix {

namespace {

using json = nlohmann::json;

/// Positions and temperatures within this of a limit are at it: the record
/// rounds to 4 decimals, and a gcode step is 0.001 mm.
constexpr double kLimitEpsilon = 1e-4;

/// Nozzle diameters this close are the same nozzle.
constexpr double kNozzleTolerance = 0.005;

constexpr std::array<const char*, 3> kAxisNames = {"x", "y", "z"};

/// A finite JSON number; anything else (bool and numeric strings included) is unset.
std::optional<double> number(const json& v) {
    if (!v.is_number()) {
        return std::nullopt;
    }
    const double d = json_util::as_double(v, NAN);
    return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
}

const json* section(const json& settings, const char* name) {
    const json* s = json_util::find_member(settings, name);
    return s != nullptr && s->is_object() ? s : nullptr;
}

/// A numeric option: @p fallback when the section leaves it out, unset when it
/// holds something that is not a number, or when there is no section.
std::optional<double> option(const json* sec, const char* key,
                             std::optional<double> fallback = std::nullopt) {
    if (sec == nullptr) {
        return std::nullopt;
    }
    const json* v = json_util::find_member(*sec, key);
    if (v == nullptr) {
        return fallback;
    }
    return number(*v);
}

/// The number after @p prefix in @p name when the rest is all digits ("" reads
/// as 0 when @p empty_is_zero); nullopt otherwise.
std::optional<int> numbered(std::string_view name, std::string_view prefix, bool empty_is_zero) {
    if (name.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    const std::string_view rest = name.substr(prefix.size());
    if (rest.empty()) {
        return empty_is_zero ? std::optional<int>(0) : std::nullopt;
    }
    if (!std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    return text_io::parse_int<int>(rest);
}

json axes_json(const FitAxes& axes) {
    json out = json::array();
    for (const auto& v : axes) {
        out.push_back(v ? json(*v) : json(nullptr));
    }
    return out;
}

FitFinding finding(FitKind kind, FitSeverity severity) {
    FitFinding f;
    f.kind = kind;
    f.severity = severity;
    return f;
}

/// A printer name compared without case, spaces or punctuation.
std::string model_key(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::vector<double> numbers_in(std::string_view text) {
    std::vector<double> out;
    std::string normalized(text);
    std::replace(normalized.begin(), normalized.end(), ';', ',');
    for (std::string_view token : text_io::lines(normalized, ',')) {
        if (auto v = text_io::parse_double(text_io::trim(token)); v && std::isfinite(*v)) {
            out.push_back(*v);
        }
    }
    return out;
}

void raise(std::optional<double>& target, double value) {
    if (!target || value > *target) {
        target = value;
    }
}

/// The flow types a list names, normalised; empty when it names none this
/// check knows, so a later source can still answer.
template <typename Range> std::vector<std::string> known_volume_types(const Range& values) {
    std::vector<std::string> out;
    bool any = false;
    for (const auto& v : values) {
        out.push_back(fit_nozzle_volume_type(v));
        any = any || !out.back().empty();
    }
    return any ? out : std::vector<std::string>{};
}

std::vector<std::string> volume_types_in(std::string_view text) {
    std::string normalized(text);
    std::replace(normalized.begin(), normalized.end(), ';', ',');
    std::vector<std::string_view> tokens;
    for (std::string_view token : text_io::lines(normalized, ',')) {
        tokens.push_back(text_io::trim(token));
    }
    return known_volume_types(tokens);
}

} // namespace

std::string fit_nozzle_volume_type(std::string_view text) {
    const std::string key = model_key(text);
    if (key == "standard") {
        return "standard";
    }
    if (key == "highflow") {
        return "high_flow";
    }
    return {};
}

FitMachine fit_machine_from_settings(const json& settings) {
    FitMachine m;
    if (!settings.is_object()) {
        return m;
    }
    const json* printer = section(settings, "printer");
    if (printer != nullptr) {
        if (const json* k = json_util::find_member(*printer, "kinematics"); k && k->is_string()) {
            m.kinematics = text_io::to_lower(k->get_ref<const std::string&>());
        }
    }

    if (m.kinematics == "delta") {
        m.print_radius = option(printer, "print_radius");
        if (!m.print_radius) {
            m.print_radius = option(printer, "delta_radius");
        }
        m.axis_min[2] = option(printer, "minimum_z_position", 0.0);
        for (const char* tower : {"stepper_a", "stepper_b", "stepper_c"}) {
            if (auto e = option(section(settings, tower), "position_endstop")) {
                m.axis_max[2] = m.axis_max[2] ? std::min(*m.axis_max[2], *e) : *e;
            }
        }
    } else {
        for (size_t i = 0; i < kAxisNames.size(); ++i) {
            const json* stepper =
                section(settings, (std::string("stepper_") + kAxisNames[i]).c_str());
            m.axis_min[i] = option(stepper, "position_min", 0.0);
            m.axis_max[i] = option(stepper, "position_max");
        }
        if (const json* carriage = section(settings, "dual_carriage")) {
            const std::string axis = json_util::safe_string(*carriage, "axis", "x");
            const int i = axis == "x" ? 0 : axis == "y" ? 1 : -1;
            const auto low = option(carriage, "position_min", 0.0);
            const auto high = option(carriage, "position_max");
            if (i >= 0) {
                if (low && m.axis_min[i]) {
                    m.axis_min[i] = std::min(*m.axis_min[i], *low);
                }
                if (high && m.axis_max[i]) {
                    m.axis_max[i] = std::max(*m.axis_max[i], *high);
                }
            }
        }
    }

    std::map<int, const json*> extruders;
    int tool_macros = 0;
    for (const auto& [name, value] : settings.items()) {
        if (auto n = numbered(name, "extruder", true); n && value.is_object()) {
            extruders[*n] = &value;
        } else if (auto t = numbered(name, "gcode_macro t", false)) {
            tool_macros = std::max(tool_macros, *t + 1);
        }
    }
    for (const auto& [n, sec] : extruders) {
        if (auto t = option(sec, "max_temp")) {
            raise(m.max_extruder_temp, *t);
        }
        if (auto d = option(sec, "nozzle_diameter")) {
            m.nozzle_diameters.push_back(*d);
        }
    }
    m.max_bed_temp = option(section(settings, "heater_bed"), "max_temp");
    const int tools = std::max(static_cast<int>(extruders.size()), tool_macros);
    if (tools > 0) {
        m.tool_count = tools;
    }
    m.tool_count_confirmed = !extruders.empty() && static_cast<int>(extruders.size()) >= tools;
    return m;
}

FitMachine fit_machine_for_screen(FitMachine base, std::string model, bool filament_system,
                                  double y_shrink_mm) {
    base.model = std::move(model);
    if (filament_system) {
        base.tool_count.reset();
    }
    if (y_shrink_mm > 0.0 && base.axis_max[1]) {
        *base.axis_max[1] -= y_shrink_mm;
    }
    return base;
}

FitMachine fit_machine_with_live_nozzles(FitMachine base, const std::vector<FitLiveNozzle>& live) {
    for (size_t i = 0; i < live.size(); ++i) {
        const auto& diameter = live[i].diameter;
        if (diameter && std::isfinite(*diameter) && *diameter > 0.0 &&
            i < base.nozzle_diameters.size()) {
            base.nozzle_diameters[i] = *diameter;
        }
        std::string type = fit_nozzle_volume_type(live[i].volume_type);
        if (!type.empty()) {
            if (base.nozzle_volume_types.size() <= i) {
                base.nozzle_volume_types.resize(i + 1);
            }
            base.nozzle_volume_types[i] = std::move(type);
        }
    }
    return base;
}

std::vector<FitFinding> check_fit(const FitMachine& machine, const FitFile& file) {
    std::vector<FitFinding> out;

    const std::optional<FitBox>& box = file.travel ? file.travel : file.print;
    if (box) {
        for (size_t i = 0; i < kAxisNames.size(); ++i) {
            const auto& low = box->min[i];
            const auto& limit_low = machine.axis_min[i];
            if (low && limit_low && *low < *limit_low - kLimitEpsilon) {
                FitFinding f = finding(FitKind::OutOfBounds, FitSeverity::Block);
                f.axis = kAxisNames[i];
                f.side = "min";
                f.value = *low;
                f.limit = *limit_low;
                out.push_back(std::move(f));
            }
            const auto& high = box->max[i];
            const auto& limit_high = machine.axis_max[i];
            if (high && limit_high && *high > *limit_high + kLimitEpsilon) {
                FitFinding f = finding(FitKind::OutOfBounds, FitSeverity::Block);
                f.axis = kAxisNames[i];
                f.side = "max";
                f.value = *high;
                f.limit = *limit_high;
                out.push_back(std::move(f));
            }
        }
        if (machine.print_radius) {
            // The box's farthest edge from the centre. Its corners are farther,
            // but a round part need not fill them, so only an edge is certain.
            std::optional<double> reach;
            for (const FitAxes* end : {&box->min, &box->max}) {
                for (size_t i = 0; i < 2; ++i) {
                    if ((*end)[i]) {
                        raise(reach, std::fabs(*(*end)[i]));
                    }
                }
            }
            if (reach && *reach > *machine.print_radius + kLimitEpsilon) {
                FitFinding f = finding(FitKind::OutOfBounds, FitSeverity::Block);
                f.axis = "radius";
                f.side = "max";
                f.value = *reach;
                f.limit = *machine.print_radius;
                out.push_back(std::move(f));
            }
        }
    }

    const struct {
        const char* heater;
        const std::optional<double>& value;
        const std::optional<double>& limit;
    } heaters[] = {{"extruder", file.max_hotend_temp, machine.max_extruder_temp},
                   {"heater_bed", file.max_bed_temp, machine.max_bed_temp}};
    for (const auto& h : heaters) {
        if (h.value && h.limit && *h.value > *h.limit + kLimitEpsilon) {
            FitFinding f = finding(FitKind::TempOverMax, FitSeverity::Block);
            f.heater = h.heater;
            f.value = *h.value;
            f.limit = *h.limit;
            out.push_back(std::move(f));
        }
    }

    if (machine.tool_count && !file.tools.empty()) {
        const int highest = *file.tools.rbegin();
        if (highest >= *machine.tool_count) {
            FitFinding f =
                finding(FitKind::TooManyTools,
                        machine.tool_count_confirmed ? FitSeverity::Block : FitSeverity::Warn);
            f.tool = highest;
            f.limit = *machine.tool_count;
            out.push_back(std::move(f));
        }
    }

    // A file naming its tools is judged on those tools' nozzles alone: a slicer
    // lists a nozzle for every extruder, used or not. File tool i is compared
    // with extruder i; the printer's own filament mapping is not followed.
    const auto used = [&file](size_t i) {
        return file.tools.empty() || file.tools.count(static_cast<int>(i)) != 0;
    };
    const size_t nozzles = std::min(file.nozzle_diameters.size(), machine.nozzle_diameters.size());
    for (size_t i = 0; i < nozzles; ++i) {
        if (!used(i)) {
            continue;
        }
        const double want = file.nozzle_diameters[i];
        const double have = machine.nozzle_diameters[i];
        if (std::fabs(want - have) > kNozzleTolerance) {
            FitFinding f = finding(FitKind::NozzleMismatch, FitSeverity::Warn);
            f.tool = static_cast<int>(i);
            f.value = want;
            f.limit = have;
            out.push_back(std::move(f));
        }
    }

    // A flow-type mismatch only warns, either way round. File tool i is not
    // necessarily the head that prints it once the printer's own filament
    // mapping applies, and Snapmaker's U1 firmware refuses a real High Flow
    // job on a standard nozzle itself.
    const size_t types =
        std::min(file.nozzle_volume_types.size(), machine.nozzle_volume_types.size());
    for (size_t i = 0; i < types; ++i) {
        const std::string want = fit_nozzle_volume_type(file.nozzle_volume_types[i]);
        const std::string have = fit_nozzle_volume_type(machine.nozzle_volume_types[i]);
        if (!used(i) || want.empty() || have.empty() || want == have) {
            continue;
        }
        FitFinding f = finding(FitKind::NozzleFlowMismatch, FitSeverity::Warn);
        f.tool = static_cast<int>(i);
        f.file_volume_type = want;
        f.printer_volume_type = have;
        out.push_back(std::move(f));
    }

    const std::string a = model_key(file.printer_model);
    const std::string b = model_key(machine.model);
    if (!a.empty() && !b.empty() && a.find(b) == std::string::npos &&
        b.find(a) == std::string::npos) {
        FitFinding f = finding(FitKind::PrinterModelMismatch, FitSeverity::Note);
        f.file_model = file.printer_model;
        f.printer_model = machine.model;
        out.push_back(std::move(f));
    }
    return out;
}

bool fit_blocks(const std::vector<FitFinding>& findings) {
    return std::any_of(findings.begin(), findings.end(),
                       [](const FitFinding& f) { return f.severity == FitSeverity::Block; });
}

std::optional<FitSeverity> fit_worst(const std::vector<FitFinding>& findings) {
    std::optional<FitSeverity> worst;
    for (const FitFinding& f : findings) {
        if (!worst || static_cast<int>(f.severity) > static_cast<int>(*worst)) {
            worst = f.severity;
        }
    }
    return worst;
}

bool fit_warns(const std::vector<FitFinding>& findings) {
    return std::any_of(findings.begin(), findings.end(),
                       [](const FitFinding& f) { return f.severity == FitSeverity::Warn; });
}

std::optional<FitBox> fit_box_from_json(const json& j) {
    const json* lo = json_util::find_member(j, "min");
    const json* hi = json_util::find_member(j, "max");
    if (lo == nullptr || hi == nullptr || !lo->is_array() || !hi->is_array()) {
        return std::nullopt;
    }
    FitBox box;
    for (size_t i = 0; i < 3; ++i) {
        if (i < lo->size()) {
            box.min[i] = number((*lo)[i]);
        }
        if (i < hi->size()) {
            box.max[i] = number((*hi)[i]);
        }
    }
    return box;
}

json fit_machine_json(const FitMachine& m) {
    json nozzles = json::array();
    for (double d : m.nozzle_diameters) {
        nozzles.push_back(d);
    }
    json types = json::array();
    for (const std::string& t : m.nozzle_volume_types) {
        types.push_back(t);
    }
    auto opt = [](const auto& v) { return v ? json(*v) : json(nullptr); };
    return json{{"kinematics", m.kinematics},
                {"axis_min", axes_json(m.axis_min)},
                {"axis_max", axes_json(m.axis_max)},
                {"print_radius", opt(m.print_radius)},
                {"max_extruder_temp", opt(m.max_extruder_temp)},
                {"max_bed_temp", opt(m.max_bed_temp)},
                {"tool_count", opt(m.tool_count)},
                {"tool_count_confirmed", m.tool_count_confirmed},
                {"nozzle_diameters", nozzles},
                {"nozzle_volume_types", types},
                {"model", m.model}};
}

json fit_finding_json(const FitFinding& f) {
    json detail;
    const char* kind = "";
    switch (f.kind) {
    case FitKind::OutOfBounds:
        kind = "out_of_bounds";
        detail = {{"axis", f.axis}, {"side", f.side}, {"value", f.value}, {"limit", f.limit}};
        break;
    case FitKind::TempOverMax:
        kind = "temp_over_max";
        detail = {{"heater", f.heater}, {"value", f.value}, {"limit", f.limit}};
        break;
    case FitKind::TooManyTools:
        kind = "too_many_tools";
        detail = {{"tool", f.tool}, {"limit", static_cast<int>(f.limit)}};
        break;
    case FitKind::NozzleMismatch:
        kind = "nozzle_mismatch";
        detail = {{"tool", f.tool}, {"file", f.value}, {"printer", f.limit}};
        break;
    case FitKind::NozzleFlowMismatch:
        kind = "nozzle_flow_mismatch";
        detail = {
            {"tool", f.tool}, {"file", f.file_volume_type}, {"printer", f.printer_volume_type}};
        break;
    case FitKind::PrinterModelMismatch:
        kind = "printer_model_mismatch";
        detail = {{"file", f.file_model}, {"printer", f.printer_model}};
        break;
    }
    return json{{"kind", kind},
                {"severity", f.severity == FitSeverity::Block  ? "block"
                             : f.severity == FitSeverity::Warn ? "warn"
                                                               : "note"},
                {"detail", detail}};
}

std::string fit_finding_text(const FitFinding& f) {
    switch (f.kind) {
    case FitKind::OutOfBounds:
        if (f.axis == "radius") {
            return fmt::format(fmt::runtime(lv_tr("Reaches {:g} mm from the center, past this "
                                                  "printer's {:g} mm radius.")),
                               f.value, f.limit);
        }
        return fmt::format(fmt::runtime(f.side == "min"
                                            ? lv_tr("Moves to {} {:g} mm, below this printer's "
                                                    "{:g} mm.")
                                            : lv_tr("Moves to {} {:g} mm, past this printer's "
                                                    "{:g} mm.")),
                           text_io::to_upper(f.axis), f.value, f.limit);
    case FitKind::TempOverMax:
        return fmt::format(fmt::runtime(f.heater == "heater_bed"
                                            ? lv_tr("Heats the bed to {:g}°C, above this "
                                                    "printer's {:g}°C limit.")
                                            : lv_tr("Heats the nozzle to {:g}°C, above this "
                                                    "printer's {:g}°C limit.")),
                           f.value, f.limit);
    case FitKind::TooManyTools:
        return fmt::format(fmt::runtime(lv_tr("Uses tool T{}, and this printer's last tool is "
                                              "T{}.")),
                           f.tool, static_cast<int>(f.limit) - 1);
    case FitKind::NozzleMismatch:
        return fmt::format(fmt::runtime(lv_tr("Sliced for a {:g} mm nozzle on T{}; the printer "
                                              "has {:g} mm.")),
                           f.value, f.tool, f.limit);
    case FitKind::NozzleFlowMismatch:
        return fmt::format(fmt::runtime(f.file_volume_type == "high_flow"
                                            ? lv_tr("Sliced for a High Flow nozzle on T{}; the "
                                                    "printer has a standard one.")
                                            : lv_tr("Sliced for a standard nozzle on T{}; the "
                                                    "printer has High Flow.")),
                           f.tool);
    case FitKind::PrinterModelMismatch:
        return fmt::format(fmt::runtime(lv_tr("Sliced for {}, not {}.")), f.file_model,
                           f.printer_model);
    }
    return {};
}

std::string fit_findings_text(const std::vector<FitFinding>& findings, FitSeverity at_least) {
    std::string lines;
    for (const FitFinding& f : findings) {
        if (static_cast<int>(f.severity) < static_cast<int>(at_least)) {
            continue;
        }
        if (!lines.empty()) {
            lines += "\n";
        }
        lines += fit_finding_text(f);
    }
    return lines;
}

FitHeadFacts fit_head_facts(std::string_view head) {
    FitHeadFacts facts;
    // Last value wins per key, as a slicer that repeats a key means its last.
    std::map<std::string, std::string, std::less<>> config;
    std::map<std::string, std::string> cura_nozzles;
    for (std::string_view raw : text_io::lines(head)) {
        std::string_view line = text_io::trim(raw);
        if (line.empty()) {
            continue;
        }
        if (line[0] == ';') {
            const std::string_view text = text_io::trim(line.substr(1));
            if (const size_t eq = text.find(" = "); eq != std::string_view::npos) {
                const std::string_view key = text_io::trim(text.substr(0, eq));
                if (key == "printer_model" || key == "nozzle_diameter" ||
                    key == "nozzle_volume_type") {
                    config[std::string(key)] = std::string(text_io::trim(text.substr(eq + 3)));
                }
            } else if (const size_t colon = text.find(':'); colon != std::string_view::npos) {
                const std::string key(text_io::trim(text.substr(0, colon)));
                const std::string value(text_io::trim(text.substr(colon + 1)));
                if (key == "TARGET_MACHINE.NAME" || key == "MACHINE_NAME") {
                    config[key] = value;
                } else if (const std::string_view suffix = ".NOZZLE.DIAMETER";
                           key.rfind("EXTRUDER_TRAIN.", 0) == 0 && key.size() > suffix.size() &&
                           std::string_view(key).substr(key.size() - suffix.size()) == suffix) {
                    cura_nozzles[key] = value;
                }
            }
            continue;
        }
        const HeaterTargetLine set = heater_target_line(line);
        if (!set.target) {
            continue;
        }
        if (set.heater == "heater_bed") {
            raise(facts.max_bed_temp, *set.target);
        } else if (set.heater.rfind("extruder", 0) == 0) {
            raise(facts.max_hotend_temp, *set.target);
        }
    }
    for (const char* key : {"printer_model", "TARGET_MACHINE.NAME", "MACHINE_NAME"}) {
        if (auto it = config.find(key); it != config.end() && !it->second.empty()) {
            facts.printer_model = it->second;
            break;
        }
    }
    if (auto it = config.find("nozzle_diameter"); it != config.end()) {
        facts.nozzle_diameters = numbers_in(it->second);
    }
    if (auto it = config.find("nozzle_volume_type"); it != config.end()) {
        facts.nozzle_volume_types = volume_types_in(it->second);
    }
    if (facts.nozzle_diameters.empty()) {
        for (const auto& [key, value] : cura_nozzles) {
            if (auto v = text_io::parse_double(value); v && std::isfinite(*v)) {
                facts.nozzle_diameters.push_back(*v);
            }
        }
    }
    return facts;
}

FitFile fit_file_from_sources(const std::optional<FitFile>& record, const FitHeadFacts* head,
                              const std::optional<FileMetadata>& metadata,
                              const std::optional<FitBox>& preview_bounds, std::set<int> tools) {
    FitFile file = record.value_or(FitFile{});
    if (!file.print && !file.travel && preview_bounds) {
        file.print = preview_bounds;
    }
    if (!file.max_hotend_temp || !file.max_bed_temp) {
        std::optional<double> hotend;
        std::optional<double> bed;
        if (head != nullptr) {
            hotend = head->max_hotend_temp;
            bed = head->max_bed_temp;
        }
        if (metadata && metadata->first_layer_extr_temp > 0.0) {
            raise(hotend, metadata->first_layer_extr_temp);
        }
        if (metadata && metadata->first_layer_bed_temp > 0.0) {
            raise(bed, metadata->first_layer_bed_temp);
        }
        if (!file.max_hotend_temp) {
            file.max_hotend_temp = hotend;
        }
        if (!file.max_bed_temp) {
            file.max_bed_temp = bed;
        }
    }
    if (file.printer_model.empty() && head != nullptr) {
        file.printer_model = head->printer_model;
    }
    if (file.nozzle_diameters.empty() && head != nullptr) {
        file.nozzle_diameters = head->nozzle_diameters;
    }
    if (file.nozzle_diameters.empty() && metadata) {
        if (!metadata->nozzle_diameter_list.empty()) {
            file.nozzle_diameters = metadata->nozzle_diameter_list;
        } else if (metadata->nozzle_diameter > 0.0) {
            file.nozzle_diameters.push_back(metadata->nozzle_diameter);
        }
    }
    const auto none_known = [](const std::vector<std::string>& types) {
        return std::all_of(types.begin(), types.end(),
                           [](const std::string& t) { return fit_nozzle_volume_type(t).empty(); });
    };
    if (none_known(file.nozzle_volume_types) && head != nullptr) {
        file.nozzle_volume_types = head->nozzle_volume_types;
    }
    if (none_known(file.nozzle_volume_types) && metadata) {
        // Listed by filament; filament i is taken to print on extruder i.
        file.nozzle_volume_types = known_volume_types(metadata->filament_volume_types);
    }
    if (!tools.empty()) {
        file.tools = std::move(tools);
    }
    return file;
}

} // namespace helix
