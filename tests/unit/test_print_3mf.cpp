// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_types.h"
#include "print_3mf.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::print_3mf;

namespace {

RouteInputs inputs(PrinterReads reads, std::optional<int> shown = 5) {
    RouteInputs in;
    in.reads = reads;
    in.shown_plate = shown;
    in.default_plate = 2;
    return in;
}

} // namespace

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

TEST_CASE("a printer that starts a named plate gets the plate, unless the start rewrites it",
          "[print_3mf][native_3mf]") {
    RouteInputs in = inputs(PrinterReads::Plates);
    Route r = classify(in);
    CHECK(r.kind == RouteKind::NativePlate);
    CHECK(r.plate == 5);
    // Without local copies the printer still starts it: nothing is extracted.
    in.keeps_local_copies = false;
    CHECK(classify(in).kind == RouteKind::NativePlate);

    // A rewrite cannot travel with that start: the plate prints extracted.
    in.keeps_local_copies = true;
    in.rewrites = true;
    r = classify(in);
    CHECK(r.kind == RouteKind::ExtractPlate);
    CHECK(r.plate == 5);
    in.keeps_local_copies = false;
    CHECK(classify(in).refusal == Refusal::NeedsLocalCopies);

    // Before the archive is read the printer starts its own default plate.
    in = inputs(PrinterReads::Plates, std::nullopt);
    CHECK(classify(in).kind == RouteKind::AsFile);
    in.rewrites = true;
    CHECK(classify(in).refusal == Refusal::NoPlateToRewrite);

    // A converter output never stands in for a plate the printer starts.
    in = inputs(PrinterReads::Plates);
    in.converted_output = "Set_plate5.gcode";
    CHECK(classify(in).kind == RouteKind::NativePlate);
}

TEST_CASE("a printer that reads nothing prints a plate extracted or converted",
          "[print_3mf][native_3mf]") {
    RouteInputs in = inputs(PrinterReads::Nothing);
    Route r = classify(in);
    CHECK(r.kind == RouteKind::ExtractPlate);
    CHECK(r.plate == 5);
    in.rewrites = true;
    CHECK(classify(in).kind == RouteKind::ExtractPlate);

    in.keeps_local_copies = false;
    CHECK(classify(in).refusal == Refusal::NeedsLocalCopies);
    in = inputs(PrinterReads::Nothing, std::nullopt);
    CHECK(classify(in).refusal == Refusal::NoSlicedPlate);

    in = inputs(PrinterReads::Nothing);
    in.keeps_local_copies = false;
    in.converted_output = "Set_plate5.gcode";
    r = classify(in);
    CHECK(r.kind == RouteKind::ConvertedOutput);
    CHECK(r.print_path == "Set_plate5.gcode");
}

TEST_CASE("a printer that picks the plate itself starts the archive and refuses rewrites",
          "[print_3mf][native_3mf]") {
    RouteInputs in = inputs(PrinterReads::Archive);
    CHECK(classify(in).kind == RouteKind::AsFile);
    in.rewrites = true;
    const Route r = classify(in);
    CHECK(r.kind == RouteKind::Refuse);
    CHECK(r.refusal == Refusal::NoPlateToRewrite);
}

TEST_CASE("the queue takes only what starts without a plate named", "[print_3mf][native_3mf]") {
    RouteInputs in = inputs(PrinterReads::Plates, 2);
    in.purpose = Purpose::Queue;
    CHECK(classify(in).kind == RouteKind::AsFile);
    in.shown_plate = 5;
    CHECK(classify(in).refusal == Refusal::QueueNotDefaultPlate);

    in = inputs(PrinterReads::Nothing);
    in.purpose = Purpose::Queue;
    CHECK(classify(in).refusal == Refusal::QueueNeedsConversion);
    in.converted_output = "Set_plate5.gcode";
    CHECK(classify(in).kind == RouteKind::ConvertedOutput);

    in = inputs(PrinterReads::Archive);
    in.purpose = Purpose::Queue;
    CHECK(classify(in).kind == RouteKind::AsFile);
}

TEST_CASE("the operations scan reads what the Print route prints", "[print_3mf][native_3mf]") {
    RouteInputs in = inputs(PrinterReads::Plates);
    CHECK(scan_from(in) == ScanFrom::PlateEntry);
    in.keeps_local_copies = false;
    CHECK(scan_from(in) == ScanFrom::PlateEntry);

    in = inputs(PrinterReads::Nothing);
    CHECK(scan_from(in) == ScanFrom::PlateEntry);
    in.keeps_local_copies = false;
    CHECK(scan_from(in) == ScanFrom::Nothing);
    in.converted_output = "Set_plate5.gcode";
    CHECK(scan_from(in) == ScanFrom::ConvertedOutput);

    // The queue's answer never decides what the detail view scans.
    in = inputs(PrinterReads::Plates, 5);
    in.purpose = Purpose::Queue;
    CHECK(scan_from(in) == ScanFrom::PlateEntry);
    CHECK(scan_from(inputs(PrinterReads::Archive)) == ScanFrom::Nothing);
}

TEST_CASE("every refusal has words for its toast", "[print_3mf]") {
    CHECK(std::string(refusal_message(Refusal::None)).empty());
    for (Refusal r :
         {Refusal::NoSlicedPlate, Refusal::NeedsLocalCopies, Refusal::QueueNeedsConversion,
          Refusal::QueueNotDefaultPlate, Refusal::NoPlateToRewrite}) {
        CHECK_FALSE(std::string(refusal_message(r)).empty());
    }
}
