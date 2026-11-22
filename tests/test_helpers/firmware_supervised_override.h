// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform_info.h"

namespace helix::test {

/// Forces helix::firmware_supervised() for one scope, then lets the environment and
/// the board decide again.
struct FirmwareSupervisedFor {
    explicit FirmwareSupervisedFor(bool on) {
        set_firmware_supervised_override(on ? 1 : 0);
    }
    ~FirmwareSupervisedFor() {
        set_firmware_supervised_override(-1);
    }
    FirmwareSupervisedFor(const FirmwareSupervisedFor&) = delete;
    FirmwareSupervisedFor& operator=(const FirmwareSupervisedFor&) = delete;
};

} // namespace helix::test
