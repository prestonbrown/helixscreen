// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The edit list a print-start rewrite applies, in the form the HelixPrint
// plugin's server.helix.print_edited takes, so the plugin can apply it beside
// the file instead of the screen downloading, rewriting and uploading it.
//
// The grammar has two implementations: GCodeFileModifier::apply_streaming and
// GcodeToolRemapper::apply_to_file here, moonraker-plugin/helix_edits.py
// there. tests/fixtures/gcode_edits/ holds cases both suites apply, byte for
// byte.

#include "gcode_file_modifier.h"
#include "helix_plugin_features.h"
#include "json_fwd.h"
#include "moonraker_error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace helix::gcode {

/// The plugin feature that serves print_edited, and the contract this app writes.
inline constexpr const char* kServerEditsFeature = "server_edits";
inline constexpr int kServerEditsMinVersion = 1;

/// One rewrite of a file: positional edits, or a tool remap (logical tool ->
/// physical head) over every line. A list carries one or the other.
struct EditList {
    std::vector<Modification> modifications;
    std::map<int, int> remap;
};

/// @p list as print_edited's `edits` array, or nullopt when it cannot travel
/// and the rewrite stays on the screen: an empty list, one carrying both
/// edits and a remap, a range on an edit that has none, or text that is not
/// UTF-8 (JSON would substitute its bytes, and the copy would print them).
std::optional<nlohmann::json> to_server_edits(const EditList& list);

/// The list a print_edited `edits` array names, or nullopt for one the
/// plugin refuses as malformed.
std::optional<EditList> from_server_edits(const nlohmann::json& edits);

/// Where a print-start rewrite runs.
enum class EditTransport {
    Plugin,          ///< print_edited: the plugin rewrites beside the file
    DownloadRewrite, ///< the screen downloads, rewrites and uploads the copy
};

/// The plugin rewrites when its feature is ready at kServerEditsMinVersion and
/// the list can travel (@p expressible). An archive plate always rewrites on
/// the screen: the plate has to be extracted from the archive first.
EditTransport choose_edit_transport(PluginFeatureState server_edits, bool archive_plate,
                                    bool expressible);

/// How long the app waits for print_edited to answer, for a file of @p file_size bytes
/// (0 when unknown, which is sized as kPrintEditedAssumedBytes). The plugin rewrites in a
/// niced worker that takes minutes for a large file on a slow host, and answers only once
/// the print has started, so this is generous: an answer that arrives after it is lost.
uint32_t print_edited_timeout_ms(uint64_t file_size);

/// The plugin's limit on its rewrite worker, tracking WORKER_BASE_TIMEOUT_S and
/// WORKER_SECONDS_PER_MB in moonraker-plugin/helix_gcode_reader.py (worker_timeout()).
inline constexpr uint32_t kPluginWorkerBaseTimeoutMs = 600 * 1000;
inline constexpr uint32_t kPluginWorkerTimeoutMsPerMiB = 60 * 1000;
/// After its rewrite the plugin answers only once Klipper acknowledged the start,
/// which a print-start macro heating synchronously holds as long as a direct
/// start's (MoonrakerJobAPI::kPrintStartTimeoutMs, asserted there).
inline constexpr uint32_t kPrintEditedStartMarginMs = 10 * 60 * 1000;
/// The app's wait: the plugin's worker limit, then the start.
inline constexpr uint32_t kPrintEditedBaseTimeoutMs =
    kPluginWorkerBaseTimeoutMs + kPrintEditedStartMarginMs;
inline constexpr uint32_t kPrintEditedTimeoutMsPerMiB = kPluginWorkerTimeoutMsPerMiB;
inline constexpr uint64_t kPrintEditedAssumedBytes = 256ull * 1024 * 1024;

/// What a failed print_edited means for the start.
enum class EditedPrintFailure {
    Unanswered,  ///< timeout or lost connection: the plugin may still start the print
    Unsupported, ///< the plugin cannot do it (no such method, an edit it does not take,
                 ///< the feature off): nothing ran, so the screen may rewrite instead
    Refused,     ///< a definite answer (a changed file, a failed write): report it
};

/// Classifies @p error from server.helix.print_edited. Only Unsupported may fall back to
/// the screen's own rewrite: after Unanswered the plugin may still start the print, and a
/// second start would print the job twice.
EditedPrintFailure classify_print_edited_failure(const MoonrakerError& error);

} // namespace helix::gcode
