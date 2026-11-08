// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reading a sliced .3mf that sits on the printer, a few small ranged reads at a
// time, and extracting one plate's G-code from a local copy of it.
//
// read_summary() costs one read of the archive's tail (its central directory)
// and, when slice_info.config is not already inside that tail, one more for it.
// read_entry() costs one read per entry, which is how a card's thumbnail is
// fetched. Every callback runs on the transfer's worker thread.

#include "i_moonraker_sub_apis.h"
#include "three_mf.h"
#include "zip_reader.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix::three_mf {

/// What a card or the detail view shows for one sliced plate.
struct PlateSummary {
    int index = 0;
    std::string name;      ///< the plate's name in the project; empty when it has none
    bool has_info = false; ///< slice_info.config carried a block for this plate
    SliceInfo info;
    std::optional<zip::ZipEntry> gcode;       ///< the plate's G-code (three_mf::sliced_plates())
    std::optional<zip::ZipEntry> thumb;       ///< the plate's render
    std::optional<zip::ZipEntry> thumb_small; ///< Metadata/plate_<N>_small.png
};

/// A sliced 3MF's plates, by plate number. Empty: a project nothing was sliced in.
struct ArchiveSummary {
    std::vector<PlateSummary> plates;
};

/// The thumbnail entry to draw for @p plate: the full render, else the small one.
const zip::ZipEntry* plate_thumbnail(const PlateSummary& plate);

/// @p plate's summary as the metadata Moonraker reports for a G-code file:
/// predicted time, weight, and per-tool materials, colors and grams, slot-aligned
/// by the slicer's filament id.
FileMetadata plate_file_metadata(const PlateSummary& plate);

/// The whole archive as one file's metadata: @p shown's filaments (what that
/// plate prints with), and the predicted time and weight of every sliced plate
/// added up, so a list card is honest about a file holding several.
FileMetadata archive_file_metadata(const ArchiveSummary& summary, const PlateSummary& shown);

/// The plate @p summary lists as @p index, or its first plate when it has no
/// such plate, or nullptr when it has none.
const PlateSummary* find_plate(const ArchiveSummary& summary, int index);

/// Largest central directory read_summary() fetches when the tail read missed part of it.
inline constexpr size_t kMaxCentralDirectoryBytes = 1024 * 1024;

/// Largest entry read_entry() inflates into memory.
inline constexpr size_t kMaxEntryBytes = 2 * 1024 * 1024;

/**
 * @brief An entry's bytes, out of a read of the archive from its local header on.
 *
 * Stored entries are copied; deflated ones inflate (tinfl: about 11KB of state
 * and a 32KB window beside the output) and are refused above @p max_bytes
 * inflated. False with @p error set when the read does not hold the entry or
 * it does not check out.
 */
bool entry_payload(const std::string& read, const zip::ZipEntry& entry, size_t max_bytes,
                   std::string& out, zip::ZipError& error);

class ThreeMfReader {
  public:
    using SummaryCallback = std::function<void(const ArchiveSummary&)>;
    using BytesCallback = std::function<void(const std::string&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    /// @p transfers must outlive every read started through this reader.
    explicit ThreeMfReader(ITransfersAPI& transfers);

    /**
     * @brief Reads the plates of the archive at @p path (gcodes root) on the printer.
     *
     * @p size is the archive's size from the file listing: the tail read is a
     * plain byte range, which every transport can make. A summary is kept per
     * (path, modified, size) and answered from memory, synchronously, until
     * the file changes. A @p size of 0 (not known) reads the tail as a suffix
     * range instead, where the transport supports_tail_reads(), and keeps nothing.
     * Once @p cancelled is set no further read is asked for and neither
     * callback runs.
     */
    void read_summary(const std::string& path, uint64_t size, double modified,
                      SummaryCallback on_done, ErrorCallback on_error,
                      ITransfersAPI::CancelFlag cancelled = nullptr);

    /// Reads and, when it is deflated, inflates one entry of the archive at
    /// @p path: one ranged read through @p transfers.
    static void read_entry(ITransfersAPI& transfers, const std::string& path,
                           const zip::ZipEntry& entry, BytesCallback on_done,
                           ErrorCallback on_error, ITransfersAPI::CancelFlag cancelled = nullptr);

    /**
     * @brief The first @p max_bytes of @p entry, inflated, read with one range.
     *
     * What a preamble scan of a plate's G-code reads without fetching the
     * whole archive. Fewer bytes when the entry is shorter; never more. A read
     * that does not hold the bytes asked for, or bytes that do not inflate, is
     * an error. Once @p cancelled is set neither callback runs.
     */
    static void read_entry_head(ITransfersAPI& transfers, const std::string& path,
                                const zip::ZipEntry& entry, size_t max_bytes, BytesCallback on_done,
                                ErrorCallback on_error,
                                ITransfersAPI::CancelFlag cancelled = nullptr);

    /**
     * @brief Writes plate @p plate's G-code from the local archive @p local_zip to @p local_gcode.
     *
     * Streams the entry through the inflater a buffer at a time, so the memory
     * it takes does not grow with the plate. Checks the entry's CRC and, when
     * the archive carries one, its .md5. A failure removes @p local_gcode.
     * Unavailable on a transport with no local copies (the firmware): Io.
     */
    static zip::ZipError extract_plate(const std::string& local_zip, int plate,
                                       const std::string& local_gcode);

    /// The sliced plates of the local archive @p local_zip, by plate number;
    /// empty when it cannot be read or nothing is sliced in it.
    static std::vector<int> local_plate_numbers(const std::string& local_zip);

    /// Drops every kept summary.
    void clear_cache();

  private:
    struct Cache;
    ITransfersAPI& transfers_;
    std::shared_ptr<Cache> cache_;
};

} // namespace helix::three_mf
