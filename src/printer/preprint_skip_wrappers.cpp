// SPDX-License-Identifier: GPL-3.0-or-later

#include "preprint_skip_wrappers.h"

#include "bed_mesh_presence.h"
#include "core/text_io.h"

namespace helix::skip_wrappers {

namespace {

constexpr const char* kBasePrefix = "_HELIX_BASE_";

constexpr Op kAllOps[] = {Op::BedMesh, Op::Qgl, Op::ZTilt};

/// One gcode_macro section an Op writes. A consuming wrapper skips its command
/// when the flag is 0 and restores the flag; a guarding one only skips.
struct Wrapper {
    const char* command;
    bool consumes;
};

std::vector<Wrapper> wrappers_for(Op op) {
    switch (op) {
    case Op::BedMesh:
        // PRINT_START commonly clears the mesh right before calibrating, which
        // would throw away the mesh a skip means to keep.
        return {{"BED_MESH_CALIBRATE", true}, {"BED_MESH_CLEAR", false}};
    case Op::Qgl:
        return {{"QUAD_GANTRY_LEVEL", true}};
    case Op::ZTilt:
        return {{"Z_TILT_ADJUST", true}};
    }
    return {};
}

const char* label_for(Op op) {
    switch (op) {
    case Op::BedMesh:
        return "bed mesh";
    case Op::Qgl:
        return "quad gantry level";
    case Op::ZTilt:
        return "z tilt";
    }
    return "";
}

std::string set_flag_line(Op op, const std::string& value) {
    return std::string("SET_GCODE_VARIABLE MACRO=") + kPrepMacro + " VARIABLE=" + flag_for(op) +
           " VALUE=" + value;
}

std::string flag_expr(Op op) {
    return std::string("printer[\"gcode_macro ") + kPrepMacro + "\"]." + flag_for(op) + "|int";
}

/// The gcode body lines, unindented. Each command on its own line: `;` starts
/// a comment in a config value.
std::vector<std::string> wrapper_body(Op op, const Wrapper& w) {
    const std::string base = std::string(kBasePrefix) + w.command + " {rawparams}";
    if (!w.consumes) {
        return {"{% if " + flag_expr(op) + " == 1 %}", base, "{% endif %}"};
    }
    return {"{% if " + flag_expr(op) + " == 0 %}",
            set_flag_line(op, "1"),
            std::string("{action_respond_info(\"HelixScreen: ") + label_for(op) +
                " skipped for this print\")}",
            "{% else %}",
            base,
            "{% endif %}"};
}

/// Lines of a config value with whitespace and blank lines dropped: Klipper
/// strips continuation-line indentation when it parses the file.
std::vector<std::string> normalized_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line(helix::text_io::trim(std::string_view(text).substr(start, end - start)));
        if (!line.empty()) {
            out.push_back(std::move(line));
        }
        start = end + 1;
    }
    return out;
}

const nlohmann::json* section(const nlohmann::json& settings, const std::string& name) {
    if (!settings.is_object()) {
        return nullptr;
    }
    auto it = settings.find(helix::text_io::to_lower(name));
    return it != settings.end() && it->is_object() ? &*it : nullptr;
}

const nlohmann::json* macro_section(const nlohmann::json& settings, const char* command) {
    return section(settings, std::string("gcode_macro ") + command);
}

bool wrapper_intact(const nlohmann::json& settings, Op op, const Wrapper& w) {
    const nlohmann::json* s = macro_section(settings, w.command);
    if (s == nullptr) {
        return false;
    }
    auto rename = s->find("rename_existing");
    auto gcode = s->find("gcode");
    if (rename == s->end() || !rename->is_string() || gcode == s->end() || !gcode->is_string()) {
        return false;
    }
    return helix::text_io::to_upper(rename->get<std::string>()) ==
               std::string(kBasePrefix) + w.command &&
           normalized_lines(gcode->get<std::string>()) == wrapper_body(op, w);
}

