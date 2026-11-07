// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_3mf.h"

#include "moonraker_types.h"
#include "translation_loader.h"

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

/// A printer that starts the plate it is named; a rewritten plate cannot
/// travel with that start, so it prints extracted.
Route route_by_plate(const RouteInputs& in) {
    if (in.purpose == Purpose::Queue) {
        return !in.shown_plate || *in.shown_plate == in.default_plate
                   ? Route{}
                   : refuse(Refusal::QueueNotDefaultPlate);
    }
    if (!in.shown_plate) {
        return in.rewrites ? refuse(Refusal::NoPlateToRewrite) : Route{};
    }
    if (!in.rewrites) {
        return plate_route(RouteKind::NativePlate, *in.shown_plate);
    }
    if (!in.keeps_local_copies) {
        return refuse(Refusal::NeedsLocalCopies);
    }
    return plate_route(RouteKind::ExtractPlate, *in.shown_plate);
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
    return in.rewrites && in.purpose == Purpose::Print ? refuse(Refusal::NoPlateToRewrite)
                                                       : Route{};
}

ScanFrom scan_from(const RouteInputs& in) {
    RouteInputs print = in;
    print.purpose = Purpose::Print;
    print.rewrites = false;
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
    case Refusal::NoPlateToRewrite:
        return TR_NOOP(
            "The printer picks this .3mf's plate itself, so the pre-print steps you turned "
            "off cannot be applied. Turn them back on to print it.");
    }
    return "";
}

} // namespace helix::print_3mf
