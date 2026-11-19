// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_cost.cpp
 * @brief Pure print-cost rules: grams from metadata, price per gram, totals,
 *        the shared record's key and parse, and which source a surface shows.
 *
 * Run with: ./build/bin/helix-tests "[print_cost]"
 */

#include "json_fwd.h"
#include "moonraker_types.h"
#include "print_cost.h"
#include "spoolman_types.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::print_cost;
using Catch::Approx;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Grams
// ---------------------------------------------------------------------------

TEST_CASE("file_tool_grams: per-tool weights give one line per printing tool",
          "[print_cost][grams]") {
    FileMetadata md;
    md.filament_weights = {0.0, 12.5, 0.0, 3.0};
    md.filament_weight_total = 99.0; // ignored when the split is known

    const auto grams = file_tool_grams(md, "PLA", 1.75f);
    REQUIRE(grams.size() == 2);
    CHECK(grams[0].tool == 1);
    CHECK(grams[0].grams == Approx(12.5));
    CHECK(grams[1].tool == 3);
    CHECK(grams[1].grams == Approx(3.0));
}

TEST_CASE("file_tool_grams: all-zero per-tool weights fall back to the file total",
          "[print_cost][grams]") {
    FileMetadata md;
    md.filament_weights = {0.0, 0.0};
    md.filament_weight_total = 40.0;

    const auto grams = file_tool_grams(md, "PLA", 1.75f);
    REQUIRE(grams.size() == 1);
    CHECK(grams[0].tool == kWholeFile);
    CHECK(grams[0].grams == Approx(40.0));
}

TEST_CASE("file_tool_grams: length alone is weighed at the material's density",
          "[print_cost][grams]") {
    FileMetadata md;
    md.filament_total = 10000.0; // 10 m

    // 10000 mm x pi x 0.875^2 mm^2 = 24052.8 mm^3 = 24.05 cm^3; PLA 1.24 g/cm^3 = 29.8 g
    const auto pla = file_tool_grams(md, "PLA", 1.75f);
    REQUIRE(pla.size() == 1);
    CHECK(pla[0].tool == kWholeFile);
    CHECK(pla[0].grams == Approx(29.83).margin(0.05));

    // No density for an unknown material: no estimate rather than a guess.
    CHECK(file_tool_grams(md, "Unobtainium", 1.75f).empty());
}

TEST_CASE("file_tool_grams: no weight and no length gives nothing", "[print_cost][grams]") {
    FileMetadata md;
    CHECK(file_tool_grams(md, "PLA", 1.75f).empty());
}

// ---------------------------------------------------------------------------
// Price per gram
// ---------------------------------------------------------------------------

TEST_CASE("price_per_gram: the spool's price over its initial weight wins", "[print_cost][price]") {
    CostSettings s;
    s.price_per_kg["PLA"] = 50.0;
    const auto p = price_per_gram(SpoolPrice{25.0, 1000.0}, "PLA", s);
    CHECK(p.source == PriceSource::Spoolman);
    CHECK(p.per_gram == Approx(0.025));
}

TEST_CASE("price_per_gram: a spool missing price or weight falls to the material setting",
          "[print_cost][price]") {
    CostSettings s;
    s.price_per_kg["PETG"] = 30.0;
    s.default_price_per_kg = 10.0;

    const auto no_price = price_per_gram(SpoolPrice{0.0, 1000.0}, "petg", s);
    CHECK(no_price.source == PriceSource::Setting);
    CHECK(no_price.per_gram == Approx(0.030));

    const auto no_weight = price_per_gram(SpoolPrice{20.0, 0.0}, "PETG", s);
    CHECK(no_weight.source == PriceSource::Setting);
    CHECK(no_weight.per_gram == Approx(0.030));
}

TEST_CASE("price_per_gram: a material without its own price uses the default, else none",
          "[print_cost][price]") {
    CostSettings s;
    s.price_per_kg["PLA"] = 20.0;
    s.default_price_per_kg = 15.0;
    const auto other = price_per_gram(std::nullopt, "ASA", s);
    CHECK(other.source == PriceSource::Setting);
    CHECK(other.per_gram == Approx(0.015));

    s.default_price_per_kg = 0.0;
    const auto none = price_per_gram(std::nullopt, "ASA", s);
    CHECK(none.source == PriceSource::None);
    CHECK(none.per_gram == 0.0);
}

// ---------------------------------------------------------------------------
// Totals
// ---------------------------------------------------------------------------

