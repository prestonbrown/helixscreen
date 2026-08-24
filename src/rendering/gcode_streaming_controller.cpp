// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "gcode_streaming_controller.h"

#include "memory_monitor.h"
#include "memory_utils.h"
#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace helix {
namespace gcode {

// =============================================================================
// BackgroundGhostBuilder Implementation
// =============================================================================

BackgroundGhostBuilder::~BackgroundGhostBuilder() {
    cancel();
}

void BackgroundGhostBuilder::start(GCodeStreamingController* controller,
                                   RenderCallback render_callback) {
    // Cancel any existing build
    cancel();

    if (!controller || !controller->is_open()) {
        spdlog::warn("[GhostBuilder] Cannot start: controller not ready");
        return;
    }

    controller_ = controller;
    render_callback_ = std::move(render_callback);

    total_layers_.store(controller_->get_layer_count());
    current_layer_.store(0);
    complete_.store(false);
    cancelled_.store(false);
    running_.store(true);

    spdlog::info("[GhostBuilder] Starting background ghost build for {} layers",
                 total_layers_.load());

    worker_ = std::thread(&BackgroundGhostBuilder::worker_thread, this);
}

void BackgroundGhostBuilder::cancel() {
    // Signal cancellation
    cancelled_.store(true);

    // Always join if joinable - even if not running (thread may have completed)
    if (worker_.joinable()) {
        spdlog::debug("[GhostBuilder] Joining ghost build thread");
        worker_.join();
    }

    running_.store(false);
    cancelled_.store(false);
}

float BackgroundGhostBuilder::get_progress() const {
    size_t total = total_layers_.load();
    if (total == 0) {
        return complete_.load() ? 1.0f : 0.0f;
    }
    return static_cast<float>(current_layer_.load()) / static_cast<float>(total);
}

bool BackgroundGhostBuilder::is_complete() const {
    return complete_.load();
}

bool BackgroundGhostBuilder::is_running() const {
    return running_.load();
}

size_t BackgroundGhostBuilder::layers_rendered() const {
    return current_layer_.load();
}

size_t BackgroundGhostBuilder::total_layers() const {
    return total_layers_.load();
}

void BackgroundGhostBuilder::notify_user_request() {
    auto now = std::chrono::steady_clock::now();
    last_user_request_ns_.store(now.time_since_epoch().count());
}

void BackgroundGhostBuilder::worker_thread() {
    spdlog::debug("[GhostBuilder] Worker thread started");

    size_t total = total_layers_.load();

    for (size_t i = 0; i < total && !cancelled_.load(); ++i) {
        // Yield to UI: pause if user recently navigated layers
        auto now = std::chrono::steady_clock::now();
        auto last_ns = last_user_request_ns_.load();
        auto last_request =
            std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_ns));
        while ((now - last_request) < YIELD_DURATION && !cancelled_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            now = std::chrono::steady_clock::now();
            last_ns = last_user_request_ns_.load();
            last_request =
                std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_ns));
        }

        if (cancelled_.load()) {
            break;
        }

        // Load layer segments - hold shared_ptr to keep data alive during callback
        auto segments = controller_->get_layer_segments(i);
        if (segments && render_callback_) {
            render_callback_(i, *segments);
        }

        current_layer_.store(i + 1);

        // Small yield between layers to avoid starving UI thread
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Set running_ false BEFORE complete_ true so callers polling is_complete()
    // always see is_running()==false when is_complete()==true (avoids race on CI)
    running_.store(false);

    if (!cancelled_.load()) {
        complete_.store(true);
        spdlog::info("[GhostBuilder] Ghost build complete ({} layers)", current_layer_.load());
    } else {
        spdlog::debug("[GhostBuilder] Ghost build cancelled at layer {}/{}", current_layer_.load(),
                      total);
    }
}

// Static member initialization
const LayerIndexStats GCodeStreamingController::empty_stats_{};

// =============================================================================
// Construction / Destruction
// =============================================================================

