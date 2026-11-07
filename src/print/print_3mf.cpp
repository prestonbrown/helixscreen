// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_3mf.h"

#include "moonraker_types.h"
#include "translation_loader.h"

#include <algorithm>

namespace helix::print_3mf {

namespace {

Route refuse(Refusal why) {
    Route r;
    r.kind = RouteKind::Refuse;
    r.refusal = why;
    return r;
}

Route plate_route(RouteKind kind, int plate) {
    Route r;
    r.kind = kind;
    r.plate = plate;
    return r;
}

/// A printer that reads nothing: a plate prints extracted, or from the
/// converter's output, which also lets it wait in the queue.
Route route_from_here(const RouteInputs& in) {
    if (!in.converted_output.empty()) {
        Route r;
        r.kind = RouteKind::ConvertedOutput;
        r.print_path = in.converted_output;
        return r;
    }
    if (in.purpose == Purpose::Queue) {
        return refuse(Refusal::QueueNeedsConversion);
    }
    if (!in.shown_plate) {
        return refuse(Refusal::NoSlicedPlate);
    }
    if (!in.keeps_local_copies) {
        return refuse(Refusal::NeedsLocalCopies);
    }
    return plate_route(RouteKind::ExtractPlate, *in.shown_plate);
}

Refusal rewrite_refusal(Rewrite rewrite) {
    return rewrite == Rewrite::Remap ? Refusal::RemapNeedsPlate : Refusal::StepsNeedPlate;
}

bool printer_lists_plate(const std::vector<int>& plates, int plate) {
    return std::find(plates.begin(), plates.end(), plate) != plates.end();
}

/// A printer that starts the plate it is named; a rewritten plate cannot
/// travel with that start, and a plate it does not list is not its to start,
/// so both print as they would on a printer that reads nothing.
Route route_by_plate(const RouteInputs& in) {
    if (in.purpose == Purpose::Queue) {
        return !in.shown_plate || *in.shown_plate == in.default_plate
                   ? Route{}
                   : refuse(Refusal::QueueNotDefaultPlate);
    }
    if (!in.shown_plate) {
        return in.rewrite != Rewrite::None ? refuse(Refusal::PlateNotLoaded) : Route{};
    }
    if (in.rewrite == Rewrite::None && printer_lists_plate(in.printer_plates, *in.shown_plate)) {
        return plate_route(RouteKind::NativePlate, *in.shown_plate);
    }
    RouteInputs here = in;
    here.converted_output.clear();
    return route_from_here(here);
}

} // namespace

PrinterReads printer_reads(const FileMetadata* printer_metadata) {
    if (printer_metadata == nullptr) {
        return PrinterReads::Nothing;
    }
    return printer_metadata->sliced_plates ? PrinterReads::Plates : PrinterReads::Archive;
}

bool user_picks_plate(PrinterReads reads) {
    return reads != PrinterReads::Archive;
}

std::vector<int> offered_plates(PrinterReads reads, const std::vector<int>& archive_plates,
                                const std::vector<int>& printer_plates) {
    if (reads != PrinterReads::Plates) {
        return archive_plates;
    }
    std::vector<int> offered;
    for (int plate : archive_plates) {
        if (printer_lists_plate(printer_plates, plate)) {
            offered.push_back(plate);
        }
    }
    return offered;
}

Route classify(const RouteInputs& in) {
    switch (in.reads) {
    case PrinterReads::Nothing:
        return route_from_here(in);
    case PrinterReads::Plates:
        return route_by_plate(in);
    case PrinterReads::Archive:
        break;
    }
    // The printer picks the plate: nothing here can say which G-code a
    // rewrite would have to edit.
    return in.rewrite != Rewrite::None && in.purpose == Purpose::Print
               ? refuse(rewrite_refusal(in.rewrite))
               : Route{};
}

ScanFrom scan_from(const RouteInputs& in) {
    RouteInputs print = in;
    print.purpose = Purpose::Print;
    print.rewrite = Rewrite::None;
    switch (classify(print).kind) {
    case RouteKind::ConvertedOutput:
        return ScanFrom::ConvertedOutput;
    case RouteKind::NativePlate:
    case RouteKind::ExtractPlate:
        return ScanFrom::PlateEntry;
    case RouteKind::AsFile:
    case RouteKind::Refuse:
        break;
    }
    return ScanFrom::Nothing;
}

std::optional<int> unrecorded_reprint_plate(const FileMetadata& printer_metadata) {
    if (printer_reads(&printer_metadata) != PrinterReads::Plates) {
        return 0;
    }
    const std::vector<int>& plates = *printer_metadata.sliced_plates;
    if (plates.size() == 1) {
        return plates.front();
    }
    return std::nullopt;
}

const char* refusal_message(Refusal refusal) {
    switch (refusal) {
    case Refusal::None:
        return "";
    case Refusal::NoSlicedPlate:
        return TR_NOOP("This .3mf has no sliced plate to print.");
    case Refusal::NeedsLocalCopies:
        return TR_NOOP(
            "This .3mf has to be converted to G-code on the printer before it can print.");
    case Refusal::QueueNeedsConversion:
        return TR_NOOP(
            "This .3mf has to be converted to G-code before it can be queued. Print it from "
            "here instead.");
    case Refusal::QueueNotDefaultPlate:
        return TR_NOOP(
            "The queue prints this .3mf's default plate. Print this plate from here instead.");
    case Refusal::StepsNeedPlate:
        return TR_NOOP("The pre-print steps you turned off cannot be applied to this .3mf. Turn "
                       "them back on to print it.");
    case Refusal::RemapNeedsPlate:
        return TR_NOOP("Filament Mapping cannot rewrite this .3mf. Print it without remapping.");
    case Refusal::PlateNotLoaded:
        return TR_NOOP("This .3mf's plates are still loading. Try again in a moment.");
    }
    return "";
}

} // namespace helix::print_3mf