TEST_CASE("electricity_cost: watts x hours x price, zero unless all are set",
          "[print_cost][electricity]") {
    CHECK(electricity_cost(200.0, 0.30, 7200.0) == Approx(0.12)); // 0.2 kW x 2 h x 0.30
    CHECK(electricity_cost(0.0, 0.30, 7200.0) == 0.0);
    CHECK(electricity_cost(200.0, 0.0, 7200.0) == 0.0);
    CHECK(electricity_cost(200.0, 0.30, 0.0) == 0.0);
}

TEST_CASE("compute: sums each tool's cost and the electricity", "[print_cost][total]") {
    CostSettings s;
    s.price_per_kg["PETG"] = 30.0;
    s.watts = 100.0;
    s.price_per_kwh = 0.50;

    std::vector<ToolInput> tools = {
        {0, 100.0, "PLA", SpoolPrice{20.0, 1000.0}}, // 2.00 from Spoolman
        {2, 50.0, "PETG", std::nullopt},             // 1.50 from the setting
    };
    const Breakdown b = compute(tools, 3600.0, s); // 0.1 kWh x 0.50 = 0.05

    REQUIRE(b.tools.size() == 2);
    CHECK(b.tools[0].tool == 0);
    CHECK(b.tools[0].cost == Approx(2.00));
    CHECK(b.tools[0].source == PriceSource::Spoolman);
    CHECK(b.tools[1].tool == 2);
    CHECK(b.tools[1].cost == Approx(1.50));
    CHECK(b.tools[1].source == PriceSource::Setting);
    CHECK(b.filament == Approx(3.50));
    CHECK(b.electricity == Approx(0.05));
    CHECK(b.total == Approx(3.55));
    CHECK(b.complete);
}

TEST_CASE("compute: a printing tool with no price leaves the total incomplete",
          "[print_cost][total]") {
    CostSettings s;
    std::vector<ToolInput> tools = {
        {0, 100.0, "PLA", SpoolPrice{20.0, 1000.0}},
        {1, 10.0, "TPU", std::nullopt},
    };
    CHECK_FALSE(compute(tools, 0.0, s).complete);
    CHECK_FALSE(compute({}, 0.0, s).complete);

    tools.pop_back();
    CHECK(compute(tools, 0.0, s).complete);
}

TEST_CASE("scaled: filament by fraction, electricity by elapsed time", "[print_cost][total]") {
    CostSettings s;
    s.watts = 100.0;
    s.price_per_kwh = 0.50;
    const Breakdown est =
        compute({{0, 100.0, "PLA", SpoolPrice{20.0, 1000.0}}}, 7200.0, s); // 2.00 + 0.10

    const Breakdown half = scaled(est, 0.5, 1800.0, s);
    CHECK(half.filament == Approx(1.00));
    CHECK(half.tools[0].grams == Approx(50.0));
    CHECK(half.electricity == Approx(0.025));
    CHECK(half.total == Approx(1.025));
    CHECK(half.complete);

    // A fraction past the ends is clamped: the actual never exceeds the file's filament.
    CHECK(scaled(est, 1.7, 0.0, s).filament == Approx(2.00));
    CHECK(scaled(est, -1.0, 0.0, s).filament == 0.0);
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

TEST_CASE("record_key: percent-encodes every byte Moonraker could read as structure",
          "[print_cost][record]") {
    CHECK(record_key("parts/benchy.gcode") == "print_cost.parts%2Fbenchy%2Egcode");
    CHECK(record_key("a b%c~_-Z9") == "print_cost.a%20b%25c~_-Z9");
    // UTF-8 bytes are encoded one by one.
    CHECK(record_key("\xC3\xA9") == "print_cost.%C3%A9");
}

namespace {
json full_record() {
    return json::parse(R"({
        "version": 1,
        "source": "print-handy",
        "filename": "benchy.gcode",
        "file_modified": 1731970000.5,
        "currency": "EUR",
        "estimate": {"total": 1.25, "filament": 1.10, "electricity": 0.15, "at": 1731970100,
                     "tools": [{"tool": 0, "grams": 12.3, "cost": 0.40, "price_source": "spoolman"},
                               {"tool": 1, "grams": 20.0, "cost": 0.70, "price_source": "setting"}]},
        "actual": {"total": 1.31, "filament": 1.12, "electricity": 0.19, "at": 1731980000,
                   "tools": []}
    })",
                       nullptr, false);
}
} // namespace

