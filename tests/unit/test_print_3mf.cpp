// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_types.h"
#include "print_3mf.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::print_3mf;

TEST_CASE("what a printer reads of a .3mf comes from its metadata answer", "[print_3mf]") {
    CHECK(printer_reads(nullptr) == PrinterReads::Nothing);
    FileMetadata md;
    md.estimated_time = 600;
    CHECK(printer_reads(&md) == PrinterReads::Archive);
    md.sliced_plates = std::vector<int>{2, 5};
    CHECK(printer_reads(&md) == PrinterReads::Plates);
    // A list with nothing sliced in it still comes from such a printer.
    md.sliced_plates = std::vector<int>{};
    CHECK(printer_reads(&md) == PrinterReads::Plates);

    CHECK(user_picks_plate(PrinterReads::Nothing));
    CHECK(user_picks_plate(PrinterReads::Plates));
    CHECK_FALSE(user_picks_plate(PrinterReads::Archive));
}

namespace {

/// One row of the route spec. The shown plate is 5 when `shown`, the
/// printer's default plate 2, and Plates printers list plates 2 and 5 unless
/// `unlisted` drops 5.
struct Row {
    PrinterReads reads;
    bool shown;
    bool unlisted;
    bool converted;
    bool local;
    Rewrite rewrite;
    Purpose purpose;
    RouteKind kind;
    Refusal refusal;
    int plate;
};

constexpr auto N = PrinterReads::Nothing;
constexpr auto A = PrinterReads::Archive;
constexpr auto P = PrinterReads::Plates;
constexpr auto None = Rewrite::None;
constexpr auto Steps = Rewrite::Steps;
constexpr auto Remap = Rewrite::Remap;
constexpr auto Pr = Purpose::Print;
constexpr auto Q = Purpose::Queue;

// clang-format off
const Row kRows[] = {
    // reads shown unlisted converted local  rewrite purpose  kind                        refusal                         plate
    // A printer that reads nothing: extracted, or the converter's output.
    {N, true,  false, false, true,  None,  Pr, RouteKind::ExtractPlate,    Refusal::None,                 5},
    {N, true,  false, false, true,  Steps, Pr, RouteKind::ExtractPlate,    Refusal::None,                 5},
    {N, true,  false, false, false, None,  Pr, RouteKind::Refuse,          Refusal::NeedsLocalCopies,     0},
    {N, false, false, false, true,  None,  Pr, RouteKind::Refuse,          Refusal::NoSlicedPlate,        0},
    {N, true,  false, true,  false, Steps, Pr, RouteKind::ConvertedOutput, Refusal::None,                 0},
    {N, true,  false, true,  true,  None,  Q,  RouteKind::ConvertedOutput, Refusal::None,                 0},
    {N, true,  false, false, true,  None,  Q,  RouteKind::Refuse,          Refusal::QueueNeedsConversion, 0},
    // A printer that picks the plate: the archive as itself; nothing to rewrite.
    {A, true,  false, false, true,  None,  Pr, RouteKind::AsFile,          Refusal::None,                 0},
    {A, true,  false, false, true,  Steps, Pr, RouteKind::Refuse,          Refusal::StepsNeedPlate,       0},
    {A, false, false, false, true,  Remap, Pr, RouteKind::Refuse,          Refusal::RemapNeedsPlate,      0},
    {A, true,  false, false, true,  Steps, Q,  RouteKind::AsFile,          Refusal::None,                 0},
    // A printer that starts the plate it is named.
    {P, true,  false, false, true,  None,  Pr, RouteKind::NativePlate,     Refusal::None,                 5},
    {P, true,  false, false, false, None,  Pr, RouteKind::NativePlate,     Refusal::None,                 5},
    {P, true,  false, true,  true,  None,  Pr, RouteKind::NativePlate,     Refusal::None,                 5},
    {P, true,  false, false, true,  Steps, Pr, RouteKind::ExtractPlate,    Refusal::None,                 5},
    {P, true,  false, false, true,  Remap, Pr, RouteKind::ExtractPlate,    Refusal::None,                 5},
    {P, true,  false, false, false, Steps, Pr, RouteKind::Refuse,          Refusal::NeedsLocalCopies,     0},
    {P, true,  true,  false, true,  None,  Pr, RouteKind::ExtractPlate,    Refusal::None,                 5},
    {P, true,  true,  true,  true,  None,  Pr, RouteKind::ExtractPlate,    Refusal::None,                 5},
    {P, false, false, false, true,  None,  Pr, RouteKind::AsFile,          Refusal::None,                 0},
    {P, false, false, false, true,  Steps, Pr, RouteKind::Refuse,          Refusal::StepsNeedPlate,       0},
    {P, false, false, false, true,  Remap, Pr, RouteKind::Refuse,          Refusal::RemapNeedsPlate,      0},
    {P, true,  false, false, true,  None,  Q,  RouteKind::Refuse,          Refusal::QueueNotDefaultPlate, 0},
    {P, false, false, false, true,  None,  Q,  RouteKind::AsFile,          Refusal::None,                 0},
};
// clang-format on

RouteInputs inputs_of(const Row& r) {
    RouteInputs in;
    in.reads = r.reads;
    if (r.shown) {
        in.shown_plate = 5;
    }
    in.printer_plates = r.unlisted ? std::vector<int>{2} : std::vector<int>{2, 5};
    in.default_plate = 2;
    if (r.converted) {
        in.converted_output = "Set_plate5.gcode";
    }
    in.keeps_local_copies = r.local;
    in.rewrite = r.rewrite;
    in.purpose = r.purpose;
    return in;
}

} // namespace

