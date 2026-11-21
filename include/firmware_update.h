// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Printer firmware updates: which mechanism this printer's own firmware has, and
// every decision around applying one.
//
// This module is the ONLY place that names a firmware's update mechanism, its
// image files or its flasher. Generic code (the Settings page, the confirm
// dialog, the progress view) asks these functions and never names a vendor.
// Adding a firmware means adding one Provider row to firmware_update.cpp.
//
// Moonraker's update_manager (Klipper, Moonraker, web clients, mods) is a
// separate, vendor-free source: update_manager_status.h.

#include "board_identity.h"
#include "print_lifecycle_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::fwupdate {

/// How a printer's own firmware gets updated.
enum class Mechanism {
    None,
    VendorScript,  ///< a root script on the printer flashes an image now, in place
    UsbAtBoot,     ///< the firmware installs an image from a USB stick at power-on
    SelfUpdating,  ///< the firmware updates itself through dialogs HelixScreen already renders
    VendorPackage, ///< the vendor's screen app installs a package; community updaters exist
    OwnerTool,     ///< a firmware maintainer's own tool owns updates
};

/// How far HelixScreen's support for a row goes.
enum class Readiness {
    Supported,               ///< apply path enabled
    NeedsDeviceVerification, ///< read from source, never run by us: apply only behind the flag
    NeedsVendorCooperation,  ///< possible, but the firmware's owner must agree a contract first
    NotPossible,             ///< no entry point HelixScreen could call
    NoActionNeeded,          ///< the firmware already handles it
};

/// What detection looks at: which board this process runs on, which mod owns the
/// host (install_provenance::detect_host_mod), and the configured printer type.
struct HostFacts {
    board::Board board = board::Board::Generic;
    std::string host_mod = "none";
    std::string printer_type;
};

/// The facts for the running process. Main thread: reads the printer profile.
HostFacts current_host_facts();

/// An image file on removable media.
struct FileEntry {
    std::string path; ///< absolute
    std::string name; ///< basename
    uint64_t size = 0;
};

/// How an image file fares against a row's image rule. Cheap checks only: the
/// vendor's own flasher verifies the contents.
enum class ImageVerdict {
    Ok,
    WrongType,  ///< wrong extension
    WrongModel, ///< the name does not carry this model's prefix
    TooSmall,   ///< smaller than any real image
};

/// One firmware's update mechanism. The table lives in firmware_update.cpp.
struct Provider;

/// The row for @p host, or nullptr when HelixScreen knows no vendor mechanism there.
const Provider* provider_for(const HostFacts& host);

/// Log name ("snapmaker_u1_stock").
const char* provider_id(const Provider& p);
/// Who owns the firmware, for display ("Snapmaker"). Never branched on.
const char* provider_owner(const Provider& p);
Mechanism provider_mechanism(const Provider& p);
Readiness provider_readiness(const Provider& p);
/// The docs page ("guide/install-u1/") that reinstalls HelixScreen after this row's
/// update removed it; "" when the row has none.
const char* provider_reinstall_guide(const Provider& p);

/// True when HELIX_FWUPDATE_UNVERIFIED_APPLY=1: rows that need device verification
/// may apply. Off by default; for the people verifying a row on their own printer.
bool unverified_apply_allowed();

/// Whether @p p (nullptr: no row) may apply here, with the runtime flag as set:
/// what decides whether the firmware part of the update page is offered at all.
bool vendor_apply_available(const Provider* p);

/// Whether HelixScreen may start this row's update: the row has an apply plan, and
/// it is Supported, or it NeedsDeviceVerification and @p unverified_allowed.
bool apply_enabled(const Provider& p, bool unverified_allowed);

/// @p file checked against the row's image rule. WrongType for a row with no images.
ImageVerdict check_image(const Provider& p, const FileEntry& file);

