// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_edit_list.h"

#include "utf8_validation.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "hv/json.hpp"

namespace helix::gcode {

namespace {

using json = nlohmann::json;

constexpr const char* kToolRemap = "tool_remap";

struct ActionSpec {
    const char* name;
    ModificationType type;
    std::initializer_list<const char*> required;
    std::initializer_list<const char*> optional;
};

// Must match _ARG_KEYS in moonraker-plugin/helix_edits.py.
const ActionSpec kActions[] = {
    {"comment_out", ModificationType::COMMENT_OUT, {}, {"comment", "end_line"}},
    {"delete", ModificationType::DELETE, {}, {"end_line"}},
    {"inject_before", ModificationType::INJECT_BEFORE, {"gcode"}, {}},
    {"inject_after", ModificationType::INJECT_AFTER, {"gcode"}, {}},
    {"replace", ModificationType::REPLACE, {"gcode"}, {"comment"}},
};

const ActionSpec* spec_for(ModificationType type) {
    for (const auto& spec : kActions) {
        if (spec.type == type) {
            return &spec;
        }
    }
    return nullptr;
}

const ActionSpec* spec_for(std::string_view name) {
    for (const auto& spec : kActions) {
        if (name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

bool takes(std::initializer_list<const char*> keys, std::string_view key) {
    for (const char* k : keys) {
        if (key == k) {
            return true;
        }
    }
    return false;
}

bool ranged(ModificationType type) {
    return type == ModificationType::COMMENT_OUT || type == ModificationType::DELETE;
}

/// A JSON integer (never a bool) in [0, INT32_MAX].
std::optional<int64_t> small_int(const json& v) {
    int64_t out = -1;
    if (v.is_number_unsigned()) {
        const auto u = v.get<uint64_t>();
        out = u > INT32_MAX ? -1 : static_cast<int64_t>(u);
    } else if (v.is_number_integer()) {
        out = v.get<int64_t>();
    }
    if (out < 0 || out > INT32_MAX) {
        return std::nullopt;
    }
    return out;
}

const std::string* string_member(const json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get_ptr<const std::string*>() : nullptr;
}

std::optional<json> remap_edit(const std::map<int, int>& remap) {
    json map = json::object();
    for (const auto& [logical, physical] : remap) {
        if (logical < 0 || physical < 0) {
            return std::nullopt;
        }
        map[std::to_string(logical)] = physical;
    }
    return json{{"line_number", 0},
                {"expect_raw_line", ""},
                {"action", kToolRemap},
                {"args", {{"map", map}}}};
}

std::optional<std::map<int, int>> parse_map(const json& map) {
    if (!map.is_object() || map.empty()) {
        return std::nullopt;
    }
    std::map<int, int> remap;
    for (const auto& [key, value] : map.items()) {
        if (key.empty() || key.size() > 9 ||
            key.find_first_not_of("0123456789") != std::string::npos) {
            return std::nullopt;
        }
        const auto physical = small_int(value);
        if (!physical) {
            return std::nullopt;
        }
        int logical = 0;
        for (const char c : key) {
            logical = logical * 10 + (c - '0');
        }
        remap[logical] = static_cast<int>(*physical);
    }
    return remap;
}

} // namespace

std::optional<json> to_server_edits(const EditList& list) {
    if (list.modifications.empty() == list.remap.empty()) {
        return std::nullopt;
    }
    if (!list.remap.empty()) {
        auto edit = remap_edit(list.remap);
        if (!edit) {
            return std::nullopt;
        }
        return json::array({*edit});
    }

    json edits = json::array();
    for (const auto& mod : list.modifications) {
        const ActionSpec* spec = spec_for(mod.type);
        if (!spec || mod.line_number == 0 || mod.line_number > INT32_MAX ||
            mod.end_line_number > INT32_MAX || !text_io::is_valid_utf8(mod.expected_line)) {
            return std::nullopt;
        }
        json args = json::object();
        if (mod.end_line_number > 0) {
            if (!ranged(mod.type) || mod.end_line_number < mod.line_number) {
                return std::nullopt;
            }
            args["end_line"] = mod.end_line_number;
        }
        if (takes(spec->required, "gcode")) {
            if (!text_io::is_valid_utf8(mod.gcode)) {
                return std::nullopt;
            }
            args["gcode"] = mod.gcode;
        }
        if (takes(spec->optional, "comment") && !mod.comment.empty()) {
            if (!text_io::is_valid_utf8(mod.comment)) {
                return std::nullopt;
            }
            args["comment"] = mod.comment;
        }
        edits.push_back({{"line_number", mod.line_number},
                         {"expect_raw_line", mod.expected_line},
                         {"action", spec->name},
                         {"args", std::move(args)}});
    }
    return edits;
}

std::optional<EditList> from_server_edits(const json& edits) {
    if (!edits.is_array()) {
        return std::nullopt;
    }
    EditList list;
    for (const auto& edit : edits) {
        if (!edit.is_object()) {
            return std::nullopt;
        }
        const std::string* action = string_member(edit, "action");
        if (!action) {
            return std::nullopt;
        }
        const auto args_it = edit.find("args");
        const json args = args_it == edit.end() ? json::object() : *args_it;
        if (!args.is_object()) {
            return std::nullopt;
        }
        std::string expect;
        if (const auto it = edit.find("expect_raw_line"); it != edit.end()) {
            if (!it->is_string()) {
                return std::nullopt;
            }
            expect = it->get<std::string>();
        }
        const auto line_it = edit.find("line_number");
        const auto line = line_it == edit.end() ? std::nullopt : small_int(*line_it);

        if (*action == kToolRemap) {
            if (edits.size() != 1 || !line || *line != 0 || !expect.empty() || args.size() != 1 ||
                !args.contains("map")) {
                return std::nullopt;
            }
            auto remap = parse_map(args["map"]);
            if (!remap) {
                return std::nullopt;
            }
            list.remap = std::move(*remap);
            continue;
        }

        const ActionSpec* spec = spec_for(*action);
        if (!spec || !line || *line < 1) {
            return std::nullopt;
        }
        for (const auto& [key, value] : args.items()) {
            if (!takes(spec->required, key) && !takes(spec->optional, key)) {
                return std::nullopt;
            }
        }
        Modification mod{spec->type, static_cast<size_t>(*line), 0, "", "", std::move(expect)};
        for (const char* key : spec->required) {
            if (!args.contains(key)) {
                return std::nullopt;
            }
        }
        if (args.contains("gcode")) {
            const std::string* gcode = string_member(args, "gcode");
            if (!gcode) {
                return std::nullopt;
            }
            mod.gcode = *gcode;
        }
        if (args.contains("comment")) {
            const std::string* comment = string_member(args, "comment");
            if (!comment) {
                return std::nullopt;
            }
            mod.comment = *comment;
        }
        if (args.contains("end_line")) {
            const auto end = small_int(args["end_line"]);
            if (!end || *end < *line) {
                return std::nullopt;
            }
            mod.end_line_number = static_cast<size_t>(*end);
        }
        list.modifications.push_back(std::move(mod));
    }
    return list;
}

uint32_t print_edited_timeout_ms(uint64_t file_size) {
    const uint64_t bytes = file_size > 0 ? file_size : kPrintEditedAssumedBytes;
    const uint64_t mib = (bytes + (1024 * 1024 - 1)) / (1024 * 1024);
    const uint64_t ms = kPrintEditedBaseTimeoutMs + mib * kPrintEditedTimeoutMsPerMiB;
    return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
}

EditedPrintFailure classify_print_edited_failure(const MoonrakerError& error) {
    if (error.type == MoonrakerErrorType::TIMEOUT ||
        error.type == MoonrakerErrorType::CONNECTION_LOST) {
        return EditedPrintFailure::Unanswered;
    }
    // Moonraker answers an unregistered method with -32601 "Method not found"; the
    // plugin answers an edit list it cannot take with 400 "Invalid edit list" and a
    // switched-off or unavailable feature with 503. Each refuses before anything runs.
    const auto starts_with = [&error](std::string_view prefix) {
        return error.message.compare(0, prefix.size(), prefix) == 0;
    };
    if ((error.code == -32601 && starts_with("Method not found")) ||
        (error.code == 400 && starts_with("Invalid edit list")) || error.code == 503) {
        return EditedPrintFailure::Unsupported;
    }
    return EditedPrintFailure::Refused;
}

EditTransport choose_edit_transport(PluginFeatureState server_edits, bool archive_plate,
                                    bool expressible) {
    if (server_edits == PluginFeatureState::Ready && !archive_plate && expressible) {
        return EditTransport::Plugin;
    }
    return EditTransport::DownloadRewrite;
}

} // namespace helix::gcode
