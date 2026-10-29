// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_inputs.h"

#include "ui_filament_mapping_card.h"
#include "ui_filename_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "file_analysis.h"
#include "gcode_parser.h"
#include "gcode_preview_fetcher.h"
#include "http_executor.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "text_io.h"
#include "tools_used_cache.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>

namespace helix {

namespace tio = helix::text_io;

FileVersion file_version_of(const std::optional<FileMetadata>& metadata) {
    if (!metadata) {
        return {};
    }
    return {metadata->size, static_cast<time_t>(metadata->modified)};
}

FilePalette file_palette_from_metadata(const FileMetadata& metadata) {
    FilePalette palette;
    // Per-tool materials from the semicolon-separated metadata
    // (e.g., "PLA;PLA;PETG" → ["PLA", "PLA", "PETG"])
    if (!metadata.filament_type.empty()) {
        for (std::string_view sv : tio::lines(metadata.filament_type, ';')) {
            palette.materials.emplace_back(sv);
        }
    }
    // Per-tool hex colors, parsed by Moonraker
    palette.colors = metadata.filament_colors;
    return palette;
}

std::vector<ToolMapping> effective_file_mappings(std::vector<ToolMapping> card_mappings,
                                                 const std::vector<GcodeToolInfo>& used_tool_info) {
    // Whenever the card is showing it has seeded its mappings, and any edit the
    // user made through the remap picker landed there, so that copy wins.
    if (!card_mappings.empty()) {
        return card_mappings;
    }
    // Card not showing (no AMS, bypassed single-lane, nothing the file uses):
    // resolve the effective (toggle-aware) mapping the same way the live render
    // does, keeping the chips, preflight and the 3D preview on one answer.
    auto& ams = AmsState::instance();
    return ams.seed_tool_mappings(used_tool_info, ams.collect_available_slots());
}

std::vector<ToolMapping> remap_modal_mappings(const std::vector<ToolMapping>& card_mappings,
                                              const std::vector<GcodeToolInfo>& used_tool_info) {
    auto& ams = AmsState::instance();
    auto mappings = ams.seed_tool_mappings(used_tool_info, ams.collect_available_slots(),
                                           ams.effective_auto_match());
    FilamentMapper::apply_picks(mappings, card_mappings);
    return mappings;
}

std::map<int, int> effective_file_remap(const std::vector<ToolMapping>& mappings) {
    // Only genuine remaps survive the identity filter: a real slot assignment
    // that differs from the firmware-default head for that tool. Judged against
    // the SAME routing the seed used, so a lane-per-tool identity never reads
    // as a remap.
    return FilamentMapper::identity_filtered_remap(mappings,
                                                   AmsState::instance().collect_firmware_routing());
}

PreflightResult file_preflight(const std::vector<GcodeToolInfo>& used_tool_info,
                               const std::vector<ToolMapping>& mappings, bool material_needed) {
    // Bypass short-circuits the slot checks: the filament comes from the
    // external spool, so no mapping can satisfy a tool and every check would
    // report EmptySlot.
    auto& ams = AmsState::instance();
    return PreflightValidator::validate(used_tool_info, ams.collect_available_slots(), mappings,
                                        ams.collect_firmware_routing(), ams.any_bypass_active(),
                                        material_needed);
}

namespace {

/// What the file view's mapping card holds for @p facts when the user has not
/// edited it.
ui::FilamentMappingCard::Mapping card_mapping_for(const FileStartFacts& facts) {
    return ui::FilamentMappingCard::compute_mapping(facts.palette.colors, facts.palette.materials,
                                                    facts.tools_used);
}

std::vector<GcodeToolInfo> used_tool_info_for(const FileStartFacts& facts) {
    return ui::FilamentMappingCard::build_used_tool_info(facts.palette.colors,
                                                         facts.palette.materials, facts.tools_used);
}

} // namespace

std::set<int> file_tools_used(const FileStartFacts& facts) {
    if (!facts.tools_used.empty() || !facts.metadata) {
        return facts.tools_used;
    }
    std::set<int> tools;
    const auto& weights = facts.metadata->filament_weights;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0.0) {
            tools.insert(static_cast<int>(i));
        }
    }
    return tools;
}

