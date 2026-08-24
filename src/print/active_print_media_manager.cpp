// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "active_print_media_manager.h"

#include "ui_filename_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "gcode_parser.h"
#include "json_utils.h"
#include "memory_monitor.h"
#include "observer_factory.h"
#include "thumbnail_cache.h"
#include "thumbnail_load_context.h"
#include "thumbnail_processor.h"

#if defined(HELIX_PLATFORM_ESP32)
#include "esp_psram_thumbnail.h"
#endif

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <stdexcept>

using helix::gcode::get_display_filename;
using helix::gcode::resolve_gcode_filename;

namespace {

std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/**
 * @brief Does a recorded thumbnail source still describe the reported print?
 *
 * True when the source names the same file Moonraker is reporting, or the
 * original that a rewritten temp copy of it resolves to. The basename compare
 * is the last rung: a preparing job records its full path while the temp
 * rewrite resolves to a bare name, so the two can differ by a directory prefix
 * and still be the same print.
 */
bool source_describes(const std::string& raw, const std::string& source) {
    if (raw == source) {
        return true;
    }
    // A rewritten temp path is the whole reason an override exists, and only
    // this app produces one - so it always belongs to a print we started, whose
    // preparing epoch set the override we are holding. Keep it even when the
    // original cannot be recovered from the string.
    if (helix::gcode::is_rewritten_gcode_path(raw)) {
        return true;
    }
    const std::string resolved = helix::gcode::resolve_gcode_filename(raw);
    if (resolved == source) {
        return true;
    }
    // Last rung: a preparing job records its full path while the name the
    // printer reports may drop the directory. Same file, different prefix.
    return basename_of(resolved) == basename_of(source);
}

} // namespace

