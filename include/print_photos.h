// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Camera photos in print history. For each job, the default webcam's snapshot
// is saved once the print finishes layer 2 and once when it ends (completed,
// cancelled or in error). The HelixPrint plugin's `photos` feature takes them
// for every print; without it, the screen takes them while it is connected.
// Either way the JPEG bytes are stored unchanged under kPhotosDir in the gcodes
// root and listed in one record per history job in kPhotosNamespace, the shape
// moonraker-plugin/helix_photos.py writes. Pure rules here; the wiring lives in
// PrintPhotosController (taking them) and HistoryPhotoViewer (showing them).

#include "helix_plugin_features.h"
#include "json_fwd.h"
#include "print_history_data.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// The plugin feature that takes the photos, and the contract this app reads.
inline constexpr const char* kPhotosFeature = "photos";
inline constexpr int kPhotosMinVersion = 1;

/// One record per history job: this namespace, key = the job id as history
/// spells it ("00002A").
inline constexpr const char* kPhotosNamespace = "helix_photos";
inline constexpr int kPhotoRecordVersion = 1;

/// Where the photos live, relative to the gcodes root.
inline constexpr const char* kPhotosDir = ".helix_photos";

inline constexpr const char* kPhotoKindLayer = "layer2";
inline constexpr const char* kPhotoKindEnd = "end";

/// A snapshot larger than this is not a photo worth keeping.
inline constexpr size_t kMaxPhotoBytes = 8 * 1024 * 1024;
/// The jobs whose photos the screen keeps when it takes them itself; the
/// oldest go first. The plugin keeps the same count, and a byte bound too.
inline constexpr size_t kMaxPhotoJobs = 500;

/// Our entry in a history job's auxiliary_data: the photo paths.
inline constexpr const char* kPhotosAuxProvider = "helix_print";
inline constexpr const char* kPhotosAuxName = "photos";

/// The file name of one photo: the start time keeps a job id history reuses
/// from overwriting the earlier job's photos.
std::string photo_file_name(const std::string& job_id, double start_time, const std::string& kind);

/// kPhotosDir + "/" + photo_file_name().
std::string photo_path(const std::string& job_id, double start_time, const std::string& kind);

/// Whether @p path names a file directly inside kPhotosDir; anything else a
/// record lists is never fetched, shown or deleted.
bool is_photo_path(const std::string& path);

/// Whether the move from layer @p previous to @p current finishes layer 2. A
/// job first seen past layer 2 (a resumed part, a restart mid-print) never
/// qualifies: its photo would show a later layer.
bool layer_two_done(std::optional<int> previous, int current);

/// Whether the job ended in a way that gets an end photo, from history's
/// status string: completed, cancelled or error.
bool ends_with_photo(const std::string& history_status);

/// The kind a photo's file name ends in ("..._layer2.jpg", "..._end.jpg"),
/// empty for any other name.
std::string photo_kind_from_path(const std::string& path);

/// Who takes the photos of a job.
enum class PhotoTaker {
    None,   ///< nobody: opted out, no camera, or another screen takes them
    Plugin, ///< the plugin's photos feature
    App,    ///< this screen, since the plugin cannot
};

/// The one choice between the plugin and this screen.
/// @p photos is plugin_feature(kPhotosFeature, kPhotosMinVersion);
/// @p opted_in the user's photos opt-in as stored, which is all the screen has
/// to go on when the plugin reports no such feature; @p can_capture that this
/// build can fetch a snapshot and the printer lists a camera; @p role this
/// instance's fallback_sender_role(), so only one HelixScreen on a printer
/// takes them.
PhotoTaker photo_taker(PluginFeatureState photos, bool opted_in, bool can_capture,
                       FallbackSenderRole role);

/// The photos a record lists, layer photo first. Empty for anything that is
/// not a record of this version; entries outside kPhotosDir are skipped.
std::vector<PrintPhoto> parse_photo_record(const nlohmann::json& record);

/// The photos a history job's auxiliary_data links, kind read from the file
/// name. Empty when it has no photos entry of ours.
std::vector<PrintPhoto> parse_photo_aux(const nlohmann::json& auxiliary_data);

/// The photos of a job from both places that list them: the ones its
/// auxiliary_data @p linked and the ones its record lists, one per kind, the
/// record's when both have one, layer photo first.
std::vector<PrintPhoto> merge_photo_lists(const std::vector<PrintPhoto>& linked,
                                          const std::vector<PrintPhoto>& recorded);

/// Whether @p a and @p b list the same photos in the same order.
bool same_photos(const std::vector<PrintPhoto>& a, const std::vector<PrintPhoto>& b);

/// Whether @p record was taken for the job that started at @p start_time:
/// history reuses ids, so the start time must agree too.
bool photo_record_matches(const nlohmann::json& record, double start_time);

/// @p record with the photo of @p kind set (replacing an earlier one), or a
/// new record when @p record is not one for this job. The photo carries
/// "source": "app".
nlohmann::json photo_record_with(const nlohmann::json& record, const std::string& job_id,
                                 const std::string& filename, double start_time,
                                 const std::string& kind, const std::string& path, size_t size,
                                 int64_t taken_at);

/// Whether @p record is of a later version than this app writes: another
/// plugin's or app's, left untouched with its files.
bool photo_record_is_newer(const nlohmann::json& record);

/// The keys of the records to drop from @p records (the whole namespace as
/// Moonraker returns it, key -> record) so at most @p max_jobs remain: the
/// malformed ones, then the oldest by start time. A newer version's record is
/// never dropped, nor counted.
std::vector<std::string> photo_records_to_prune(const nlohmann::json& records, size_t max_jobs);

/// Every photo path @p record lists, inside kPhotosDir; none for a record of
/// another version, whose files are not this app's to trust or delete.
std::vector<std::string> photo_record_paths(const nlohmann::json& record);

} // namespace helix
