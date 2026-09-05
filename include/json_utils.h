// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "hv/json.hpp"

namespace helix::json_util {

/// Safely extract a string from a JSON field that may be null.
/// nlohmann .value("key", "") throws type_error.302 when the field is JSON null.
///
/// @param accept_number  Also accept a JSON integer, returned as its decimal
///        text. Off by default because a number arriving where a string was
///        declared is usually a bug worth defaulting away. Some firmwares do
///        send one anyway - a field they format back unquoted makes the round
///        trip as a number even though their own schema calls it a string -
///        and a reader that has confirmed that is the case opts in here rather
///        than hand-rolling the widened copy.
inline std::string safe_string(const nlohmann::json& j, const char* key,
                               const std::string& def = "", bool accept_number = false) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    const auto& v = j[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (accept_number && v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    return def;
}

// Forward declaration: safe_int is defined below the detail:: converters it uses,
// but is declared here to keep the four original helpers together.
inline int safe_int(const nlohmann::json& j, const char* key, int def = 0);

/// Safely extract a float from a JSON field that may be number, string, or null.
inline float safe_float(const nlohmann::json& j, const char* key, float def = 0.0f) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    const auto& v = j[key];
    float result = def;
    if (v.is_number()) {
        result = v.get<float>();
    } else if (v.is_string()) {
        try {
            result = std::stof(v.get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return std::isfinite(result) ? result : def;
}

/// Safely extract a double from a JSON field that may be number, string, or null.
inline double safe_double(const nlohmann::json& j, const char* key, double def = 0.0) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    const auto& v = j[key];
    double result = def;
    if (v.is_number()) {
        result = v.get<double>();
    } else if (v.is_string()) {
        try {
            result = std::stod(v.get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return std::isfinite(result) ? result : def;
}

/// Safely extract a bool from a JSON field that may be bool, number, string, or null.
///
/// Coercion policy (deliberate — do not widen without thought):
///   - JSON bool              -> used directly
///   - JSON number            -> 0 is false, any other finite value is true.
///                               Non-finite (NaN/Inf) returns `def`.
///   - JSON string            -> ONLY an exact, case-insensitive match against
///                               "true"/"false", "1"/"0", "yes"/"no", "on"/"off"
///                               is honored. Anything else returns `def`.
///   - null / missing / other -> `def`
///
/// The string whitelist is closed on purpose. The tempting shorthand — treating
/// any non-empty string as true — reads the string "false" as TRUE, which is
/// strictly worse than having no value at all. An unrecognized spelling is a
/// payload we do not understand, so we return the caller's default rather than
/// guess at it.
///
/// Prefer `.find()` + `is_boolean()` at sites where a wrong-typed value should
/// be treated as "no reading available" and skipped entirely, rather than
/// silently collapsing to `def` — see ams_backend_snapmaker.cpp for that idiom.
/// Use this helper when a default genuinely is the right answer.
inline bool safe_bool(const nlohmann::json& j, const char* key, bool def = false) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    const auto& v = j[key];
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_number()) {
        const double d = v.get<double>();
        if (!std::isfinite(d)) {
            return def;
        }
        return d != 0.0;
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (s == "true" || s == "1" || s == "yes" || s == "on") {
            return true;
        }
        if (s == "false" || s == "0" || s == "no" || s == "off") {
            return false;
        }
        return def;
    }
    return def;
}

namespace detail {

/// Convert a JSON value to int64_t. Returns false (leaving `out` untouched) if
/// the value is not a number/numeric-string or does not fit in an int64_t.
inline bool to_i64(const nlohmann::json& v, std::int64_t& out) {
    if (v.is_number_unsigned()) {
        const std::uint64_t u = v.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(u);
        return true;
    }
    if (v.is_number_integer()) {
        out = v.get<std::int64_t>();
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        // -2^63 is exactly representable as a double; 2^63 is too, hence the
        // asymmetric comparison (values >= 2^63 do not fit).
        if (!std::isfinite(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
            return false;
        }
        out = static_cast<std::int64_t>(d);
        return true;
    }
    if (v.is_string()) {
        try {
            out = std::stoll(v.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

/// Convert a JSON value to uint64_t. Returns false (leaving `out` untouched) if
/// the value is not a number/numeric-string, is negative, or overflows.
inline bool to_u64(const nlohmann::json& v, std::uint64_t& out) {
    if (v.is_number_unsigned()) {
        out = v.get<std::uint64_t>();
        return true;
    }
    if (v.is_number_integer()) {
        const std::int64_t i = v.get<std::int64_t>();
        if (i < 0) {
            return false;
        }
        out = static_cast<std::uint64_t>(i);
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        // 2^64 is exactly representable as a double; values >= it do not fit.
        if (!std::isfinite(d) || d < 0.0 || d >= 18446744073709551616.0) {
            return false;
        }
        out = static_cast<std::uint64_t>(d);
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        // std::stoull silently WRAPS a negative literal ("-1" -> 2^64-1), so
        // reject a sign explicitly before parsing.
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                continue;
            }
            if (c == '-') {
                return false;
            }
            break;
        }
        try {
            out = std::stoull(s);
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

} // namespace detail

/// Safely extract an int from a JSON field that may be number, string, or null.
/// Returns `def` for null/missing/wrong-type, for non-finite floats, and for
/// values outside the int range — a JSON 5000000000 yields `def`, not a
/// truncated 705032704.
inline int safe_int(const nlohmann::json& j, const char* key, int def) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    std::int64_t out = 0;
    if (!detail::to_i64(j[key], out)) {
        return def;
    }
    if (out < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        out > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return def;
    }
    return static_cast<int>(out);
}

/// Safely extract an int64_t from a JSON field that may be number, string, or null.
/// Returns `def` for null/missing/wrong-type, for non-finite floats, and for
/// values outside the int64_t range.
inline std::int64_t safe_int64(const nlohmann::json& j, const char* key, std::int64_t def = 0) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    std::int64_t out = 0;
    return detail::to_i64(j[key], out) ? out : def;
}

/// Safely extract a uint64_t from a JSON field that may be number, string, or null.
/// Returns `def` for null/missing/wrong-type, for non-finite floats, for negative
/// values (including the string "-1", which std::stoull would otherwise wrap), and
/// for values outside the uint64_t range.
inline std::uint64_t safe_uint64(const nlohmann::json& j, const char* key, std::uint64_t def = 0) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    std::uint64_t out = 0;
    return detail::to_u64(j[key], out) ? out : def;
}

/// Safely extract a size_t from a JSON field that may be number, string, or null.
/// As safe_uint64, plus a narrowing guard: on 32-bit targets (AD5M/MIPS32, K1
/// armv7) a value that fits in a uint64_t but not a size_t returns `def` rather
/// than truncating.
inline std::size_t safe_size_t(const nlohmann::json& j, const char* key, std::size_t def = 0) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    std::uint64_t out = 0;
    if (!detail::to_u64(j[key], out)) {
        return def;
    }
    // Round-trip rather than compare against size_t's max. Where the two types
    // are the same width that comparison is tautologically false, and clang
    // diagnoses it under -Werror even though the if-constexpr discards the
    // branch — the body of a discarded branch is still analysed outside a
    // template. Narrow-then-widen is correct at every width and needs no guard.
    const std::size_t narrowed = static_cast<std::size_t>(out);
    if (static_cast<std::uint64_t>(narrowed) != out) {
        return def;
    }
    return narrowed;
}

/// The payload object of a Moonraker notification frame, or nullptr.
///
/// Notifications carry their payload as the single element of a `params` array
/// - `notify_history_changed` and `notify_filelist_changed` both do - and a
/// method callback is handed the whole JSON-RPC message, so every reader has to
/// walk down to it. Returns nullptr for a frame shaped any other way, which is
/// what a caller must treat as "nothing to act on"; reading the fields is left
/// to the caller, because each notification names different ones.
inline const nlohmann::json* notification_payload(const nlohmann::json& msg) {
    const auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_array() || params_it->empty()) {
        return nullptr;
    }
    const nlohmann::json& payload = (*params_it)[0];
    return payload.is_object() ? &payload : nullptr;
}

/// The `action` field of a Moonraker notification payload, or an empty string.
inline std::string notification_action(const nlohmann::json& msg) {
    const nlohmann::json* payload = notification_payload(msg);
    return payload ? safe_string(*payload, "action") : std::string();
}

} // namespace helix::json_util