bool has_builtin(const nlohmann::json& settings, Op op) {
    switch (op) {
    case Op::BedMesh:
        return section(settings, "bed_mesh") != nullptr;
    case Op::Qgl:
        return section(settings, "quad_gantry_level") != nullptr;
    case Op::ZTilt:
        return section(settings, "z_tilt") != nullptr || section(settings, "z_tilt_ng") != nullptr;
    }
    return false;
}

} // namespace

const char* command_for(Op op) {
    return wrappers_for(op).front().command;
}

std::vector<std::string> wrapped_commands(const std::vector<Op>& ops) {
    std::vector<std::string> out;
    for (Op op : ops) {
        for (const auto& w : wrappers_for(op)) {
            out.emplace_back(w.command);
        }
    }
    return out;
}

const char* flag_for(Op op) {
    switch (op) {
    case Op::BedMesh:
        return "run_bed_mesh";
    case Op::Qgl:
        return "run_qgl";
    case Op::ZTilt:
        return "run_z_tilt";
    }
    return "";
}

bool ours_intact(const nlohmann::json& configfile_settings, Op op) {
    for (const auto& w : wrappers_for(op)) {
        if (!wrapper_intact(configfile_settings, op, w)) {
            return false;
        }
    }
    return true;
}

std::vector<Op> active(const nlohmann::json& configfile_settings) {
    std::vector<Op> ops;
    if (macro_section(configfile_settings, kPrepMacro) == nullptr) {
        return ops;
    }
    for (Op op : kAllOps) {
        if (ours_intact(configfile_settings, op)) {
            ops.push_back(op);
        }
    }
    return ops;
}

namespace {

bool fold_bool(bool& slot, bool value) {
    const bool changed = slot != value;
    slot = value;
    return changed;
}

/// `<object>.applied` from a frame, when the frame carries it.
const nlohmann::json* applied_field(const nlohmann::json& status, const char* object) {
    auto obj = status.find(object);
    if (obj == status.end() || !obj->is_object()) {
        return nullptr;
    }
    auto it = obj->find("applied");
    return it != obj->end() && it->is_boolean() ? &*it : nullptr;
}

const char* option_id(Op op) {
    switch (op) {
    case Op::BedMesh:
        return "bed_mesh";
    case Op::Qgl:
        return "qgl";
    case Op::ZTilt:
        return "z_tilt";
    }
    return "";
}

} // namespace

bool update_gates(Gates& gates, const nlohmann::json& status) {
    if (!status.is_object()) {
        return false;
    }
    bool changed = false;
    if (auto mesh = status.find("bed_mesh"); mesh != status.end() && mesh->is_object()) {
        if (auto m = mesh->find("probed_matrix"); m != mesh->end()) {
            changed |= fold_bool(gates.mesh_loaded, probed_matrix_has_mesh(*m));
        }
    }
    if (const auto* a = applied_field(status, "quad_gantry_level")) {
        changed |= fold_bool(gates.qgl_applied, a->get<bool>());
    }
    for (const char* object : {"z_tilt", "z_tilt_ng"}) {
        if (const auto* a = applied_field(status, object)) {
            changed |= fold_bool(gates.z_tilt_applied, a->get<bool>());
        }
    }
    auto prep = status.find(std::string("gcode_macro ") + kPrepMacro);
    if (prep != status.end() && prep->is_object()) {
        for (Op op : kAllOps) {
            auto v = prep->find(flag_for(op));
            if (v != prep->end() && v->is_number()) {
                changed |=
                    fold_bool(gates.skip_pending[static_cast<size_t>(op)], v->get<int>() == 0);
            }
        }
    }
    return changed;
}

std::vector<Op> offerable(const std::vector<Op>& active, const Gates& gates) {
    std::vector<Op> ops;
    for (Op op : active) {
        const bool open = op == Op::BedMesh ? gates.mesh_loaded
                          : op == Op::Qgl   ? gates.qgl_applied
                                            : gates.z_tilt_applied;
        if (open) {
            ops.push_back(op);
        }
    }
    return ops;
}

