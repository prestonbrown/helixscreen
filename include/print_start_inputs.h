// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file print_start_inputs.h
 * @brief The file half of the pre-start checks, computed without a file view.
 *
 * The print-select file view derives a print's tool info, lane mapping, remap
 * and empty-slot pre-flight from the file's palette, its used tools and the live
 * filament state. A start with no screen (a queued job started from the home
 * tile) needs the same answers, so the derivation and the file read behind it
 * live here, and the file view calls them too.
 */

#include "async_lifetime_guard.h"
#include "filament_mapper.h"
#include "file_version.h"
#include "fit_check.h"
#include "gcode_footer_summary.h"
#include "moonraker_types.h"
#include "preflight_validator.h"
#include "print_start_checks.h"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix {

class IMoonrakerAPI;

/// The slicer palette Moonraker's metadata carries: per-tool colors as given,
/// per-tool materials split from the ';'-separated filament_type.
struct FilePalette {
    std::vector<std::string> colors;
    std::vector<std::string> materials;
};

[[nodiscard]] FilePalette file_palette_from_metadata(const FileMetadata& metadata);

/// The version of the file @p metadata describes; zero when there is none.
[[nodiscard]] FileVersion file_version_of(const std::optional<FileMetadata>& metadata);

/// The file half of the pre-start checks' input.
struct FileStartFacts {
    std::string filename; ///< gcodes-relative
    std::optional<FileMetadata> metadata;
    FilePalette palette;
    std::set<int> tools_used;       ///< empty when the scan could not answer
    std::vector<double> tool_grams; ///< empty means unknown, never zero
    /// The plugin's analysis as the fit check reads it; unset without one.
    std::optional<FitFile> fit_record;
};

/// The mapping a print uses: @p card_mappings when the file view's mapping card
/// holds any (its seed, or the user's edit), else the automatic seed for
/// @p used_tool_info against the live slots.
[[nodiscard]] std::vector<ToolMapping>
effective_file_mappings(std::vector<ToolMapping> card_mappings,
                        const std::vector<GcodeToolInfo>& used_tool_info);

/// The mapping the remap modal opens with: the user's picks in @p card_mappings
/// laid over the modal's suggestions for @p used_tool_info against the live slots
/// (AmsState::seed_tool_mappings() with the effective auto-colour answer). The
/// suggestions keep colour matches even where Print carries out no stored pick,
/// because the modal's apply carries them out itself.
[[nodiscard]] std::vector<ToolMapping>
remap_modal_mappings(const std::vector<ToolMapping>& card_mappings,
                     const std::vector<GcodeToolInfo>& used_tool_info);

/// The tool-to-slot remap @p mappings send: only genuine remaps, judged against
/// the backend's firmware routing.
[[nodiscard]] std::map<int, int> effective_file_remap(const std::vector<ToolMapping>& mappings);

/// The empty-slot pre-flight for @p used_tool_info printed through @p mappings.
/// @p material_needed: PrintPreparationManager::enabled_options_need_material().
[[nodiscard]] PreflightResult file_preflight(const std::vector<GcodeToolInfo>& used_tool_info,
                                             const std::vector<ToolMapping>& mappings,
                                             bool material_needed);

/// Fill the file fields of @p ctx from @p facts and the live filament state,
/// the derivation the file view uses when its mapping card holds no user edit:
/// FilamentMappingCard::build_used_tool_info(), AmsState::seed_tool_mappings(),
/// AmsState::collect_available_slots(), FilamentMapper::identity_filtered_remap().
/// Sets ctx.has_file_facts and the fit check's file facts (the plugin's
/// record and the metadata; a queued start reads no head and has no preview).
void fill_file_context(PrintStartContext& ctx, const FileStartFacts& facts);

/// The connected printer's limits as this screen knows them
/// (fit_machine_for_screen() over the config the discovery read, then the
/// nozzles its extruders report live: fit_machine_with_live_nozzles()), or
/// unset before the printer's config was read.
[[nodiscard]] std::optional<FitMachine> current_fit_machine();

/// The tools @p facts shows the file using: its scanned tools, else the tools
/// its metadata gives filament weight to. Empty when neither answers; a palette
/// alone lists the slicer's filaments, not the tools that print.
[[nodiscard]] std::set<int> file_tools_used(const FileStartFacts& facts);

/// The empty-slot pre-flight for @p facts, computed with the same inputs.
[[nodiscard]] PreflightResult preflight_for(const FileStartFacts& facts, bool material_needed);