namespace helix {

// Singleton storage
static std::unique_ptr<ActivePrintMediaManager> g_instance;

void init_active_print_media_manager() {
    if (g_instance) {
        spdlog::warn("[ActivePrintMediaManager] Already initialized");
        return;
    }
    g_instance = std::make_unique<ActivePrintMediaManager>(::get_printer_state());
    spdlog::debug("[ActivePrintMediaManager] Initialized");
}

void deinit_active_print_media_manager() {
    g_instance.reset();
    spdlog::debug("[ActivePrintMediaManager] Deinitialized");
}

ActivePrintMediaManager& get_active_print_media_manager() {
    if (!g_instance) {
        throw std::runtime_error("ActivePrintMediaManager not initialized");
    }
    return *g_instance;
}

ActivePrintMediaManager::ActivePrintMediaManager(PrinterState& printer_state)
    : printer_state_(printer_state) {
    // Observe print_filename_ subject to react to filename changes.
    // Use observe_string_immediate so process_filename runs SYNCHRONOUSLY
    // when the subject changes. This is critical: process_filename clears
    // the stale print_thumbnail_path_ from the previous print BEFORE any
    // deferred observers fire (e.g., print_start_navigation's push_overlay
    // → on_activate, which reads print_thumbnail_path_ to populate the UI).
    // Without this, the race window allows stale thumbnails to be cached
    // and displayed for the wrong print.
    // Safety: process_filename only clears subjects, queues updates, and
    // starts async operations — no observer lifecycle changes or widget
    // destruction, so immediate dispatch is safe.
    print_filename_observer_ = helix::ui::observe_string_immediate<ActivePrintMediaManager>(
        printer_state_.get_print_filename_subject(), this,
        [](ActivePrintMediaManager* self, const char* filename) {
            self->process_filename(filename);
        },
        printer_state_.get_subjects_lifetime());

    // Adopt the preparing job's identity the moment a job starts preparing,
    // and release it when that claim did not become the running print. Doing
    // this here rather than at each start path is the point: the previous
    // arrangement required every caller to remember a second call, and one of
    // them (the reprint path) never did.
    // observe_int_immediate, not _sync: _sync routes through queue_update, so the
    // identity change would land AFTER a synchronously-dispatched filename
    // update had already early-returned on the stale override. Safe to dispatch
    // immediately by the same reasoning as the filename observer above - this
    // handler only mutates identity fields and queues updates; it touches no
    // observer lifecycle and destroys no widgets.
    preparing_epoch_observer_ = helix::ui::observe_int_immediate<ActivePrintMediaManager>(
        printer_state_.get_preparing_epoch_subject(), this,
        [](ActivePrintMediaManager* self, int epoch) {
            if (epoch > 0) {
                const std::string full = self->printer_state_.preparing_job().full_path();
                self->set_thumbnail_source(full);
                // set_thumbnail_source only re-processes an EXISTING Moonraker
                // filename. At commit there may not be one yet - that is the
                // whole reason the identity is recorded - so resolve straight
                // from the job. Idempotent: process_filename short-circuits if
                // the effective name is already current.
                self->process_filename(full.c_str());
                return;
            }
            // Confirmed means the printer took OUR job, so the override still
            // describes what is printing - a rewritten temp file may be what
            // print_stats reports. Every other exit means it does not.
            if (self->printer_state_.last_preparing_exit() != PreparingExit::Confirmed) {
                // Release the identity, but do NOT blank the display subjects:
                // clear_print_info() defers that blanking, which would land
                // after the incoming filename had already been resolved and
                // wipe it. Whatever the printer reports next repopulates them.
                self->release_identity();
                return;
            }
            // Confirmed. The commit-time load may have run before Moonraker had
            // the file - the longer the pre-start block, the likelier - and
            // nothing else refunds its retry budget: process_filename() will
            // early-return from here on, because the effective filename has not
            // changed. Give it one fresh ladder now that the file must exist.
            self->rearm_media_if_incomplete();
        },
        printer_state_.get_subjects_lifetime());

    spdlog::debug("[ActivePrintMediaManager] Observer attached to print_filename subject");
}

ActivePrintMediaManager::~ActivePrintMediaManager() {
    // ObserverGuard handles cleanup automatically
    // NOTE: No logging here - spdlog may be destroyed before this singleton
    cancel_thumbnail_retry();
    unregister_moonraker_listeners();
}

void ActivePrintMediaManager::set_api(IMoonrakerAPI* api) {
    if (api_ != api) {
        unregister_moonraker_listeners();
    }
    api_ = api;
    spdlog::debug("[ActivePrintMediaManager] API set: {}", api ? "valid" : "nullptr");
    if (api_) {
        register_moonraker_listeners();
    }
}

void ActivePrintMediaManager::set_thumbnail_source(const std::string& original_filename) {
    // Resolve first. The override exists to display the ORIGINAL name rather
    // than the rewritten one Moonraker reports, and a caller can hand us the
    // rewritten name directly - Reprint does, because it replays whatever
    // print_stats last said, which for a modified print is
    // `.helix_temp/modified_<ts>_orig.gcode`. Storing that raw would also
    // suppress process_filename()'s own auto-resolve, which is guarded on this
    // field being empty, and the panel would show `modified_1748..._orig`.
    // Resolving is identity for a name that is already clean.
    thumbnail_source_filename_ =
        original_filename.empty() ? original_filename : resolve_gcode_filename(original_filename);
    spdlog::debug("[ActivePrintMediaManager] Thumbnail source set to: {}",
                  thumbnail_source_filename_.empty() ? "(cleared)" : thumbnail_source_filename_);

    // If we have a current print filename, re-process it with the new source
    const char* current = lv_subject_get_string(printer_state_.get_print_filename_subject());
    if (current && current[0] != '\0' && !original_filename.empty()) {
        spdlog::info("[ActivePrintMediaManager] Re-processing with source override: {}",
                     original_filename);
        process_filename(current);
    }
}

void ActivePrintMediaManager::clear_thumbnail_source() {
    thumbnail_source_filename_.clear();
    last_effective_filename_.clear();
    last_loaded_thumbnail_filename_.clear();
    cancel_thumbnail_retry();
    spdlog::debug("[ActivePrintMediaManager] Thumbnail source cleared");
}

void ActivePrintMediaManager::publish_thumbnail(const std::string& for_file,
                                                const std::string& path) {
    assert(!path.empty() &&
           "never publish an empty thumbnail path - use no_thumbnail_placeholder()");
    printer_state_.set_print_thumbnail(for_file, path);
}

void ActivePrintMediaManager::set_thumbnail_path(const std::string& for_file,
                                                 const std::string& path) {
    // Set the thumbnail path directly (bypasses Moonraker API lookup). The
    // caller owns the identity: this lands at print-start-confirmed time, which
    // can arrive before Moonraker reports the filename, so neither
    // thumbnail_source_filename_ nor last_effective_filename_ is trustworthy
    // here — the latter may still name the PREVIOUS print.
    // An empty path is the caller saying "no pre-extracted thumbnail"; that is
    // published as the placeholder, never as "".
    publish_thumbnail(for_file, path.empty() ? no_thumbnail_placeholder() : path);
    // A pre-extracted thumbnail (USB / embedded G-code) skips the fetch, but it
    // is NOT a completed load: recovery stays armed.
    thumbnail_origin_ = path.empty() ? ThumbnailOrigin::None : ThumbnailOrigin::PreSet;
    spdlog::debug("[ActivePrintMediaManager] Thumbnail path set directly for '{}': {}", for_file,
                  path);
}

bool ActivePrintMediaManager::has_thumbnail_for(const std::string& filename) {
    const char* current = lv_subject_get_string(printer_state_.get_print_thumbnail_path_subject());
    // The placeholder is what "no thumbnail" looks like on the wire now that the
    // empty string is never published. It must NOT read as a thumbnail here, or
    // the clear below would make load_thumbnail_for_file() skip its own fetch
    // and every print would stop at the placeholder.
    return current && current[0] != '\0' && strcmp(current, no_thumbnail_placeholder()) != 0 &&
           !filename.empty() && printer_state_.get_print_thumbnail_file() == filename;
}

void ActivePrintMediaManager::process_filename(const char* raw_filename) {
    // Empty filename means print ended or idle - DON'T clear immediately
    // The thumbnail/metadata should persist so the user can see what was printing
    // (especially after cancel→firmware_restart where Klipper reports empty filename)
    // Clearing will happen naturally when a NEW print starts with a different filename
    if (!raw_filename || raw_filename[0] == '\0') {
        if (!last_was_empty_) {
            spdlog::debug("[ActivePrintMediaManager] Filename empty - preserving current display");
            last_was_empty_ = true;
        }
        return;
    }
    last_was_empty_ = false;
    helix::MemoryMonitor::log_now("active_media_process_filename", spdlog::level::debug);

    std::string filename = raw_filename;

    // A thumbnail source describes ONE print, and nothing retires it when that
    // print ends: the preparing epoch only re-points it for a print started
    // FROM the app, and a Confirmed exit deliberately keeps it. So a print
    // started anywhere else - Mainsail, Fluidd, the printer's own screen -
    // inherits the previous print's override, computes the previous print's
    // effective filename, matches last_effective_filename_, and early-returns
    // below. The thumbnail subject is never republished and the preview renders
    // the previous print's image for the whole job (#1339).
    //
    // Retire the override here, where we can see the name the printer actually
    // reports, rather than guessing at print-end: an empty filename between
    // jobs is deliberately preserved so a finished print stays readable.
    if (!thumbnail_source_filename_.empty() &&
        !source_describes(filename, thumbnail_source_filename_)) {
        spdlog::debug("[ActivePrintMediaManager] Thumbnail source '{}' does not describe '{}' "
                      "- retiring it",
                      thumbnail_source_filename_, filename);
        clear_thumbnail_source();
    }

    // Auto-resolve temp file patterns to original filename if no override is set
    std::string resolved = resolve_gcode_filename(filename);
    if (resolved != filename && thumbnail_source_filename_.empty()) {
        spdlog::debug("[ActivePrintMediaManager] Auto-resolved temp filename: {} -> {}", filename,
                      resolved);
        thumbnail_source_filename_ = resolved;
    }

    // Compute effective filename (respects thumbnail_source override)
    std::string effective_filename =
        thumbnail_source_filename_.empty() ? filename : thumbnail_source_filename_;

    // Skip if effective filename hasn't changed (makes processing idempotent)
    if (effective_filename == last_effective_filename_) {
        return;
    }
    last_effective_filename_ = effective_filename;

    // Update display filename subject
    std::string display_name = get_display_filename(effective_filename);
    spdlog::debug("[ActivePrintMediaManager] Display filename: {}", display_name);

    // Thread-safe update to display filename subject (RAII via unique_ptr)
    // Capture printer_state_ reference to avoid using global in tests
    PrinterState* state = &printer_state_;
    helix::ui::queue_update<std::string>(
        std::make_unique<std::string>(display_name),
        [state](std::string* name) { state->set_print_display_filename(*name); });

    // Load thumbnail if filename changed
    if (!effective_filename.empty() && effective_filename != last_loaded_thumbnail_filename_) {
        // Whether the published path survives is decided by the path's own
        // identity, not by this manager's history. A path published FOR this
        // file (USB / PrintStartController pre-set) is kept; anything else is
        // cleared so load_thumbnail_for_file() can't short-circuit on it.
        //
        // The old guard asked "have WE loaded a thumbnail before?" and skipped
        // the clear when we hadn't. A manager that never processed the previous
        // print — fresh after a reconnect or a restart mid-print — therefore
        // adopted that print's path as if it were ours.
        const bool preset_for_this_file = has_thumbnail_for(effective_filename);
        if (!preset_for_this_file) {
            // The clear belongs to the file we are about to load for: "nothing
            // yet for effective_filename", not "nothing for the previous print".
            publish_thumbnail(effective_filename, no_thumbnail_placeholder());
#if defined(HELIX_PLATFORM_ESP32)
            // Same clear for the PSRAM slot: on ESP32 the image lives in a
            // PSRAM buffer rather than at a path, and a stale buffer would
            // short-circuit exactly like a stale path. Main thread:
            // process_filename runs from an immediate observer on
            // print_filename, which PrinterState only sets there.
            printer_state_.set_print_psram_thumbnail(nullptr);
#endif
        }
        // New file: drop any pending retry for the previous file and reset
        // the per-filename retry budget.
        cancel_thumbnail_retry();
        thumbnail_retry_count_ = 0;
        thumbnail_origin_ = preset_for_this_file ? ThumbnailOrigin::PreSet : ThumbnailOrigin::None;
        load_thumbnail_for_file(effective_filename);
        last_loaded_thumbnail_filename_ = effective_filename;
    }
}

void ActivePrintMediaManager::load_thumbnail_for_file(const std::string& filename) {
    // A path already published FOR THIS FILE (e.g., PrintStartController set it
    // from USB) makes the thumbnail download unnecessary. We still need metadata
    // for layer_count and estimated_time, so don't early-return. A path
    // published for some OTHER file is not ours to reuse, so it does not skip
    // anything.
    const bool skip_thumbnail = has_thumbnail_for(filename);
    if (skip_thumbnail) {
        spdlog::debug(
            "[ActivePrintMediaManager] Thumbnail already set for '{}', will fetch metadata only",
            filename);
        // Record the provenance WITHOUT claiming the load finished: a pre-set
        // path skips the fetch but must leave the retry ladder and the
        // notification re-triggers armed. Only a completed fetch sets Fetched.
        if (thumbnail_origin_ != ThumbnailOrigin::Fetched) {
            thumbnail_origin_ = ThumbnailOrigin::PreSet;
        }
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[ActivePrintMediaManager] No API available - skipping thumbnail load");
        return;
    }

    // One staleness context per load. Creating it bumps thumbnail_load_generation_,
    // so every callback still in flight from an earlier load now reports stale.
    // Created only after the early-return checks, so no generation is burned when
    // no async op starts. The context also carries our lifetime token, which is
    // what lets it be handed straight to the thumbnail cache below.
    //
    // NOTE: is_valid() consults that token, so it is NOT a thread guard — every
    // check below sits inside an already-marshalled tok.defer() body (L081 Mech C).
    ThumbnailLoadContext ctx = ThumbnailLoadContext::create(lifetime_, &thumbnail_load_generation_);

    // Resolve to original filename if this is a modified temp file
    // (Moonraker only has metadata for original files, not modified copies)
    std::string metadata_filename = resolve_gcode_filename(filename);

    spdlog::debug("[ActivePrintMediaManager] Loading metadata for: {}", metadata_filename);

    // Get file metadata for layer count, estimated time, and optionally thumbnail.
    //
    // THREADING: get_file_metadata invokes its success callback on the Moonraker
    // WebSocket background thread. The ONLY work allowed on that thread is
    // this-free local parsing — everything that touches `this`, `printer_state_`,
    // `api_`, the load context, or any subject is marshalled to the main thread
    // via tok.defer(). The metadata struct is copied by value into the deferred
    // body so the bg-thread reference can't dangle.
    api_->files().get_file_metadata(
        metadata_filename,
        [this, tok = lifetime_.token(), ctx, skip_thumbnail, filename,
         metadata_filename](const FileMetadata& metadata) {
            // bg thread: copy the metadata, then marshal ALL member access to main.
            tok.defer("ActivePrintMediaManager::on_metadata", [this, ctx, skip_thumbnail, filename,
                                                               metadata_filename, metadata]() {
                // main thread, `this` valid + lifetime-checked.
                // Drop stale callbacks superseded by a newer load.
                if (!ctx.is_valid()) {
                    spdlog::trace("[ActivePrintMediaManager] Stale metadata callback, ignoring");
                    return;
                }

                // Slice layer heights power the Z-height current-layer derivation
                // for printers whose slicer never reports a layer number. Thread
                // these through independently of layer_count: heights are useful
                // even when the total came from the gcode-header fallback below.
                if (metadata.layer_height > 0.0) {
                    printer_state_.set_print_layer_heights(metadata.layer_height,
                                                           metadata.first_layer_height);
                    spdlog::debug("[ActivePrintMediaManager] Set layer heights from metadata: "
                                  "layer={:.3f}mm first={:.3f}mm",
                                  metadata.layer_height, metadata.first_layer_height);
                }

                // Set total layer count from metadata
                if (metadata.layer_count > 0) {
                    printer_state_.set_print_layer_total(static_cast<int>(metadata.layer_count));
                    spdlog::debug("[ActivePrintMediaManager] Set total layers from metadata: {}",
                                  metadata.layer_count);
                } else if (lv_subject_get_int(printer_state_.get_print_layer_total_subject()) > 0) {
                    // Retry pass: an earlier attempt already filled the layer
                    // total (gcode header scan) — don't re-download the header.
                    spdlog::debug("[ActivePrintMediaManager] Layer total already set, "
                                  "skipping gcode header re-scan");
                } else {
                    // Moonraker didn't provide layer count — scan gcode header directly.
                    // Download the first 16KB and parse slicer comments for layer info.
                    // Started on the main thread; its bg callback follows the same pattern.
                    spdlog::info("[ActivePrintMediaManager] No layer count in metadata, "
                                 "scanning gcode header");
                    bool need_est_time = (metadata.estimated_time <= 0);
                    api_->transfers().download_file_partial(
                        "gcodes", metadata_filename, 16 * 1024,
                        [this, tok = lifetime_.token(), ctx,
                         need_est_time](const std::string& content) {
                            // bg thread (HttpExecutor::slow worker): parse locally only.
                            auto header =
                                helix::gcode::extract_header_metadata_from_content(content);
                            // main thread: re-check staleness + apply.
                            tok.defer("ActivePrintMediaManager::on_gcode_header",
                                      [this, ctx, need_est_time, header]() {
                                          if (!ctx.is_valid()) {
                                              return;
                                          }
                                          if (header.layer_count > 0) {
                                              printer_state_.set_print_layer_total(
                                                  static_cast<int>(header.layer_count));
                                              spdlog::info("[ActivePrintMediaManager] Set total "
                                                           "layers from gcode header: {}",
                                                           header.layer_count);
                                          }
                                          if (need_est_time && header.estimated_time_seconds > 0) {
                                              printer_state_.set_estimated_print_time(
                                                  static_cast<int>(header.estimated_time_seconds));
                                              spdlog::info(
                                                  "[ActivePrintMediaManager] Set estimated "
                                                  "time from gcode header: {}s",
                                                  static_cast<int>(header.estimated_time_seconds));
                                          }
                                      });
                        },
                        [this, tok = lifetime_.token(), ctx, filename](const MoonrakerError& err) {
                            // Reaching here means metadata came back without a
                            // layer count and the header scan also failed, so
                            // nothing has set layer_total. Without a retry this
                            // print shows layers 0/0 for its entire duration.
                            std::string message = err.message;
                            tok.defer("ActivePrintMediaManager::on_gcode_header_error",
                                      [this, ctx, filename, message = std::move(message)]() {
                                          if (!ctx.is_valid()) {
                                              return; // superseded by a newer load
                                          }
                                          spdlog::warn(
                                              "[ActivePrintMediaManager] Gcode header fetch "
                                              "failed for '{}': {}",
                                              filename, message);
                                          schedule_thumbnail_retry(filename);
                                      });
                        });
                }

                // Store slicer's estimated print time for remaining time fallback
                if (metadata.estimated_time > 0) {
                    printer_state_.set_estimated_print_time(
                        static_cast<int>(metadata.estimated_time));
                    spdlog::debug(
                        "[ActivePrintMediaManager] Set estimated print time from metadata: {}s",
                        metadata.estimated_time);
                }

                // Skip thumbnail fetch if one is already set
                if (skip_thumbnail) {
                    spdlog::debug(
                        "[ActivePrintMediaManager] Skipping thumbnail fetch (already set)");
                    return;
                }

                // Get the largest thumbnail available
                std::string thumbnail_rel_path = metadata.get_largest_thumbnail();
                if (thumbnail_rel_path.empty()) {
                    // Metadata record exists but has no thumbnails. Briefly this
                    // can mean Moonraker is still mid-scan, but the common cause
                    // is a file sliced WITHOUT thumbnails — a permanent
                    // condition. Retry only MAX_EMPTY_THUMBNAIL_RETRIES times and
                    // warn once; filelist_changed/klippy_ready re-triggers cover
                    // the late-scan case beyond that.
                    spdlog::log(thumbnail_retry_count_ == 0 ? spdlog::level::warn
                                                            : spdlog::level::debug,
                                "[ActivePrintMediaManager] No thumbnail in metadata for '{}' "
                                "(attempt {}/{}) - file may lack thumbnails or scan is "
                                "incomplete",
                                metadata_filename, thumbnail_retry_count_ + 1,
                                MAX_EMPTY_THUMBNAIL_RETRIES + 1);
                    schedule_thumbnail_retry(filename, MAX_EMPTY_THUMBNAIL_RETRIES);
                    return;
                }

                spdlog::debug("[ActivePrintMediaManager] Found thumbnail: {}", thumbnail_rel_path);

#if defined(HELIX_PLATFORM_ESP32)
                // ESP32 (Task 11 R2): no disk thumbnail cache on this platform
                // (Task 10 R6), so ThumbnailCache/ThumbnailProcessor are
                // bypassed entirely. Fetch the PNG bytes over the HTTP lane and
                // decode them into a PSRAM-backed lv_image_dsc_t instead of a
                // cache file — same shape as the print-select card fetch in
                // ui_panel_print_select.cpp. print_thumbnail_path_ carries the
                // shared no_thumbnail_placeholder() (benchy) on this platform; the
                // real image arrives via print_psram_thumb_gen, whose observer
                // replaces the placeholder src with the PSRAM descriptor.
                //
                // Moonraker's thumbnail relative_path is relative to the gcode
                // file's PARENT directory, so a print from a subdirectory needs
                // that directory prepended before the path can be downloaded.
                std::string gcode_dir;
                const auto slash_pos = metadata_filename.find_last_of('/');
                if (slash_pos != std::string::npos) {
                    gcode_dir = metadata_filename.substr(0, slash_pos);
                }
                const std::string resolved_thumb_path =
                    resolve_thumbnail_path(thumbnail_rel_path, gcode_dir);
                constexpr size_t ESP32_THUMBNAIL_MAX_BYTES = 512 * 1024;

                // MANDATORY threading: EspHttpLane invokes on_success/on_error
                // directly on its own worker thread with no marshaling. Both
                // callbacks below do only local byte-copy/PSRAM work there and
                // route every member touch through tok.defer(). `this` is
                // captured only to pass into the deferred body, never
                // dereferenced on the worker.
                api_->transfers().download_file_partial(
                    "gcodes", resolved_thumb_path, ESP32_THUMBNAIL_MAX_BYTES,
                    [this, tok = lifetime_.token(), ctx,
                     resolved_thumb_path](const std::string& png_bytes) {
                        // lane worker: PSRAM copy only, no LVGL, no members.
                        auto thumb = helix::ui::EspPsramThumbnail::create(png_bytes);
                        if (!thumb) {
                            spdlog::warn("[ActivePrintMediaManager] PSRAM alloc failed for "
                                         "thumbnail: {}",
                                         resolved_thumb_path);
                            return;
                        }
                        // The last shared_ptr release must happen on the UI
                        // thread — see esp_psram_thumbnail.h. If tok is already
                        // expired, defer() drops the lambda on this worker and
                        // the destructor's on_main_thread() guard skips the
                        // cache drop, which is the safe degenerate case.
                        tok.defer(
                            "ActivePrintMediaManager::on_psram_thumbnail",
                            [this, ctx, resolved_thumb_path, thumb = std::move(thumb)]() mutable {
                                if (!ctx.is_valid()) {
                                    spdlog::trace("[ActivePrintMediaManager] Stale PSRAM "
                                                  "thumbnail callback, ignoring");
                                    return;
                                }
                                printer_state_.set_print_psram_thumbnail(std::move(thumb));
                                if (thumbnail_retry_count_ > 0) {
                                    spdlog::info("[ActivePrintMediaManager] PSRAM thumbnail "
                                                 "loaded after {} retries: {}",
                                                 thumbnail_retry_count_, resolved_thumb_path);
                                } else {
                                    spdlog::info("[ActivePrintMediaManager] PSRAM thumbnail "
                                                 "loaded: {}",
                                                 resolved_thumb_path);
                                }
                                thumbnail_origin_ = ThumbnailOrigin::Fetched;
                                thumbnail_retry_count_ = 0;
                                cancel_thumbnail_retry();
                                helix::MemoryMonitor::log_now("thumbnail_loaded",
                                                              spdlog::level::debug);
                            });
                    },
                    [this, tok = lifetime_.token(), ctx, filename](const MoonrakerError& error) {
                        // lane worker: copy the message, marshal ALL member
                        // access (retry bookkeeping) to main.
                        std::string message = error.message;
                        tok.defer("ActivePrintMediaManager::on_thumbnail_error",
                                  [this, ctx, filename, message = std::move(message)]() {
                                      if (!ctx.is_valid()) {
                                          return; // superseded by a newer load
                                      }
                                      spdlog::warn("[ActivePrintMediaManager] PSRAM thumbnail "
                                                   "download failed for '{}' (attempt {}/{}): {}",
                                                   filename, thumbnail_retry_count_ + 1,
                                                   MAX_THUMBNAIL_ATTEMPTS, message);
                                      schedule_thumbnail_retry(filename);
                                  });
                    });
#else
                // Detail-sized thumbnails (200-400px) — works for both card and detail
                // views since LVGL scales down efficiently. The load's own context goes
                // to the cache, so a result superseded by a newer load is dropped at the
                // cache boundary; our success callback re-checks it after marshalling
                // because the cache's guard alone says nothing about `this`.
                ThumbnailRequest req;
                req.key = thumbnail_rel_path;
                req.target =
                    helix::ThumbnailProcessor::get_target_for_display(helix::ThumbnailSize::Detail);
                req.api = api_;
                // Moonraker's mtime for the gcode file this thumbnail came out
                // of. Without it the cache serves whatever it rendered the first
                // time this filename was printed, so a re-slice under the same
                // name shows the old model for the entire job. Zero (metadata
                // that omits it) degrades to skipping validation, as before.
                req.source_modified = static_cast<time_t>(metadata.modified);

                get_thumbnail_cache().fetch(
                    req, ctx,
                    [this, tok = lifetime_.token(), ctx, filename](const std::string& lvgl_path,
                                                                   bool /*degraded*/) {
                        // bg thread (thumbnail prescale worker): no member access here.
                        std::string path = lvgl_path;
                        tok.defer("ActivePrintMediaManager::on_thumbnail", [this, ctx, filename,
                                                                            path]() {
                            if (!ctx.is_valid()) {
                                spdlog::trace("[ActivePrintMediaManager] Stale thumbnail "
                                              "callback, ignoring");
                                return;
                            }
                            publish_thumbnail(filename, path);
                            if (thumbnail_retry_count_ > 0) {
                                spdlog::info("[ActivePrintMediaManager] Thumbnail loaded after "
                                             "{} retries: {}",
                                             thumbnail_retry_count_, path);
                            } else {
                                spdlog::info("[ActivePrintMediaManager] Thumbnail path set: {}",
                                             path);
                            }
                            thumbnail_origin_ = ThumbnailOrigin::Fetched;
                            thumbnail_retry_count_ = 0;
                            cancel_thumbnail_retry();
                            helix::MemoryMonitor::log_now("thumbnail_loaded", spdlog::level::debug);
                        });
                    },
                    [this, tok = lifetime_.token(), ctx, filename](const std::string& error) {
                        // bg thread (HTTP worker): copy the message, marshal
                        // ALL member access (retry bookkeeping) to main.
                        std::string message = error;
                        tok.defer("ActivePrintMediaManager::on_thumbnail_error",
                                  [this, ctx, filename, message = std::move(message)]() {
                                      if (!ctx.is_valid()) {
                                          return; // superseded by a newer load
                                      }
                                      spdlog::warn("[ActivePrintMediaManager] Thumbnail download "
                                                   "failed for '{}' (attempt {}/{}): {}",
                                                   filename, thumbnail_retry_count_ + 1,
                                                   MAX_THUMBNAIL_ATTEMPTS, message);
                                      schedule_thumbnail_retry(filename);
                                  });
                    });
#endif
            });
        },
        [this, tok = lifetime_.token(), ctx, filename,
         metadata_filename](const MoonrakerError& err) {
            // bg thread (WebSocket response router): copy the message, marshal
            // ALL member access (retry bookkeeping) to main. Happens when
            // Moonraker hasn't finished scanning a just-uploaded file
            // (OrcaSlicer upload-and-print) or on transient RPC failures.
            std::string message = err.message;
            tok.defer("ActivePrintMediaManager::on_metadata_error",
                      [this, ctx, filename, metadata_filename, message = std::move(message)]() {
                          if (!ctx.is_valid()) {
                              return; // superseded by a newer load
                          }
                          spdlog::warn("[ActivePrintMediaManager] Thumbnail metadata fetch failed "
                                       "for '{}' (attempt {}/{}): {}",
                                       metadata_filename, thumbnail_retry_count_ + 1,
                                       MAX_THUMBNAIL_ATTEMPTS, message);
                          schedule_thumbnail_retry(filename);
                      });
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

// ============================================================================
// Bounded thumbnail retry
// ============================================================================

uint32_t ActivePrintMediaManager::retry_delay_ms(int retry_number) {
    switch (retry_number) {
    case 1:
        return 2000;
    case 2:
        return 5000;
    case 3:
        return 10000;
    case 4:
        return 20000;
    default:
        return 30000;
    }
}

void ActivePrintMediaManager::schedule_thumbnail_retry(const std::string& filename,
                                                       int max_retries) {
    // Main thread only — callers are either main-thread code or bg callbacks
    // that marshalled here via tok.defer().
    if (filename.empty() || filename != last_effective_filename_) {
        spdlog::debug("[ActivePrintMediaManager] Skipping retry for '{}' - no longer current",
                      filename);
        return;
    }
    if (retry_timer_) {
        return; // a retry is already pending for this filename
    }
    if (thumbnail_retry_count_ >= max_retries) {
        spdlog::warn("[ActivePrintMediaManager] Giving up on thumbnail for '{}' after {} attempts",
                     filename, thumbnail_retry_count_ + 1);
        return;
    }

    thumbnail_retry_count_++;
    uint32_t delay = retry_delay_ms(thumbnail_retry_count_);
    retry_filename_ = filename;
    retry_generation_ = thumbnail_load_generation_.load(std::memory_order_relaxed);
    retry_timer_.reset(lv_timer_create(retry_timer_cb, delay, this));
    lv_timer_set_repeat_count(retry_timer_.get(), 1); // one-shot

    spdlog::info("[ActivePrintMediaManager] Scheduling thumbnail retry for '{}' in {} ms "
                 "(attempt {}/{})",
                 filename, delay, thumbnail_retry_count_ + 1, max_retries + 1);
}

void ActivePrintMediaManager::rearm_media_if_incomplete() {
    if (last_effective_filename_.empty() || !api_) {
        return;
    }
    const bool have_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject()) > 0;
    const bool have_thumbnail = thumbnail_origin_ == ThumbnailOrigin::Fetched ||
                                thumbnail_origin_ == ThumbnailOrigin::PreSet;
    if (have_layers && have_thumbnail) {
        return; // nothing missing
    }

    spdlog::info("[ActivePrintMediaManager] Print confirmed with incomplete media "
                 "(layers={}, thumbnail={}) - re-arming load for '{}'",
                 have_layers, have_thumbnail, last_effective_filename_);
    cancel_thumbnail_retry();
    thumbnail_retry_count_ = 0;
    load_thumbnail_for_file(last_effective_filename_);
}

void ActivePrintMediaManager::cancel_thumbnail_retry() {
    // LvglTimerGuard::reset() neuters via lv_timer_cancel_safe() instead of
    // deleting — safe even when called from inside an UpdateQueue callback
    // while lv_timer_handler is iterating the timer list (#750/#751).
    retry_timer_.reset();
    retry_filename_.clear();
}

void ActivePrintMediaManager::retry_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ActivePrintMediaManager*>(lv_timer_get_user_data(timer));
    // One-shot timer (repeat_count 1): LVGL deletes it right after this
    // callback returns, so release the guard's reference first.
    self->retry_timer_.release();
    self->on_retry_timer_fired();
}

void ActivePrintMediaManager::on_retry_timer_fired() {
    if (retry_filename_.empty() || retry_filename_ != last_effective_filename_) {
        spdlog::debug("[ActivePrintMediaManager] Stale retry (file changed), skipping");
        return;
    }
    if (retry_generation_ != thumbnail_load_generation_.load(std::memory_order_relaxed)) {
        spdlog::debug("[ActivePrintMediaManager] Stale retry (superseded load), skipping");
        return;
    }
    if (thumbnail_origin_ == ThumbnailOrigin::Fetched) {
        return; // a fetch completed in the meantime
    }
    spdlog::info("[ActivePrintMediaManager] Retrying thumbnail load for '{}' (attempt {}/{})",
                 retry_filename_, thumbnail_retry_count_ + 1, MAX_THUMBNAIL_ATTEMPTS);
    load_thumbnail_for_file(retry_filename_);
}

// ============================================================================
// Moonraker notification re-triggers
// ============================================================================

void ActivePrintMediaManager::register_moonraker_listeners() {
    if (!api_ || listener_api_ == api_) {
        return;
    }
    listener_api_ = api_;

    const std::string suffix = std::to_string(reinterpret_cast<uintptr_t>(this));
    filelist_handler_name_ = "apmm_filelist_" + suffix;
    klippy_ready_handler_name_ = "apmm_klippy_ready_" + suffix;

    // THREADING: method callbacks fire on the WebSocket background thread.
    // Parse into locals there; ALL member access happens on the main thread
    // via token.defer() (same idiom as AmsBackendAd5xIfs's klippy listener).
    // The token captured at registration time expires when the manager is
    // destroyed, so late notifications no-op.
    auto token = lifetime_.token();

    // Re-trigger when the file we're printing (re)appears in the file list —
    // covers Moonraker finishing its metadata scan after the print started.
    api_->register_method_callback(
        "notify_filelist_changed", filelist_handler_name_, [this, token](const json& msg) {
            // bg thread: defensive parse into plain strings only — no member
            // access. Moonraker can send null/missing fields (use find()+is_*).
            std::string action;
            std::string item_path;
            std::string source_path;
            const auto params_it = msg.find("params");
            if (params_it != msg.end() && params_it->is_array() && !params_it->empty()) {
                const json& p = (*params_it)[0];
                if (p.is_object()) {
                    action = json_util::safe_string(p, "action");
                    const auto item_it = p.find("item");
                    if (item_it != p.end() && item_it->is_object()) {
                        item_path = json_util::safe_string(*item_it, "path");
                    }
                    const auto src_it = p.find("source_item");
                    if (src_it != p.end() && src_it->is_object()) {
                        source_path = json_util::safe_string(*src_it, "path");
                    }
                }
            }
            token.defer("ActivePrintMediaManager::on_filelist_changed",
                        [this, action = std::move(action), item_path = std::move(item_path),
                         source_path = std::move(source_path)]() {
                            handle_filelist_changed(action, item_path, source_path);
                        });
        });

    // Re-trigger on klippy ready — covers WebSocket reconnect mid-print where
    // the filename subject never changes, so no observer fires.
    api_->register_method_callback(
        "notify_klippy_ready", klippy_ready_handler_name_, [this, token](const json& /*msg*/) {
            token.defer("ActivePrintMediaManager::on_klippy_ready",
                        [this]() { retrigger_thumbnail_load("klippy_ready"); });
        });

    spdlog::debug("[ActivePrintMediaManager] Registered Moonraker notification listeners");
}

void ActivePrintMediaManager::unregister_moonraker_listeners() {
    // NOTE: during teardown this is a no-op BY DESIGN. Both teardown paths
    // (Application::shutdown and the soft-restart path in application.cpp)
    // call set_moonraker_manager(nullptr) at step 1, long before
    // deinit_active_print_media_manager() runs — so get_moonraker_manager()
    // is always null here during teardown and the unregistration is skipped.
    // The branch below only executes on a set_api() transition (api swapped
    // or cleared while the app is running).
    //
    // Safety for the skipped case does NOT depend on unregistration: the
    // registered lambdas capture a lifetime token that expires when this
    // manager is destroyed (stale notifications parse into locals bg-side and
    // the token.defer() apply no-ops), and the client owning the callback map
    // is destroyed later in the same teardown sequence.
    auto* mgr = get_moonraker_manager();
    if (mgr && listener_api_) {
        if (!filelist_handler_name_.empty()) {
            listener_api_->unregister_method_callback("notify_filelist_changed",
                                                      filelist_handler_name_);
        }
        if (!klippy_ready_handler_name_.empty()) {
            listener_api_->unregister_method_callback("notify_klippy_ready",
                                                      klippy_ready_handler_name_);
        }
    }
    listener_api_ = nullptr;
    filelist_handler_name_.clear();
    klippy_ready_handler_name_.clear();
}

void ActivePrintMediaManager::handle_filelist_changed(const std::string& action,
                                                      const std::string& item_path,
                                                      const std::string& source_path) {
    // Main thread (marshalled via token.defer).
    if (thumbnail_origin_ == ThumbnailOrigin::Fetched || last_effective_filename_.empty()) {
        return;
    }

    // Only actions that can make metadata/thumbnails (re)appear. Notably NOT
    // delete_file — re-querying a just-deleted file would start a
    // guaranteed-to-fail retry ladder.
    if (action != "create_file" && action != "modify_file" && action != "move_file" &&
        action != "root_update") {
        return;
    }

    if (action == "root_update") {
        spdlog::info("[ActivePrintMediaManager] filelist root_update with thumbnail missing");
        retrigger_thumbnail_load("filelist_changed");
        return;
    }

    // Metadata lookups use the resolved filename, so match against both forms.
    const std::string metadata_filename = resolve_gcode_filename(last_effective_filename_);
    const bool item_matches = !item_path.empty() && (item_path == metadata_filename ||
                                                     item_path == last_effective_filename_);
    const bool source_matches = !source_path.empty() && (source_path == metadata_filename ||
                                                         source_path == last_effective_filename_);

    if (action == "move_file" && !item_matches && source_matches) {
        // The printing file was moved/renamed: its metadata now lives under
        // the destination path (item). Reload from there — but only if the
        // notification actually carried a destination.
        if (item_path.empty()) {
            return;
        }
        spdlog::info("[ActivePrintMediaManager] current print '{}' moved to '{}' - reloading "
                     "thumbnail from destination",
                     metadata_filename, item_path);
        thumbnail_retry_count_ = 0;
        cancel_thumbnail_retry();
        load_thumbnail_for_file(item_path);
        return;
    }

    if (!item_matches && !source_matches) {
        return;
    }
    spdlog::info("[ActivePrintMediaManager] filelist change ({}) matches current print '{}'",
                 action, metadata_filename);
    retrigger_thumbnail_load("filelist_changed");
}

void ActivePrintMediaManager::retrigger_thumbnail_load(const char* reason) {
    // Main thread (marshalled via token.defer).
    if (thumbnail_origin_ == ThumbnailOrigin::Fetched || last_effective_filename_.empty()) {
        return;
    }
    spdlog::info("[ActivePrintMediaManager] {} - reloading thumbnail for '{}'", reason,
                 last_effective_filename_);
    thumbnail_retry_count_ = 0;
    cancel_thumbnail_retry();
    load_thumbnail_for_file(last_effective_filename_);
}

void ActivePrintMediaManager::release_identity() {
    thumbnail_source_filename_.clear();
    last_effective_filename_.clear();
    last_loaded_thumbnail_filename_.clear();
    cancel_thumbnail_retry();
    thumbnail_retry_count_ = 0;
    thumbnail_origin_ = ThumbnailOrigin::None;
    spdlog::debug("[ActivePrintMediaManager] Released print identity");
}

void ActivePrintMediaManager::clear_print_info() {
    release_identity();

    // Thread-safe clear of shared subjects. Deferred through the lifetime guard
    // rather than a bare queue_update so the publish can route through
    // publish_thumbnail() — the one enforcement point for the never-empty
    // invariant — without a raw `this` outliving the manager.
    lifetime_.defer("ActivePrintMediaManager::clear_print_info", [this]() {
        // Everything for the previous print is being dropped, including the
        // identity — there is no file this clear is "for".
        publish_thumbnail("", no_thumbnail_placeholder());
#if defined(HELIX_PLATFORM_ESP32)
        // Releases the PSRAM buffer once the UI widgets have dropped their
        // own references; this deferred body runs on the main thread, which
        // the thumbnail's destructor requires.
        printer_state_.set_print_psram_thumbnail(nullptr);
#endif
        printer_state_.set_print_display_filename("");
        spdlog::debug("[ActivePrintMediaManager] Cleared print info subjects");
    });
}

} // namespace helix