TEST_CASE("every .3mf route follows the spec table", "[print_3mf][native_3mf]") {
    std::set<RouteKind> kinds;
    std::set<Refusal> refusals;
    int row = 0;
    for (const Row& r : kRows) {
        CAPTURE(row++);
        const Route route = classify(inputs_of(r));
        CHECK(route.kind == r.kind);
        CHECK(route.refusal == r.refusal);
        CHECK(route.plate == r.plate);
        CHECK(route.print_path ==
              (r.kind == RouteKind::ConvertedOutput ? "Set_plate5.gcode" : std::string()));
        kinds.insert(route.kind);
        refusals.insert(route.refusal);
    }
    // The table reaches every outcome.
    CHECK(kinds.size() == 5);
    CHECK(refusals.size() == 7);
}

TEST_CASE("the operations scan reads what the Print route prints", "[print_3mf][native_3mf]") {
    RouteInputs in;
    in.reads = PrinterReads::Plates;
    in.shown_plate = 5;
    in.printer_plates = {5};
    CHECK(scan_from(in) == ScanFrom::PlateEntry);
    in.keeps_local_copies = false;
    CHECK(scan_from(in) == ScanFrom::PlateEntry);

    in.reads = PrinterReads::Nothing;
    in.keeps_local_copies = true;
    CHECK(scan_from(in) == ScanFrom::PlateEntry);
    in.keeps_local_copies = false;
    CHECK(scan_from(in) == ScanFrom::Nothing);
    in.converted_output = "Set_plate5.gcode";
    CHECK(scan_from(in) == ScanFrom::ConvertedOutput);

    // The queue's answer and a rewrite never decide what the detail view scans.
    in = RouteInputs{};
    in.reads = PrinterReads::Plates;
    in.shown_plate = 5;
    in.printer_plates = {5};
    in.purpose = Purpose::Queue;
    in.rewrite = Rewrite::Steps;
    CHECK(scan_from(in) == ScanFrom::PlateEntry);
    in.reads = PrinterReads::Archive;
    CHECK(scan_from(in) == ScanFrom::Nothing);
}

TEST_CASE("the stepper offers only plates a printer that starts them lists", "[print_3mf]") {
    const std::vector<int> archive{2, 5, 7};
    CHECK(offered_plates(PrinterReads::Nothing, archive, {}) == archive);
    CHECK(offered_plates(PrinterReads::Plates, archive, {5, 7, 9}) == std::vector<int>{5, 7});
    CHECK(offered_plates(PrinterReads::Plates, archive, {}).empty());
}

TEST_CASE("a reprint with no recorded start names a plate only when that is certain",
          "[print_3mf][reprint]") {
    CHECK(unrecorded_reprint_plate(nullptr) == 0);
    FileMetadata md;
    CHECK(unrecorded_reprint_plate(&md) == 0);
    md.sliced_plates = std::vector<int>{5};
    CHECK(unrecorded_reprint_plate(&md) == 5);
    md.sliced_plates = std::vector<int>{2, 5};
    CHECK_FALSE(unrecorded_reprint_plate(&md));
    md.sliced_plates = std::vector<int>{};
    CHECK_FALSE(unrecorded_reprint_plate(&md));
}

TEST_CASE("every refusal has words for its toast", "[print_3mf]") {
    CHECK(std::string(refusal_message(Refusal::None)).empty());
    for (Refusal r :
         {Refusal::NoSlicedPlate, Refusal::NeedsLocalCopies, Refusal::QueueNeedsConversion,
          Refusal::QueueNotDefaultPlate, Refusal::StepsNeedPlate, Refusal::RemapNeedsPlate}) {
        CHECK_FALSE(std::string(refusal_message(r)).empty());
    }
}
