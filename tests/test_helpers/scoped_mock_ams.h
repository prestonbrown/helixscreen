// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace helix::test {

/// A started mock AMS installed on AmsState for one test, removed again on
/// destruction.
struct ScopedMockAms {
    AmsBackendMock* backend = nullptr;

    /// @p slot_count slots, as the mock seeds them.
    explicit ScopedMockAms(int slot_count) {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<AmsBackendMock>(slot_count);
        backend = owned.get();
        backend->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        backend->start();
        // The started backend publishes its slots through the update queue;
        // nothing reading AmsState runs queued work on a test's behalf.
        ui::UpdateQueue::instance().drain();
    }

    /// One slot per lane, loaded with {rgb, material} in order.
    explicit ScopedMockAms(const std::vector<std::pair<uint32_t, const char*>>& lanes)
        : ScopedMockAms(static_cast<int>(lanes.size())) {
        for (size_t i = 0; i < lanes.size(); ++i) {
            load(static_cast<int>(i), lanes[i].first, lanes[i].second, SlotStatus::LOADED);
        }
        // The slot-count and availability subjects follow through the queue.
        ui::UpdateQueue::instance().drain();
    }

    ~ScopedMockAms() {
        ui::UpdateQueue::instance().drain();
        backend->stop();
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }

    ScopedMockAms(const ScopedMockAms&) = delete;
    ScopedMockAms& operator=(const ScopedMockAms&) = delete;

    /// sync_external_identity() drops SlotStatus (no real backend accepts a
    /// user-written status), so the status goes through force_slot_status().
    void load(int slot_index, uint32_t rgb, const char* material, SlotStatus status) {
        auto slot = backend->get_slot_info(slot_index);
        slot.color_rgb = rgb;
        slot.material = material;
        backend->sync_external_identity(slot_index, slot);
        backend->force_slot_status(slot_index, status);
    }
};

} // namespace helix::test
