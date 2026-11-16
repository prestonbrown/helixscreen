// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "hv/json.hpp" // JSON_HPP_OK: pages_payload held by value

/// The home layout a printer's own HelixScreen publishes to Moonraker's
/// database, and the validation a remote client applies before seeding from it.
/// Pure JSON in, JSON out: no I/O and no LVGL.
namespace helix::home_layout {

inline constexpr const char* kNamespace = "helix-screen";
inline constexpr const char* kKey = "home_layout";
inline constexpr int kSchema = 1;

/// The published value for `serialized_pages` (the shape
/// PanelWidgetConfig::serialize_pages() emits). The default-layout stamp and
/// the page-id counter describe the publisher's own bookkeeping, so they are
/// left out.
nlohmann::json build_payload(const nlohmann::json& serialized_pages,
                             const std::string& grid_signature, const std::string& app_version,
                             int64_t saved_at_unix);

struct ParsedLayout {
    int cols;
    int rows;
    /// `{pages, main_page_index}`, the input shape of restore_pages().
    nlohmann::json pages_payload;
};

/// Removes screen-scoped widget entries from `pages_payload["pages"][*]["widgets"]`,
/// drops the pages that removal left empty, and moves `main_page_index` to the
/// same surviving page (0 when that page was dropped). Returns false when no
/// page remains or `pages` is not an array.
bool strip_screen_scoped(nlohmann::json& pages_payload);

/// The printer-scoped layout in a published value, or nullopt when the value is
/// not a schema-`kSchema` object with a "<cols>x<rows>" grid (each 1..64) and at
/// least one page that keeps an entry.
std::optional<ParsedLayout> parse_payload(const nlohmann::json& value);

} // namespace helix::home_layout