/// The files directly inside each of @p roots that pass check_image(), in name order.
/// Not recursive: every vendor here wants the image in the stick's root.
std::vector<FileEntry> find_images(const Provider& p, const std::vector<std::string>& roots);

/// What would stop an update from starting right now.
enum class Blocker {
    None,
    JobRunning,        ///< a print is preparing, running or paused
    PrintStateUnknown, ///< the printer did not say whether a job runs
    UpdateRunning,     ///< another update holds the machine
    NotEnoughSpace,    ///< the image does not fit where the flasher unpacks it
    SpaceUnknown,      ///< free space where the flasher unpacks could not be read
};

/// Everything first_blocker() weighs. A field a source cannot answer stays at its
/// default, which blocks: print_stats_state "" is unknown.
struct ApplyInputs {
    PrintState lifecycle = PrintState::Idle; ///< the app's own view, Preparing included
    /// print_stats.state queried from the printer right before starting; "" = no answer
    std::string print_stats_state;
    bool update_running = false;
    /// Free bytes where the flasher unpacks; nullopt = unread. Ignored when
    /// required_bytes is 0 (a source that needs no local space).
    std::optional<uint64_t> free_bytes;
    uint64_t required_bytes = 0;
};

/// The first reason not to start, or Blocker::None. Order: job, print state, update
/// running, space.
Blocker first_blocker(const ApplyInputs& in);

/// Whether print_stats.state @p state lets an update start: standby, complete,
/// cancelled and error do; printing, paused and anything unrecognised do not.
bool print_stats_state_permits_update(std::string_view state);

/// What the confirm dialog must tell the user before a row's update starts.
enum class ConfirmPoint {
    KeepPowerOn,  ///< losing power mid-flash can leave the printer unbootable
    StopsKlipper, ///< the printer stops and is unusable until it finishes
    PrinterReboots,
    RemovesHelixScreen, ///< the update uninstalls HelixScreen; reinstall it afterwards
    ServicesRestart,    ///< Klipper or Moonraker restart while it updates
    UpgradesOsPackages, ///< the host's operating system packages are upgraded
    MayNeedReboot,      ///< the printer may need a reboot afterwards
    StayOnUntilDone,    ///< do not turn the printer off until the update finishes
};

/// Whether a confirm listing @p points carries the danger style: it flashes the
/// printer, upgrades its operating system or removes HelixScreen.
bool confirm_is_destructive(const std::vector<ConfirmPoint>& points);

/// The row's confirm points, in the order the dialog lists them.
std::vector<ConfirmPoint> confirm_points(const Provider& p);

/// How to run one update.
struct ApplyPlan {
    /// The launcher: returns at once, leaving the flasher running detached in its own
    /// session with its output in log_path. Empty when the row has no apply plan.
    std::vector<std::string> argv;
    std::string log_path;
    /// A file whose presence means the vendor's flasher is already running, or "".
    std::string lock_path;
    /// Where the flasher unpacks the image; free space there is a precondition. "" = none.
    std::string unpack_dir;
};

/// The plan for flashing @p image_path, logging to @p log_path. argv is empty for a
/// row without an apply plan.
ApplyPlan apply_plan(const Provider& p, const std::string& image_path, const std::string& log_path);

/// Where an update stands, as one output line of the flasher reports it.
enum class Stage {
    None, ///< the line carries no progress
    Unpacking,
    MainBoard,  ///< the main MCU
    Toolhead,   ///< a toolhead MCU; ProgressEvent::index says which, from 1
    MainSystem, ///< the operating system image
    Rebooting,
    Failed,
};

struct ProgressEvent {
    Stage stage = Stage::None;
    std::optional<int> percent;
    int index = 0;
    /// From this line on the update cannot be stopped or undone; the printer reboots.
    bool point_of_no_return = false;
};

/// One line of the row's flasher output. Stage::None for lines that carry no progress.
ProgressEvent parse_progress(const Provider& p, std::string_view line);

} // namespace helix::fwupdate
