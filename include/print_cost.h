// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file print_cost.h
 * @brief What a print costs: filament per tool, optional electricity, and the
 *        shared cost record another program on the printer host may write.
 *
 * Pure rules, no LVGL and no I/O. Filament cost per tool is grams times the
 * price per gram of the spool feeding that tool (its price over its initial
 * weight), else the user's price per kg for the material. Electricity is the
 * printer's average draw times the print time at a price per kWh, and is zero
 * unless both are set.
 *
 * A program on the printer host that also prices prints writes one record per file
 * to Moonraker database namespace `helix-screen`, key record_key(filename).
 * When a record answers for the file, its number is shown in place of ours, so
 * a printer never shows two prices for one print (prestonbrown/helixscreen#2132).
 */

#pragma once

#include "json_fwd.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct FileMetadata;

namespace helix::print_cost {

/// Where a tool's price per gram came from.
enum class PriceSource { None = 0, Spoolman, Setting };

/// The record's spelling of @p source: "spoolman", "setting", or "" for None.
[[nodiscard]] const char* price_source_name(PriceSource source);
/// The inverse of price_source_name(); anything else reads as None.
[[nodiscard]] PriceSource parse_price_source(std::string_view name);

/// The tool index a whole-file figure is charged to: the file states no
/// per-tool split, so the active spool (or the only tool's lane) prices it.
inline constexpr int kWholeFile = -1;

/// Grams one tool prints.
struct ToolGrams {
    int tool = kWholeFile;
    double grams = 0.0;
};

/**
 * @brief Grams of filament the file prints, per tool where the slicer says.
 *
 * `filament_weights` (per tool) when any entry is positive, one line per
 * positive tool; else `filament_weight_total` as one kWholeFile line; else
 * `filament_total` (mm) at the density of @p material (filament::file_weight_g).
 * Empty when none of these yields a positive weight.
 */
[[nodiscard]] std::vector<ToolGrams> file_tool_grams(const FileMetadata& metadata,
                                                     std::string_view material, float diameter_mm);

/// A Spoolman spool's price facts (SpoolInfo::effective_price(), initial_weight_g).
struct SpoolPrice {
    double price = 0.0;
    double initial_weight_g = 0.0;
};

/// The user's cost settings, in the display currency.
struct CostSettings {
    /// Price per kg keyed by canonical material name (filament::find_material's
    /// name, else the upper-cased name as typed).
    std::unordered_map<std::string, double> price_per_kg;
    double default_price_per_kg = 0.0; ///< Any material without its own price
    double watts = 0.0;                ///< Printer's average draw while printing
    double price_per_kwh = 0.0;
};

/// The key price_per_kg uses for @p material.
[[nodiscard]] std::string material_key(std::string_view material);

struct GramPrice {
    double per_gram = 0.0;
    PriceSource source = PriceSource::None;
};

/// Spool price over its initial weight when both are positive, else the
/// material's price per kg, else the default per kg, else None.
[[nodiscard]] GramPrice price_per_gram(const std::optional<SpoolPrice>& spool,
                                       std::string_view material, const CostSettings& settings);

/// watts x hours x price per kWh; zero unless all three are positive.
[[nodiscard]] double electricity_cost(double watts, double price_per_kwh, double seconds);

/// One tool to price: its grams, its material, and its spool when Spoolman has one.
struct ToolInput {
    int tool = kWholeFile;
    double grams = 0.0;
    std::string material;
    std::optional<SpoolPrice> spool;
};

struct ToolLine {
    int tool = kWholeFile;
    double grams = 0.0;
    double cost = 0.0;
    PriceSource source = PriceSource::None;
};

struct Breakdown {
    std::vector<ToolLine> tools;
    double filament = 0.0;
    double electricity = 0.0;
    double total = 0.0;
    /// Every tool that prints has a price. A total missing a tool is not shown.
    bool complete = false;
};

/// Price each tool and add electricity for @p seconds.
[[nodiscard]] Breakdown compute(const std::vector<ToolInput>& tools, double seconds,
                                const CostSettings& settings);

/**
 * @brief @p estimate's filament scaled to @p filament_fraction (clamped to 0..1)
 *        plus electricity for @p elapsed_seconds.
 *
 * The cost so far while printing (fraction = progress), and the actual cost
 * when it ends (fraction = filament used over the file's total).
 */
[[nodiscard]] Breakdown scaled(const Breakdown& estimate, double filament_fraction,
                               double elapsed_seconds, const CostSettings& settings);

// ---------------------------------------------------------------------------
// Shared cost record
// ---------------------------------------------------------------------------

inline constexpr const char* kRecordNamespace = "helix-screen";
inline constexpr const char* kRecordKeyRoot = "print_cost";
inline constexpr int kRecordVersion = 1;

/**
 * @brief The database key for @p filename's record: "print_cost.<encoded>".
 *
 * Moonraker reads '.' in a key as a path separator, so the name is
 * percent-encoded: ASCII letters, digits, '-', '_' and '~' stay; every other
 * byte, '.', '/' and '%' included, becomes %XX in upper-case hex.
 * "parts/benchy.gcode" is "print_cost.parts%2Fbenchy%2Egcode".
 */
[[nodiscard]] std::string record_key(std::string_view filename);

/// One cost block of a record (estimate or actual).
struct RecordCost {
    double total = 0.0;
    double filament = 0.0;
    double electricity = 0.0;
    std::vector<ToolLine> tools;
    double at = 0.0; ///< Unix seconds the block was written (estimate) or the print ended (actual)
};

struct Record {
    std::string source;   ///< Writer's name, e.g. "print-handy"
    std::string currency; ///< ISO 4217 code
    double file_modified = 0.0;
    std::optional<RecordCost> estimate;
    std::optional<RecordCost> actual;
};

/// The record in @p value; nullopt when it is not a version-1 object with a
/// currency and at least one cost block carrying a non-negative total.
[[nodiscard]] std::optional<Record> parse_record(const nlohmann::json& value);

/// What a surface shows.
enum class Choice {
    None,        ///< Nothing to show
    Own,         ///< HelixScreen's own figure
    Record,      ///< The record's figure
    AwaitRecord, ///< The record's writer tracks this file but has not written the actual yet
};

/**
 * @brief Pick the source for the estimate (before and during a print).
 *
 * The record's estimate when it was written for this version of the file
 * (file_modified within a second of @p file_modified), else our own when it
 * is complete, else None.
 */
[[nodiscard]] Choice choose_estimate(const std::optional<Record>& record, double file_modified,
                                     bool own_complete);

/**
 * @brief Pick the source for the actual cost of a print that just ended.
 *
 * The record's actual when it ended no earlier than @p print_started_at
 * (Unix seconds; 0 = unknown, any actual counts); else AwaitRecord while the
 * record has an estimate for this file, since its writer will add the actual;
 * else our own when complete; else None.
 */
[[nodiscard]] Choice choose_actual(const std::optional<Record>& record, double file_modified,
                                   double print_started_at, bool own_complete);

} // namespace helix::print_cost
