// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "display_backend.h"
#include "indev_delete_watch.h"

#include <algorithm>
#include <lvgl.h>
#include <vector>

namespace helix {

/**
 * @brief The input devices a backend opened, with their fds, while LVGL still has them
 *
 * lv_evdev deletes a device whose read fails (a USB mouse or keyboard unplugged) and
 * closes its fd. An entry kept after that names a freed indev, whose address a later
 * lv_indev_create() may reuse, and a closed fd, whose number a later open may reuse, so
 * each entry is dropped on its device's LV_EVENT_DELETE. Main thread only, like every
 * indev. Not copyable or movable: the delete listener holds this object's address.
 */
class InputWakeSourceList {
  public:
    InputWakeSourceList() = default;
    InputWakeSourceList(const InputWakeSourceList&) = delete;
    InputWakeSourceList& operator=(const InputWakeSourceList&) = delete;

    /// Tracks @p indev, read from @p fd, until LVGL deletes it.
    void add(lv_indev_t* indev, int fd) {
        sources_.push_back({indev, fd});
        deletes_.watch(indev, nullptr, &on_deleted, this);
    }

    [[nodiscard]] const std::vector<InputWakeSource>& sources() const {
        return sources_;
    }

  private:
    static void on_deleted(void* ctx, const lv_indev_t* indev) {
        auto& sources = static_cast<InputWakeSourceList*>(ctx)->sources_;
        sources.erase(
            std::remove_if(sources.begin(), sources.end(),
                           [indev](const InputWakeSource& s) { return s.indev == indev; }),
            sources.end());
    }

    std::vector<InputWakeSource> sources_;
    IndevDeleteWatch deletes_;
};

} // namespace helix
