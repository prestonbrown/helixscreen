// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pre_print_option.h"

#include "json_utils.h"
#include "macro_param_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace {

// JSON-string -> enum mappings. Keep these centralized so the parser and any
// future serializer agree on the wire format.

std::optional<PrePrintCategory> parse_category(const std::string& s) {
    if (s == "mechanical")
        return PrePrintCategory::Mechanical;
    if (s == "quality")
        return PrePrintCategory::Quality;
    if (s == "monitoring")
        return PrePrintCategory::Monitoring;
    return std::nullopt;
}

std::optional<PrePrintStrategyKind> parse_strategy_kind(const std::string& s) {
    if (s == "macro_param")
        return PrePrintStrategyKind::MacroParam;
    if (s == "pre_start_gcode")
        return PrePrintStrategyKind::PreStartGcode;
    if (s == "queue_ahead_job")
        return PrePrintStrategyKind::QueueAheadJob;
    if (s == "runtime_command")
        return PrePrintStrategyKind::RuntimeCommand;
    return std::nullopt;
}

// Replace every occurrence of `needle` in `haystack` with `replacement`.
std::string replace_all(std::string haystack, const std::string& needle,
                        const std::string& replacement) {
    if (needle.empty())
        return haystack;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        haystack.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return haystack;
}

} // namespace

const PrePrintOption* PrePrintOptionSet::find(const std::string& id) const {
    auto it = std::find_if(options.begin(), options.end(),
                           [&](const PrePrintOption& o) { return o.id == id; });
    return (it != options.end()) ? &(*it) : nullptr;
}