GCodeStreamingController::GCodeStreamingController()
    : cache_(GCodeLayerCache::DEFAULT_BUDGET_NORMAL) {
    // Select cache budget tier based on total system RAM
    auto mem = get_system_memory_info();
    size_t budget;
    const char* tier_name;

    if (mem.is_constrained_device()) {
        budget = GCodeLayerCache::DEFAULT_BUDGET_CONSTRAINED;
        tier_name = "constrained";
    } else if (mem.is_normal_device()) {
        budget = GCodeLayerCache::DEFAULT_BUDGET_NORMAL;
        tier_name = "normal";
    } else {
        budget = GCodeLayerCache::DEFAULT_BUDGET_GOOD;
        tier_name = "good";
    }

    cache_.set_memory_budget(budget);

    // Enable adaptive mode on constrained/normal devices (not desktop)
    if (!mem.is_good_device()) {
        cache_.set_adaptive_mode(true, 15, MIN_CACHE_BUDGET, budget);
    }

    spdlog::info("[StreamingController] {} device (total: {}MB, available: {}MB), "
                 "using {}MB cache budget{}",
                 tier_name, mem.total_mb(), mem.available_mb(), budget / (1024 * 1024),
                 mem.is_good_device() ? "" : " with adaptive mode");

    register_memory_responder();
}

GCodeStreamingController::GCodeStreamingController(size_t cache_budget_bytes)
    : cache_(std::max(cache_budget_bytes, MIN_CACHE_BUDGET)) {
    spdlog::debug("[StreamingController] Created with {:.1f}MB cache budget",
                  static_cast<double>(cache_budget_bytes) / (1024 * 1024));
    register_memory_responder();
}

void GCodeStreamingController::register_memory_responder() {
    // Capture weak_ptr so the callback becomes a no-op after destruction.
    // MemoryMonitor copies the callback list before iterating, so
    // remove_pressure_responder() in the destructor doesn't prevent an
    // already-copied callback from firing.  Use lock() (not expired())
    // to hold the sentinel alive for the duration of the call — this
    // closes the TOCTOU window where the destructor could destroy members
    // between the liveness check and respond_to_memory_pressure()
    // (prestonbrown/helixscreen#733).
    std::weak_ptr<bool> weak = prevent_uaf_sentinel_;
    memory_responder_id_ = helix::MemoryMonitor::instance().add_pressure_responder(
        [this, weak](helix::MemoryPressureLevel level) {
            auto guard = weak.lock();
            if (!guard)
                return;
            if (level >= helix::MemoryPressureLevel::warning) {
                respond_to_memory_pressure();
            }
        });
}

GCodeStreamingController::~GCodeStreamingController() {
    // Join the prefetch worker first: it dereferences `this` on every iteration,
    // so it must be gone before any member starts being destroyed. close() also
    // stops it, but the destructor cannot rely on close() having been called.
    stop_prefetch_worker(/*permanent=*/true);

    // Move sentinel into local — weak.lock() in the callback now returns
    // nullptr for NEW calls, but an already-in-flight lock() still holds
    // a strong reference via the same control block.
    auto sentinel_local = std::move(prevent_uaf_sentinel_);

    // Remove from MemoryMonitor to prevent future invocations
    if (memory_responder_id_ != 0) {
        helix::MemoryMonitor::instance().remove_pressure_responder(memory_responder_id_);
        memory_responder_id_ = 0;
    }

    // Wait for any in-flight pressure callback to finish.  The callback
    // holds a shared_ptr from weak.lock(); when it returns, refcount
    // drops back to 1 (our local).  This closes the TOCTOU window where
    // the callback passed the liveness check but hasn't called
    // respond_to_memory_pressure() yet (#733).
    while (sentinel_local && sentinel_local.use_count() > 1) {
        std::this_thread::yield();
    }
    sentinel_local.reset();

    // Clear completion callback under lock before waiting for the future,
    // matching close() semantics.  Prevents the async thread from invoking
    // a stale callback during destruction.
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        index_complete_callback_ = nullptr;
    }

    // Wait for any async indexing to complete
    if (index_future_.valid()) {
        indexing_.store(false); // Signal cancellation
        try {
            index_future_.wait();
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }
}

