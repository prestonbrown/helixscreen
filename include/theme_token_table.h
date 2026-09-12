// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace helix::theme_tokens {

/// One <color>/<px>/<string> design token from ui_xml/*.xml, resolved
/// last-wins across alphabetically-sorted files (same semantics as the
/// runtime scanner in theme_manager.cpp).
struct TokenEntry {
    const char* type; // "color" | "px" | "string"
    const char* name; // full token name (suffix included)
    const char* value;
};

// Defined in src/generated/theme_token_table.cpp (make regen-tokens).
extern const TokenEntry k_token_table[];
extern const size_t k_token_table_count;

/// True when the aggregation functions should consult the table instead of
/// scanning ui_xml/. Default: ON for HELIX_PLATFORM_ESP32 and
/// HELIX_RELEASE_BUILD (immutable ui_xml), OFF for dev builds (preserves the
/// edit-XML-and-relaunch workflow).
/// Runtime override either way: HELIX_TOKEN_TABLE=1 / =0.
bool enabled();

/// True when the compiled table carries entries of this element type. The
/// generator emits <color>, <px> and <string>; ui_xml also defines <str>, <int>
/// and <percentage> tokens the table knows nothing about, and answering those
/// from the table would return an empty map rather than the values a scan finds.
/// Derived from the table's own contents so it cannot drift from the generator.
bool covers(const char* element_type);

/// True when a token query should be answered from the compiled table instead
/// of scanning @p directory. @p table_enabled is enabled(); taking it as an
/// argument keeps the whole decision pure, so the rule is testable without a
/// release build (tests never define HELIX_RELEASE_BUILD, so enabled() is
/// always false there).
bool answers_from_table(bool table_enabled, const char* element_type, const char* directory,
                        const char* canonical_dir);

/// Table-backed equivalents of theme_manager_parse_all_xml_for_element /
/// _for_suffix. Always available (independent of enabled()) so the parity
/// test can compare them against the live scanner.
std::unordered_map<std::string, std::string> for_element(const char* element_type);
std::unordered_map<std::string, std::string> for_suffix(const char* element_type,
                                                        const char* suffix);

} // namespace helix::theme_tokens