void fill_file_context(PrintStartContext& ctx, const FileStartFacts& facts) {
    auto card = card_mapping_for(facts);
    ctx.has_file_facts = true;
    ctx.metadata = facts.metadata;
    ctx.effective_remap =
        effective_file_remap(effective_file_mappings(card.mappings, used_tool_info_for(facts)));
    ctx.mappings = std::move(card.mappings);
    ctx.tool_info = std::move(card.tool_info);
    ctx.available_slots = std::move(card.slots);
    ctx.filament_materials = facts.palette.materials;
    ctx.tools_used = file_tools_used(facts);
    ctx.tool_grams = facts.tool_grams;
    ctx.fit_file = fit_file_from_sources(facts.fit_record, nullptr, facts.metadata, std::nullopt,
                                         ctx.tools_used);
}

namespace {

/// @p name when the printer database knows it as a real machine; the
/// pseudo-entries (Custom/Other, Unknown) and an empty name say nothing about
/// which printer this is.
std::string known_printer_model(const std::string& name) {
    if (name.empty()) {
        return {};
    }
    for (const PrinterListEntry& entry : PrinterDetector::get_list_entries("")) {
        if (entry.name == name && !entry.manufacturer.empty()) {
            return name;
        }
    }
    return {};
}

/// What each extruder reports about its nozzle, by extruder number.
std::vector<FitLiveNozzle> live_nozzles(const PrinterTemperatureState& temperatures) {
    std::vector<FitLiveNozzle> live;
    for (const auto& [name, info] : temperatures.extruders()) {
        const std::optional<int> n = tool_number_for_extruder(name);
        if (!n) {
            continue;
        }
        if (live.size() <= static_cast<size_t>(*n)) {
            live.resize(static_cast<size_t>(*n) + 1);
        }
        FitLiveNozzle& nozzle = live[static_cast<size_t>(*n)];
        if (info.nozzle_diameter > 0.0) {
            nozzle.diameter = info.nozzle_diameter;
        }
        nozzle.volume_type = info.nozzle_volume_type;
    }
    return live;
}

} // namespace

std::optional<FitMachine> current_fit_machine() {
    PrinterState& state = get_printer_state();
    const std::optional<FitMachine>& base = state.get_discovery().fit_machine();
    if (!base) {
        return std::nullopt;
    }
    AmsState& ams = AmsState::instance();
    return fit_machine_with_live_nozzles(
        fit_machine_for_screen(*base, known_printer_model(state.profile_state().printer_type()),
                               ams.is_available(), ams.printable_y_shrink_mm()),
        live_nozzles(state.temperature_state()));
}

PreflightResult preflight_for(const FileStartFacts& facts, bool material_needed) {
    const auto tools = used_tool_info_for(facts);
    return file_preflight(tools, effective_file_mappings(card_mapping_for(facts).mappings, tools),
                          material_needed);
}

// ============================================================================
// FileStartFactsLoader
// ============================================================================

FileStartFactsLoader::FileStartFactsLoader(IMoonrakerAPI* api) : api_(api) {}

FileStartFactsLoader::~FileStartFactsLoader() = default;

void FileStartFactsLoader::set_api(IMoonrakerAPI* api) {
    api_ = api;
}

void FileStartFactsLoader::cancel() {
    ++generation_;
}

