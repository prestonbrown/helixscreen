// tests/test_helpers/registered_backend.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// An AmsBackend that AmsState has stamped an index onto, which is what any test
// asserting on lane records needs.
#pragma once

#include "ams_backend.h"
#include "ams_state.h"
#include "lane_source_store.h"

#include <memory>
#include <utility>

namespace helix::test {

/// Owns a backend through AmsState so that registration stamps its backend
/// index, then hands it back for driving.
///
/// Registration is not scenery. AmsBackend::lane_id() answers INVALID_LANE_ID
/// until AmsState::add_backend() stamps an index, and the lane funnels drop
/// what that id names, so an unregistered backend files nothing and every
/// assertion against the store reads empty. set_backend() clears first, and
/// clear_backends() resets the lane store, so a harness always starts on block
/// 0 with an empty store whatever a previous test left behind.
///
/// Construct it with the backend's own constructor arguments:
///
///     RegisteredBackend<AmsBackendAd5xIfs> harness(nullptr, nullptr);
///     Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");
///     const auto lane = helix::ams::lane_sources(harness.lane(0));
template <typename BackendT> class RegisteredBackend {
  public:
    template <typename... Args> explicit RegisteredBackend(Args&&... args) {
        auto owned = std::make_unique<BackendT>(std::forward<Args>(args)...);
        backend_ = owned.get();
        helix::AmsState::instance().set_backend(std::move(owned));
    }

    ~RegisteredBackend() {
        helix::AmsState::instance().clear_backends();
    }

    RegisteredBackend(const RegisteredBackend&) = delete;
    RegisteredBackend& operator=(const RegisteredBackend&) = delete;

    BackendT& operator*() const {
        return *backend_;
    }

    BackendT* operator->() const {
        return backend_;
    }

    /// The lane this backend's slot occupies, asked of the backend itself.
    /// Recomputing it beside the backend lets a case pass against a lane the
    /// backend never writes.
    [[nodiscard]] helix::ams::LaneId lane(int slot) const {
        return backend_->lane_id(slot);
    }

  private:
    BackendT* backend_;
};

} // namespace helix::test