bool any_skip_pending(const Gates& gates) {
    for (bool pending : gates.skip_pending) {
        if (pending) {
            return true;
        }
    }
    return false;
}

nlohmann::json status_fields(const std::vector<Op>& active, bool has_z_tilt) {
    nlohmann::json fields = nlohmann::json::object();
    if (active.empty()) {
        return fields;
    }
    nlohmann::json flags = nlohmann::json::array();
    for (Op op : active) {
        flags.push_back(flag_for(op));
        if (op == Op::Qgl) {
            fields["quad_gantry_level"] = nlohmann::json::array({"applied"});
        } else if (op == Op::ZTilt) {
            fields[has_z_tilt ? "z_tilt" : "z_tilt_ng"] = nlohmann::json::array({"applied"});
        }
    }
    fields[std::string("gcode_macro ") + kPrepMacro] = std::move(flags);
    return fields;
}

PrePrintOption option_for(Op op) {
    PrePrintOption opt;
    opt.id = option_id(op);
    opt.category = PrePrintCategory::Mechanical;
    opt.order = static_cast<int>(op);
    opt.default_enabled = true; // PRINT_START runs the step unless told to skip it
    opt.requires_macro = kPrepMacro;
    opt.strategy_kind = PrePrintStrategyKind::PreStartGcode;
    PrePrintStrategyPreStartGcode line;
    line.gcode_template = set_flag_line(op, "{value}");
    line.emit_when_disabled = true;
    opt.strategy = std::move(line);
    return opt;
}

bool is_wrapper_option(const PrePrintOption& opt) {
    return opt.requires_macro == kPrepMacro;
}

LoadWatch::Verdict LoadWatch::feed(bool ready, bool error) {
    if (error) {
        return Verdict::Failed;
    }
    if (!ready) {
        left_ready_ = true;
        return Verdict::Waiting;
    }
    return left_ready_ ? Verdict::Loaded : Verdict::Waiting;
}

std::vector<Op> wrappable(const nlohmann::json& configfile_settings) {
    std::vector<Op> ops;
    for (Op op : kAllOps) {
        if (!has_builtin(configfile_settings, op)) {
            continue;
        }
        bool foreign = false;
        for (const auto& w : wrappers_for(op)) {
            if (macro_section(configfile_settings, w.command) != nullptr &&
                !wrapper_intact(configfile_settings, op, w)) {
                foreign = true;
            }
        }
        if (!foreign) {
            ops.push_back(op);
        }
    }
    return ops;
}

std::string generate(const std::vector<Op>& ops) {
    if (ops.empty()) {
        return "";
    }
    std::string out =
        "# helix_skips v1 - generated by HelixScreen, which rewrites this file.\n"
        "# Lets the print details panel skip a leveling step for one print\n"
        "# without editing PRINT_START. Uninstall the helper macros to remove it.\n\n";

    out += std::string("[gcode_macro ") + kPrepMacro + "]\n";
    out += "description: Reset the HelixScreen per-print skips\n";
    for (Op op : ops) {
        out += std::string("variable_") + flag_for(op) + ": 1\n";
    }
    out += "gcode:\n";
    for (Op op : ops) {
        out += "    " + set_flag_line(op, "1") + "\n";
    }

    for (Op op : ops) {
        for (const auto& w : wrappers_for(op)) {
            out += std::string("\n[gcode_macro ") + w.command + "]\n";
            out += std::string("rename_existing: ") + kBasePrefix + w.command + "\n";
            out += "description: Wrapped by HelixScreen for per-print skips\n";
            out += "gcode:\n";
            for (const auto& line : wrapper_body(op, w)) {
                out += "    " + line + "\n";
            }
        }
    }
    return out;
}

} // namespace helix::skip_wrappers
