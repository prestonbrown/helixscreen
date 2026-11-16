// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_plugin_features.h"
#include "json_fwd.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// What fills the printer's disks, by consumer: the HelixPrint plugin's
/// `storage` report when it is ready, else the same breakdown summed from the
/// file API. Pure rules only, exception-free; the fetching lives in
/// storage_report_service.h.
namespace helix::storage {

inline constexpr const char* kStorageFeature = "storage";
inline constexpr int kStorageMinVersion = 1;
inline constexpr const char* kAutoCleanupFeature = "storage_auto_cleanup";
inline constexpr int kAutoCleanupMinVersion = 1;

/// The free space the gcodes volume keeps after a print's copy is written:
/// the plugin's FREE_SPACE_FLOOR_BYTES, which the app's own uploads keep too.
inline constexpr uint64_t kFreeSpaceFloorBytes = 100ULL * 1024 * 1024;

/// A timelapse render younger than this may still be being written; the
/// fallback cleanup leaves it.
inline constexpr double kFallbackMinAgeS = 300.0;

/// Where a consumer's files live.
enum class Root {
    Gcodes,
    Timelapse,
    Logs,
    Data, ///< Moonraker's data directory, which no file root exposes
};

/// One consumer, in the plugin's report order. Its row on the Storage page
/// (label, icon) lives in settings_storage_overlay.xml.
struct ConsumerInfo {
    const char* key;
    Root root;
    /// Cleanup may remove its files; the rest hold the user's files or state
    /// another feature needs.
    bool cleanable;
    /// The body of the cleanup confirmation, English; the UI translates it
    const char* confirm;
};

inline constexpr std::array<ConsumerInfo, 9> kConsumers = {{
    {"helix_temp", Root::Gcodes, true,
     TR_NOOP("Delete every modified print copy that is not in use? Files you uploaded are not "
             "touched.")},
    {"3mf_outputs", Root::Gcodes, false, ""},
    {"analysis_sidecars", Root::Gcodes, true,
     TR_NOOP("Delete the layer indexes? Previews read each file again until it is analyzed "
             "again.")},
    {"photos", Root::Gcodes, true,
     TR_NOOP("Delete every print photo? Print history keeps its "
             "entries.")},
    {"timelapse", Root::Timelapse, true, TR_NOOP("Delete every timelapse video?")},
    {"logs", Root::Logs, true,
     TR_NOOP("Delete the rotated logs? The logs in use are kept, and debug bundles still work.")},
    {"thumbnails", Root::Gcodes, false, ""},
    {"user_gcode", Root::Gcodes, false, ""},
    {"plr", Root::Data, false, ""},
}};

inline constexpr std::size_t kConsumerCount = kConsumers.size();

/// The index of consumer @p key in kConsumers.
std::optional<std::size_t> consumer_index(std::string_view key);

struct ConsumerUsage {
    uint64_t bytes = 0;
    uint64_t files = 0;
    /// Measured: its root exists here
    bool available = false;
    /// Has a cleanup action now
    bool cleanable = false;
};

struct StorageReport {
    std::optional<uint64_t> total;
    std::optional<uint64_t> free;
    std::array<ConsumerUsage, kConsumerCount> consumers{};
    /// The listing stopped at kMaxFallbackDirectories; sums are a lower bound.
    bool partial = false;
};

enum class ReportSource {
    Plugin,  ///< server.helix.storage
    FileApi, ///< the known directories listed and summed by the app
};

/// Where the Storage page reads from: the plugin's report only when its
/// `storage` feature is Ready.
ReportSource report_source(PluginFeatureState storage);

/// The report in a server.helix.storage result; nullopt when it is not one.
std::optional<StorageReport> parse_storage_result(const nlohmann::json& result);

// ---------------------------------------------------------------------------
// File-API fallback
// ---------------------------------------------------------------------------

/// The fallback lists at most this many directories per report.
inline constexpr std::size_t kMaxFallbackDirectories = 256;

/// The consumer a file under the gcodes root belongs to (the plugin's
/// classify_gcodes_path): print copies sit in gcode::staging_dirs() and the
/// plugin's link directory. Without the plugin no file is a .3mf output.
std::size_t classify_gcodes_path(std::string_view rel);

/// A file of a server.files.get_directory listing.
struct ListedFile {
    std::string name;
    uint64_t size = 0;
    double modified = 0.0;
};

/// A server.files.get_directory result.
struct DirectoryListing {
    std::vector<std::string> dirs;
    std::vector<ListedFile> files;
    std::optional<uint64_t> total;
    std::optional<uint64_t> free;
};

std::optional<DirectoryListing> parse_directory_result(const nlohmann::json& result);

/// Whether consumer @p index can be cleaned up without the plugin. Only
/// timelapse: the app cannot ask which print copy, layer index or file a
/// waiting power-loss snapshot needs, photos belong to records only the
/// plugin keeps, and Moonraker's logs root takes no deletes.
bool fallback_cleanable(std::size_t index);

/// Whether the app may clean anything itself: not while an installed plugin
/// reports its storage feature unavailable, since that is the plugin refusing
/// this printer's directories, and that refusal is final.
bool fallback_may_clean(bool plugin_installed, const PluginFeatureMap& features);

/// Whether the timelapse root in a server.files.roots result may be cleaned:
/// it exists and is neither the gcodes or config root nor above either. A
/// result that does not name all three is not safe. Moonraker reports paths
/// as configured, and the app cannot resolve them on the printer, so two
/// spellings of one directory pass here; timelapse_listing_is_own() is the
/// check that does not depend on spelling.
bool timelapse_root_safe(const nlohmann::json& roots_result);

/// Whether the timelapse root's own listing looks like timelapse's alone: no
/// subdirectory and only render outputs (fallback_name_allowed). The gcodes
/// root holds G-code and `.thumbs`, the config root `.cfg` files and the data
/// directory folders, so a root that is really one of those under another
/// spelling fails here.
bool timelapse_listing_is_own(const DirectoryListing& listing);

/// How long a cleanup request may wait for its answer before the page stops
/// waiting and calls it failed.
inline constexpr uint32_t kCleanupRpcTimeoutMs = 120000;

/// Whether a file name passes its consumer's fallback name rule (the
/// plugin's name_allowed): timelapse render outputs by extension; nothing
/// for any other consumer.
bool fallback_name_allowed(std::size_t index, std::string_view name);

/// Whether the fallback cleanup of consumer @p index removes a listed file
/// (its name relative to the consumer's root): fallback_cleanable, the name
/// rule, and at least @p older_than seconds old (never under
/// kFallbackMinAgeS).
bool fallback_removes(std::size_t index, const ListedFile& file, double now, double older_than);

/// How a cleanup went, for its message.
struct CleanupOutcome {
    uint64_t freed = 0;
    uint64_t errors = 0;  ///< files that could not be deleted
    bool refused = false; ///< the request itself failed; nothing is known
};

/// What the page says after a cleanup.
enum class CleanupMessage {
    Freed,         ///< "Freed {}"
    FreedPartly,   ///< "Freed {}, some files could not be deleted"
    NothingFreed,  ///< "Could not free space"
    NothingToFree, ///< "Nothing to clean up"
};

CleanupMessage cleanup_message(const CleanupOutcome& outcome);

/// Whether an upload of @p size bytes may go to the gcodes volume.
enum class UploadSpace {
    Ok,
    TooFull, ///< it would leave less than kFreeSpaceFloorBytes
    Unknown, ///< free space could not be read: the upload goes ahead
};

UploadSpace upload_space(std::optional<uint64_t> free_bytes, uint64_t size);

/// The HTTP code the plugin's print_modified / print_edited answer when the
/// print would leave less than the floor free. It is final: the copy is gone
/// (print_modified deletes it) or was never written (print_edited), and no
/// other start may be tried in its place.
inline constexpr int kNoSpaceRpcCode = 507;

constexpr bool is_no_space_refusal(int rpc_code) {
    return rpc_code == kNoSpaceRpcCode;
}

/// The message tag for a print start that failed with @p rpc_code: the space
/// message for the plugin's out-of-space refusal, else "Failed to start
/// print: {}" with the error.
const char* print_start_failure_tag(int rpc_code);

/// What the app says when the printer has no room for a print's copy.
inline constexpr const char* kNoUploadSpaceMessage =
    TR_NOOP("Not enough free space on the printer for this file. Free some in Settings > "
            "System > Storage.");

} // namespace helix::storage
