// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Define STB implementations in this compilation unit only
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include "thumbnail_processor.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "helix_thread.h"
#include "lvgl_image_writer.h"
#include "memory_monitor.h"
#include "system/crash_handler.h"
#include "thumbnail_cache.h"

#include <hv/hthreadpool.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

// stb headers - single-file libraries for image processing
#include "stb_image.h"
#include "stb_image_resize.h"

// LVGL headers for correct binary format
#include <lvgl/src/draw/lv_image_dsc.h>

namespace helix {

// Default cache directory - will be overridden by ThumbnailCache when it initializes.
// This is just a fallback for early initialization before ThumbnailCache runs.
static constexpr const char* DEFAULT_CACHE_DIR = "/tmp/helix_thumbs";

// Thread pool configuration
static constexpr int MIN_WORKER_THREADS = 1;
static constexpr int MAX_WORKER_THREADS = 2; // Don't starve UI thread on single-core

// Safety limits to prevent memory exhaustion and integer overflow
static constexpr size_t MAX_PNG_INPUT_SIZE = 10 * 1024 * 1024; // 10 MB compressed
static constexpr int MAX_SOURCE_DIMENSION = 4096;              // 4K max source
static constexpr int MAX_OUTPUT_DIMENSION = 1024;              // 1K max output

// ============================================================================
// Singleton
// ============================================================================

ThumbnailProcessor& ThumbnailProcessor::instance() {
    static ThumbnailProcessor instance;
    return instance;
}

ThumbnailProcessor::ThumbnailProcessor()
    : thread_pool_(std::make_shared<HThreadPool>(MIN_WORKER_THREADS, MAX_WORKER_THREADS)),
      cache_dir_(get_helix_cache_dir("helix_thumbs")) {
    if (cache_dir_.empty()) {
        cache_dir_ = DEFAULT_CACHE_DIR; // last-resort; ThumbnailCache re-points later
    }
    // Ensure cache directory exists
    try {
        std::filesystem::create_directories(cache_dir_);
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailProcessor] Failed to create cache directory: {}", e.what());
    }

    // Start thread pool
    thread_pool_->start(MIN_WORKER_THREADS);
    spdlog::debug("[ThumbnailProcessor] Initialized with {} worker threads, cache: {}",
                  MIN_WORKER_THREADS, cache_dir_);
}

ThumbnailProcessor::~ThumbnailProcessor() {
    shutdown();
}

void ThumbnailProcessor::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) {
        return;
    }
    shutdown_ = true;

    if (thread_pool_) {
        thread_pool_->stop();
        thread_pool_.reset();
    }
    // Note: Don't log here - this may be called during static destruction
    // when spdlog is already destroyed (static destruction order fiasco)
}

// ============================================================================
// Public API
// ============================================================================

void ThumbnailProcessor::deliver_result(const ProcessResult& result, const std::string& source,
                                        ProcessSuccessCallback on_success,
                                        ProcessErrorCallback on_error) {
    if (result.success) {
        spdlog::debug("[ThumbnailProcessor] Processed {} -> {} ({}x{})", source, result.output_path,
                      result.output_width, result.output_height);
        if (on_success) {
            // CRITICAL: Defer callback to main UI thread to avoid LVGL threading
            // issues. Without this, callbacks can trigger widget operations from
            // worker thread, causing "lv_inv_area() rendering_in_progress"
            // assertion on slow devices.
            struct SuccessCtx {
                ProcessSuccessCallback callback;
                std::string path;
            };
            auto ctx = std::make_unique<SuccessCtx>(SuccessCtx{on_success, result.output_path});
            helix::ui::queue_update<SuccessCtx>(std::move(ctx),
                                                [](SuccessCtx* c) { c->callback(c->path); });
        }
    } else {
        spdlog::warn("[ThumbnailProcessor] Failed to process {}: {}", source, result.error);
        if (on_error) {
            // CRITICAL: Defer callback to main UI thread (same reason as on_success)
            struct ErrorCtx {
                ProcessErrorCallback callback;
                std::string error;
            };
            auto ctx = std::make_unique<ErrorCtx>(ErrorCtx{on_error, result.error});
            helix::ui::queue_update<ErrorCtx>(std::move(ctx),
                                              [](ErrorCtx* c) { c->callback(c->error); });
        }
    }
}

