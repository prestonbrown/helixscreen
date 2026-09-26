// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_openams.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace helix {

/// Friend-class shim for AmsBackendOpenAms: the override store and map a
/// persistence case has to seed and read back.
class OpenAmsTestAccess {
  public:
    static void inject_override_store(AmsBackendOpenAms& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.override_store_ = std::move(s);
    }

    static void seed_override(AmsBackendOpenAms& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }

    static std::optional<helix::ams::FilamentSlotOverride> get_override(const AmsBackendOpenAms& b,
                                                                        int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

} // namespace helix
