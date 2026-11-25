// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "indev_delete_watch.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <lvgl.h>
#include <unistd.h>
#include <vector>

namespace helix {

/// An input device whose events arrive on a file descriptor the main loop can wait on.
struct InputWakeSource {
    lv_indev_t* indev = nullptr;
    int fd = -1;
};

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

    /// Opens @p path and makes its input device from the fd through @p make_device,
    /// tracking it here. Opened here rather than by LVGL's path-taking creators so the
    /// fd is known: the main loop waits on it while the display sleeps. nullptr when
    /// the node cannot be opened, or when @p make_device returns nullptr; nothing is
    /// tracked then.
    ///
    /// @p make_device takes ownership of the fd, and must close it itself when it
    /// returns nullptr. LVGL closes the fd when it deletes the device it made.
    template <class MakeDevice> lv_indev_t* open_device(const char* path, MakeDevice make_device) {
        const int fd = ::open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) {
            spdlog::warn("[Input] Cannot open input device {}: {}", path, strerror(errno));
            return nullptr;
        }
        lv_indev_t* indev = make_device(fd);
        if (indev) {
            add(indev, fd);
        }
        return indev;
    }

#if LV_USE_EVDEV
    /// open_device() with LVGL's evdev driver, which closes the fd when it makes no
    /// device of it.
    lv_indev_t* open_evdev(lv_indev_type_t type, const char* path) {
        return open_device(path, [type](int fd) { return lv_evdev_create_fd(type, fd); });
    }
#endif

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
