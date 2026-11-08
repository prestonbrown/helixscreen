// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/// Telemetry held while the display sleeps is applied at least this often. Matches the
/// temperature history's 1 Hz sample rate, so the graph keeps its resolution across sleep.
inline constexpr uint32_t kAsleepStatusIntervalMs = 1000;

/// True when every object attribute in a notify_status_update delta is on the telemetry
/// list. An empty delta, an unknown object or any other attribute is false, so an
/// attribute nobody listed is applied at once rather than held.
bool status_is_telemetry_only(const nlohmann::json& status);

/// The telemetry list, as "object-prefix.attribute" pairs.
const std::vector<std::string_view>& telemetry_attributes();

/**
 * @brief Folds telemetry-only status deltas into one, latest value per object attribute
 *
 * Applying the merged delta once leaves the same state as applying each delta in turn,
 * and every state it produces is one the printer reported. Main thread only.
 */
class StatusCoalescer {
  public:
    struct Held {
        nlohmann::json status;  ///< Merged delta: the latest value of each attribute held
        double eventtime = 0.0; ///< Newest frame's eventtime
        uint64_t klippy_epoch = 0;
    };

    /// Folds a telemetry-only delta in. Latest value wins per object attribute.
    void hold(const nlohmann::json& status, double eventtime, uint64_t klippy_epoch,
              uint32_t now_ms);
    bool empty() const {
        return !m_held.has_value();
    }
    /// The epoch of the held frames. Meaningful only while !empty().
    uint64_t klippy_epoch() const {
        return m_held ? m_held->klippy_epoch : 0;
    }
    /// True once the oldest held frame is at least @p interval_ms old.
    bool due(uint32_t now_ms, uint32_t interval_ms) const;
    /// The merged delta, leaving the coalescer empty.
    std::optional<Held> take();
    void clear() {
        m_held.reset();
    }

  private:
    std::optional<Held> m_held;
    uint32_t m_first_hold_ms = 0;
};

} // namespace helix
