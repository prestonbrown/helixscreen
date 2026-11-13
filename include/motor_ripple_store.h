// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The last motor ripple sweep, kept in the printer's Moonraker database so the
// print-start check can warn about a file's wall speed after a restart.

#include "json_fwd.h"
#include "motor_ripple_types.h"

#include <optional>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration::ripple {

inline constexpr const char* kRippleDbNamespace = "helix-screen";
inline constexpr const char* kRippleDbKey = "speed_sweep";
inline constexpr int kRippleSchemaVersion = 1;

[[nodiscard]] nlohmann::json ripple_result_to_json(const SweepResult& r);
/// nullopt for a wrong version or any field of the wrong shape.
[[nodiscard]] std::optional<SweepResult> ripple_result_from_json(const nlohmann::json& j);

/// This printer's saved result. Main thread only: load() and save() hand
/// Moonraker's answers back through the update queue.
class RippleResultCache {
  public:
    static RippleResultCache& instance();

    /// Replace the cached result with the database's, or with none.
    void load(IMoonrakerAPI& api);
    /// Cache and store a completed run.
    void save(IMoonrakerAPI& api, const SweepResult& result);
    void clear();

    [[nodiscard]] const std::optional<SweepResult>& result() const {
        return result_;
    }
    [[nodiscard]] std::vector<RippleBand> bands() const;

  private:
    std::optional<SweepResult> result_;
    uint64_t generation_ = 0;
};

} // namespace helix::calibration::ripple