void FileStartFactsLoader::load(const std::string& filename, ReadyFn on_ready, DownloadFn download,
                                MetadataFn on_metadata, FileRead read) {
    const uint32_t generation = ++generation_;
    if (!api_) {
        FileStartFacts facts;
        facts.filename = filename;
        on_ready(std::move(facts));
        return;
    }

    const FileAnalysisSource analysis_source =
        file_analysis_source(get_printer_state().plugin_status_state().plugin_feature(
                                 kFileAnalysisFeature, kFileAnalysisMinVersion),
                             filename);

    auto after_metadata = [this, generation, filename, on_ready = std::move(on_ready),
                           download = std::move(download), on_metadata = std::move(on_metadata),
                           read, analysis_source](std::optional<FileMetadata> metadata) {
        if (generation != generation_) {
            return;
        }
        if (on_metadata) {
            on_metadata(metadata);
        }
        auto facts = std::make_shared<FileStartFacts>();
        facts->filename = filename;
        facts->metadata = metadata;
        if (metadata) {
            facts->palette = file_palette_from_metadata(*metadata);
        }
        const FileVersion version = file_version_of(metadata);

        auto cached = tools_used_cache().lookup(filename, version);
        if (cached) {
            facts->tools_used = *cached;
        }
        if (read == FileRead::MetadataOnly) {
            on_ready(std::move(*facts));
            return;
        }

        // What the plugin's record left unanswered, read from the file itself.
        auto read_file = [this, facts, filename, version, metadata, download, read,
                          on_ready](bool tools_known) {
            ScanRequest req;
            req.file_key = filename;
            req.version = version;
            req.gcode_end_byte = metadata ? metadata->gcode_end_byte : 0;
            req.need = gcode::footer_read_need(tools_known, !facts->palette.colors.empty(),
                                               !facts->tool_grams.empty());
            req.palette_size = facts->palette.colors.size();
            req.download = download;
            req.allow_full_scan = read == FileRead::Whole;

            auto answer = [facts, on_ready]() { on_ready(std::move(*facts)); };

            ScanHooks hooks;
            hooks.on_footer = [facts, answer](const gcode::GcodeFooterSummary& s,
                                              bool tools_answered) {
                gcode::backfill_from_footer(s, facts->palette.colors, facts->tool_grams);
                if (tools_answered) {
                    facts->tools_used = s.tools_used;
                }
                answer();
            };
            hooks.on_footer_unanswered = [answer](bool full_scan_follows) {
                if (!full_scan_follows) {
                    answer();
                }
            };
            hooks.on_tools_scanned = [facts, answer](std::set<int> tools, bool /*read_file*/) {
                facts->tools_used = std::move(tools);
                answer();
            };
            if (scan(std::move(req), std::move(hooks)) != ScanStart::Started) {
                answer();
            }
        };

        if (analysis_source == FileAnalysisSource::Fallback) {
            read_file(cached.has_value());
            return;
        }
        // The plugin's record is the first source; each field it leaves
        // unanswered falls back to the file read.
        const bool cache_known = cached.has_value();
        auto on_reply = [this, generation, facts, filename, version, read_file,
                         cache_known](FileAnalysisReply reply) {
            if (generation != generation_) {
                return;
            }
            bool tools_known = cache_known;
            if (reply.status == FileAnalysisReply::Status::Ready && reply.record) {
                apply_file_analysis(*facts, *reply.record);
                if (!reply.record->tools.empty()) {
                    tools_used_cache().store(filename, version, facts->tools_used);
                    tools_known = true;
                }
                spdlog::debug("[FileStartFacts] Analysis record for {}: {} tool(s)", filename,
                              reply.record->tools.size());
            }
            read_file(tools_known);
        };
        api_->get_client().send_jsonrpc(
            kFileAnalysisRpc, nlohmann::json{{"filename", filename}},
            lifetime_.bg_cb("FileStartFactsLoader::analysis",
                            [on_reply](const nlohmann::json& response) {
                                const nlohmann::json* result =
                                    json_util::find_member(response, "result");
                                on_reply(parse_file_analysis_reply(
                                    result != nullptr ? *result : nlohmann::json::object()));
                            }),
            lifetime_.bg_cb("FileStartFactsLoader::analysis_failed",
                            [on_reply, filename](const MoonrakerError& error) {
                                spdlog::debug("[FileStartFacts] No analysis for {}: {}", filename,
                                              error.message);
                                on_reply(FileAnalysisReply{});
                            }),
            0, /*silent=*/true);
    };

    api_->files().get_file_metadata(
        filename,
        lifetime_.bg_cb(
            "FileStartFactsLoader::metadata",
            [after_metadata](const FileMetadata& metadata) mutable { after_metadata(metadata); }),
        lifetime_.bg_cb("FileStartFactsLoader::metadata_failed",
                        [after_metadata, filename](const MoonrakerError& error) mutable {
                            spdlog::debug("[FileStartFacts] No metadata for {}: {}", filename,
                                          error.message);
                            after_metadata(std::nullopt);
                        }),
        /*silent=*/true);
}

FileStartFactsLoader::ScanStart FileStartFactsLoader::scan(ScanRequest req, ScanHooks hooks) {
    const uint32_t generation = ++generation_;
    // The footer answers THREE questions — which tools, what colors, how many
    // grams — and ToolsUsedCache persists only the first. Gating the whole read
    // on that one cached answer would leave a file whose Moonraker metadata
    // carries no filament_colors with no palette at all.
    if (!req.need.any()) {
        spdlog::debug("[FileStartFacts] Tools, palette and grams all known — no footer read");
        return ScanStart::NothingToRead;
    }
    // A .3mf is a zip with no G-code lines to read.
    if (!api_ || req.file_key.empty() || gcode::is_3mf(req.file_key)) {
        return ScanStart::Unreadable;
    }
    // The file's own footer already states which tools it uses and in what
    // colors, so ask for that first — a single small range request instead of
    // the whole file. Whether it may fall through to the whole-file scan is
    // `need`'s call: recovering a tool set earns that cost, recovering a
    // palette does not.
    start_tail_read(generation, std::move(req), std::move(hooks));
    return ScanStart::Started;
}

