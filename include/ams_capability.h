// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_capability.h
 * @brief RTTI-free lookup of the optional interfaces a filament backend implements
 *
 * Generic code asks a question only some backends can answer:
 *
 *     if (auto* cal = backend->as<ICfsCalibration>()) { ... }
 *
 * A backend publishes an interface by inheriting it and returning that subobject
 * from capability() for its tag, then deferring to its base for any other tag.
 */

#pragma once

#include "helix_type_tag.h"

namespace helix {

class AmsCapabilityHost {
  public:
    /**
     * @brief The interface subobject for @p tag, or nullptr when not implemented
     *
     * The result must be a pointer to exactly the interface @p tag names, since
     * as<I>() converts the void* straight back to I*. In a class with several
     * bases that takes a static_cast<I*>(this), which adjusts the address.
     */
    [[nodiscard]] virtual void* capability(TypeTag tag) {
        (void)tag;
        return nullptr;
    }

    /// This backend's implementation of interface @p I, or nullptr.
    template <class I> [[nodiscard]] I* as() const {
        return static_cast<I*>(const_cast<AmsCapabilityHost*>(this)->capability(type_tag<I>()));
    }

  protected:
    ~AmsCapabilityHost() = default;
};

} // namespace helix
