// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Local evidence about Moonraker, gathered without asking Moonraker
 *
 * Every Moonraker-derived section of a debug bundle is fetched over HTTP from
 * Moonraker itself, so the one failure that most needs explaining produces a
 * bundle that says nothing about it: AD5X bundles TAU4PW4H / 865DXBQ7 carry five
 * `{"error": "No response from ..."}` entries, a `connection_state: failed`, and
 * no way to tell "not running" from "running, bound somewhere we did not dial".
 *
 * These probes read /proc instead. They only mean anything when Moonraker is
 * supposed to be on this machine — on a remote printer our own /proc says
 * nothing about it — so the caller gates on is_moonraker_on_same_host().
 */
namespace helix::diag {

/// A process matched by find_moonraker_processes().
struct ProcMatch {
    long pid = 0;
    std::string cmdline; ///< NULs collapsed to spaces
};

/**
 * @brief Local addresses in LISTEN state on @p port, from /proc/net/tcp content
 *
 * @param proc_net_tcp Raw file contents (/proc/net/tcp or /proc/net/tcp6).
 * @param port         Host-order port to match.
 * @param ipv6         True when parsing tcp6 (32-hex-char addresses).
 * @return Decoded "addr:port" strings, one per listening socket; empty if none.
 *
 * Endianness: the kernel prints the address with %08X applied to a __be32, so
 * the printed number depends on the host's byte order — "0100007F" is 127.0.0.1
 * on little-endian, and the same address prints as "7F000001" on big-endian.
 * The decode recovers the bytes through the host's own representation rather
 * than assuming an order, so it is correct on both. It matters here: the
 * reporter's AD5X is MIPS. The port field is `%04X` of a host-order u16 and is
 * unambiguous either way, which is why matching is done on the port.
 */
std::vector<std::string> parse_listeners_for_port(const std::string& proc_net_tcp, uint16_t port,
                                                  bool ipv6);

/**
 * @brief Split "http://127.0.0.1:7125" into host and port
 *
 * Exists so the probe can read the endpoint off the Moonraker API's base URL
 * instead of from Config: the bundle collects on HttpExecutor::slow(), and
 * Config is main-thread-only (see debug_bundle_collector.cpp's note on
 * collect_update_info()).
 *
 * @param base_url Scheme optional; a bracketed IPv6 host is unwrapped.
 * @param host     Out: host with no scheme, port, or path. Untouched on failure.
 * @param port     Out: explicit port, else 80/443 by scheme, else @p fallback.
 * @return False when no host could be recovered.
 */
bool split_host_port(const std::string& base_url, std::string& host, uint16_t& port,
                     uint16_t fallback = 7125);

/// Collapse a raw /proc/<pid>/cmdline (NUL-separated argv) into one line.
/// Trailing NULs are dropped; embedded ones become single spaces.
std::string decode_proc_cmdline(const std::string& raw);

/// True when @p cmdline contains any of @p needles (case-sensitive substring).
bool cmdline_matches_any(const std::string& cmdline, const std::vector<std::string>& needles);

/// Log-file locations implied by a moonraker/klippy command line. Empty strings
/// where the flag was absent.
struct LogPathHints {
    std::string log_file;    ///< -l / --logfile: the log itself
    std::string data_path;   ///< -d / --datapath: logs live in <data_path>/logs
    std::string config_file; ///< -c / --configfile: logs are typically ../logs
};

/// Extract the log-location flags from a process command line.
LogPathHints parse_log_hints(const std::string& cmdline);

/**
 * @brief Candidate on-disk paths for @p log_name, derived from running processes
 *
 * @param procs    Output of find_moonraker_processes().
 * @param log_name Bare file name, e.g. "moonraker.log" or "klippy.log".
 * @return Absolute paths, most authoritative first, deduplicated.
 *
 * Deliberately derived from the daemon's own argv rather than from a hardcoded
 * list of per-platform data roots. The reporter's platform is an AD5X running
 * ZMOD, whose layout is not documented here and which nobody on this project has
 * a device to check — a guessed path list would be fiction that looks like
 * knowledge, and would silently return nothing on every layout not on the list.
 * argv is ground truth wherever the daemon is actually running. An empty result
 * is a real answer ("we could not tell where the logs are"), not a failure to
 * try harder.
 */
std::vector<std::string> candidate_log_paths(const std::vector<ProcMatch>& procs,
                                             const std::string& log_name);

/// Listening addresses on @p port across /proc/net/tcp and tcp6. Empty when
/// nothing is listening or /proc is unavailable.
std::vector<std::string> listeners_on_port(uint16_t port);

/// Processes whose cmdline mentions Moonraker or Klipper. Empty on a system
/// without /proc, or when neither is running — which is itself the finding.
std::vector<ProcMatch> find_moonraker_processes();

/**
 * @brief How to bolt a HelixScreen config onto a Moonraker whose own config is
 *        out of the file API's reach
 *
 * Stock Creality K2 launches `moonraker.py -c /usr/share/moonraker/moonraker.conf`
 * while the file manager's only writable config root is
 * /mnt/UDISK/printer_data/config, so that moonraker.conf is a 404 over HTTP and
 * no Moonraker call can edit it. HelixScreen runs on that printer as root, so the
 * file is reachable locally — but only if we know precisely which file to touch
 * and exactly what to append.
 */
struct LocalIncludePlan {
    /// True only when every input needed to write safely was recovered.
    bool viable = false;
    /// Absolute path of the config Moonraker actually loaded — the file to append to.
    std::string vendor_config_abs;
    /// Absolute path of the helixscreen.conf the include will point at.
    std::string helix_conf_abs;
    /// The same file addressed through the file API, relative to the config root.
    std::string helix_conf_upload;
    /// The exact line to append to @ref vendor_config_abs.
    std::string include_line;
    /// Why the plan is not viable. Empty when it is.
    std::string error;
};

/**
 * @brief Decide whether, and how, to reach Moonraker's config as a local file
 *
 * @param procs           Output of find_moonraker_processes(); the caller must
 *                        already have established that Moonraker is on this host.
 * @param config_root_abs Absolute path of the file manager's writable "config"
 *                        root, from server.files.roots.
 *
 * The include is always absolute: Moonraker resolves a relative include against
 * the *including* file's directory, so a bare "helixscreen.conf" written into a
 * vendor config under /usr/share names a file that does not exist — and an
 * include with no matching file makes Moonraker refuse to start outright.
 *
 * Not viable when the loaded config already sits under @p config_root_abs. That
 * is not the K2 situation, and writing the file behind Moonraker's back there
 * would paper over whatever else made the file API fail. Both sides are resolved
 * through symlinks before that comparison — on the AD5M /root/printer_data/config
 * IS /opt/config, and a literal prefix test would miss it.
 */
LocalIncludePlan plan_local_include(const std::vector<ProcMatch>& procs,
                                    const std::string& config_root_abs);

/**
 * @brief Append @p include_line to a local config file, without truncating it
 *
 * The one write in this module, and the riskiest thing in the Spoolman flow: the
 * file belongs to the vendor firmware and a half-written one leaves the printer
 * with a Moonraker that will not start. So the existing content is read whole,
 * the new line appended, the result written to a temp file in the same directory,
 * fsync'd, and renamed over the original, with the directory fsync'd after — a
 * crash OR a power cut mid-write loses the temp file, not the config. (The rename
 * alone would order only the directory entry, not the temp file's data: with
 * delayed allocation that is exactly how a power cut yields a zero-length config.)
 *
 * A @p config_abs that is a symlink is resolved first and the real file edited,
 * since rename() would otherwise replace the link itself with a regular file.
 *
 * Idempotent, and it shares MoonrakerConfigManager::has_include_line() with the
 * file-API path so the two can never disagree about whether a write is needed:
 * a file already including @p include_target is left untouched and the call
 * still succeeds.
 *
 * Blocking file IO — never call this from the LVGL main thread.
 *
 * @param config_abs     Absolute path of the config to edit. Must already exist;
 *                       creating it would replace a file we failed to read with
 *                       one defining no [server] at all.
 * @param include_target What to include, e.g. "/mnt/.../helixscreen.conf". The
 *                       `[include ...]` line is built from it.
 * @param error          Out: reason on failure, cleared on success.
 * @return False when the file could not be read or the replacement not landed.
 */
bool append_include_to_local_config(const std::string& config_abs,
                                    const std::string& include_target, std::string& error);

} // namespace helix::diag