void FileStartFactsLoader::start_tail_read(uint32_t generation, ScanRequest req, ScanHooks hooks) {
    // Sized from Moonraker's gcode_end_byte when it reported one (the footer
    // starts exactly there), otherwise a fixed window. Everything after that
    // offset is the slicer's settings block.
    const size_t window =
        gcode::gcode_tail_window_bytes(req.version.size_bytes, req.gcode_end_byte);
    const std::string file_path = req.file_key;
    const auto need = req.need;

    spdlog::debug("[FileStartFacts] Footer read: last {} bytes of {} (size={}, gcode_end_byte={}, "
                  "need tools={} palette={} grams={})",
                  window, file_path, req.version.size_bytes, req.gcode_end_byte, need.tools,
                  need.palette, need.grams);

    auto tok = lifetime_.token();
    auto shared_req = std::make_shared<const ScanRequest>(std::move(req));
    auto shared_hooks = std::make_shared<const ScanHooks>(std::move(hooks));

    // Callable from any thread: `this` is only dereferenced inside the defer.
    auto fall_back = [this, tok, generation, shared_req, shared_hooks](const char* why) {
        const bool full_scan =
            shared_req->allow_full_scan && shared_req->need.full_scan_justified();
        if (full_scan) {
            spdlog::debug("[FileStartFacts] Footer read did not answer ({}) — full scan", why);
        } else {
            // This read was only ever after the palette and the grams. Reading
            // a hundred-plus megabytes to recover them is precisely the cost
            // the streaming path exists to avoid.
            spdlog::debug("[FileStartFacts] Footer read did not answer ({}) — no palette/grams "
                          "for this file",
                          why);
        }
        tok.defer("FileStartFacts::footer_unanswered",
                  [this, generation, shared_req, shared_hooks, full_scan]() {
                      if (generation != generation_) {
                          return;
                      }
                      if (shared_hooks->on_footer_unanswered) {
                          shared_hooks->on_footer_unanswered(full_scan);
                      }
                      if (full_scan && generation == generation_) {
                          start_full_scan(generation, *shared_req, *shared_hooks);
                      }
                  });
    };

    // === Runs on a background thread: pure parse over a local ===
    auto on_tail = [this, tok, generation, shared_req, shared_hooks, fall_back,
                    need](const std::string& tail) mutable {
        const gcode::GcodeFooterSummary summary = gcode::parse_gcode_footer_summary(tail);

        // usable() gates the TOOLS answer specifically (a usage line naming at
        // least one nonzero tool). A read issued only for the palette must not
        // be judged by it: a footer with a colour line and no usage line
        // answers this read completely.
        const bool answered = need.tools
                                  ? summary.usable()
                                  : (!summary.colours.empty() || !summary.grams_per_tool.empty());
        if (!answered) {
            fall_back(summary.has_usage_line ? "usage vector is all zero"
                                             : "no per-tool usage line");
            return;
        }
        const bool tools_answered = need.tools && summary.usable();
        tok.defer("FileStartFacts::footer_answered",
                  [this, generation, shared_req, shared_hooks, summary, tools_answered]() {
                      if (generation != generation_) {
                          return;
                      }
                      // The slicer's own accounting of what it emitted is the
                      // file's answer, cached like a completed scan.
                      if (tools_answered) {
                          tools_used_cache().store(shared_req->file_key, shared_req->version,
                                                   summary.tools_used);
                      }
                      if (shared_hooks->on_footer) {
                          shared_hooks->on_footer(summary, tools_answered);
                      }
                  });
    };

    // The file is on this machine (a USB stick, or Moonraker's own copy): read
    // the tail straight off disk instead of asking Moonraker for those bytes.
    const std::string local = shared_req->local_source ? shared_req->local_source() : "";
    if (!local.empty()) {
        spdlog::debug("[FileStartFacts] Footer read from the local copy: {}", local);
        helix::http::HttpExecutor::slow().submit([local, window, on_tail, fall_back]() mutable {
            std::string tail = gcode::read_file_tail(local, window);
            if (tail.empty()) {
                fall_back("local tail read failed");
                return;
            }
            on_tail(tail);
        });
        return;
    }
    if (!shared_req->on_moonraker) {
        fall_back("USB file unreadable");
        return;
    }
    if (!api_->transfers().supports_tail_reads()) {
        fall_back("the transport reads no file tails");
        return;
    }
    api_->transfers().download_file_tail(
        "gcodes", file_path, window, [on_tail](const std::string& tail) mutable { on_tail(tail); },
        [fall_back](const MoonrakerError& error) mutable {
            // === BG THREAD: fall_back marshals before touching `this` ===
            spdlog::debug("[FileStartFacts] Footer read failed: {}", error.message);
            fall_back("transport error");
        });
}