// =============================================================================
// File Operations
// =============================================================================

bool GCodeStreamingController::open_file(const std::string& filepath) {
    close();

    spdlog::info("[StreamingController] Opening file: {}", filepath);

    // Create file data source
    auto source = std::make_unique<FileDataSource>(filepath);
    if (!source->is_valid()) {
        spdlog::error("[StreamingController] Failed to open file: {}", filepath);
        return false;
    }

    data_source_ = std::move(source);

    // Build index synchronously
    helix::MemoryMonitor::log_now("gcode_indexing_start");
    if (!build_index()) {
        spdlog::error("[StreamingController] Failed to build index for: {}", filepath);
        data_source_.reset();
        return false;
    }
    helix::MemoryMonitor::log_now("gcode_indexing_done");

    is_open_.store(true);
    spdlog::info("[StreamingController] Opened {} with {} layers", filepath,
                 index_.get_layer_count());

    return true;
}

void GCodeStreamingController::open_file_async(const std::string& filepath,
                                               std::function<void(bool)> on_complete) {
    close();

    spdlog::info("[StreamingController] Opening file async: {}", filepath);

    // Create file data source
    auto source = std::make_unique<FileDataSource>(filepath);
    if (!source->is_valid()) {
        spdlog::error("[StreamingController] Failed to open file: {}", filepath);
        if (on_complete) {
            on_complete(false);
        }
        return;
    }

    data_source_ = std::move(source);
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        index_complete_callback_ = on_complete;
    }
    indexing_.store(true);
    index_progress_.store(0.0f);

    // Build index in background thread.
    // Wrap the whole body in try/catch: a throw escaping this lambda (OOM under
    // memory pressure, fmt format_error from a logging call, etc.) would unwind
    // through std::async's noexcept boundary and terminate the process. On
    // constrained devices (AD5M/AD5X) allocations during indexing of a large
    // gcode file can fail — surface that as a failed load, not a crash.
    index_future_ = std::async(std::launch::async, [this, filepath]() {
        bool success = false;
        try {
            success = build_index();
        } catch (const std::exception& e) {
            spdlog::error("[StreamingController] Exception during build_index: {}", e.what());
            success = false;
        } catch (...) {
            spdlog::error("[StreamingController] Unknown exception during build_index");
            success = false;
        }

        indexing_.store(false);
        index_progress_.store(1.0f);

        try {
            if (success) {
                is_open_.store(true);
                spdlog::info("[StreamingController] Async open complete: {} layers",
                             index_.get_layer_count());
            } else {
                spdlog::error("[StreamingController] Async indexing failed");
                data_source_.reset();
            }

            // Capture callback under lock to prevent race with close()
            // The callback may have been nullified if close() was called
            std::function<void(bool)> callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = index_complete_callback_;
            }

            if (callback) {
                spdlog::debug("[StreamingController] Invoking completion callback (success={})",
                              success);
                callback(success);
                spdlog::debug("[StreamingController] Completion callback returned");
            } else {
                spdlog::debug("[StreamingController] No completion callback registered");
            }
        } catch (const std::exception& e) {
            spdlog::error("[StreamingController] Exception in completion path: {}", e.what());
        } catch (...) {
            spdlog::error("[StreamingController] Unknown exception in completion path");
        }

        return success;
    });
}

bool GCodeStreamingController::open_source(std::unique_ptr<GCodeDataSource> source) {
    close();

    if (!source || !source->is_valid()) {
        spdlog::error("[StreamingController] Invalid data source");
        return false;
    }

    data_source_ = std::move(source);

    if (!build_index()) {
        spdlog::error("[StreamingController] Failed to build index from source");
        data_source_.reset();
        return false;
    }

    is_open_.store(true);
    return true;
}

