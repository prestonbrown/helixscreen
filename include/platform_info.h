// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix {

/// Returns true when running on Android (compile-time on real builds, overridable for tests)
bool is_android_platform();

/// True when this platform offers the host power controls (shutdown/reboot
/// dialog, screen power actions). Android is an app on someone's tablet: there
/// is no init system to reboot and the host-power RPCs are not wanted there.
/// The single source of this rule — the platform_host_power_supported subject
/// is seeded from it and the shutdown widget/dialog gate on that.
bool platform_host_power_supported();

/// True when this install's updates are owned by something other than HelixScreen
/// by DEFAULT, so the in-app updater must stay quiet unless explicitly turned on.
///
/// Today only a Snapmaker U1 running PAXX Extended Firmware's own HelixScreen package
/// (u1_firmware_manages_updates), which extended-pkg downloads as a pinned,
/// sha256-verified tarball into /oem/apps/helixscreen. Self-updating there rewrites a
/// package the firmware believes it owns, and the firmware hook does not export
/// HELIX_DISABLE_AUTO_UPDATES. A U1 on stock firmware, where our installer owns
/// /userdata/helixscreen, self-updates.
///
/// The single source of this rule; updates_externally_managed() consults it for the
/// default and an explicit HELIX_DISABLE_AUTO_UPDATES (either direction) still wins,
/// which is how a dev box force-enables self-update.
bool platform_defaults_to_external_updates();

/// @p install_root for a caller that writes into it, or empty when the firmware owns
/// the running install (platform_defaults_to_external_updates()): the app never writes
/// into a firmware's package directory, whose next package update replaces it anyway.
std::string writable_install_root(const std::string& install_root);

/// True when a Snapmaker U1's firmware owns the HelixScreen install that is running.
/// Decided under @p probe_root, failing toward "managed" because self-updating a
/// firmware's package is the worse mistake:
///  - no /oem/apps/helixscreen directory: false;
///  - @p running_exe is the same file (device and inode) as
///    `/oem/apps/helixscreen/<any>/bin/helix-screen`: true. Identity rather than a
///    path, because the firmware launches the package through a bind mount;
///  - otherwise true unless @p running_exe resolves inside /userdata/helixscreen, so an
///    exe that cannot be stat'd or resolved counts as the firmware's.
bool u1_firmware_manages_updates(const std::string& running_exe = "/proc/self/exe",
                                 const std::string& probe_root = "/");

/// True when HelixScreen runs ON the printer it drives, rather than beside it.
///
/// Every printer-embedded package is cross-built with its own HELIX_PLATFORM_* define,
/// and on all of them Moonraker is reachable at 127.0.0.1 because it is the same box.
/// A desktop or Pi build has no such guarantee: the printer is somewhere else on the
/// network and the user genuinely has to say where.
///
/// The single source of this rule. Consumers ask the capability question rather than
/// testing platform macros of their own, so adding a printer means editing this one
/// function. Currently used by the first-run wizard to decide whether asking for a
/// Moonraker address is a real question or a wasted step.
bool is_printer_embedded();

/// Test helper: override the platform check. Pass -1 to reset to compile-time default.
void set_platform_override(int override_value);

/// Test helper: override the printer-embedded answer. Pass -1 to reset to the
/// compile-time platform answer.
void set_printer_embedded_override(int override_value);

/// Test helper: override the external-updates default. Pass -1 to reset to the
/// compile-time platform answer.
void set_external_updates_default_override(int override_value);

/// Log platform info (kernel, arch, hostname, memory) at INFO level
void log_platform_info();

/// Short human-readable host arch line for the About screen and debug bundles.
/// Format: "<kernel-arch> · <N>-bit userspace" — surfaces the common
/// "aarch64 kernel + 32-bit userspace" Pi configuration without coercing the
/// user to migrate.
std::string host_arch_string();

} // namespace helix
