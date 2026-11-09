// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// ExactSizeImage: when a cache generation starts, and when it may not.

#include "../../src/ui/panel_widgets/exact_size_image.h"
#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "prerendered_images.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::ui::ExactSizeImage;

namespace {

/// A generation decodes and resizes a real PNG on a worker thread; a sharded
/// sweep keeps every core busy, so the wait is generous.
constexpr auto kGenerationBudget = std::chrono::seconds(30);

constexpr int32_t kSide = 60;

/// Poll the disk only. Draining the UpdateQueue would deliver a running
/// generation's result, which is what frees the slot a missing source took.
bool file_appears(const std::string& path, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::filesystem::exists(path);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a source that is not on disk starts no generation and holds no slot",
                 "[exact_size_image]") {
    lv_obj_t* img = lv_image_create(test_screen());
    lv_obj_set_size(img, kSide, kSide);
    lv_obj_update_layout(img);

    {
        ExactSizeImage exact(
            "ExactSizeImageTest", [img]() { return img; },
            [](lv_obj_t* obj, const std::string& src) { lv_image_set_src(obj, src.c_str()); });

        const std::string real = "A:assets/images/printers/generic-corexy.png";
        const std::string cache_path = helix::get_cached_printer_image_path(real, kSide, kSide);
        std::error_code ec;
        std::filesystem::remove(cache_path, ec);

        // A generation for the missing source would take the one slot, and the
        // real source's request would wait for its result to be delivered.
        exact.generate_if_missing("A:assets/images/printers/no-such-printer.png");
        exact.generate_if_missing(real);
        CHECK(file_appears(cache_path, kGenerationBudget));

        helix::ui::UpdateQueue::instance().drain();
        exact.cancel();
        std::filesystem::remove(cache_path, ec);
    }
    lv_obj_delete(img);
}
