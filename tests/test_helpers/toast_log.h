// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_toast_manager.h"

#include "../ui_test_utils.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Every toast asked for while this is in scope, with the thread that asked.
class ToastLog {
  public:
    struct Shown {
        ToastSeverity severity;
        std::string message;
        uint32_t duration_ms;
        std::thread::id thread;
    };
    std::vector<Shown> shown;

    ToastLog() {
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity severity, const std::string& message, uint32_t duration_ms) {
                shown.push_back({severity, message, duration_ms, std::this_thread::get_id()});
            });
    }
    ~ToastLog() {
        helix::ui::set_test_toast_hook(nullptr);
    }
    ToastLog(const ToastLog&) = delete;
    ToastLog& operator=(const ToastLog&) = delete;

    void expect_one(ToastSeverity severity, const std::string& message,
                    uint32_t duration_ms) const {
        REQUIRE(shown.size() == 1);
        CHECK(shown[0].severity == severity);
        CHECK(shown[0].message == message);
        CHECK(shown[0].duration_ms == duration_ms);
    }
};

} // namespace helix::test
