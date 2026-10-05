// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "backdrop_blur.h"

/// Sets whether backdrops snapshot for one test, restoring the previous mode.
class SnapshotBackdropsMode {
  public:
    explicit SnapshotBackdropsMode(bool enabled)
        : saved_(helix::ui::detail::snapshot_backdrops_enabled()) {
        helix::ui::detail::set_snapshot_backdrops_enabled(enabled);
    }
    ~SnapshotBackdropsMode() {
        helix::ui::detail::set_snapshot_backdrops_enabled(saved_);
    }
    SnapshotBackdropsMode(const SnapshotBackdropsMode&) = delete;
    SnapshotBackdropsMode& operator=(const SnapshotBackdropsMode&) = delete;

  private:
    bool saved_;
};
