// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_error_envelope.h"

#include "json_utils.h"

#include <array>

#include "hv/json.hpp"

namespace helix {
namespace {

/// One firmware's envelope shape. Adding a firmware that wraps its errors
/// the same way is a row here and no call-site change.
struct EnvelopeShape {
    const char* code_key;
    const char* msg_key;
    /// The scheme of a code under this key that starts with
    /// `kErrorTablePrefix`; any other value is opaque whatever its key.
    FirmwareCodeScheme scheme;
};

constexpr std::array<EnvelopeShape, 2> kShapes{{
    // Creality K2 / CFS: {"code":"key849","msg":"...","values":[1,"A"]}
    {"code", "msg", FirmwareCodeScheme::ErrorTable},
    // Snapmaker U1: {"coded": "0003-0522-0002-0008", "oneshot": 0, "msg":"..."}
    {"coded", "msg", FirmwareCodeScheme::Opaque},
}};

/// What every KlipperErrorTable key starts with.
constexpr std::string_view kErrorTablePrefix = "key";

FirmwareCodeScheme scheme_of(const EnvelopeShape& shape, const std::string& code) {
    return shape.scheme == FirmwareCodeScheme::ErrorTable && code.rfind(kErrorTablePrefix, 0) == 0
               ? FirmwareCodeScheme::ErrorTable
               : FirmwareCodeScheme::Opaque;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// One past the `}` closing the object that opens at `open`, ignoring braces
/// inside string literals; npos when it never closes.
size_t object_end(std::string_view text, size_t open) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return i + 1;
        }
    }
    return std::string_view::npos;
}

/// The string member `key` of `obj`, or nullopt when absent or not a string.
std::optional<std::string> string_member(const nlohmann::json& obj, const char* key) {
    const auto it = helix::json_util::read_field<std::string>(obj, key);
    if (!it) {
        return std::nullopt;
    }
    return *it;
}

std::string trimmed_prefix(std::string_view before) {
    size_t n = before.size();
    while (n > 0 && is_space(before[n - 1])) {
        --n;
    }
    while (n > 0 && before[n - 1] == '!') {
        --n;
    }
    while (n > 0 && is_space(before[n - 1])) {
        --n;
    }
    return std::string(before.substr(0, n));
}

std::string meaningful_suffix(std::string_view after) {
    for (char c : after) {
        if (!is_space(c)) {
            return std::string(after);
        }
    }
    return {};
}

} // namespace

std::string FirmwareErrorEnvelope::splice(const std::string& body) const {
    std::string out = prefix;
    if (!out.empty() && !body.empty()) {
        out += ' ';
    }
    out += body;
    out += suffix;
    return out;
}

std::optional<FirmwareErrorEnvelope> find_firmware_error_envelope(std::string_view text) {
    for (size_t open = text.find('{'); open != std::string_view::npos;
         open = text.find('{', open + 1)) {
        // A JSON key must follow, which skips `{}` and Python-repr `{'...'}`
        // before paying for a parse.
        size_t k = open + 1;
        while (k < text.size() && is_space(text[k])) {
            ++k;
        }
        if (k >= text.size() || text[k] != '"') {
            continue;
        }
        const size_t end = object_end(text, open);
        if (end == std::string_view::npos) {
            continue;
        }
        const auto obj = nlohmann::json::parse(text.substr(open, end - open), nullptr, false);
        if (obj.is_discarded() || !obj.is_object()) {
            continue;
        }
        for (const auto& shape : kShapes) {
            auto code = string_member(obj, shape.code_key);
            if (!code) {
                continue;
            }
            const FirmwareCodeScheme scheme = scheme_of(shape, *code);
            auto msg = string_member(obj, shape.msg_key);
            // An error-table code has curated wording of its own, so the
            // firmware may omit the message; any other code means nothing
            // without one.
            if (!msg && scheme != FirmwareCodeScheme::ErrorTable) {
                continue;
            }
            FirmwareErrorEnvelope env;
            env.prefix = trimmed_prefix(text.substr(0, open));
            env.msg = msg ? std::move(*msg) : std::string();
            env.code = std::move(*code);
            env.scheme = scheme;
            if (const auto it = obj.find("values"); it != obj.end()) {
                env.values_json = it->dump();
            }
            env.suffix = meaningful_suffix(text.substr(end));
            return env;
        }
        // Not an envelope: resume after it, so an envelope nested inside
        // stays part of this object's raw text.
        open = end - 1;
    }
    return std::nullopt;
}

std::string decode_firmware_error_text(const std::string& text) {
    const auto env = find_firmware_error_envelope(text);
    return (env && !env->msg.empty()) ? env->text() : text;
}

} // namespace helix
