// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sound_backend.h"
#include "sound_manager.h"

#include <memory>

namespace helix {

/// Test access into the SoundManager singleton. The M300 install-gate tests
/// need to arrange backend states that initialize() cannot reach on a host:
/// a PWM sysfs backend installed where /sys/class/pwm does not exist, or a
/// stand-in for SDL/ALSA where host audio may or may not be available.
class SoundManagerTestAccess {
  public:
    /// Install a backend without going through initialize()'s host-audio
    /// probe. Replaces any active backend (callers shut the manager down
    /// first when a sequencer is running).
    static void install_backend(SoundManager& sm, std::shared_ptr<SoundBackend> backend) {
        sm.backend_ = std::move(backend);
    }

    /// The active backend, to assert which one won a gate decision.
    static std::shared_ptr<SoundBackend> backend(SoundManager& sm) {
        return sm.backend_;
    }

    /// Run the eager host-audio probe, to learn what this environment would
    /// provide — tests assert against the recovery path's outcome relative
    /// to it, keeping them green on hosts with and without audio.
    static std::shared_ptr<SoundBackend> create_backend(SoundManager& sm) {
        return sm.create_backend();
    }
};

} // namespace helix