void ThumbnailProcessor::process_file_async(const std::string& png_path,
                                            const std::string& source_path,
                                            const ThumbnailTarget& target,
                                            ProcessSuccessCallback on_success,
                                            ProcessErrorCallback on_error) {
    auto path_copy = png_path;
    auto source_copy = source_path;
    // ThumbnailCache sweeps a source's variants by this same key.
    std::string key = ThumbnailCache::compute_hash(source_path);

    // Same locked-commit structure as process_async() — see the #1202 commentary
    // there for why commit() must happen under mutex_.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutdown_ && thread_pool_) {
            std::string cache_dir_copy = cache_dir_;
            std::shared_ptr<ThumbnailWriteJournal> journal_copy = write_journal_.lock();

            thread_pool_->commit([this, path_copy = std::move(path_copy),
                                  source_copy = std::move(source_copy), key = std::move(key),
                                  cache_dir_copy = std::move(cache_dir_copy),
                                  journal_copy = std::move(journal_copy), target, on_success,
                                  on_error]() {
                // libhv's HThreadPool spawns its own workers, so each task
                // makes sure the worker it lands on has a signal stack.
                helix::install_thread_altstack();
                // The read happens HERE, on the worker. Callers used to slurp the
                // PNG on the main thread purely to hand the bytes straight back
                // to this pool - once per file while a listing populated.
                std::ifstream file(path_copy, std::ios::binary | std::ios::ate);
                if (!file) {
                    deliver_result(ProcessResult{false, "", "Cannot read PNG: " + path_copy, 0, 0},
                                   source_copy, on_success, on_error);
                    return;
                }

                const std::streamsize size = file.tellg();
                file.seekg(0, std::ios::beg);

                std::vector<uint8_t> png_data(static_cast<size_t>(size));
                if (!file.read(reinterpret_cast<char*>(png_data.data()), size)) {
                    deliver_result(
                        ProcessResult{false, "", "Failed to read PNG data: " + path_copy, 0, 0},
                        source_copy, on_success, on_error);
                    return;
                }
                file.close();

                ProcessResult result =
                    do_process(png_data, source_copy, key, target, cache_dir_copy, journal_copy);
                deliver_result(result, source_copy, on_success, on_error);
            });
            return;
        }
    }

    if (on_error) {
        on_error("Thumbnail processor is shut down");
    }
}

void ThumbnailProcessor::process_async(const std::vector<uint8_t>& png_data,
                                       const std::string& source_path,
                                       const ThumbnailTarget& target,
                                       ProcessSuccessCallback on_success,
                                       ProcessErrorCallback on_error) {
    // Copy the inputs BEFORE the critical section: commit() captures them by
    // value and a multi-megabyte PNG copy has no business inside a lock that
    // shutdown() needs to acquire.
    auto png_copy = png_data;
    auto source_copy = source_path;
    std::string key = ThumbnailCache::compute_hash(source_path);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutdown_ && thread_pool_) {
            std::string cache_dir_copy = cache_dir_; // guard against set_cache_dir()

            // Snapshotted with the directory, not looked up when the task runs:
            // the write lands in cache_dir_copy, so it must be reported to the
            // journal that was watching cache_dir_copy. Taking a strong
            // reference here also means do_process() never touches mutex_,
            // which shutdown() may be holding while it waits for this task.
            std::shared_ptr<ThumbnailWriteJournal> journal_copy = write_journal_.lock();

            // commit() MUST run while the lock is held, not merely be reached
            // after a null-check. Two reasons, both load-bearing (#1202):
            //
            //  1. shutdown() resets thread_pool_ under this same mutex, so a
            //     check-then-unlock-then-dereference is a use-after-free
            //     window on the pool object. That is the race ThreadSanitizer
            //     reported here.
            //  2. HThreadPool::commit() is not a passive dereference — it
            //     opens with `if (status == STOP) start();`, so a commit
            //     racing shutdown() would RESURRECT the pool and spawn worker
            //     threads after teardown, which is exactly the static
            //     destruction hazard shutdown() exists to avoid.
            //
            // Holding the lock is cheap here: commit() takes its own
            // task_mutex briefly, emplaces into an unbounded queue and
            // notifies. It does not block.
            thread_pool_->commit(
                [this, png_copy = std::move(png_copy), source_copy = std::move(source_copy),
                 key = std::move(key), cache_dir_copy = std::move(cache_dir_copy),
                 journal_copy = std::move(journal_copy), target, on_success, on_error]() {
                    helix::install_thread_altstack();
                    ProcessResult result = do_process(png_copy, source_copy, key, target,
                                                      cache_dir_copy, journal_copy);
                    deliver_result(result, source_copy, on_success, on_error);
                });
            return;
        }
    }

    // Shut down. Reported OUTSIDE the lock so a caller's error handler cannot
    // re-enter this object and deadlock on mutex_.
    if (on_error) {
        on_error("ThumbnailProcessor is shutdown");
    }
}

