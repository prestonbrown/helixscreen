// src/printer/snapmaker_exceptions.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_exceptions.h"

#include "core/text_io.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <cctype>

#include "hv/json.hpp"

namespace helix::snapmaker {
namespace {

/// One fault we have wording for. Matched on (id, index, code); level is not
/// part of the identity, because the firmware may raise the same fault at a
/// different level depending on what it decides to do about it.
struct KnownException {
    int id;
    int index;
    int code;
    const char* message;
};

constexpr std::array<KnownException, 3> kKnown{{
    {522, 0, 17, "Power was lost during the print; the printer stopped and saved its progress"},
    {530, 0, 11,
     "Remove the PEI sheet from the bed, then start again: probing through the sheet "
     "gives wrong results"},
    {531, 0, 16, "That setting cannot be changed while a print is running"},
}};

/// Is `s[at..at+3]` four digits?
bool four_digits(const std::string& s, size_t at) {
    if (at + 4 > s.size()) {
        return false;
    }
    for (size_t i = at; i < at + 4; ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }
    return true;
}

/// Read one of the exceptions array's numeric fields. A field that arrives as
/// any other type reads as 0: the shape of a printer's payload is data, not a
/// contract violation to throw on, so a string field is never parsed.
int field_int(const nlohmann::json& entry, const char* key) {
    auto it = entry.find(key);
    return it != entry.end() && it->is_number_integer() ? it->get<int>() : 0;
}

/// Read the persistence flag. Both spellings are live: a Python bool
/// serialises true/false, an int serialises 0/1.
bool field_persistent(const nlohmann::json& entry, const char* key) {
    auto it = entry.find(key);
    if (it == entry.end()) {
        return false;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    return it->is_number_integer() && it->get<int>() != 0;
}

} // namespace

std::string format_exception_code(const ExceptionCode& c) {
    return fmt::format("{:04d}-{:04d}-{:04d}-{:04d}", c.level, c.id, c.index, c.code);
}

std::optional<ExceptionCode> decode_exception_code(const std::string& text) {
    // Scan for NNNN-NNNN-NNNN-NNNN. Requiring all four groups is what keeps the
    // three-part basic form from decoding as a shifted four-part one.
    for (size_t i = 0; i + 19 <= text.size(); ++i) {
        if (!four_digits(text, i) || text[i + 4] != '-' || !four_digits(text, i + 5) ||
            text[i + 9] != '-' || !four_digits(text, i + 10) || text[i + 14] != '-' ||
            !four_digits(text, i + 15)) {
            continue;
        }
        // A fifth group means this is not the shape we think it is.
        if (i + 19 < text.size() && text[i + 19] == '-') {
            continue;
        }
        // Each field is exactly four validated digits, so each parse below is
        // bounded to 0..9999.
        ExceptionCode c;
        c.level = helix::text_io::parse_leading<int>(text.substr(i, 4)).value_or(0);
        c.id = helix::text_io::parse_leading<int>(text.substr(i + 5, 4)).value_or(0);
        c.index = helix::text_io::parse_leading<int>(text.substr(i + 10, 4)).value_or(0);
        c.code = helix::text_io::parse_leading<int>(text.substr(i + 15, 4)).value_or(0);
        return c;
    }
    return std::nullopt;
}

std::string_view exception_message(const ExceptionCode& c) {
    for (const auto& k : kKnown) {
        if (k.id == c.id && k.index == c.index && k.code == c.code) {
            return k.message;
        }
    }
    return {};
}

ExceptionSeverity severity_of(int level) {
    switch (level) {
    case 1:
        return ExceptionSeverity::Informational;
    case 2:
        return ExceptionSeverity::Pause;
    default:
        return ExceptionSeverity::Cancel;
    }
}

bool status_carries_exceptions(const nlohmann::json& status) {
    if (!status.is_object()) {
        return false;
    }
    auto manager = status.find("exception_manager");
    if (manager == status.end() || !manager->is_object()) {
        return false;
    }
    auto exceptions = manager->find("exceptions");
    return exceptions != manager->end() && exceptions->is_array();
}

std::optional<ActiveException> read_exception_entry(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return std::nullopt;
    }
    ActiveException active;
    active.code.level = field_int(entry, "level");
    active.code.id = field_int(entry, "id");
    active.code.index = field_int(entry, "index");
    active.code.code = field_int(entry, "code");
    auto firmware_text = entry.find("message");
    if (firmware_text != entry.end() && firmware_text->is_string()) {
        active.message = firmware_text->get<std::string>();
    }
    active.persistent = field_persistent(entry, "is_persistent");
    return active;
}

std::string coded_line(const ActiveException& fault) {
    return fmt::format("!! {} {}", snapmaker::format_exception_code(fault.code), fault.message);
}

std::vector<ActiveException> read_active_exceptions(const nlohmann::json& status) {
    std::vector<ActiveException> out;
    if (!status_carries_exceptions(status)) {
        return out;
    }
    // status_carries_exceptions() has proven both keys present.
    const auto& manager = *status.find("exception_manager");
    for (const auto& entry : *manager.find("exceptions")) {
        if (auto active = read_exception_entry(entry)) {
            out.push_back(std::move(*active));
        }
    }
    return out;
}

} // namespace helix::snapmaker
