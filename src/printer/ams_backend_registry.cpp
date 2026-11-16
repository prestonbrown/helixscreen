// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_registry.h"

#include "consumption_sink.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

int AmsBackendRegistry::add(std::unique_ptr<AmsBackend> backend, EventCallback on_event) {
    int index = 0;
    int slot_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = static_cast<int>(backends_.size());
        backends_.push_back(std::move(backend));
        AmsBackend* b = backends_.back().get();
        if (!b) {
            return index;
        }
        b->set_event_callback([on_event = std::move(on_event), index](const std::string& event,
                                                                      const std::string& data) {
            on_event(index, event, data);
        });
        // No-op for real backends; mocks use it to simulate action:prompt dialogs.
        if (gcode_response_callback_) {
            b->set_gcode_response_callback(gcode_response_callback_);
        }
        slot_count = b->get_system_info().total_slots;
    }

    ensure_sinks(index, slot_count);
    return index;
}

void AmsBackendRegistry::ensure_sinks(int index, int slot_count) {
    int have = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        have = static_cast<int>(sinks_[index].size());
    }
    if (slot_count <= have) {
        return;
    }
    // Outside the lock: a sink registered mid-print snapshots at once, and the
    // snapshot looks its backend up through get(). The tracker's gating
    // (unknown weight, Spoolman link, native tracking) decides per tick whether
    // each sink consumes deltas.
    std::vector<FilamentConsumptionTracker::SinkHandle> handles;
    handles.reserve(static_cast<size_t>(slot_count - have));
    auto& tracker = FilamentConsumptionTracker::instance();
    for (int slot = have; slot < slot_count; ++slot) {
        handles.push_back(tracker.register_sink(std::make_unique<AmsSlotSink>(index, slot)));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& registered = sinks_[index];
        registered.insert(registered.end(), handles.begin(), handles.end());
    }
    spdlog::debug("[AMS State] Registered {} consumption sinks for backend {} (slots {}-{})",
                  slot_count - have, index, have, slot_count - 1);
}

void AmsBackendRegistry::clear() {
    // Sinks go first: unregistering flushes each one, and the flush reads its
    // backend through get(), so the backend must still be registered then.
    std::map<int, std::vector<FilamentConsumptionTracker::SinkHandle>> sinks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks.swap(sinks_);
    }
    auto& tracker = FilamentConsumptionTracker::instance();
    for (auto& [idx, handles] : sinks) {
        for (auto* h : handles) {
            tracker.unregister_sink(h);
        }
    }

    // Unlinked before they stop, so no reader reaches a backend mid-teardown,
    // and stopped outside the lock, so a backend's stop() may ask the registry
    // anything.
    std::vector<std::unique_ptr<AmsBackend>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doomed.swap(backends_);
    }
    for (auto& b : doomed) {
        if (b) {
            b->stop();
        }
    }
}

void AmsBackendRegistry::release_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : backends_) {
        if (b) {
            b->release_subscriptions();
        }
    }
    backends_.clear();
}

AmsBackend* AmsBackendRegistry::get(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(backends_.size())) {
        return nullptr;
    }
    return backends_[index].get();
}

int AmsBackendRegistry::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(backends_.size());
}

std::optional<AmsType> AmsBackendRegistry::primary_type() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backends_.empty() || !backends_[0]) {
        return std::nullopt;
    }
    return backends_[0]->get_type();
}

std::optional<BackendTraits> AmsBackendRegistry::primary_traits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backends_.empty() || !backends_[0]) {
        return std::nullopt;
    }
    return backends_[0]->traits();
}

bool AmsBackendRegistry::any_filament_batch_in_flight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(backends_.begin(), backends_.end(), [](const auto& backend) {
        return backend && backend->filament_batch_in_flight();
    });
}

void AmsBackendRegistry::set_gcode_response_callback(
    std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    gcode_response_callback_ = std::move(callback);
    for (auto& backend : backends_) {
        if (backend) {
            backend->set_gcode_response_callback(gcode_response_callback_);
        }
    }
    spdlog::debug("[AMS State] Gcode response callback {}",
                  gcode_response_callback_ ? "set" : "cleared");
}

} // namespace helix