ProcessResult ThumbnailProcessor::process_sync(const std::vector<uint8_t>& png_data,
                                               const std::string& source_path,
                                               const ThumbnailTarget& target) {
    // Get cache_dir and its matching write listener under lock for thread safety
    std::string cache_dir_copy;
    std::shared_ptr<ThumbnailWriteJournal> journal_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_dir_copy = cache_dir_;
        journal_copy = write_journal_.lock();
    }
    return do_process(png_data, source_path, ThumbnailCache::compute_hash(source_path), target,
                      cache_dir_copy, journal_copy);
}

std::string ThumbnailProcessor::get_if_processed(const std::string& source_path,
                                                 const ThumbnailTarget& target) const {
    // Get cache_dir under lock for thread safety
    std::string cache_dir_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_dir_copy = cache_dir_;
    }

    std::string filename =
        generate_cache_filename(ThumbnailCache::compute_hash(source_path), target);
    std::string full_path = cache_dir_copy + "/" + filename;

    if (std::filesystem::exists(full_path)) {
        spdlog::trace("[ThumbnailProcessor] Cache hit: {}", full_path);
        return "A:" + full_path;
    }

    return "";
}

void ThumbnailProcessor::set_cache_dir(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_ != path) {
        spdlog::debug("[ThumbnailProcessor] Cache directory updated: {}", path);
        cache_dir_ = path;

        try {
            std::filesystem::create_directories(cache_dir_);
        } catch (const std::filesystem::filesystem_error& e) {
            spdlog::warn("[ThumbnailProcessor] Failed to create cache directory {}: {}", cache_dir_,
                         e.what());
        }
    }
}

void ThumbnailProcessor::set_write_journal(std::weak_ptr<ThumbnailWriteJournal> journal) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_journal_ = std::move(journal);
}

void ThumbnailProcessor::clear_cache() {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
            if (entry.path().extension() == ".bin") {
                std::filesystem::remove(entry.path());
            }
        }
        spdlog::info("[ThumbnailProcessor] Cache cleared");
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailProcessor] Failed to clear cache: {}", e.what());
    }
}

size_t ThumbnailProcessor::pending_tasks() const {
    // Was an unlocked null-check followed by an unlocked dereference — the same
    // use-after-free window as process_async(), against shutdown()'s reset
    // (#1202). All three reads are cheap, so the whole thing goes under the lock.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_pool_) {
        return 0;
    }
    // taskNum() alone only counts tasks still sitting in the queue: a task a
    // worker has already popped and is executing drops out of it before the
    // task itself finishes, so it must not be read as "nothing pending" on
    // its own. currentThreadNum() - idleThreadNum() adds back exactly what
    // HThreadPool::wait()'s own idle condition checks
    // (`tasks.empty() && idle_thread_num == cur_thread_num`).
    size_t queued = thread_pool_->taskNum();
    int busy_workers = thread_pool_->currentThreadNum() - thread_pool_->idleThreadNum();
    return queued + (busy_workers > 0 ? static_cast<size_t>(busy_workers) : 0);
}

void ThumbnailProcessor::wait_for_completion() {
    // Same defect as pending_tasks(), but wait() BLOCKS until the queue drains,
    // so it must not be called with mutex_ held — shutdown() would be stuck
    // behind it. Take a strong reference under the lock instead and wait on
    // that: the pool then outlives a concurrent shutdown() reset, and wait()
    // (unlike commit()) will not restart a stopped pool.
    std::shared_ptr<HThreadPool> pool;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool = thread_pool_;
    }
    if (pool) {
        pool->wait();
    }
}

void ThumbnailProcessor::submit_test_task(std::function<void()> task) {
    // commit() must run under mutex_, the same as process_async()/process_from_path()
    // above (#1202): HThreadPool::commit() opens with `if (status == STOP) start();`,
    // so releasing the lock first would let a concurrent shutdown() stop the pool and
    // then have this commit() resurrect it.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutdown_ && thread_pool_) {
        thread_pool_->commit([task = std::move(task)] {
            helix::install_thread_altstack();
            task();
        });
    }
}

// ============================================================================
// Private Implementation
// ============================================================================