/// Gathers FileStartFacts for one file: metadata, then the tools/palette/grams
/// footer read, then a whole-file tools scan when the footer cannot answer.
/// The file view runs its own scan through the same steps, handing in its
/// shared download so the viewer preview and the scan use one transfer.
/// How much of a file FileStartFactsLoader::load() may read past Moonraker's
/// metadata and the tools cache.
enum class FileRead {
    MetadataOnly, ///< nothing of the file itself
    Footer,       ///< the footer's one bounded range read, never a download
    Whole,        ///< the footer, then a download and scan when it does not answer
};

class FileStartFactsLoader {
  public:
    /// Downloads the file and answers ok plus a local path the scan may read.
    using DownloadFn =
        std::function<void(std::function<void(bool ok, const std::string& local_path)>)>;
    using ReadyFn = std::function<void(FileStartFacts facts)>;

    explicit FileStartFactsLoader(IMoonrakerAPI* api);
    ~FileStartFactsLoader();
    FileStartFactsLoader(const FileStartFactsLoader&) = delete;
    FileStartFactsLoader& operator=(const FileStartFactsLoader&) = delete;

    void set_api(IMoonrakerAPI* api);

    /// Hears the metadata read's answer (nullopt when it failed) before the
    /// file is read, so a caller can size its wait from the file.
    using MetadataFn = std::function<void(const std::optional<FileMetadata>& metadata)>;

    /// Gather facts for @p filename and answer once, on the main thread. A
    /// failed metadata read, footer read or download still answers, with what
    /// it has. A later load() supersedes an unanswered one.
    /// @p read bounds how much of the file itself is read (FileRead).
    void load(const std::string& filename, ReadyFn on_ready, DownloadFn download = nullptr,
              MetadataFn on_metadata = nullptr, FileRead read = FileRead::Whole);

    /// One file's tools/palette/grams read: the half of load() after the metadata.
    struct ScanRequest {
        std::string file_key; ///< ToolsUsedCache key: PrintPreparationManager::scan_key()
        FileVersion version;  ///< the ToolsUsedCache entry's version
        uint64_t gcode_end_byte = 0;
        gcode::FooterReadNeed need;
        size_t palette_size = 0; ///< the whole-file scan stops once every palette tool is seen
        /// A readable copy on this host to read the tail from, or empty. Asked
        /// only when the footer read is issued.
        std::function<std::string()> local_source;
        bool on_moonraker = true; ///< false for a USB file Moonraker has no copy of
        DownloadFn download;      ///< null: download to a temp file, deleted after the scan
        /// false: a footer that does not answer ends the read; no download.
        bool allow_full_scan = true;
    };

    /// What a scan reports. Each runs on the main thread, and none runs once a
    /// later scan(), load() or cancel() has superseded the scan. A tool set
    /// read from the file is in the ToolsUsedCache, under the request's
    /// file_key and version, before its hook runs; the empty set of a file
    /// that could not be had is never cached.
    struct ScanHooks {
        /// The footer answered what it was read for. @p tools_answered: it
        /// was read for the tools and its summary.tools_used is the answer.
        std::function<void(const gcode::GcodeFooterSummary& summary, bool tools_answered)>
            on_footer;
        /// The footer did not answer. @p full_scan_follows is need.full_scan_justified().
        std::function<void(bool full_scan_follows)> on_footer_unanswered;
        /// The whole-file scan finished. @p read_file is false when the file
        /// could not be had, in which case @p tools is empty and says nothing.
        std::function<void(std::set<int> tools, bool read_file)> on_tools_scanned;
    };

    enum class ScanStart {
        NothingToRead, ///< need asks nothing: no hook will run
        Unreadable,    ///< no API, or a .3mf with no G-code lines: no hook will run
        Started,
    };

    /// Read @p req's file; supersedes any scan or load still unanswered.
    [[nodiscard]] ScanStart scan(ScanRequest req, ScanHooks hooks);

    /// Drop every unanswered scan or load.
    void cancel();

  private:
    void start_tail_read(uint32_t generation, ScanRequest req, ScanHooks hooks);
    void start_full_scan(uint32_t generation, ScanRequest req, ScanHooks hooks);
    /// Download to a temp file named for this scan (@p generation), so an
    /// earlier scan of the same file deleting its own copy cannot take this one.
    void download_to_temp(const std::string& file_key, uint32_t generation,
                          std::function<void(bool ok, const std::string& local_path)> done);

    IMoonrakerAPI* api_ = nullptr;
    uint32_t generation_ = 0;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
