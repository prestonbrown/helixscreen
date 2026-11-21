// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

namespace helix {

/**
 * @brief Resolve the HelixScreen data root directory from a binary path
 *
 * Given the absolute path to the helix-screen binary, strips known
 * binary directory suffixes (/build/bin, /bin) and validates that the
 * resulting directory contains a ui_xml/ subdirectory.
 *
 * @param exe_path Absolute path to the running binary
 * @return Resolved data root path, or empty string if not found
 */
std::string resolve_data_root_from_exe(const std::string& exe_path);

/// The version directory of PAXX's HelixScreen package (under /oem/apps/helixscreen)
/// that @p running_exe runs from: the subdirectory whose `bin/helix-screen` is the same
/// file (device and inode) as @p running_exe, so a launch through the lmd hook's bind
/// mount over /usr/bin/gui still names it. A real directory wins over a link to one
/// (`latest`). nullopt when there is no package dir or @p running_exe is not one of its
/// binaries. Paths probe under @p probe_root and come back with it prefixed.
std::optional<std::string>
u1_firmware_package_root(const std::string& running_exe = "/proc/self/exe",
                         const std::string& probe_root = "/");

/**
 * @brief The install tree the running binary belongs to
 *
 * resolve_data_root_from_exe(@p exe_path) when the binary's own path leads to one.
 * Otherwise the PAXX package version directory that @p exe_path is the binary of
 * (u1_firmware_package_root), when it is a valid data root: PAXX launches the
 * package binary through a bind mount over /usr/bin/gui, which names no install
 * tree. Every caller that needs "where is my install" asks this, never
 * /proc/self/exe's directory.
 *
 * @param exe_path Absolute path to the running binary (readlink of /proc/self/exe)
 * @param probe_root Root the firmware package directory is probed under
 * @return The install root, or empty when none is found
 */
std::string resolve_install_root(const std::string& exe_path, const std::string& probe_root = "/");

/**
 * @brief Check whether a directory is a valid HelixScreen data root
 *
 * A valid data root must contain a ui_xml/ subdirectory.
 *
 * @param dir Directory path to check
 * @return true if the directory contains ui_xml/
 */
bool is_valid_data_root(const std::string& dir);

/**
 * @brief Resolve the writable user-config directory
 *
 * Returns $HELIX_CONFIG_DIR if set and non-empty; otherwise returns the
 * relative default "config". Does not create the directory — callers are
 * responsible for fs::create_directories before writing.
 *
 * Used for runtime-mutable state (settings.json, tool_spools.json,
 * telemetry_*.json, custom_images/, user themes, pending_remap.json,
 * etc.). On read-only baselines (Yocto squashfs) HELIX_CONFIG_DIR points
 * at a writable partition (e.g. /etc/klipper/config/helixscreen).
 *
 * Not thread-safe with concurrent setenv(); call after env vars are
 * stable (before worker threads spawn). Safe to call from any thread
 * once env is frozen.
 */
std::string get_user_config_dir();

/**
 * @brief Resolve the read-only data root for shipped seed configs
 *
 * Returns $HELIX_DATA_DIR if set and non-empty; otherwise returns "."
 * (relative to cwd, which is the install root after Application bootstrap).
 * Yocto sets HELIX_DATA_DIR=/usr/share/helixscreen.
 *
 * Same thread-safety caveats as get_user_config_dir().
 */
std::string get_data_dir();

/**
 * @brief Root directory containing ui_xml/ and assets/
 *
 * Defaults to "." — byte-compatible with the historical CWD assumption
 * (Application chdir()s into the data root at startup, so relative paths
 * already resolve). Embedded targets with no working directory (ESP-IDF
 * VFS has none — every relative path fails there) call
 * set_asset_root("/littlefs") before any UI/theme init.
 *
 * Same thread-safety caveats as get_user_config_dir(): set once during
 * single-threaded startup, read-only afterwards.
 */
const std::string& asset_root();

/** @brief Override the asset root (empty string resets to "."). */
void set_asset_root(const std::string& root);

/**
 * @brief Join a relative asset path onto asset_root()
 *
 * Identity when the root is "." (desktop: asset_path("ui_xml") == "ui_xml",
 * keeping every existing path byte-identical); prefix-joined otherwise
 * ("/littlefs/ui_xml").
 */
std::string asset_path(const std::string& relpath);

/**
 * @brief LVGL "A:" URI for registering a bundled XML component/asset
 *
 * Returns "A:" + asset_path(relpath). Use this instead of a hardcoded
 * "A:ui_xml/..." literal when calling lv_xml_register_component_from_file (or any
 * lv_fs open): the literal is only correct when asset_root is "." (desktop), and
 * silently fails on firmware where the bundle is mounted at "/assets"
 * ("A:ui_xml/..." → missing file; the correct path is "A:/assets/ui_xml/...").
 * Byte-identical to the old literal on desktop.
 */
std::string asset_component_uri(const std::string& relpath);

/**
 * @brief Build a writable path under the user-config directory
 *
 * Returns get_user_config_dir() + "/" + relpath, with any trailing slash
 * on the config dir stripped. Used for files the app mutates.
 */
std::string writable_path(const std::string& relpath);

/**
 * @brief Locate a config file with user-override-then-seed precedence
 *
 * Looks first at writable_path(relpath); falls back to
 * get_data_dir()/assets/config/<relpath>. If neither exists, returns
 * the writable-config path so callers' ENOENT errors point at the
 * user-facing location, not the seed dir.
 */
std::string find_readable(const std::string& relpath);

} // namespace helix
