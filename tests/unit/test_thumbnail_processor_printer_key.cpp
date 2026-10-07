// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_processor_printer_key.cpp
 * @brief A pre-scaled thumbnail is named for the printer it was requested on, even when a
 *        switch lands while it waits for a worker.
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/thumbnail_processor_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "http_request_epoch.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture,
                 "Thumbnail processor names its output with the key taken at submit",
                 "[thumbnail][multi-printer]") {
    std::ifstream file("assets/images/benchy_thumbnail_white.png", std::ios::binary);
    REQUIRE(file);
    const std::vector<uint8_t> png((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());

    const auto dir = std::filesystem::temp_directory_path() / "helix_thumb_printer_key";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    helix::ThumbnailProcessor* proc = ThumbnailProcessorTestAccess::make();
    proc->set_cache_dir(dir.string());

    helix::http_epoch::set_base_url("http://10.0.0.1:7125", true);
    const std::string key_at_submit = ThumbnailCache::compute_hash("benchy.png");

    // Occupy both workers so the job waits in the queue while the printer changes.
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    proc->submit_test_task([released] { released.wait(); });
    proc->submit_test_task([released] { released.wait(); });

    std::string output_path;
    helix::ThumbnailTarget target;
    target.width = 120;
    target.height = 120;
    proc->process_async(
        png, "benchy.png", target, [&](const std::string& path) { output_path = path; }, nullptr);

    helix::http_epoch::set_base_url("http://10.0.0.2:7125", true);
    REQUIRE(ThumbnailCache::compute_hash("benchy.png") != key_at_submit);
    release.set_value();
    proc->wait_for_completion();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE_FALSE(output_path.empty());
    CHECK(std::filesystem::path(output_path).filename().string().rfind(key_at_submit + "_", 0) ==
          0);

    ThumbnailProcessorTestAccess::destroy(proc);
    std::filesystem::remove_all(dir);
}
