// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_lvgl_frame_check_failures.cpp
 * @brief Rendering a busy screen fails no LVGL argument check
 *
 * A check that fails on every frame costs a formatted warning per frame, which on a
 * Pi 3B took half the frame budget. Such a failure is either our bad call or LVGL
 * asking a question it should not, and both are fixed rather than tolerated.
 */

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "lvgl_log_handler.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Every "Check failed" line LVGL logs while it is alive.
class CheckFailureCapture {
  public:
    CheckFailureCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(512)) {
        helix::logging::register_lvgl_log_handler();
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        sink_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }
    ~CheckFailureCapture() {
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) {
                sinks.erase(it);
                break;
            }
        }
        logger_->set_level(prev_level_);
    }
    CheckFailureCapture(const CheckFailureCapture&) = delete;
    CheckFailureCapture& operator=(const CheckFailureCapture&) = delete;

    std::string failures() const {
        std::string out;
        for (const auto& line : sink_->last_formatted())
            if (line.find("Check failed") != std::string::npos)
                out += line;
        return out;
    }

  private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_ = spdlog::level::info;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Rendering the controls panel fails no LVGL argument check",
                 "[lvgl][check_arg]") {
    ControlsPanel panel(state(), nullptr);
    panel.init_subjects();
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
    REQUIRE(root != nullptr);
    panel.setup(root, test_screen());
    helix::ui::UpdateQueue::instance().drain();

    {
        CheckFailureCapture capture;
        for (int frame = 0; frame < 10; frame++) {
            lv_obj_scroll_by(root, 0, frame % 2 ? 40 : -40, LV_ANIM_OFF);
            lv_obj_invalidate(test_screen());
            lv_refr_now(lv_display_get_default());
            process_lvgl(16);
        }
        INFO(capture.failures());
        CHECK(capture.failures().empty());
    }

    panel.on_deactivate(DeactivateReason::NavigateAway);
    lv_obj_delete(root);
    helix::ui::UpdateQueue::instance().drain();
    panel.deinit_subjects();
    helix::ui::UpdateQueue::instance().drain();
}
