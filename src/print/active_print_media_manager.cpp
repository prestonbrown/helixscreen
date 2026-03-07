// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "active_print_media_manager.h"

#include "ui_filename_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "memory_monitor.h"
#include "observer_factory.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>

using helix::gcode::get_display_filename;
using helix::gcode::resolve_gcode_filename;

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
    // Observe print_filename_ subject to react to filename changes
    print_filename_observer_ = helix::ui::observe_string<ActivePrintMediaManager>(
        printer_state_.get_print_filename_subject(), this,
        [](ActivePrintMediaManager* self, const char* filename) {
            self->process_filename(filename);
        });

    spdlog::debug("[ActivePrintMediaManager] Observer attached to print_filename subject");
}

ActivePrintMediaManager::~ActivePrintMediaManager() {
    // ObserverGuard handles cleanup automatically
    // NOTE: No logging here - spdlog may be destroyed before this singleton
}

void ActivePrintMediaManager::set_api(MoonrakerAPI* api) {
    api_ = api;
    spdlog::debug("[ActivePrintMediaManager] API set: {}", api ? "valid" : "nullptr");
}

void ActivePrintMediaManager::set_thumbnail_source(const std::string& original_filename) {
    thumbnail_source_filename_ = original_filename;
    spdlog::debug("[ActivePrintMediaManager] Thumbnail source set to: {}",
                  original_filename.empty() ? "(cleared)" : original_filename);

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
    spdlog::debug("[ActivePrintMediaManager] Thumbnail source cleared");
}

void ActivePrintMediaManager::set_thumbnail_path(const std::string& path) {
    // Set the thumbnail path directly (bypasses Moonraker API lookup)
    printer_state_.set_print_thumbnail_path(path);
    spdlog::debug("[ActivePrintMediaManager] Thumbnail path set directly: {}", path);
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
        // Clear stale thumbnail path from the previous print so load_thumbnail_for_file()
        // doesn't short-circuit with the old thumbnail. This fixes the bug where starting
        // a new print via Mainsail would show the previous print's thumbnail.
        // Only clear if we previously loaded a thumbnail for a different file — if
        // last_loaded_thumbnail_filename_ is empty, this is the first print and any
        // existing thumbnail was intentionally pre-set (e.g., USB/PrintStartController).
        if (!last_loaded_thumbnail_filename_.empty()) {
            printer_state_.set_print_thumbnail_path("");
        }
        load_thumbnail_for_file(effective_filename);
        last_loaded_thumbnail_filename_ = effective_filename;
    }
}

void ActivePrintMediaManager::load_thumbnail_for_file(const std::string& filename) {
    // If we already have a directly-set thumbnail path, don't overwrite it.
    // This happens when PrintStartController sets the path from a pre-extracted
    // USB thumbnail before the filename observer fires.
    const char* current_thumb =
        lv_subject_get_string(printer_state_.get_print_thumbnail_path_subject());
    if (current_thumb && current_thumb[0] != '\0') {
        spdlog::debug("[ActivePrintMediaManager] Thumbnail already set ({}), skipping API lookup",
                      current_thumb);
        return;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[ActivePrintMediaManager] No API available - skipping thumbnail load");
        return;
    }

    // Increment generation to invalidate any in-flight async operations
    // (only after early-return checks to avoid incrementing when no async op starts)
    ++thumbnail_load_generation_;
    uint32_t current_gen = thumbnail_load_generation_;

    // Resolve to original filename if this is a modified temp file
    // (Moonraker only has metadata for original files, not modified copies)
    std::string metadata_filename = resolve_gcode_filename(filename);

    spdlog::debug("[ActivePrintMediaManager] Loading thumbnail for: {}", metadata_filename);

    // Get file metadata to find thumbnail path
    api_->files().get_file_metadata(
        metadata_filename,
        [this, current_gen](const FileMetadata& metadata) {
            // Check if this callback is still relevant
            if (current_gen != thumbnail_load_generation_) {
                spdlog::trace(
                    "[ActivePrintMediaManager] Stale metadata callback (gen {} != {}), ignoring",
                    current_gen, thumbnail_load_generation_);
                return;
            }

            // Also set total layer count from metadata while we have it
            if (metadata.layer_count > 0) {
                int layer_count = static_cast<int>(metadata.layer_count);
                PrinterState* state = &printer_state_;
                helix::ui::queue_update<int>(
                    std::make_unique<int>(layer_count),
                    [state](int* count) { state->set_print_layer_total(*count); });
                spdlog::debug("[ActivePrintMediaManager] Set total layers from metadata: {}",
                              metadata.layer_count);
            }

            // Store slicer's estimated print time for remaining time fallback
            if (metadata.estimated_time > 0) {
                int est_time = static_cast<int>(metadata.estimated_time);
                PrinterState* state = &printer_state_;
                helix::ui::queue_update<int>(
                    std::make_unique<int>(est_time),
                    [state](int* seconds) { state->set_estimated_print_time(*seconds); });
                spdlog::debug("[ActivePrintMediaManager] Set estimated print time from metadata: {}s",
                              metadata.estimated_time);
            }

            // Get the largest thumbnail available
            std::string thumbnail_rel_path = metadata.get_largest_thumbnail();
            if (thumbnail_rel_path.empty()) {
                spdlog::debug("[ActivePrintMediaManager] No thumbnail available in metadata");
                return;
            }

            spdlog::debug("[ActivePrintMediaManager] Found thumbnail: {}", thumbnail_rel_path);

            // Use detail-sized thumbnails (200-400px) — works for both card and detail views
            // since LVGL scales down efficiently
            ThumbnailLoadContext ctx;
            ctx.alive = alive_;
            ctx.generation = nullptr; // Using manual gen check below
            ctx.captured_gen = current_gen;

            get_thumbnail_cache().fetch_for_detail_view(
                api_, thumbnail_rel_path, ctx,
                [this, current_gen](const std::string& lvgl_path) {
                    // Note: alive check is done by fetch_for_detail_view's guard.
                    // We still need generation check since we passed nullptr for generation.
                    if (current_gen != thumbnail_load_generation_) {
                        spdlog::trace(
                            "[ActivePrintMediaManager] Stale thumbnail callback, ignoring");
                        return;
                    }

                    // Thread-safe update to thumbnail path subject (RAII via unique_ptr)
                    PrinterState* state = &printer_state_;
                    helix::ui::queue_update<std::string>(
                        std::make_unique<std::string>(lvgl_path), [state](std::string* path) {
                            state->set_print_thumbnail_path(*path);
                            spdlog::info("[ActivePrintMediaManager] Thumbnail path set: {}", *path);
                            helix::MemoryMonitor::log_now("thumbnail_loaded", spdlog::level::debug);
                        });
                },
                [](const std::string& error) {
                    spdlog::warn("[ActivePrintMediaManager] Failed to fetch thumbnail: {}", error);
                });
        },
        [metadata_filename](const MoonrakerError& err) {
            spdlog::debug("[ActivePrintMediaManager] Failed to get file metadata for '{}': {}",
                          metadata_filename, err.message);
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

void ActivePrintMediaManager::clear_print_info() {
    thumbnail_source_filename_.clear();
    last_effective_filename_.clear();
    last_loaded_thumbnail_filename_.clear();

    // Thread-safe clear of shared subjects (capture printer_state_ for testability)
    PrinterState* state = &printer_state_;
    helix::ui::queue_update([state]() {
        state->set_print_thumbnail_path("");
        state->set_print_display_filename("");
        spdlog::debug("[ActivePrintMediaManager] Cleared print info subjects");
    });
}

} // namespace helix