std::optional<PrePrintOption> parse_pre_print_option(const nlohmann::json& j) {
    if (!j.is_object()) {
        spdlog::warn("[PrePrintOption] Skipping option: JSON value is not an object");
        return std::nullopt;
    }

    PrePrintOption opt;

    // --- id (required) ---
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty()) {
        spdlog::warn("[PrePrintOption] Skipping option: missing or empty 'id' field");
        return std::nullopt;
    }
    opt.id = j["id"].get<std::string>();

    // --- strategy (required) ---
    if (!j.contains("strategy") || !j["strategy"].is_string()) {
        spdlog::warn("[PrePrintOption] Skipping option '{}': missing 'strategy' field", opt.id);
        return std::nullopt;
    }
    auto kind = parse_strategy_kind(j["strategy"].get<std::string>());
    if (!kind) {
        spdlog::warn("[PrePrintOption] Skipping option '{}': unknown strategy '{}'", opt.id,
                     j["strategy"].get<std::string>());
        return std::nullopt;
    }
    opt.strategy_kind = *kind;

    // --- optional metadata ---
    //
    // safe_* rather than .value() throughout this function: .value() throws
    // type_error.302 on a key that is PRESENT with a null value (a missing key
    // is fine), and there is no try/catch anywhere on this path — not here, not
    // in parse_pre_print_option_set, not at the printer_detector.cpp call site.
    // A null would have unwound out of printer detection entirely. The existing
    // design already degrades per-option (return nullopt, caller skips it), so
    // the required-field checks below now do that job for nulls too: a null
    // `param_name` reads as empty and is rejected with the accurate "requires
    // non-empty 'param_name'" message instead of throwing.
    opt.label_key = helix::json_util::safe_string(j, "label_key");
    opt.description_key = helix::json_util::safe_string(j, "description_key");
    opt.icon = helix::json_util::safe_string(j, "icon");
    opt.default_enabled = helix::json_util::safe_bool(j, "default_enabled", false);
    opt.order = helix::json_util::safe_int(j, "order", 0);
    opt.requires_macro = helix::json_util::safe_string(j, "requires_macro");

    // category — defaults to Mechanical if absent or unknown (warned).
    if (j.contains("category") && j["category"].is_string()) {
        auto cat = parse_category(j["category"].get<std::string>());
        if (cat) {
            opt.category = *cat;
        } else {
            spdlog::warn("[PrePrintOption] Option '{}': unknown category '{}', defaulting to "
                         "'mechanical'",
                         opt.id, j["category"].get<std::string>());
        }
    }

    // --- strategy payload ---
    switch (opt.strategy_kind) {
    case PrePrintStrategyKind::MacroParam: {
        PrePrintStrategyMacroParam p;
        p.param_name = helix::json_util::safe_string(j, "param_name");
        p.enable_value = helix::json_util::safe_string(j, "enable_value");
        p.skip_value = helix::json_util::safe_string(j, "skip_value");
        p.default_value = helix::json_util::safe_string(j, "default_value");
        // Optional adaptive-mesh modifier (see PrePrintStrategyMacroParam doc).
        p.adaptive_param = helix::json_util::safe_string(j, "adaptive_param");
        p.adaptive_value = helix::json_util::safe_string(j, "adaptive_value", "1");
        if (p.param_name.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': MacroParam strategy requires "
                         "non-empty 'param_name'",
                         opt.id);
            return std::nullopt;
        }
        // Empty enable/skip values would render as `KEY=` (no value) — silent
        // garbage that the macro will misparse. Reject at parse time.
        if (p.enable_value.empty() || p.skip_value.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': MacroParam strategy requires "
                         "non-empty 'enable_value' and 'skip_value' (got enable='{}' skip='{}')",
                         opt.id, p.enable_value, p.skip_value);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    case PrePrintStrategyKind::PreStartGcode: {
        PrePrintStrategyPreStartGcode p;
        p.gcode_template = helix::json_util::safe_string(j, "gcode_template");
        p.emit_when_disabled = helix::json_util::safe_bool(j, "emit_when_disabled", true);
        if (p.gcode_template.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': PreStartGcode strategy requires "
                         "non-empty 'gcode_template'",
                         opt.id);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    case PrePrintStrategyKind::QueueAheadJob: {
        PrePrintStrategyQueueAheadJob p;
        p.job_path = helix::json_util::safe_string(j, "job_path");
        if (p.job_path.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': QueueAheadJob strategy requires "
                         "non-empty 'job_path'",
                         opt.id);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    case PrePrintStrategyKind::RuntimeCommand: {
        PrePrintStrategyRuntimeCommand p;
        p.command_enabled = helix::json_util::safe_string(j, "command_enabled");
        p.command_disabled = helix::json_util::safe_string(j, "command_disabled");
        if (p.command_enabled.empty() && p.command_disabled.empty()) {
            spdlog::warn("[PrePrintOption] Skipping option '{}': RuntimeCommand strategy requires "
                         "at least one of 'command_enabled' / 'command_disabled'",
                         opt.id);
            return std::nullopt;
        }
        opt.strategy = std::move(p);
        break;
    }
    }

    return opt;
}

PrePrintOptionSet parse_pre_print_option_set(const nlohmann::json& j) {
    PrePrintOptionSet set;

    if (!j.is_object()) {
        spdlog::warn("[PrePrintOption] Option set JSON is not an object; returning empty set");
        return set;
    }

    set.macro_name = helix::json_util::safe_string(j, "macro_name");
    set.setup_gcode = helix::json_util::safe_string(j, "setup_gcode");

    if (j.contains("options")) {
        const auto& arr = j["options"];
        if (!arr.is_array()) {
            spdlog::warn("[PrePrintOption] 'options' is not an array; ignoring");
        } else {
            set.options.reserve(arr.size());
            for (const auto& entry : arr) {
                if (auto parsed = parse_pre_print_option(entry); parsed) {
                    set.options.push_back(std::move(*parsed));
                }
            }
        }
    }

    std::sort(set.options.begin(), set.options.end(),
              [](const PrePrintOption& a, const PrePrintOption& b) {
                  if (a.category != b.category) {
                      return static_cast<int>(a.category) < static_cast<int>(b.category);
                  }
                  return a.order < b.order;
              });

    return set;
}

std::string render_macro_param(const PrePrintOption& opt, bool enabled) {
    const auto* p = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
    if (!p) {
        spdlog::warn("[PrePrintOption] render_macro_param called on option '{}' with non-"
                     "MacroParam strategy",
                     opt.id);
        return {};
    }
    const std::string& value = enabled ? p->enable_value : p->skip_value;
    return p->param_name + "=" + value;
}

std::string render_pre_start_gcode(const PrePrintOption& opt, bool enabled,
                                   const PreStartGcodeContext& ctx) {
    const auto* p = std::get_if<PrePrintStrategyPreStartGcode>(&opt.strategy);
    if (!p) {
        spdlog::warn("[PrePrintOption] render_pre_start_gcode called on option '{}' with non-"
                     "PreStartGcode strategy",
                     opt.id);
        return {};
    }
    std::string out = replace_all(p->gcode_template, "{value}", enabled ? "1" : "0");

    // {?ext}...{/?} — a segment emitted only when the extruder temperature
    // is known. Some firmwares (Creality K1 family) read that parameter
    // through Python get_float(minval=180), which rejects a literal 0 as out
    // of range, so "unknown" has to mean "omit the parameter" there rather
    // than the bare 0 the Jinja-guarded macros tolerate. The bed parameter
    // needs no marker: get_float(minval=0) accepts 0, and a bed target of 0
    // (unheated bed) is a real, slicable configuration that must pass
    // through as-is rather than read as "unknown".
    static constexpr std::string_view EXT_OPEN = "{?ext}";
    static constexpr std::string_view SEG_CLOSE = "{/?}";
    if (ctx.extruder_temp > 0) {
        out = replace_all(std::move(out), std::string(EXT_OPEN), "");
        out = replace_all(std::move(out), std::string(SEG_CLOSE), "");
    } else {
        for (size_t open = out.find(EXT_OPEN); open != std::string::npos;
             open = out.find(EXT_OPEN)) {
            const size_t close = out.find(SEG_CLOSE, open);
            if (close == std::string::npos) {
                break; // unmatched marker: leave the rest untouched
            }
            out.erase(open, close - open + SEG_CLOSE.size());
        }
    }

    out = replace_all(std::move(out), "{file}", ctx.filename);
    out = replace_all(std::move(out), "{bed_temp}", std::to_string(ctx.bed_temp));
    return replace_all(std::move(out), "{extruder_temp}", std::to_string(ctx.extruder_temp));
}

bool is_macro_gate_closed(const PrePrintOption& opt) {
    if (opt.requires_macro.empty()) {
        return false;
    }
    return !helix::MacroParamCache::instance().has_macro(opt.requires_macro);
}

PrePrintOptionSet filter_macro_gated_options(const PrePrintOptionSet& input) {
    PrePrintOptionSet out = input; // copies macro_name + setup_gcode + options
    out.options.erase(
        std::remove_if(out.options.begin(), out.options.end(),
                       [](const PrePrintOption& o) { return is_macro_gate_closed(o); }),
        out.options.end());
    return out;
}
