// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Vendor state pushed by the HelixPrint plugin. A filament backend that polls
// its vendor's state (a file, a REST bridge) takes it from the plugin's
// notify_helix_vendor_state instead while the plugin's feature for that vendor
// is ready, and polls again the moment it is not. The payload is what the
// poll reads, so a backend applies a push through the same function as a poll
// that saw a change.

#include "async_lifetime_guard.h"
#include "helix_plugin_features.h"
#include "json_fwd.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace helix {

class IMoonrakerClient;

/// The plugin's broadcast, params [{"vendor": <name>, "payload": <state>}].
inline constexpr const char* kVendorStateNotifyMethod = "notify_helix_vendor_state";

/// Where a backend reads its vendor state from.
enum class VendorStateSource {
    Poll, ///< the backend's own poll
    Push, ///< the plugin's notify_helix_vendor_state
};

/// The one rule every backend shares: push while the plugin's feature for the
/// vendor is Ready, poll in every other state (no plugin, a plugin without the
/// feature or below the contract, or the feature unavailable on this printer).
VendorStateSource vendor_state_source(PluginFeatureState feature);

/// The payload of a notify_helix_vendor_state notification when it is for
/// @p vendor; nullopt for another vendor or a malformed message.
std::optional<nlohmann::json> vendor_state_payload(const nlohmann::json& notification,
                                                   const std::string& vendor);

/// A backend's source across the plugin's probes.
class VendorStateTracker {
  public:
    /// Fold the plugin's answer as of probe number @p probe_seq, read while
    /// the client is on connection @p connection. A Ready counts only from a
    /// probe first seen on the current connection: after a reconnect the
    /// feature map is the previous connection's until discovery probes again.
    /// True when the backend owes one read of its vendor state now: on
    /// switching to Push, and on each later probe while pushing, since the
    /// reconnect or plugin restart behind a probe can have missed a change.
    /// Main thread.
    bool update(PluginFeatureState feature, int probe_seq, uint64_t connection);

    /// Back to polling, with every probe seen so far forgotten. Main thread.
    void reset();

    /// Whether the backend takes its state from the push. Any thread.
    bool pushing() const {
        return pushing_.load();
    }

  private:
    std::atomic<bool> pushing_{false};
    int probe_seq_ = -1;
    uint64_t probe_connection_ = 0;
};

/// While pushing, a backend still polls this often, so a push that stops
/// arriving mid-session costs at most this long.
inline constexpr std::chrono::seconds kVendorPushBackstop{30};

/// What PrinterState holds for plugin feature @p feature at contract
/// @p min_version, and how many plugin probes it has applied. Absent and 0
/// before its subjects exist. Main thread.
struct PluginFeatureProbe {
    PluginFeatureState state = PluginFeatureState::Absent;
    int seq = 0;
};
PluginFeatureProbe current_plugin_feature(const std::string& feature, int min_version);

/// Deliver notify_helix_vendor_state for @p vendor to @p on_payload on the main
/// thread, while @p token lives. @p handler_name names the registration for
/// unsubscribe_vendor_state.
void subscribe_vendor_state(IMoonrakerClient& client, const std::string& handler_name,
                            const std::string& vendor, LifetimeToken token,
                            std::function<void(const nlohmann::json&)> on_payload);

void unsubscribe_vendor_state(IMoonrakerClient& client, const std::string& handler_name);

} // namespace helix