std::string ThumbnailProcessor::generate_cache_filename(const std::string& cache_key,
                                                        const ThumbnailTarget& target) const {
    // Always ARGB8888 now
    const char* format_str = "ARGB8888";

    // Generate filename: {hash}_{w}x{h}_{format}.bin
    // NOTE: Must use .bin extension for LVGL's bin decoder (lv_bin_decoder.c only accepts .bin)
    char filename[128];
    std::snprintf(filename, sizeof(filename), "%s_%dx%d_%s.bin", cache_key.c_str(), target.width,
                  target.height, format_str);

    return filename;
}

// A complete PNG starts with the 8-byte signature and ends with an IEND chunk.
// stb_image is fragile on malformed/truncated input — crafted or severed streams
// can drive heap overreads — and the gcode-header extraction fallback can hand us
// a PNG cut mid-stream by the 100 KB partial-download boundary. A bad decode runs
// on the worker thread and corrupts the heap, surfacing later as an unrelated
// glibc abort on the main thread (debug bundle 783DVYKD). Reject non-PNG and
// truncated data before stb_image ever touches it.
static bool is_complete_png(const std::vector<uint8_t>& data) {
    static const unsigned char SIG[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const unsigned char IEND[4] = {0x49, 0x45, 0x4E, 0x44}; // "IEND"

    if (data.size() < 16) {
        return false; // too small to hold signature + a terminating IEND chunk
    }
    if (std::memcmp(data.data(), SIG, sizeof(SIG)) != 0) {
        return false; // not a PNG
    }
    // A complete stream ends with [len=0]["IEND"][CRC], so the IEND marker sits in
    // the last 12 bytes; scan the final 16 to tolerate a stray trailing byte.
    constexpr size_t window = 16;
    const unsigned char* tail = data.data() + (data.size() - window);
    for (size_t i = 0; i + sizeof(IEND) <= window; ++i) {
        if (std::memcmp(tail + i, IEND, sizeof(IEND)) == 0) {
            return true;
        }
    }
    return false; // no IEND near the end → truncated/corrupt
}

ProcessResult
ThumbnailProcessor::do_process(const std::vector<uint8_t>& png_data, const std::string& source_path,
                               const std::string& cache_key, const ThumbnailTarget& target,
                               const std::string& cache_dir,
                               const std::shared_ptr<ThumbnailWriteJournal>& journal) {
    ProcessResult result;

    if (png_data.empty()) {
        result.error = "Empty PNG data";
        return result;
    }

    // Safety check: reject excessively large PNG files
    if (png_data.size() > MAX_PNG_INPUT_SIZE) {
        result.error = "PNG too large (" + std::to_string(png_data.size() / 1024 / 1024) +
                       " MB, max " + std::to_string(MAX_PNG_INPUT_SIZE / 1024 / 1024) + " MB)";
        return result;
    }

    // Reject truncated/corrupt or non-PNG data before stb_image touches it (see
    // is_complete_png). All thumbnails in this pipeline are PNG (Moonraker-served
    // or extracted from gcode), so a complete-PNG gate is both correct and tightest.
    if (!is_complete_png(png_data)) {
        result.error = "Incomplete or non-PNG thumbnail data (truncated or corrupt)";
        return result;
    }

    // ========================================================================
    // Step 1: Decode PNG with stb_image
    // ========================================================================
    int src_width = 0, src_height = 0, src_channels = 0;

    helix::MemoryMonitor::log_now("thumbnail_decode_start");

    // Bracket the decode so the next crash bundle shows directly whether one
    // was in flight, instead of it having to be inferred from a post-restart
    // log the way 6F3QJLFG's was (prestonbrown/helixscreen#960). Worker thread
    // — breadcrumb::note() is written for concurrent producers.
    crash_handler::breadcrumb::note("thumb", "decode_begin", static_cast<long>(png_data.size()));

    // Read the dimensions from the header first: the decode allocates
    // width*height*4 bytes, which on a 128 MB device must not be attempted for
    // an image over the cap.
    if (!stbi_info_from_memory(png_data.data(), static_cast<int>(png_data.size()), &src_width,
                               &src_height, &src_channels)) {
        result.error = std::string("Failed to read PNG header: ") + stbi_failure_reason();
        return result;
    }
    if (src_width > MAX_SOURCE_DIMENSION || src_height > MAX_SOURCE_DIMENSION) {
        result.error = "Source image too large (" + std::to_string(src_width) + "x" +
                       std::to_string(src_height) + ", max " +
                       std::to_string(MAX_SOURCE_DIMENSION) + ")";
        return result;
    }

    // stbi_load_from_memory returns RGBA data (4 channels) when we request it
    unsigned char* src_pixels = stbi_load_from_memory(
        png_data.data(), static_cast<int>(png_data.size()), &src_width, &src_height, &src_channels,
        4 // Request RGBA output regardless of source format
    );

    if (!src_pixels) {
        result.error = std::string("Failed to decode PNG: ") + stbi_failure_reason();
        return result;
    }

    spdlog::trace("[ThumbnailProcessor] Decoded {}x{} ({} channels)", src_width, src_height,
                  src_channels);

    // ========================================================================
    // Step 2: Calculate output dimensions (preserve aspect ratio, cover target)
    // ========================================================================
    // Scale to fit within the target area while maintaining aspect ratio.
    // Using min() ensures the image never exceeds the target dimensions.

    float scale_x = static_cast<float>(target.width) / src_width;
    float scale_y = static_cast<float>(target.height) / src_height;
    float scale = std::min(scale_x, scale_y);

    int out_width = static_cast<int>(src_width * scale);
    int out_height = static_cast<int>(src_height * scale);

    // Ensure minimum dimensions
    out_width = std::max(out_width, 1);
    out_height = std::max(out_height, 1);

    // Clamp output dimensions to prevent integer overflow in buffer allocation
    out_width = std::min(out_width, MAX_OUTPUT_DIMENSION);
    out_height = std::min(out_height, MAX_OUTPUT_DIMENSION);

    spdlog::trace("[ThumbnailProcessor] Scaling {}x{} -> {}x{} (scale: {:.2f})", src_width,
                  src_height, out_width, out_height, scale);

    // ========================================================================
    // Step 3: Resize with stb_image_resize (high-quality Mitchell filter)
    // ========================================================================
    std::vector<unsigned char> resized_pixels(out_width * out_height * 4);

    int resize_result =
        stbir_resize_uint8(src_pixels, src_width, src_height, 0,            // input
                           resized_pixels.data(), out_width, out_height, 0, // output
                           4                                                // RGBA channels
        );

    // Free source pixels - we're done with them
    stbi_image_free(src_pixels);

    if (!resize_result) {
        result.error = "Failed to resize image";
        return result;
    }

    // ========================================================================
    // Step 4: Convert RGBA to ARGB8888 (LVGL's expected format)
    // ========================================================================
    // stb_image gives us RGBA (R,G,B,A order)
    // LVGL ARGB8888 expects (B,G,R,A order) - actually BGRA in memory
    //
    // Wait, let's check the LVGL format more carefully...
    // LV_COLOR_FORMAT_ARGB8888 on little-endian is stored as B,G,R,A in memory
    // because when read as uint32_t, it's 0xAARRGGBB

    for (size_t i = 0; i < resized_pixels.size(); i += 4) {
        // Swap R and B channels: RGBA -> BGRA
        std::swap(resized_pixels[i], resized_pixels[i + 2]);
    }

    helix::MemoryMonitor::log_now("thumbnail_resize_done");

    // Detail encodes the output geometry as w*10000+h — the crash record is
    // line-oriented, and one number reads more cleanly than a formatted string.
    crash_handler::breadcrumb::note("thumb", "decode_end",
                                    static_cast<long>(out_width) * 10000 + out_height);

    // ========================================================================
    // Step 5: Write LVGL binary file
    // ========================================================================
    std::string filename = generate_cache_filename(cache_key, target);
    std::string output_path = cache_dir + "/" + filename;

    if (!write_lvbin(output_path, out_width, out_height, target.color_format, resized_pixels.data(),
                     resized_pixels.size())) {
        result.error = "Failed to write .bin file";
        return result;
    }

    // Tell ThumbnailCache about the bytes we just put in its directory. It has
    // no other way to find out short of re-walking the whole cache, which is
    // the cost its index exists to avoid (prestonbrown/helixscreen#1207).
    // Reported only on success, so the index never learns about a file that was
    // not written.
    if (journal) {
        journal->note_write(output_path);
    }

    result.success = true;
    result.output_path = "A:" + output_path;
    result.output_width = out_width;
    result.output_height = out_height;

    return result;
}

bool ThumbnailProcessor::write_lvbin(const std::string& path, int width, int height,
                                     uint8_t color_format, const uint8_t* pixel_data,
                                     size_t data_size) {
    return helix::write_lvgl_bin(path, width, height, color_format, pixel_data, data_size);
}

} // namespace helix
