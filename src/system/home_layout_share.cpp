// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "home_layout_share.h"

#include "core/text_io.h"
#include "json_utils.h"
#include "panel_widget_registry.h"

#include <string_view>
#include <utility>

namespace helix::home_layout {

namespace {

constexpr int kMaxGridAxis = 64;

/// One axis of a "<cols>x<rows>" signature, or 0 when it is not a plain
/// decimal in 1..kMaxGridAxis.
int parse_axis(std::string_view text) {
    const auto n = text_io::parse_int<int>(text);
    return (n && *n >= 1 && *n <= kMaxGridAxis) ? *n : 0;
}

bool parse_grid(const std::string& signature, int& cols, int& rows) {
    const auto x = signature.find('x');
    if (x == std::string::npos) {
        return false;
    }
    cols = parse_axis(std::string_view(signature).substr(0, x));
    rows = parse_axis(std::string_view(signature).substr(x + 1));
    return cols > 0 && rows > 0;
}

} // namespace

nlohmann::json build_payload(const nlohmann::json& serialized_pages,
                             const std::string& grid_signature, const std::string& app_version,
                             int64_t saved_at_unix) {
    nlohmann::json out;
    out["schema"] = kSchema;
    out["grid"] = grid_signature;
    const nlohmann::json* pages = json_util::find_member(serialized_pages, "pages");
    out["pages"] = pages ? *pages : nlohmann::json::array();
    out["main_page_index"] = json_util::safe_int(serialized_pages, "main_page_index", 0);
    out["saved_at"] = saved_at_unix;
    out["app_version"] = app_version;
    return out;
}

bool strip_screen_scoped(nlohmann::json& pages_payload) {
    const nlohmann::json* pages_in = json_util::find_member(pages_payload, "pages");
    if (!pages_in || !pages_in->is_array()) {
        return false;
    }

    const int main_in = json_util::safe_int(pages_payload, "main_page_index", 0);
    nlohmann::json kept = nlohmann::json::array();
    int main_out = 0;
    int index = 0;
    for (const nlohmann::json& page : *pages_in) {
        nlohmann::json out_page = page;
        bool emptied = false;
        const nlohmann::json* widgets = json_util::find_member(page, "widgets");
        if (widgets && widgets->is_array()) {
            nlohmann::json remaining = nlohmann::json::array();
            for (const nlohmann::json& w : *widgets) {
                if (!is_screen_scoped(json_util::safe_string(w, "id"))) {
                    remaining.push_back(w);
                }
            }
            emptied = remaining.empty() && !widgets->empty();
            out_page["widgets"] = std::move(remaining);
        }
        if (!emptied) {
            if (index == main_in) {
                main_out = static_cast<int>(kept.size());
            }
            kept.push_back(std::move(out_page));
        }
        ++index;
    }

    if (kept.empty()) {
        return false;
    }
    pages_payload["pages"] = std::move(kept);
    pages_payload["main_page_index"] = main_out;
    return true;
}

std::optional<ParsedLayout> parse_payload(const nlohmann::json& value) {
    if (!value.is_object() || json_util::safe_int(value, "schema", 0) != kSchema) {
        return std::nullopt;
    }

    ParsedLayout parsed{0, 0, nlohmann::json::object()};
    if (!parse_grid(json_util::safe_string(value, "grid"), parsed.cols, parsed.rows)) {
        return std::nullopt;
    }

    const nlohmann::json* pages = json_util::find_member(value, "pages");
    if (!pages || !pages->is_array() || pages->empty()) {
        return std::nullopt;
    }
    parsed.pages_payload["pages"] = *pages;
    parsed.pages_payload["main_page_index"] = json_util::safe_int(value, "main_page_index", 0);

    if (!strip_screen_scoped(parsed.pages_payload)) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace helix::home_layout
