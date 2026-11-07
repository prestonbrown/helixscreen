// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How a sliced .3mf prints: the one owner of that decision.
//
// A printer either reads nothing of a .3mf (stock Moonraker: a plate prints
// as G-code extracted here, or as the printer-side converter's output), reads
// the archive and picks the plate itself, or starts whichever plate a print
// start names. classify() turns that, the plate the user picked and what the
// start needs into a route; print select, its queue button, the operations
// scan and the preparation manager each ask it and show the answer their own
// way. No I/O, no LVGL.

#include "moonraker_types.h"

#include <optional>
#include <string>

namespace helix::print_3mf {

/// What the printer does with a .3mf, from its metadata answer.
enum class PrinterReads {
    Nothing, ///< no metadata: the archive is described and printed from here
    Archive, ///< metadata, no plate list: the printer starts the archive and picks its plate
    Plates,  ///< metadata listing the sliced plates: a start names the plate that prints
};

/// What @p printer_metadata (nullptr: the printer answered none) says the
/// printer does with the .3mf it describes. `sliced_plates` decides Plates.
[[nodiscard]] PrinterReads printer_reads(const FileMetadata* printer_metadata);

/// Whether the user picks the plate that prints (the plate stepper shows).
[[nodiscard]] bool user_picks_plate(PrinterReads reads);

enum class Purpose {
    Print, ///< started from this screen
    Queue, ///< queued: the printer's job queue starts it later, naming the file alone
};

/// What decides a .3mf's route.
struct RouteInputs {
    PrinterReads reads = PrinterReads::Archive;
    /// The plate the user picked (the archive's plate number); nullopt before
    /// the archive is read, or when it holds no sliced plate.
    std::optional<int> shown_plate;
    /// The plate the printer's metadata describes, which a start naming no plate prints.
    int default_plate = 0;
    /// The printer-side converter's current output of the shown plate, if any.
    std::string converted_output;
    /// The transport can download a whole file and upload one (extraction needs both).
    bool keeps_local_copies = true;
    /// The start rewrites the G-code that prints (a step the file embeds
    /// turned off, START_PRINT skip parameters, a filament remap).
    bool rewrites = false;
    Purpose purpose = Purpose::Print;
};

enum class RouteKind {
    AsFile,          ///< the archive starts as itself, naming no plate
    ConvertedOutput, ///< the converter's output starts, as any G-code does
    NativePlate,     ///< the archive starts with the plate named (print_start::plate_params())
    ExtractPlate,    ///< the plate is extracted here, staged as G-code and started
    Refuse,          ///< nothing starts; refusal says why
};

enum class Refusal {
    None,
    NoSlicedPlate,        ///< the archive holds no plate to print
    NeedsLocalCopies,     ///< extracting needs a transport that keeps local copies
    QueueNeedsConversion, ///< the queue cannot extract; only a converted plate can wait there
    QueueNotDefaultPlate, ///< a queued start prints the printer's default plate, not this one
    NoPlateToRewrite,     ///< the start would rewrite an archive the printer picks the plate of
};

struct Route {
    RouteKind kind = RouteKind::AsFile;
    int plate = 0;          ///< NativePlate and ExtractPlate: the plate that prints
    std::string print_path; ///< ConvertedOutput: the file that starts
    Refusal refusal = Refusal::None;
};

/// The route a .3mf takes for @p in.
[[nodiscard]] Route classify(const RouteInputs& in);

enum class ScanFrom {
    Nothing,         ///< no G-code of this archive prints from here to scan
    PlateEntry,      ///< the shown plate's G-code entry, read out of the archive
    ConvertedOutput, ///< the converter's output, read like any G-code
};

/// What the pre-print operations scan reads for @p in: the G-code its Print
/// route prints, so the toggles act on what runs.
[[nodiscard]] ScanFrom scan_from(const RouteInputs& in);

/// The English toast for @p refusal (translate it with lv_tr); empty for None.
[[nodiscard]] const char* refusal_message(Refusal refusal);

} // namespace helix::print_3mf