void GCodeStreamingController::close() {
    // Join the prefetch worker before anything it touches is torn down - it
    // holds `this` and reaches into cache_, index_ and data_source_.
    stop_prefetch_worker();

    // Wait for async operations
    if (index_future_.valid()) {
        indexing_.store(false);
        try {
            index_future_.wait();
        } catch (...) {
        }
    }

    // Clear callback under lock to prevent race with async invocation
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        index_complete_callback_ = nullptr;
    }

    cache_.clear();
    index_.clear();
    data_source_.reset();
    is_open_.store(false);

    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        metadata_extracted_ = false;
        header_metadata_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(name_table_mutex_);
        merged_object_name_table_.clear();
        merged_object_name_lookup_.clear();
    }

    spdlog::debug("[StreamingController] Closed");
}

bool GCodeStreamingController::is_open() const {
    return is_open_.load() && !indexing_.load();
}

bool GCodeStreamingController::is_indexing() const {
    return indexing_.load();
}

float GCodeStreamingController::get_index_progress() const {
    if (!indexing_.load()) {
        return is_open_.load() ? 1.0f : 0.0f;
    }
    return index_progress_.load();
}

std::string GCodeStreamingController::get_source_name() const {
    if (data_source_) {
        return data_source_->source_name();
    }
    return "";
}

// =============================================================================
// Layer Access
// =============================================================================

std::shared_ptr<const std::vector<ToolpathSegment>>
GCodeStreamingController::get_layer_segments(size_t layer_index) {
    if (!is_open() || layer_index >= index_.get_layer_count()) {
        return nullptr;
    }

    // Get from cache (loads if needed)
    auto result = cache_.get_or_load(layer_index, make_loader());

    if (result.load_failed) {
        spdlog::warn("[StreamingController] Failed to load layer {}", layer_index);
        return nullptr;
    }

    // Warm the neighbours on the worker. Doing this inline meant a main-thread
    // caller paid for up to 2*radius+1 extra seek-and-parses before returning.
    schedule_prefetch(layer_index);

    // Return shared_ptr - data stays valid as long as caller holds the pointer
    return result.segments;
}

std::shared_ptr<const std::vector<ToolpathSegment>>
GCodeStreamingController::try_get_layer_segments(size_t layer_index) {
    if (!is_open() || layer_index >= index_.get_layer_count()) {
        return nullptr;
    }

    // Deliberately no prefetch: this exists for callers that must not do work.
    return cache_.try_get(layer_index);
}

void GCodeStreamingController::request_layer(size_t layer_index) {
    if (!is_open() || layer_index >= index_.get_layer_count()) {
        return;
    }

    schedule_prefetch(layer_index);
}

void GCodeStreamingController::schedule_prefetch(size_t center_layer) {
    start_prefetch_worker();

    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        if (stop_prefetch_) {
            return;
        }
        // Overwrite rather than queue: only the newest centre is worth loading,
        // so dragging through layers cannot build a backlog of stale work.
        pending_prefetch_center_ = center_layer;
    }
    prefetch_cv_.notify_one();
}

void GCodeStreamingController::start_prefetch_worker() {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    if (prefetch_thread_.joinable() || stop_prefetch_) {
        return;
    }
    prefetch_thread_ = std::thread(&GCodeStreamingController::prefetch_worker, this);
}

void GCodeStreamingController::stop_prefetch_worker(bool permanent) {
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        stop_prefetch_ = true;
        pending_prefetch_center_.reset();
    }
    prefetch_cv_.notify_all();

    if (prefetch_thread_.joinable()) {
        prefetch_thread_.join();
    }

    // close() rearms so the next open() gets a worker again. The destructor does
    // not: leaving the flag set means a concurrent schedule_prefetch() cannot
    // spawn a thread onto a half-destroyed `this`.
    if (!permanent) {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        stop_prefetch_ = false;
    }
}

void GCodeStreamingController::wait_for_prefetch_idle() {
    std::unique_lock<std::mutex> lock(prefetch_mutex_);
    if (!prefetch_thread_.joinable()) {
        return; // never started - nothing can be pending
    }
    prefetch_idle_cv_.wait(lock,
                           [this] { return !prefetch_running_ && !pending_prefetch_center_; });
}

