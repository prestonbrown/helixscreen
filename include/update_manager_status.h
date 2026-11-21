// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Moonraker update_manager, read for the firmware-update page: the entries
// machine.update.status reports and the notify_update_response stream an upgrade
// produces. Pure parsing; no vendor knowledge (that is firmware_update.h).

#include "firmware_update.h"
#include "json_fwd.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::fwupdate {

/// update_manager's entry for HelixScreen itself. Settings > Updates owns it.
inline constexpr const char* kHelixScreenEntry = "helixscreen";

/// One update_manager entry.
struct ManagedEntry {
    std::string name;           ///< "klipper", "mainsail", "system"
    std::string kind;           ///< configured_type: "git_repo", "web", "zip", "python", "system"
    std::string version;        ///< installed; "" when the entry has none (system)
    std::string remote_version; ///< available; "" or "?" when unknown
    int package_count = 0;      ///< system: packages with updates
    bool valid = true;          ///< is_valid and not corrupt: an invalid repo needs recovery
    bool update_available = false;
};

struct UpdateStatus {
    bool busy = false; ///< an update_manager job is running
    /// Every entry except HelixScreen's own, in Moonraker's order.
    std::vector<ManagedEntry> entries;
};

/// machine.update.status's `result`. nullopt when it is not an object carrying
/// version_info. A busy field that is absent reads as busy.
std::optional<UpdateStatus> parse_update_status(const nlohmann::json& result);

/// One notify_update_response line.
struct UpdateResponse {
    std::string application;
    std::string message;
    bool complete = false;
};

/// What the confirm for updating @p e must say. The `system` entry upgrades the
/// host's operating system packages (apt) and may need a reboot; every other entry
/// restarts its Klipper or Moonraker service.
std::vector<ConfirmPoint> confirm_points(const ManagedEntry& e);

/// How a managed update ended, judged from machine.update.status after contact with
/// Moonraker was lost mid-update.
enum class ManagedOutcome {
    StillRunning, ///< update_manager is still busy: keep waiting
    Done,         ///< not busy, and @p name no longer offers an update
    Unknown,      ///< not busy, but the entry still offers one or is gone: check the printer
};
ManagedOutcome managed_outcome_after_lost_contact(const UpdateStatus& status,
                                                  const std::string& name);

/// The payload of a notify_update_response notification (the full JSON-RPC message).
/// nullopt when it is not one.
std::optional<UpdateResponse> parse_update_response(const nlohmann::json& notification);

/// The percentage a progress message reports ("Receiving objects:  45% (12/27)"),
/// the last one when it carries several; nullopt when it reports none.
std::optional<int> percent_in_message(std::string_view message);

/// Whether @p response says the upgrade it reports on
/// failed. Moonraker reports a failure as a message, not a field.
bool response_reports_failure(const UpdateResponse& response);

} // namespace helix::fwupdate
