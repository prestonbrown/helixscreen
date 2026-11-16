// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_slot_override_store.h"

#include <chrono>

// Friend of FilamentSlotOverrideStore: reaches its private tunables.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_load_timeout(helix::ams::FilamentSlotOverrideStore& store,
                                 std::chrono::milliseconds ms) {
        store.load_timeout_ = ms;
    }
};