void GCodeStreamingController::prefetch_worker() {
    spdlog::debug("[StreamingController] Prefetch worker started");

    for (;;) {
        size_t center = 0;
        {
            std::unique_lock<std::mutex> lock(prefetch_mutex_);
            prefetch_cv_.wait(lock, [this] { return stop_prefetch_ || pending_prefetch_center_; });
            if (stop_prefetch_) {
                break;
            }
            center = *pending_prefetch_center_;
            pending_prefetch_center_.reset();
            prefetch_running_ = true;
        }

        // Marks this batch finished and wakes wait_for_prefetch_idle() on every
        // exit path below, including the `continue`s.
        struct BatchGuard {
            GCodeStreamingController* self;
            ~BatchGuard() {
                {
                    std::lock_guard<std::mutex> lock(self->prefetch_mutex_);
                    self->prefetch_running_ = false;
                }
                self->prefetch_idle_cv_.notify_all();
            }
        } batch_guard{this};

        if (!is_open()) {
            continue;
        }

        const size_t layer_count = index_.get_layer_count();
        if (layer_count == 0) {
            continue;
        }

        const size_t radius = prefetch_radius_;
        const size_t start = (center > radius) ? (center - radius) : 0;
        const size_t end = std::min(center + radius, layer_count - 1);

        for (size_t i = start; i <= end; ++i) {
            // Bail out the moment a newer centre lands or we are shutting down -
            // finishing a stale range wastes I/O the constrained devices need.
            {
                std::lock_guard<std::mutex> lock(prefetch_mutex_);
                if (stop_prefetch_ || pending_prefetch_center_) {
                    break;
                }
            }
            cache_.get_or_load(i, make_loader());
        }
    }

    spdlog::debug("[StreamingController] Prefetch worker stopped");
}

bool GCodeStreamingController::is_layer_cached(size_t layer_index) const {
    return cache_.is_cached(layer_index);
}

void GCodeStreamingController::prefetch_around(size_t center_layer, size_t radius) {
    if (!is_open()) {
        return;
    }

    if (index_.get_layer_count() == 0) {
        return; // Nothing to prefetch
    }

    // Delegates to the worker rather than loading inline. The radius argument is
    // honoured by setting the worker's radius for subsequent runs; callers that
    // want a one-shot synchronous load should use get_layer_segments() per layer
    // and accept the cost explicitly.
    prefetch_radius_ = radius;
    schedule_prefetch(center_layer);
}

// =============================================================================
// Layer Information
// =============================================================================

size_t GCodeStreamingController::get_layer_count() const {
    return is_open_.load() ? index_.get_layer_count() : 0;
}

float GCodeStreamingController::get_layer_z(size_t layer_index) const {
    return index_.get_layer_z(layer_index);
}

int GCodeStreamingController::find_layer_at_z(float z) const {
    return index_.find_layer_at_z(z);
}

const LayerIndexStats& GCodeStreamingController::get_index_stats() const {
    if (index_.is_valid()) {
        return index_.get_stats();
    }
    return empty_stats_;
}

size_t GCodeStreamingController::get_file_size() const {
    return data_source_ ? data_source_->file_size() : 0;
}

// =============================================================================
// Cache Management
// =============================================================================

float GCodeStreamingController::get_cache_hit_rate() const {
    return cache_.hit_rate();
}

size_t GCodeStreamingController::get_cache_memory_usage() const {
    return cache_.memory_usage_bytes();
}

size_t GCodeStreamingController::get_cache_budget() const {
    return cache_.memory_budget_bytes();
}

void GCodeStreamingController::set_cache_budget(size_t budget_bytes) {
    cache_.set_memory_budget(std::max(budget_bytes, MIN_CACHE_BUDGET));
}

void GCodeStreamingController::set_adaptive_cache(bool enable) {
    if (enable) {
        // Use current budget as max (respects device tier set at construction)
        cache_.set_adaptive_mode(true, 15, MIN_CACHE_BUDGET, cache_.memory_budget_bytes());
    } else {
        cache_.set_adaptive_mode(false);
    }
}

