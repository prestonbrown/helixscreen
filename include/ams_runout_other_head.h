// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_runout_other_head.h
 * @brief Capability of a filament system that reports what a runout-paused print
 *        could continue on: the other loaded heads and the filament that ran out
 *
 * Reached with AmsBackend::as<IRunoutOtherHead>(). The Snapmaker U1 implements it,
 * and MultiACE through it. Without it the runout flow offers no other head.
 */

#pragma once

#include "runout_other_head.h"

#include <optional>

namespace helix {

class IRunoutOtherHead {
  public:
    /**
     * @brief What helix::runout_other_head needs to offer continuing a
     *        runout-paused print on another loaded head (#2137).
     *
     * @param runout_head The head whose filament ran out.
     * @note Implementations take the backend's own `mutex_`; callers must NOT hold it.
     * @return nullopt when there is nothing to decide on for @p runout_head.
     */
    [[nodiscard]] virtual std::optional<runout_other_head::RunoutFacts>
    runout_other_head_facts(int runout_head) const = 0;

  protected:
    ~IRunoutOtherHead() = default;
};

} // namespace helix
