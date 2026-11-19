// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_cost.h"

#include "filament_database.h"
#include "json_utils.h"
#include "moonraker_types.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "hv/json.hpp"

namespace helix::print_cost {

namespace {

/// A record's file_modified matching the file's, to Moonraker's float precision.
constexpr double kModifiedToleranceS = 1.0;

bool same_file_version(const Record& record, double file_modified) {
    return std::fabs(record.file_modified - file_modified) < kModifiedToleranceS;
}

std::optional<RecordCost> parse_cost(const nlohmann::json& parent, const char* key) {
    const nlohmann::json* block = json_util::find_object(parent, key);
    if (block == nullptr) {
        return std::nullopt;
    }
    const nlohmann::json* total = json_util::find_member(*block, "total");
    if (total == nullptr || !total->is_number()) {
        return std::nullopt;
    }
    RecordCost cost;
    cost.total = json_util::as_double(*total);
    if (!std::isfinite(cost.total) || cost.total < 0.0) {
        return std::nullopt;
    }
    cost.filament = json_util::safe_double(*block, "filament");
    cost.electricity = json_util::safe_double(*block, "electricity");
    cost.at = json_util::safe_double(*block, "at");
    if (const nlohmann::json* tools = json_util::find_array(*block, "tools")) {
        for (const auto& t : *tools) {
            if (!t.is_object()) {
                continue;
            }
            ToolLine line;
            line.tool = json_util::safe_int(t, "tool", kWholeFile);
            line.grams = json_util::safe_double(t, "grams");
            line.cost = json_util::safe_double(t, "cost");
            line.source = parse_price_source(json_util::safe_string(t, "price_source"));
            cost.tools.push_back(line);
        }
    }
    return cost;
}

} // namespace

const char* price_source_name(PriceSource source) {
    switch (source) {
    case PriceSource::Spoolman:
        return "spoolman";
    case PriceSource::Setting:
        return "setting";
    case PriceSource::None:
        break;
    }
    return "";
}

PriceSource parse_price_source(std::string_view name) {
    if (name == "spoolman") {
        return PriceSource::Spoolman;
    }
    if (name == "setting") {
        return PriceSource::Setting;
    }
    return PriceSource::None;
}

std::vector<ToolGrams> file_tool_grams(const FileMetadata& metadata, std::string_view material,
                                       float diameter_mm) {
    std::vector<ToolGrams> out;
    for (size_t i = 0; i < metadata.filament_weights.size(); ++i) {
        if (metadata.filament_weights[i] > 0.0) {
            out.push_back({static_cast<int>(i), metadata.filament_weights[i]});
        }
    }
    if (!out.empty()) {
        return out;
    }
    const float grams = filament::file_weight_g(metadata.filament_weight_total,
                                                metadata.filament_total, material, diameter_mm);
    if (grams > 0.0f) {
        out.push_back({kWholeFile, static_cast<double>(grams)});
    }
    return out;
}

std::string material_key(std::string_view material) {
    const auto mat = filament::find_material(material);
    std::string key = mat.has_value() ? std::string(mat->name) : std::string(material);
    for (char& c : key) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return key;
}

GramPrice price_per_gram(const std::optional<SpoolPrice>& spool, std::string_view material,
                         const CostSettings& settings) {
    if (spool.has_value() && spool->price > 0.0 && spool->initial_weight_g > 0.0) {
        return {spool->price / spool->initial_weight_g, PriceSource::Spoolman};
    }
    if (!material.empty()) {
        const auto it = settings.price_per_kg.find(material_key(material));
        if (it != settings.price_per_kg.end() && it->second > 0.0) {
            return {it->second / 1000.0, PriceSource::Setting};
        }
    }
    if (settings.default_price_per_kg > 0.0) {
        return {settings.default_price_per_kg / 1000.0, PriceSource::Setting};
    }
    return {};
}

double electricity_cost(double watts, double price_per_kwh, double seconds) {
    if (!(watts > 0.0) || !(price_per_kwh > 0.0) || !(seconds > 0.0)) {
        return 0.0;
    }
    return (watts / 1000.0) * (seconds / 3600.0) * price_per_kwh;
}

Breakdown compute(const std::vector<ToolInput>& tools, double seconds,
                  const CostSettings& settings) {
    Breakdown out;
    out.complete = !tools.empty();
    for (const auto& t : tools) {
        const GramPrice price = price_per_gram(t.spool, t.material, settings);
        ToolLine line;
        line.tool = t.tool;
        line.grams = t.grams;
        line.source = price.source;
        line.cost = t.grams * price.per_gram;
        if (price.source == PriceSource::None && t.grams > 0.0) {
            out.complete = false;
        }
        out.filament += line.cost;
        out.tools.push_back(line);
    }
    out.electricity = electricity_cost(settings.watts, settings.price_per_kwh, seconds);
    out.total = out.filament + out.electricity;
    return out;
}

Breakdown scaled(const Breakdown& estimate, double filament_fraction, double elapsed_seconds,
                 const CostSettings& settings) {
    const double f =
        std::isfinite(filament_fraction) ? std::clamp(filament_fraction, 0.0, 1.0) : 0.0;
    Breakdown out;
    out.complete = estimate.complete;
    for (const auto& line : estimate.tools) {
        ToolLine s = line;
        s.grams = line.grams * f;
        s.cost = line.cost * f;
        out.filament += s.cost;
        out.tools.push_back(s);
    }
    out.electricity = electricity_cost(settings.watts, settings.price_per_kwh, elapsed_seconds);
    out.total = out.filament + out.electricity;
    return out;
}

std::string record_key(std::string_view filename) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = kRecordKeyRoot;
    out += '.';
    for (char ch : filename) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

std::optional<Record> parse_record(const nlohmann::json& value) {
    if (!value.is_object() || json_util::safe_int(value, "version") != kRecordVersion) {
        return std::nullopt;
    }
    Record record;
    record.currency = json_util::safe_string(value, "currency");
    if (record.currency.empty()) {
        return std::nullopt;
    }
    record.source = json_util::safe_string(value, "source");
    record.file_modified = json_util::safe_double(value, "file_modified");
    record.estimate = parse_cost(value, "estimate");
    record.actual = parse_cost(value, "actual");
    if (!record.estimate.has_value() && !record.actual.has_value()) {
        return std::nullopt;
    }
    return record;
}

Choice choose_estimate(const std::optional<Record>& record, double file_modified,
                       bool own_complete) {
    if (record.has_value() && record->estimate.has_value() &&
        same_file_version(*record, file_modified)) {
        return Choice::Record;
    }
    return own_complete ? Choice::Own : Choice::None;
}

Choice choose_actual(const std::optional<Record>& record, double file_modified,
                     double print_started_at, bool own_complete) {
    if (record.has_value() && same_file_version(*record, file_modified)) {
        if (record->actual.has_value() &&
            (print_started_at <= 0.0 || record->actual->at >= print_started_at)) {
            return Choice::Record;
        }
        if (record->estimate.has_value()) {
            return Choice::AwaitRecord;
        }
    }
    return own_complete ? Choice::Own : Choice::None;
}

} // namespace helix::print_cost