void GCodeStreamingController::clear_cache() {
    cache_.clear();
}

void GCodeStreamingController::respond_to_memory_pressure() {
    size_t before = cache_.memory_usage_bytes();
    cache_.respond_to_pressure(0.5f);
    size_t after = cache_.memory_usage_bytes();
    spdlog::warn("[StreamingController] Responded to memory pressure, cache now at {:.1f}MB",
                 static_cast<double>(after) / (1024 * 1024));
    if (before > after) {
        crash_handler::breadcrumb::note("stream_evict", "press", static_cast<long>(before - after));
    }
}

// =============================================================================
// Metadata Access
// =============================================================================

const GCodeHeaderMetadata* GCodeStreamingController::get_header_metadata() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return header_metadata_.get();
}

// =============================================================================
// Private Implementation
// =============================================================================

std::vector<ToolpathSegment> GCodeStreamingController::load_layer(size_t layer_index) {
    std::vector<ToolpathSegment> segments;

    // Guard against close() being called between prefetch iterations
    if (!is_open_.load() || !data_source_ || !index_.is_valid()) {
        return segments;
    }

    auto entry = index_.get_entry(layer_index);
    if (!entry.is_valid()) {
        spdlog::warn("[StreamingController] Invalid index entry for layer {}", layer_index);
        return segments;
    }

    // Read layer bytes from source
    auto bytes = data_source_->read_range(entry.file_offset, entry.byte_length);
    if (bytes.empty()) {
        spdlog::warn("[StreamingController] Failed to read bytes for layer {} "
                     "(offset={}, length={})",
                     layer_index, entry.file_offset, entry.byte_length);
        return segments;
    }

    // Parse the bytes line by line
    GCodeParser parser;
    // Seed with the initial tool from the index. Layer chunks don't include
    // the file prologue, so a fresh parser would default to T0 and tag every
    // segment with the wrong tool index — rendering a T3-only print in T0's
    // color.
    parser.set_active_tool_index(index_.get_stats().initial_tool_index);
    // Seed with the head position at this layer's boundary. Without this,
    // the first move of each layer would be drawn from (0,0) — visible as
    // stray travel/extrusion lines from origin in the 2D viewer.
    parser.set_initial_position(entry.start_x, entry.start_y, entry.start_z);
    // Seed with the active ;TYPE: section. Without this, segments in the
    // prologue (purge under ;TYPE:Custom) get tagged Unknown and the bbox
    // filter in auto_fit can't exclude them from the viewport.
    parser.set_initial_feature_type(entry.start_feature_type);
    // Seed with the M82/M83 extrusion mode and running E value. Without this a
    // fresh parser assumes absolute E and reads relative deltas as absolute
    // positions, so its computed delta is the difference between consecutive
    // deltas. Spiral-vase G-code repeats one E value for the whole spiral, which
    // makes that difference exactly zero and classifies the entire model as
    // travel (#1127); ordinary G-code just loses most of its extrusion flags.
    parser.set_initial_extrusion_mode(entry.is_absolute_extrusion(), entry.start_e);
    // Seed layer-marker mode. `;LAYER_CHANGE` lives at the end of the *previous*
    // layer's byte range, so a chunk parse never sees one and would fall back to
    // legacy "every Z change starts a layer" splitting — one Layer object per
    // move for vase-mode G-code.
    parser.set_use_layer_markers(index_.uses_layer_markers());

    // Walk lines directly out of the byte buffer into a reused scratch string. Feeding an
    // istringstream instead would hold three copies of the layer bytes simultaneously
    // (vector + temporary std::string + the stream's internal buffer).
    // Line splitting matches std::getline: '\n' terminated and excluded, any trailing '\r'
    // left on the line for the parser to trim, final line without a newline still parsed.
    const char* data = reinterpret_cast<const char*>(bytes.data());
    const size_t byte_count = bytes.size();
    std::string line;
    size_t pos = 0;

    while (pos < byte_count) {
        const void* nl = std::memchr(data + pos, '\n', byte_count - pos);
        size_t line_end =
            nl ? static_cast<size_t>(static_cast<const char*>(nl) - data) : byte_count;
        line.assign(data + pos, line_end - pos);
        parser.parse_line(line);
        pos = nl ? line_end + 1 : byte_count;
    }

    // Get parsed result
    auto result = parser.finalize();

    // Extract metadata from first layer parsed (thread-safe)
    if (!result.layers.empty()) {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        if (!metadata_extracted_) {
            header_metadata_ = std::make_unique<GCodeHeaderMetadata>();
            header_metadata_->slicer = result.slicer_name;
            header_metadata_->filament_type = result.filament_type;
            header_metadata_->estimated_time_seconds = result.estimated_print_time_minutes * 60.0;
            header_metadata_->filament_used_mm = result.total_filament_mm;
            header_metadata_->layer_count = static_cast<uint32_t>(index_.get_layer_count());
            header_metadata_->tool_colors = result.tool_color_palette;
            metadata_extracted_ = true;
        }
    }

    // Collect all segments from all parsed layers
    // (usually just one layer, but parser may split on Z changes).
    // `result` dies with this call, so steal the single-layer vector rather than copying it.
    if (result.layers.size() == 1) {
        segments = std::move(result.layers[0].segments);
    } else {
        for (const auto& layer : result.layers) {
            segments.insert(segments.end(), layer.segments.begin(), layer.segments.end());
        }
    }

    // Remap local object name indices to the merged string table
    if (!result.object_name_table.empty()) {
        remap_object_name_indices(segments, result.object_name_table);
    }

    spdlog::debug("[StreamingController] Loaded layer {} ({} segments, {} bytes)", layer_index,
                  segments.size(), bytes.size());

    return segments;
}