void FileStartFactsLoader::start_full_scan(uint32_t generation, ScanRequest req, ScanHooks hooks) {
    // Early-exit stop set (full slicer palette): once every palette tool has
    // been seen the result can't grow — stop reading. Tools beyond the
    // palette are dropped by every downstream consumer anyway.
    std::set<int> stop_set;
    for (size_t i = 0; i < req.palette_size; ++i) {
        stop_set.insert(static_cast<int>(i));
    }

    auto tok = lifetime_.token();
    auto shared_hooks = std::make_shared<const ScanHooks>(std::move(hooks));
    // Our own temp copy goes once the scan has read it; a caller's download is
    // the caller's to keep (the file view's preview reads the same copy).
    const bool own_copy = !req.download;
    const std::string file_key = req.file_key;
    const FileVersion version = req.version;

    // Callable from any thread: `this` is only dereferenced inside the defer.
    auto finish = [this, tok, generation, shared_hooks, file_key, version](std::set<int> tools,
                                                                           bool read_file) {
        tok.defer("FileStartFacts::tools_scanned",
                  [this, generation, shared_hooks, file_key, version, tools = std::move(tools),
                   read_file]() mutable {
                      if (generation != generation_) {
                          return;
                      }
                      // Even an empty set read from the file is its answer (a
                      // single-extruder file); a file that could not be had
                      // answered nothing.
                      if (read_file) {
                          tools_used_cache().store(file_key, version, tools);
                      }
                      if (shared_hooks->on_tools_scanned) {
                          shared_hooks->on_tools_scanned(std::move(tools), read_file);
                      }
                  });
    };

    auto on_downloaded = [finish, stop_set, own_copy](bool ok, const std::string& path) {
        if (!ok) {
            // No file to read: an empty set that says nothing about the file,
            // so it must never be cached as its answer.
            spdlog::debug("[FileStartFacts] Tools scan: no G-code file - degrading");
            finish({}, false);
            return;
        }
        // Line by line on the slow HTTP lane: off the main thread, and the
        // scanner never holds the whole file.
        helix::http::HttpExecutor::slow().submit([finish, stop_set, own_copy, path]() {
            bool opened = false;
            std::set<int> tools = gcode::scan_tools_used_from_file(path, stop_set, &opened);
            if (!opened) {
                // A copy gone before the scan read it is no file at all, not
                // an empty one.
                spdlog::debug("[FileStartFacts] Tools scan: {} is gone - degrading", path);
                finish({}, false);
                return;
            }
            if (own_copy) {
                std::remove(path.c_str());
            }
            // The scan read the real file, so even an empty set is an answer:
            // a single-extruder file.
            finish(std::move(tools), true);
        });
    };

    if (req.download) {
        req.download(on_downloaded);
    } else {
        download_to_temp(req.file_key, generation, on_downloaded);
    }
}

void FileStartFactsLoader::download_to_temp(
    const std::string& file_key, uint32_t generation,
    std::function<void(bool ok, const std::string& local_path)> done) {
    const std::string dir = get_helix_cache_dir("gcode_temp");
    if (!api_ || dir.empty() || !api_->transfers().supports_local_copies()) {
        done(false, {});
        return;
    }
    const std::string path = dir + "/" +
                             ui::GcodePreviewFetcher::cache_file_name(
                                 "start_", file_key + "#" + std::to_string(generation));
    api_->transfers().download_file_to_path(
        "gcodes", file_key, path, [done, path](const std::string&) { done(true, path); },
        [done, path](const MoonrakerError& error) {
            spdlog::warn("[FileStartFacts] Download for the tools scan failed: {}", error.message);
            std::remove(path.c_str());
            done(false, {});
        });
}

} // namespace helix
