// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_backend.h"
#include "ams_types.h"
#include "filament_consumption_tracker.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief The registered AMS backends, their consumption sinks and the stored
 *        gcode-response callback.
 *
 * add(), clear() and get() run on the main thread: get() hands back a raw
 * pointer after unlocking, which only stays valid on the thread that alone can
 * call clear(). count(), primary_type(), primary_traits() and any_filament_batch_in_flight() may
 * run anywhere: they hold mutex_ for as long as they touch a backend and return
 * no pointer, so nothing they answer outlives clear().
 */
class AmsBackendRegistry {
  public:
    using EventCallback =
        std::function<void(int backend_index, const std::string& event, const std::string& data)>;

    /// Register @p backend at the next index (the caller has already stamped
    /// it with AmsBackend::set_backend_index()): route its events
    /// to @p on_event, hand it the stored gcode callback, and register one
    /// consumption sink per slot. A null backend still takes an index.
    int add(std::unique_ptr<AmsBackend> backend, EventCallback on_event);

    /// Register a consumption sink for each of backend @p index's first
    /// @p slot_count slots that has none yet. A backend can report slots after
    /// add() (an ACE frame announcing its bays), and those consume like the rest.
    void ensure_sinks(int index, int slot_count);

    /// Unregister every sink, then stop and destroy every backend.
    void clear();

    /// Abandon subscriptions and drop the backends without stopping them; for
    /// static destruction, when the client they would unsubscribe from may be gone.
    void release_all();

    /// Main thread only; see the class comment.
    [[nodiscard]] AmsBackend* get(int index) const;
    [[nodiscard]] int count() const;
    [[nodiscard]] std::optional<AmsType> primary_type() const;
    [[nodiscard]] std::optional<BackendTraits> primary_traits() const;
    [[nodiscard]] bool any_filament_batch_in_flight() const;

    void set_gcode_response_callback(std::function<void(const std::string&)> callback);

    /// The live list, for main-thread walks. Only the main thread mutates it,
    /// so a main-thread reader needs no lock.
    [[nodiscard]] const std::vector<std::unique_ptr<AmsBackend>>& backends() const {
        return backends_;
    }

  private:
    /// Lock order: mutex_ -> AmsBackend::mutex_. primary_type(), primary_traits() and
    /// any_filament_batch_in_flight() call into a backend while holding it, so
    /// a backend must never reach the registry (or AmsState::get_backend())
    /// under its own lock. Not recursive: no method here calls another under
    /// the lock, and a call that did would deadlock at once rather than nest.
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<AmsBackend>> backends_;
    /// One FilamentConsumptionTracker sink per slot, keyed by backend index.
    std::map<int, std::vector<FilamentConsumptionTracker::SinkHandle>> sinks_;
    std::function<void(const std::string&)> gcode_response_callback_;
};

} // namespace helix