bool GCodeStreamingController::build_index() {
    if (!data_source_) {
        return false;
    }

    // Use the virtual method to get an indexable file path
    // FileDataSource returns its original filepath; sources that stage data on
    // disk return the staged path once ensure_indexable() has materialized it
    std::string file_path = data_source_->indexable_file_path();

    if (!file_path.empty()) {
        return index_.build_from_file(file_path);
    }

    // Sources without file path (e.g., MemoryDataSource) cannot be indexed
    // with the current file-based indexer
    spdlog::warn("[StreamingController] Data source has no indexable file path");
    return false;
}

std::function<std::vector<ToolpathSegment>(size_t)> GCodeStreamingController::make_loader() {
    return [this](size_t layer_index) { return load_layer(layer_index); };
}

std::string GCodeStreamingController::get_object_name(int16_t index) const {
    if (index < 0)
        return {};
    std::lock_guard<std::mutex> lock(name_table_mutex_);
    if (static_cast<size_t>(index) >= merged_object_name_table_.size())
        return {};
    return merged_object_name_table_[index];
}

void GCodeStreamingController::remap_object_name_indices(
    std::vector<ToolpathSegment>& segments, const std::vector<std::string>& local_table) {
    // Build mapping from local indices to merged indices
    std::lock_guard<std::mutex> lock(name_table_mutex_);

    std::vector<int16_t> local_to_merged(local_table.size());
    for (size_t i = 0; i < local_table.size(); ++i) {
        const auto& name = local_table[i];
        auto it = merged_object_name_lookup_.find(name);
        if (it != merged_object_name_lookup_.end()) {
            local_to_merged[i] = it->second;
        } else {
            auto idx = static_cast<int16_t>(merged_object_name_table_.size());
            merged_object_name_table_.push_back(name);
            merged_object_name_lookup_[name] = idx;
            local_to_merged[i] = idx;
        }
    }

    // Remap all segment indices
    for (auto& seg : segments) {
        if (seg.object_name_index >= 0 &&
            static_cast<size_t>(seg.object_name_index) < local_to_merged.size()) {
            seg.object_name_index = local_to_merged[static_cast<size_t>(seg.object_name_index)];
        }
    }
}

} // namespace gcode
} // namespace helix

#endif // HELIX_HAS_GCODE_VIEWER