TEST_CASE("parse_record: reads every field of a version-1 record", "[print_cost][record]") {
    const auto r = parse_record(full_record());
    REQUIRE(r.has_value());
    CHECK(r->source == "print-handy");
    CHECK(r->currency == "EUR");
    CHECK(r->file_modified == Approx(1731970000.5));
    REQUIRE(r->estimate.has_value());
    CHECK(r->estimate->total == Approx(1.25));
    CHECK(r->estimate->filament == Approx(1.10));
    CHECK(r->estimate->electricity == Approx(0.15));
    REQUIRE(r->estimate->tools.size() == 2);
    CHECK(r->estimate->tools[1].tool == 1);
    CHECK(r->estimate->tools[1].grams == Approx(20.0));
    CHECK(r->estimate->tools[1].cost == Approx(0.70));
    CHECK(r->estimate->tools[0].source == PriceSource::Spoolman);
    CHECK(r->estimate->tools[1].source == PriceSource::Setting);
    REQUIRE(r->actual.has_value());
    CHECK(r->actual->total == Approx(1.31));
    CHECK(r->actual->at == Approx(1731980000.0));
}

TEST_CASE("parse_record: rejects what cannot be shown safely", "[print_cost][record]") {
    CHECK_FALSE(parse_record(json()).has_value());
    CHECK_FALSE(parse_record(json::array()).has_value());

    json wrong_version = full_record();
    wrong_version["version"] = 2;
    CHECK_FALSE(parse_record(wrong_version).has_value());

    json no_currency = full_record();
    no_currency.erase("currency");
    CHECK_FALSE(parse_record(no_currency).has_value());

    json no_blocks = full_record();
    no_blocks.erase("estimate");
    no_blocks.erase("actual");
    CHECK_FALSE(parse_record(no_blocks).has_value());

    // A block without a usable total is dropped; the other block keeps the record.
    json bad_estimate = full_record();
    bad_estimate["estimate"]["total"] = "1.25";
    bad_estimate["actual"]["total"] = -1.0;
    CHECK_FALSE(parse_record(bad_estimate).has_value());
    bad_estimate["actual"]["total"] = 0.0;
    const auto kept = parse_record(bad_estimate);
    REQUIRE(kept.has_value());
    CHECK_FALSE(kept->estimate.has_value());
    CHECK(kept->actual.has_value());
}

// ---------------------------------------------------------------------------
// Source choice
// ---------------------------------------------------------------------------

TEST_CASE("choose_estimate: the record for this version of the file wins", "[print_cost][choice]") {
    const auto r = parse_record(full_record());
    REQUIRE(r.has_value());

    CHECK(choose_estimate(r, 1731970000.5, true) == Choice::Record);
    CHECK(choose_estimate(r, 1731970000.0, false) == Choice::Record); // float slack
    // Re-sliced under the same name: the record describes the old file.
    CHECK(choose_estimate(r, 1731975000.0, true) == Choice::Own);
    CHECK(choose_estimate(r, 1731975000.0, false) == Choice::None);
    CHECK(choose_estimate(std::nullopt, 1731970000.5, true) == Choice::Own);
    CHECK(choose_estimate(std::nullopt, 1731970000.5, false) == Choice::None);

    auto actual_only = r;
    actual_only->estimate.reset();
    CHECK(choose_estimate(actual_only, 1731970000.5, true) == Choice::Own);
}

TEST_CASE("choose_actual: the record's actual for this print, else wait for it",
          "[print_cost][choice]") {
    const auto r = parse_record(full_record()); // actual.at 1731980000
    REQUIRE(r.has_value());
    constexpr double kModified = 1731970000.5;

    CHECK(choose_actual(r, kModified, 1731979000.0, true) == Choice::Record);
    CHECK(choose_actual(r, kModified, 0.0, false) == Choice::Record); // start unknown
    // The actual is from an earlier run of the file: its writer has yet to add ours.
    CHECK(choose_actual(r, kModified, 1731990000.0, true) == Choice::AwaitRecord);

    auto estimate_only = r;
    estimate_only->actual.reset();
    CHECK(choose_actual(estimate_only, kModified, 1731979000.0, true) == Choice::AwaitRecord);

    // A record for another version of the file says nothing about this print.
    CHECK(choose_actual(r, kModified + 60.0, 1731979000.0, true) == Choice::Own);
    CHECK(choose_actual(std::nullopt, kModified, 0.0, true) == Choice::Own);
    CHECK(choose_actual(std::nullopt, kModified, 0.0, false) == Choice::None);
}

TEST_CASE("A cost formats as an amount even at zero", "[print_cost][price]") {
    CHECK(helix::format_price_with_currency(-0.0, "USD", true) == "$0.00");
    CHECK(helix::format_price_with_currency(-1, "USD", true).empty());
}
