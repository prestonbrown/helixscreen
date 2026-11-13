#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: moonraker
# Moonraker update_manager configuration
#
# Reads: PLATFORM, INSTALL_DIR, SUDO
# Writes: -

# Source guard
[ -n "${_HELIX_MOONRAKER_SOURCED:-}" ] && return 0
_HELIX_MOONRAKER_SOURCED=1

# Common moonraker.conf locations
# ZMOD-on-AD5X notes:
#   /opt/config is a symlink to /usr/data/config; printer_data lives under both.
#   We list both forms so symlink-aware and -unaware path resolutions both hit.
# COSMOS (OpenCentauri, Elegoo Centauri Carbon) note:
#   /etc/klipper/config has no printer_data component at all. set_install_paths
#   points KLIPPER_CONFIG_DIR there for platform=cc1, so the dynamic probe below
#   normally wins; the static entry is the safety net for a COSMOS box that was
#   not detected as cc1 (forced --platform, future Elegoo model, etc.).
# Creality K2 note:
#   procd starts Moonraker with -c /usr/share/moonraker/moonraker.conf, which
#   find_moonraker_conf reads from the running process. The static entry, last
#   so no other layout's vendor copy can shadow its own conf, covers a K2 whose
#   Moonraker is stopped. The path is Creality's vendor package, K2-only in
#   practice: K1 keeps its conf under /usr/data, listed above it.
MOONRAKER_CONF_PATHS="
/home/pi/printer_data/config/moonraker.conf
/home/biqu/printer_data/config/moonraker.conf
/home/mks/printer_data/config/moonraker.conf
/home/qidi/printer_data/config/moonraker.conf
/root/printer_data/config/moonraker.conf
/opt/config/printer_data/config/moonraker.conf
/opt/config/moonraker.conf
/usr/data/config/printer_data/config/moonraker.conf
/usr/data/printer_data/config/moonraker.conf
/etc/klipper/config/moonraker.conf
/usr/share/moonraker/moonraker.conf
"

# Common Moonraker SOURCE roots (the checkout/package that holds the Python
# code, NOT printer_data). Mirrors the discovery list in
# moonraker-plugin/install.sh, extended with the buildroot vendor layouts:
#   /home/lava/moonraker        Snapmaker U1 (klipper runs as 'lava')
#   /usr/data/moonraker         Creality K1 series
#   /mnt/UDISK/moonraker        Creality K2 series
#   /usr/share/moonraker        Creality stock (vendor-installed package)
#   /root/printer_software/...  FlashForge AD5M Klipper Mod
# Overridable so tests can point at a fixture tree, same as MOONRAKER_CONF_PATHS.
MOONRAKER_SRC_PATHS="
/home/pi/moonraker
/home/biqu/moonraker
/home/mks/moonraker
/home/qidi/moonraker
/home/klipper/moonraker
/home/lava/moonraker
/root/moonraker
/root/printer_software/moonraker
/usr/data/moonraker
/usr/share/moonraker
/mnt/UDISK/moonraker
/userdata/moonraker
/opt/moonraker
"

# Locate Moonraker's update_manager component package.
# Three on-disk layouts are covered:
#   <root>/moonraker/components/update_manager            -- git checkout (~/moonraker)
#   <root>/components/update_manager                      -- the package dir itself
#   <root>/moonraker/moonraker/components/update_manager  -- repo nested in an install dir
#
# The third form is Creality's. Measured on a K1C (192.168.30.182) running
# Moonraker v0.10.0-10: the install dir is /usr/data/moonraker, the git repo is
# cloned to /usr/data/moonraker/moonraker, and the python package is a further
# level down, so the real path is
#   /usr/data/moonraker/moonraker/moonraker/components/update_manager
# Both of the first two forms miss that, which would have made the probe return
# "undetermined" on every K1/K2 -- exactly the platforms the gate exists for.
# Returns: path to the update_manager directory, or empty string.
find_moonraker_update_manager_dir() {
    local root

    # Dynamic: the detected Klipper user's home first (same precedence as
    # find_moonraker_conf), then the static fallback list.
    for root in ${KLIPPER_HOME:+"${KLIPPER_HOME}/moonraker"} $MOONRAKER_SRC_PATHS; do
        [ -n "$root" ] || continue
        moonraker_update_manager_dir_under "$root" && return 0
    done

    # Last, the running Moonraker itself, seen through its own root: Forge-X
    # runs it in a chroot (/data/.mod/.forge-x) that no static path names.
    local pkg
    pkg=$(running_moonraker_package_dir)
    if [ -n "$pkg" ] && [ -d "$pkg/components/update_manager" ]; then
        echo "$pkg/components/update_manager"
        return 0
    fi

    echo ""
}

# Print the update_manager directory of the Moonraker install at $1, in any of
# the three layouts above. Returns 1, printing nothing, when there is none.
moonraker_update_manager_dir_under() {
    local sub
    for sub in "$1/moonraker/components/update_manager" \
               "$1/components/update_manager" \
               "$1/moonraker/moonraker/components/update_manager"; do
        if [ -d "$sub" ]; then
            echo "$sub"
            return 0
        fi
    done
    return 1
}

# Print the running Moonraker's python package directory (the one holding
# moonraker.py) as a path from here. Empty when no Moonraker is running, or
# when it runs as a module (python -m moonraker), which names no file.
running_moonraker_package_dir() {
    local script
    script=$(running_moonraker_process | sed -n 2p)
    # A relative script path names nothing from here.
    case "$script" in
        /*) ;;
        *) echo ""; return 0 ;;
    esac
    echo "$(running_moonraker_root)$(dirname "$script")"
}

# Probe whether the installed Moonraker honours release_info.json's asset_name.
#
# Why a source probe and not a version string: Moonraker reports versions like
# "v0.9.3-73-gfab6c5c1", which cannot be ordered reliably across branches and
# vendor forks. The file layout is decisive instead. Moonraker commit
# 530f1c2016 (2025-01-19, first tagged in v0.10.0) added asset_name support AND
# renamed zip_deploy.py -> net_deploy.py, so the two facts travel together.
#
# Without asset_name support, NetDeploy/ZipDeploy seeds release_asset =
# assets[0]. GitHub sorts release assets by name, so assets[0] for
# prestonbrown/helixscreen is "ad5m.sym.zst" -- a zstd symbol file.
# _extract_release() then does shutil.rmtree(self.path) + mkdir BEFORE opening
# the zip, so pressing Update in Mainsail/Fluidd DELETES the install directory
# and dies with "File is not a zip file" (prestonbrown/helixscreen#993).
#
# Echoes exactly one of: supported | unsupported | undetermined
moonraker_asset_name_support() {
    moonraker_asset_name_support_in "$(find_moonraker_update_manager_dir)"
}

# The verdict above for the update_manager directory $1 (empty: none found).
moonraker_asset_name_support_in() {
    local um="$1"

    if [ -z "$um" ]; then
        # No Moonraker source anywhere we know to look (vendor layout we don't
        # recognise, container, remote Moonraker...). Can't reason about it.
        echo "undetermined"
        return 0
    fi

    if [ -f "$um/net_deploy.py" ]; then
        if grep -q 'asset_name' "$um/net_deploy.py" 2>/dev/null; then
            echo "supported"
        else
            # Renamed but asset_name stripped/absent -- a fork we must not trust.
            echo "unsupported"
        fi
        return 0
    fi

    # Pre-530f1c2016 module names: zip_deploy.py (2024-01-20 onward) and
    # web_deploy.py (earlier still). Neither reads asset_name.
    if [ -f "$um/zip_deploy.py" ] || [ -f "$um/web_deploy.py" ]; then
        echo "unsupported"
        return 0
    fi

    # Found the package but none of the modules we know. Don't guess.
    echo "undetermined"
}

# Find moonraker.conf
# Returns: path to moonraker.conf or empty string
#
# NEVER returns a mod-owned path (host_path_is_mod_owned): the firmware mods
# keep their moonraker.conf git-tracked in their own repo, so a stanza written
# there dirties their checkout and their OTA drops it again. On a mod host the
# sanctioned include point is the mod's user.moonraker.conf (included by the
# mod's conf), which the host profile recorded in HOST_MOONRAKER_USER_CONF, or
# which a mod-owned candidate names in its own [include] line.
find_moonraker_conf() {
    # Mod host: answer before any filesystem discovery can find the mod's own
    # conf (or a symlink to it). The user conf may not exist yet; the stanza
    # writer creates it there.
    if [ -n "${HOST_MOONRAKER_USER_CONF:-}" ]; then
        echo "$HOST_MOONRAKER_USER_CONF"
        return 0
    fi

    # Dynamic: the platform's own config dir first -- KLIPPER_CONFIG_DIR when a
    # firmware declared one (COSMOS), else <KLIPPER_HOME>/printer_data/config.
    local config_dir
    config_dir="$(klipper_config_dir)"
    if [ -n "$config_dir" ] && moonraker_conf_answer "${config_dir}/moonraker.conf"; then
        return 0
    fi

    # The conf the running Moonraker loads, from its command line.
    local loaded
    loaded=$(running_moonraker_conf)
    if [ -n "$loaded" ] && moonraker_conf_answer "$loaded"; then
        return 0
    fi

    # Static fallback
    for conf in $MOONRAKER_CONF_PATHS; do
        if moonraker_conf_answer "$conf"; then
            return 0
        fi
    done
    echo ""
}

# Print what find_moonraker_conf answers for candidate conf @1: @1 itself, or
# for a mod-owned @1 the user conf it includes (mod_user_moonraker_conf).
# Returns 1, printing nothing, when @1 is missing or neither is ours to edit.
moonraker_conf_answer() {
    [ -f "$1" ] || return 1
    if ! host_path_is_mod_owned "$1"; then
        printf '%s\n' "$1"
        return 0
    fi
    mod_user_moonraker_conf "$1"
}

# Print the user conf mod-owned moonraker.conf @1 includes
# ([include ...user.moonraker.conf]), resolved the way Moonraker resolves it:
# a relative include against the directory of @1 as the path was given, so a
# conf linked into the mod's tree resolves beside the link (Z-Mod's AD5X
# config dir), not inside the tree. Returns 1 when @1 includes none, or the
# one it names is mod-owned as well.
mod_user_moonraker_conf() {
    local inc
    inc=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]]*user\.moonraker\.conf\)[[:space:]]*\][[:space:]]*$/\1/p' "$1" 2>/dev/null | sed -n 1p)
    [ -n "$inc" ] || return 1
    case "$inc" in
        /*) inc="$(running_moonraker_root)$inc" ;;
        *) inc="$(dirname "$1")/$inc" ;;
    esac
    host_path_is_mod_owned "$inc" && return 1
    printf '%s\n' "$inc"
}

# Print the mod-owned moonraker.conf that includes user conf @1, from the
# candidates find_moonraker_conf walks. Empty when @1 is no mod's user conf.
# A stanza an earlier install appended to the mod's conf is reachable only
# from there: the user conf includes nothing back.
mod_conf_including_user_conf() {
    local config_dir cand
    config_dir="$(klipper_config_dir)"
    for cand in ${config_dir:+"${config_dir}/moonraker.conf"} "$(running_moonraker_conf)" $MOONRAKER_CONF_PATHS; do
        [ -f "$cand" ] && host_path_is_mod_owned "$cand" || continue
        if [ "$(mod_user_moonraker_conf "$cand")" = "$1" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 0
}

# Check if update_manager section for helixscreen already exists
# Args: $1 = moonraker.conf path
# Returns: 0 if exists, 1 if not
has_update_manager_section() {
    local conf="$1"
    grep -q '^\[update_manager helixscreen\]' "$conf" 2>/dev/null
}

# Print each file holding [update_manager helixscreen] among moonraker.conf @1
# and the files its [include] lines reach, one per line; nothing when none does.
# Some firmware keeps the web UIs' updater stanzas in an included mainsail.conf.
update_manager_stanza_files() {
    helix_conf_files_matching "$1" "$(running_moonraker_root)" '^\[update_manager helixscreen\]'
}

# True, after a warning, when config file @1, which holds section @2, belongs
# to the firmware mod: the mod's own files are never edited here.
conf_left_to_mod() {
    host_path_is_mod_owned "$1" || return 1
    log_warn "$2 is in $1, which the firmware mod owns - leaving it in place"
}

# Warn that the [update_manager helixscreen] stanza in mod-owned conf @1 now
# duplicates the one in user conf @2, and say how to remove it. The mod's conf
# is a file in the mod's git tree: the entry marks that tree modified, and
# Moonraker refuses to update a modified git_repo, so the mod's own updates
# stay blocked until the file is restored. That file is the mod's to restore.
warn_mod_conf_stanza() {
    local real
    real=$(host_canonical_path "$1")
    log_warn "An earlier HelixScreen install added [update_manager helixscreen] to"
    log_warn "  $1"
    log_warn "which belongs to the firmware mod. Moonraker reports the mod's git tree as"
    log_warn "modified and will not update the mod until that file is restored."
    log_warn "HelixScreen's updater entry is now in $2. To unblock the mod's updates:"
    log_warn "  git -C $(dirname "$real") checkout -- $(basename "$real")"
    log_warn "then restart Moonraker."
}

# warn_mod_conf_stanza for each mod-owned file in list @1 (one per line).
warn_mod_conf_stanzas() {
    [ -n "$1" ] || return 0
    printf '%s\n' "$1" | while IFS= read -r _wmcs_file; do
        [ -n "$_wmcs_file" ] && warn_mod_conf_stanza "$_wmcs_file" "$2"
    done
    return 0
}

# update_manager_stanza_files @1 without the firmware mod's files, each of
# which is warned about and left out: the stanza files the installer may edit.
update_manager_editable_stanza_files() {
    update_manager_stanza_files "$1" | while IFS= read -r _umes_file; do
        conf_left_to_mod "$_umes_file" "[update_manager helixscreen]" || printf '%s\n' "$_umes_file"
    done
}

# The channel Moonraker's type:web updater should follow. Moonraker accepts
# only stable and beta here; the dev channel has no Moonraker lane and rides
# beta, the closest published feed.
update_manager_web_channel() {
    case "${R2_CHANNEL:-stable}" in
        beta | dev) echo "beta" ;;
        *) echo "stable" ;;
    esac
}

# Generate update_manager configuration block
generate_update_manager_config() {
    local web_channel
    web_channel=$(update_manager_web_channel)
    cat << EOF

# HelixScreen Update Manager
# Added by HelixScreen installer - enables one-click updates from Mainsail/Fluidd
# NOTE: type: web is used instead of type: zip as a workaround for
# mainsail-crew/mainsail#2444 (zip type always shows UP-TO-DATE).
# A systemd path unit handles service restart after Moonraker extracts the update.
[update_manager helixscreen]
type: web
channel: ${web_channel}
repo: prestonbrown/helixscreen
path: ${INSTALL_DIR}
EOF
}

# Add update_manager section to moonraker.conf
# Args: $1 = moonraker.conf path
add_update_manager_section() {
    local conf="$1"
    local fs

    # The stanza's `path:` hands INSTALL_DIR to Moonraker's NetDeploy, whose
    # update flow rmtree()s the path before extracting. Every writer funnels
    # through here (fresh add + migrate_to_web_type), so this one guard covers
    # every UNARMED stanza write. Armed payload runs are exempt BY DESIGN - the
    # armed path is instead refused upstream in configure_moonraker_updates
    # whenever INSTALL_DIR is mod-owned (an operator-chosen in-tree root; the
    # probed default lives outside the mod's namespaces), so the exemption
    # this guard grants can never put an updater against the mod's tree.
    host_refuse_mod_owned "arming the Moonraker updater against" "$INSTALL_DIR"

    fs=$(file_sudo "$conf")

    # Create backup
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    # Append configuration
    generate_update_manager_config | $fs tee -a "$conf" >/dev/null

    log_success "Added update_manager section to $conf"
}

# Check if moonraker.conf has old git_repo-style helixscreen section
# Args: $1 = moonraker.conf path
# Returns: 0 if old git_repo section found, 1 if not
has_old_git_repo_section() {
    local conf="$1"
    # Look for helixscreen section with type: git_repo
    if grep -q '^\[update_manager helixscreen\]' "$conf" 2>/dev/null; then
        # Extract the section and check for git_repo type
        awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'git_repo'
        return $?
    fi
    return 1
}

# Check if moonraker.conf has old zip-style helixscreen section
# Args: $1 = moonraker.conf path
# Returns: 0 if old zip section found, 1 if not
has_old_zip_section() {
    local conf="$1"
    if grep -q '^\[update_manager helixscreen\]' "$conf" 2>/dev/null; then
        awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'zip'
        return $?
    fi
    return 1
}

# Migrate old section (git_repo or zip) to type: web
# Args: $1 = moonraker.conf path
migrate_to_web_type() {
    local conf="$1"

    log_info "Migrating update_manager to type: web..."

    # Remove old section. Adding the web section after a failed removal would
    # leave two [update_manager helixscreen] sections, which Moonraker rejects.
    if ! remove_update_manager_section "$conf" 2>/dev/null; then
        log_warn "Could not remove the old update_manager section; leaving it as it is"
        return 1
    fi

    # Add new web section
    add_update_manager_section "$conf"

    # Clean up old sparse clone directory if it exists (from git_repo era)
    local old_repo_dir="${INSTALL_DIR}-repo"
    if [ -d "$old_repo_dir" ]; then
        log_info "Removing old updater repo at $old_repo_dir..."
        $SUDO rm -rf "$old_repo_dir"
    fi

    log_success "Migrated to type: web update manager"
}

# Path to os-release; overridable so tests can point at a fixture.
: "${OS_RELEASE_FILE:=/etc/os-release}"

# Detect buildroot-based firmware (K1/AD5M/Snapmaker U1 etc.).
# Mirrors platform.sh's buildroot check.
# Returns: 0 if buildroot, 1 otherwise
is_buildroot_distro() {
    [ -f "$OS_RELEASE_FILE" ] && grep -q "buildroot" "$OS_RELEASE_FILE" 2>/dev/null
}

# Commands that mean the OS has a package manager Moonraker could actually use.
# Overridable so the BATS suite can simulate firmware without one.
: "${OS_PACKAGE_MANAGER_CMDS:=apt-get apt}"

# Returns: 0 if some usable OS package manager is on PATH, 1 otherwise.
has_os_package_manager() {
    local cmd
    for cmd in $OS_PACKAGE_MANAGER_CMDS; do
        command -v "$cmd" >/dev/null 2>&1 && return 0
    done
    return 1
}

# True when Moonraker's System Update Provider cannot work on this OS.
#
# Moonraker's PackageDeploy tries PackageKit over DBus first, then falls back to
# the apt CLI and nothing else (system_deploy.py _get_fallback_provider:
# "Currently only the API Fallback provider is available"). On firmware with
# neither, the moment ANY [update_manager] section exists Moonraker emits four
# permanent warnings into Mainsail/Fluidd — three "Unable to find DBus PolKit
# Interface" (one per PackageKit permission check) plus "Unable to initialize
# System Update Provider for distribution" — and logs a traceback on every
# refresh. Since we are the ones adding that section, we are the ones who put
# the warnings there.
#
# The absence of a package manager IS the condition, so probe for that rather
# than for a distro name. The name check alone missed two shipped platforms:
#   CC1  (COSMOS/Yocto) — no /etc/os-release at all, so any grep on it fails
#   K2+  (OpenWrt)      — ID="openwrt", nothing matching "buildroot"
# is_buildroot_distro stays as a first term so the platforms this already
# covered keep behaving identically.
# Returns: 0 if the provider is unusable here, 1 if it should be left enabled.
system_updates_unavailable() {
    is_buildroot_distro && return 0
    has_os_package_manager || return 0
    return 1
}

# Check if moonraker.conf has a bare top-level [update_manager] section
# (exactly "[update_manager]", NOT "[update_manager <name>]").
# Args: $1 = moonraker.conf path
# Returns: 0 if present, 1 if not
has_bare_update_manager_section() {
    local conf="$1"
    # Anchored: optional surrounding whitespace, literal [update_manager], nothing else.
    grep -qE '^[[:space:]]*\[update_manager\][[:space:]]*$' "$conf" 2>/dev/null
}

# Check if the bare [update_manager] section already contains an
# enable_system_updates key (any value — respect the user's choice).
# Args: $1 = moonraker.conf path
# Returns: 0 if the key is present in the bare section, 1 if not
bare_update_manager_has_enable_key() {
    local conf="$1"
    awk '
        /^[[:space:]]*\[update_manager\][[:space:]]*$/ { in_section=1; next }
        in_section && /^[[:space:]]*\[/ { in_section=0 }
        in_section && /^[[:space:]]*enable_system_updates[[:space:]]*:/ { found=1; exit }
        END { exit (found ? 0 : 1) }
    ' "$conf"
}

# Ensure "enable_system_updates: False" exists under a top-level
# [update_manager] section, but only where the OS package manager is missing.
# This silences the "Unable to initialize System Update Provider" and "Unable to
# find DBus PolKit Interface" warnings in Mainsail/Fluidd without affecting our
# [update_manager helixscreen] one-click updater. On a distro with a working
# package manager (Pi/x86) the warnings never fire and OS updates actually work,
# so we leave them enabled. See system_updates_unavailable().
#
# Merge semantics (never blindly append — a duplicate bare section is a fatal
# Moonraker config error):
#   - bare [update_manager] exists WITH the key -> leave it alone (user choice)
#   - bare [update_manager] exists WITHOUT the key -> add the key under it
#   - no bare [update_manager] -> insert one (with the key) before our
#     [update_manager helixscreen] block, or appended if that block is absent
# Idempotent: safe to run repeatedly.
# Args: $1 = moonraker.conf path
disable_system_updates_on_buildroot() {
    local conf="$1"

    system_updates_unavailable || return 0
    [ -f "$conf" ] || return 0

    # Already fully configured — nothing to do (keeps idempotency cheap and
    # respects an existing user-set value).
    if has_bare_update_manager_section "$conf" && bare_update_manager_has_enable_key "$conf"; then
        return 0
    fi

    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    if has_bare_update_manager_section "$conf"; then
        # Merge: insert the key on the line right after the bare section header.
        log_info "Adding enable_system_updates: False to existing [update_manager] (no OS package manager)"
        $fs awk '
            { print }
            !done && /^[[:space:]]*\[update_manager\][[:space:]]*$/ {
                print "enable_system_updates: False"
                done=1
            }
        ' "$conf" > "${conf}.tmp" && $fs mv "${conf}.tmp" "$conf"
    elif has_update_manager_section "$conf"; then
        # Insert a fresh bare section immediately before our helixscreen block.
        log_info "Adding [update_manager] enable_system_updates: False (no OS package manager)"
        $fs awk '
            !done && /^\[update_manager helixscreen\]/ {
                print "# Disable OS package updates: this firmware has no apt/PackageKit."
                print "# Silences Moonraker'\''s \"Unable to initialize System Update Provider\" warning."
                print "[update_manager]"
                print "enable_system_updates: False"
                print ""
                done=1
            }
            { print }
        ' "$conf" > "${conf}.tmp" && $fs mv "${conf}.tmp" "$conf"
    else
        # No helixscreen block yet — append a bare section at EOF.
        log_info "Adding [update_manager] enable_system_updates: False (no OS package manager)"
        {
            printf '\n'
            printf '# Disable OS package updates: this firmware has no apt/PackageKit.\n'
            printf '# Silences Moonraker'\''s "Unable to initialize System Update Provider" warning.\n'
            printf '[update_manager]\n'
            printf 'enable_system_updates: False\n'
        } | $fs tee -a "$conf" >/dev/null
    fi

    log_note "Disabled OS package updates in $conf (no OS package manager)"
}

# Remove options that do not belong in the helixscreen update_manager section.
# managed_services and install_script are not read by a type: web stanza, so
# Moonraker logs an "unparsed config option" warning for each.
# persistent_files IS read by type: web, but Moonraker matches it against
# top-level entry names of path: only (no globs, no nested paths), and our
# release zip ships a top-level config/. Listing config would restore the saved
# directory to config/config after extraction; a nested entry such as
# config/settings.json never matches. It therefore cannot preserve anything
# useful and is removed. User config survives through the symlinks into
# printer_data/config/helixscreen/ instead (setup_config_symlink).
# Args: $1 = moonraker.conf path
# Point an existing stanza's `path:` at INSTALL_DIR.
# The value is interpolated once, when the section is first added, and nothing
# revisits it. Moonraker's NetDeploy rmtree()s that path before extracting, so a
# stale one aims a delete-and-repopulate at a tree we no longer own while the
# web UI reports success.
# Args: $1 = moonraker.conf path
sync_update_manager_path() {
    local conf="$1"
    local current

    current=$(awk '
        /^\[update_manager helixscreen\]/ { found=1; next }
        found && /^\[/ { exit }
        found && /^path:/ { sub(/^path:[[:space:]]*/, ""); print; exit }
    ' "$conf" 2>/dev/null)

    if [ -z "$current" ] || [ "$current" = "$INSTALL_DIR" ]; then
        return 0
    fi

    # Repointing an updater at a mod-owned tree is exactly as destructive as
    # arming one there, so it answers to the same guard.
    host_refuse_mod_owned "pointing the Moonraker updater at" "$INSTALL_DIR"

    log_info "Repointing update_manager path: ${current} -> ${INSTALL_DIR}"
    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    $fs awk -v want="$INSTALL_DIR" '
        /^\[update_manager helixscreen\]/ { in_section=1 }
        in_section && /^\[/ && !/^\[update_manager helixscreen\]/ { in_section=0 }
        in_section && /^path:/ { print "path: " want; next }
        { print }
    ' "$conf" > "${conf}.tmp" && $fs mv "${conf}.tmp" "$conf"

    log_success "update_manager path now names ${INSTALL_DIR}"
    _UPDATE_MANAGER_SYNCED=yes
}

# Point an existing stanza's `channel:` at the channel this install resolved.
# The value is interpolated when the section is first added and nothing
# revisits it, so a user who switches the app to beta keeps being offered
# stable from Mainsail/Fluidd until the stanza is rewritten.
# Args: $1 = moonraker.conf path
sync_update_manager_channel() {
    local conf="$1"
    local current want

    want=$(update_manager_web_channel)

    current=$(awk '
        /^\[update_manager helixscreen\]/ { found=1; next }
        found && /^\[/ { exit }
        found && /^channel:/ { sub(/^channel:[[:space:]]*/, ""); print; exit }
    ' "$conf" 2>/dev/null)

    # A stanza without a channel line (hand-written) is Moonraker's default
    # stable; leave it to the operator rather than inserting one.
    if [ -z "$current" ] || [ "$current" = "$want" ]; then
        return 0
    fi

    log_info "Updating update_manager channel: ${current} -> ${want}"
    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    $fs awk -v want="$want" '
        /^\[update_manager helixscreen\]/ { in_section=1 }
        in_section && /^\[/ && !/^\[update_manager helixscreen\]/ { in_section=0 }
        in_section && /^channel:/ { print "channel: " want; next }
        { print }
    ' "$conf" > "${conf}.tmp" && $fs mv "${conf}.tmp" "$conf"

    log_success "update_manager channel now ${want}"
    _UPDATE_MANAGER_SYNCED=yes
}

cleanup_unsupported_options() {
    local conf="$1"

    # Check if any unsupported options exist in the helixscreen section.
    # persistent_files has indented continuation lines; the others are single-line.
    if ! awk '
        /^\[update_manager helixscreen\]/{found=1; next}
        found && /^\[/{exit}
        found && /^(persistent_files|managed_services|install_script):/{print; exit}
    ' "$conf" | grep -q .; then
        return 0
    fi

    log_info "Removing unsupported options from [update_manager helixscreen]"
    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    # Remove matching key: lines within the helixscreen section.
    # persistent_files also has indented continuation lines (4-space indent).
    $fs awk '
        /^\[update_manager helixscreen\]/ { in_section=1 }
        in_section && /^\[/ && !/^\[update_manager helixscreen\]/ { in_section=0 }
        in_section && /^(persistent_files|managed_services|install_script):/ { skip_block=1; next }
        skip_block && /^    / { next }
        skip_block { skip_block=0 }
        { print }
    ' "$conf" > "${conf}.tmp" && $fs mv "${conf}.tmp" "$conf"

    log_success "Cleaned up unsupported options from moonraker.conf"
}

# Write release_info.json naming the installed binary's version. Moonraker
# type:web and the installer's "Update <from> -> <to>" line both read it, and a
# tree updated by copying files in keeps whatever version it was first written
# with, so a file naming another version is rewritten.
write_release_info() {
    local release_info="${INSTALL_DIR}/release_info.json"

    # Try to detect version from binary
    local version=""
    if [ -x "${INSTALL_DIR}/bin/helix-screen" ]; then
        version=$("${INSTALL_DIR}/bin/helix-screen" --version 2>/dev/null | head -n 1 | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+[^ ]*' || echo "")
    fi

    if [ -f "$release_info" ]; then
        # No version to compare against: keep what is there.
        [ -n "$version" ] || return 0
        [ "$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$release_info" | head -n 1)" = "$version" ] && return 0
    fi

    if [ -z "$version" ]; then
        log_warn "Could not detect version for release_info.json"
        return 0
    fi

    # Resolve the platform-specific asset name through the single source of
    # truth in platform.sh (shared with mk/cross.mk's baked release_info.json,
    # so the two never drift). A wrong/missing asset_name makes Moonraker fall
    # back to the alphabetically-first release asset — a .sym debug file — and
    # die with "File is not a zip file" (prestonbrown/helixscreen#993).
    local asset_name
    asset_name="$(helix_self_update_asset "${PLATFORM:-pi}")"

    log_info "Writing release_info.json (${version})..."
    cat > "${release_info}.tmp" << EOF
{"project_name":"helixscreen","project_owner":"prestonbrown","version":"${version}","asset_name":"${asset_name}"}
EOF
    # Try without sudo first (self-update: INSTALL_DIR is user-owned under NoNewPrivileges).
    # Fall back to sudo for fresh installs where the directory may be root-owned.
    mv "${release_info}.tmp" "$release_info" 2>/dev/null || \
        $SUDO mv "${release_info}.tmp" "$release_info" 2>/dev/null || true
}

# Ensure helixscreen is in moonraker.asvc (service allowlist)
# Moonraker requires services to be listed here before it can manage them.
#
# The allowlist lives one directory above the config dir, so it is derived from
# moonraker.conf rather than from any printer_data assumption. That derivation
# holds on the non-printer_data layouts too:
#   /home/pi/printer_data/config/moonraker.conf -> /home/pi/printer_data/moonraker.asvc
#   /etc/klipper/config/moonraker.conf          -> /etc/klipper/moonraker.asvc  (COSMOS)
# The COSMOS path was verified on a real CC1 -- that IS where its asvc file is.
#
# Args: $1 = moonraker.conf path (used to derive the data dir holding the asvc)
ensure_moonraker_asvc() {
    local conf="$1"
    # The data dir is two levels up from <config dir>/moonraker.conf
    local data_dir
    data_dir="$(dirname "$(dirname "$conf")")"
    local asvc="${data_dir}/moonraker.asvc"

    if [ ! -f "$asvc" ]; then
        log_info "No moonraker.asvc found at $asvc, skipping"
        return 0
    fi

    if grep -q '^helixscreen$' "$asvc" 2>/dev/null; then
        return 0
    fi

    local fs
    fs=$(file_sudo "$asvc")
    log_info "Adding helixscreen to $asvc..."
    # Ensure file ends with a newline before appending (#408)
    if [ -s "$asvc" ] && [ "$(tail -c 1 "$asvc" | wc -l)" -eq 0 ]; then
        echo "" | $fs tee -a "$asvc" >/dev/null
    fi
    echo "helixscreen" | $fs tee -a "$asvc" >/dev/null
    log_success "Added helixscreen to Moonraker service allowlist"
}

# Remove helixscreen from moonraker.asvc (service allowlist)
#
# The counterpart to ensure_moonraker_asvc(). Without it an uninstall left
# helixscreen in the allowlist permanently, since nothing else ever prunes that
# file (observed on a CC1 running COSMOS).
#
# Args: $1 = moonraker.conf path
remove_moonraker_asvc() {
    local conf="$1"
    # The data dir is two levels up from <config dir>/moonraker.conf
    local data_dir
    data_dir="$(dirname "$(dirname "$conf")")"
    local asvc="${data_dir}/moonraker.asvc"

    if [ ! -f "$asvc" ]; then
        return 0
    fi

    if ! grep -q '^helixscreen$' "$asvc" 2>/dev/null; then
        return 0
    fi

    local fs
    fs=$(file_sudo "$asvc")
    log_info "Removing helixscreen from $asvc..."
    # Anchored, so a neighbouring entry like helixscreen-old is left alone.
    $fs sed -i '/^helixscreen$/d' "$asvc" 2>/dev/null || \
    $fs sed -i '' '/^helixscreen$/d' "$asvc" 2>/dev/null || true
    log_success "Removed helixscreen from Moonraker service allowlist"
}

# Locate the helix_print.py the HelixPrint plugin install symlinks into
# Moonraker's components directory.
#
# update_manager is a core Moonraker component, so the parent of its package
# directory IS the components directory - in whichever of the three on-disk
# layouts find_moonraker_update_manager_dir resolved, including the nested
# Creality one.
#
# Returns: path to helix_print.py (symlink or regular file), or empty string.
find_helix_print_plugin() {
    local um
    um=$(find_moonraker_update_manager_dir)
    if [ -z "$um" ]; then
        echo ""
        return 0
    fi

    local target
    target="$(dirname "$um")/helix_print.py"
    # -L before -f: a symlink into an install directory this uninstall already
    # removed is dangling, and -f is false for it.
    if [ -L "$target" ] || [ -f "$target" ]; then
        echo "$target"
    else
        echo ""
    fi
}

# Remove the plugin symlink and the [helix_print] section directly.
#
# A plain section strip, NOT remove_update_manager_section's comment-block
# heuristic: nothing generates a comment header above [helix_print], so
# buffering the preceding comment run would eat a neighbouring line's comment.
#
# Idempotent and silent when there is nothing left to remove.
_remove_helix_print_inline() {
    local target
    target=$(find_helix_print_plugin)

    if [ -L "$target" ]; then
        local ps
        ps=$(path_sudo "$target")
        log_info "Removing plugin symlink $target..."
        if $ps rm -f "$target"; then
            log_success "Removed the HelixPrint plugin symlink"
        else
            log_warn "Could not remove $target - remove it by hand"
        fi
    elif [ -n "$target" ]; then
        log_warn "$target is a regular file, not a symlink this installer created - leaving it in place"
    fi

    local conf section_conf
    conf=$(find_moonraker_conf)
    [ -n "$conf" ] || return 0
    # Every file Moonraker reads the plugin's sections from: the conf and its
    # includes. That covers the [update_manager helix_print] a git-clone
    # install of the plugin wrote.
    for section_conf in $(helix_print_owned_files "$conf" "$(running_moonraker_root)"); do
        conf_left_to_mod "$section_conf" "[helix_print]" && continue
        local fs
        fs=$(file_sudo "$section_conf")
        $fs cp "$section_conf" "${section_conf}.bak.helixscreen-uninstall" 2>/dev/null || true
        log_info "Removing HelixPrint sections from $section_conf..."
        if helix_rewrite_conf "$section_conf" "$HELIX_PRINT_STRIP_AWK" "$fs"; then
            log_success "Removed HelixPrint sections from $section_conf"
        else
            log_warn "Could not remove HelixPrint sections from $section_conf - edit it by hand, backup at ${section_conf}.bak.helixscreen-uninstall"
        fi
    done
}

# Remove the HelixPrint Moonraker plugin: the helix_print.py symlink in
# Moonraker's components directory, and the [helix_print] section in
# moonraker.conf.
#
# Left behind, that symlink points into a deleted install directory. Moonraker
# loads helix_print as an optional component, so the load does not abort the
# server, but it logs an "Unable to load component: (helix_print)" traceback on
# every startup and reports the name in /server/info's failed_components, which
# clients surface as a permanently failed component.
#
# The plugin's own installer owns this removal - it also strips any PRINT_START
# phase-tracking instrumentation and restarts Moonraker - so it is delegated to
# whenever it is still on disk. The inline pass then mops up: the plugin
# script's Moonraker search covers the desktop and Pi layouts, while
# MOONRAKER_SRC_PATHS here also covers the buildroot vendor trees, and it is the
# only pass on an install tree that is already gone.
#
# Always returns 0. The plugin being absent is the common case, and a failed
# removal must never abort the rest of the uninstall.
remove_moonraker_plugin() {
    local target conf
    target=$(find_helix_print_plugin)
    conf=$(find_moonraker_conf)

    local has_section=false
    if [ -n "$conf" ] && [ -n "$(helix_print_owned_files "$conf" "$(running_moonraker_root)")" ]; then
        has_section=true
    fi

    if [ -z "$target" ] && [ "$has_section" = "false" ]; then
        return 0
    fi

    local script="${INSTALL_DIR}/moonraker-plugin/install.sh"
    if [ -f "$script" ]; then
        log_info "Removing the HelixPrint Moonraker plugin..."
        local status=0
        sh "$script" --uninstall-auto || status=$?
        case "$status" in
            0)
                log_success "Removed the HelixPrint Moonraker plugin"
                ;;
            2)
                log_warn "HelixPrint plugin removed, but a config file needs a look - see the output above"
                ;;
            *)
                log_warn "HelixPrint plugin removal reported failure (exit $status) - checking what is left"
                ;;
        esac
    fi

    _remove_helix_print_inline
    return 0
}

# The URL this host reaches Moonraker at (moonraker_local_url): the port comes
# from the platform's own moonraker.conf, which on a mod host is the mod's file
# holding [server], else from whatever find_moonraker_conf finds.
installer_moonraker_url() {
    local conf
    conf="$(klipper_config_dir)"
    conf="${conf:+${conf}/moonraker.conf}"
    [ -n "$conf" ] && [ -f "$conf" ] || conf=$(find_moonraker_conf)
    moonraker_local_url "$conf" "$(running_moonraker_root)"
}

# Moonraker loads the HelixPrint plugin from INSTALL_DIR/moonraker-plugin, so a
# Moonraker running as a user other than INSTALL_DIR's owner needs search on
# INSTALL_DIR and read on the plugin tree, which an install under umask 077
# leaves it neither of. Adds exactly that: search, never list, on INSTALL_DIR,
# and on the plugin tree 0755 directories with files readable by all
# (executables keep their x). INSTALL_DIR/config, when it is a directory of its
# own, is closed to other users, since the search bit lets anyone reach a path
# inside INSTALL_DIR they can name and config/ holds the API key and pairing
# secrets. Nothing above INSTALL_DIR, which the installer did not make, and
# nothing else inside it changes. A root Moonraker reads everything; with none
# running there is no user to open the tree to; a mod payload root is the mod's.
open_plugin_to_moonraker() {
    local plugin="${INSTALL_DIR}/moonraker-plugin" uid owner s
    [ "${HELIX_MOD_PAYLOAD:-}" = "1" ] && return 0
    [ -d "$plugin" ] || return 0
    uid=$(running_moonraker_ids)
    uid="${uid%% *}"
    case "$uid" in '' | 0) return 0 ;; esac
    # shellcheck disable=SC2012  # BusyBox find has no -printf; ls -n is the portable numeric owner
    owner=$(ls -ldn "$INSTALL_DIR" 2>/dev/null | awk '{ print $3 }')
    [ "$uid" != "$owner" ] || return 0
    log_info "Moonraker runs as uid ${uid}: making the HelixPrint plugin readable to it"
    s=$(file_sudo "$INSTALL_DIR")
    if [ -d "$INSTALL_DIR/config" ] && [ ! -L "$INSTALL_DIR/config" ]; then
        $s chmod go-rwx "$INSTALL_DIR/config" ||
            log_warn "Could not close ${INSTALL_DIR}/config to other users"
    fi
    if ! { $s chmod go+x "$INSTALL_DIR" && $s chmod -R go+rX "$plugin"; }; then
        log_warn "Could not open ${plugin} to Moonraker; installing the plugin will refuse until it can read it"
    fi
    return 0
}

# Restart Moonraker to pick up configuration changes: moonraker_restart's
# config change, over Moonraker's HTTP API at installer_moonraker_url. It
# reports what it did and what is left to the user, a Moonraker that is not
# running or a print holding the restart off included. Always returns 0: a
# restart that did not happen must never abort an install. The verdict, a
# HELIX_RESTART_* code, is left in MOONRAKER_RESTART_RC for a caller that has to
# know the new config is loaded (image.sh's first-boot moonraker step).
restart_moonraker() {
    log_info "Restarting Moonraker to apply configuration..."
    # shellcheck disable=SC2034  # consumed by image.sh (_image_first_boot_step)
    MOONRAKER_RESTART_RC=0
    # shellcheck disable=SC2034  # consumed by image.sh (_image_first_boot_step)
    moonraker_restart config "$(installer_moonraker_url)" log_info log_warn || MOONRAKER_RESTART_RC=$?
    return 0
}

# What configure_moonraker_updates would add, for the plan. Read-only: the
# same gates and the same conf, with nothing written. Sets MOONRAKER_ADDS
# (space-separated: any of update-manager, allowlist; empty if none).
# Args: $1 = platform
detect_moonraker_integration() {
    local conf asvc
    MOONRAKER_ADDS=""
    [ "${1:-}" = "ad5m" ] && return 0
    # A desktop install talks to a printer elsewhere; this box's Moonraker,
    # if it has one, does not manage it.
    [ "${1:-}" = "x86-desktop" ] && return 0
    [ "${HELIX_MOD_PAYLOAD:-}" = "1" ] && return 0

    conf=$(find_moonraker_conf)
    [ -n "$conf" ] || return 0

    if [ -z "$(update_manager_stanza_files "$conf")" ] \
        && [ "$(moonraker_asset_name_support)" != "unsupported" ]; then
        MOONRAKER_ADDS="update-manager"
    fi
    asvc="$(dirname "$(dirname "$conf")")/moonraker.asvc"
    if [ -f "$asvc" ] && ! grep -q '^helixscreen$' "$asvc" 2>/dev/null; then
        MOONRAKER_ADDS="${MOONRAKER_ADDS:+$MOONRAKER_ADDS }allowlist"
    fi
    return 0
}

# Configure Moonraker update_manager
# Called during installation on platforms with web UI (Pi, K1 with Simple AF)
configure_moonraker_updates() {
    local platform=$1

    # Skip on AD5M (stock Flashforge firmware lacks Mainsail/Fluidd).
    # NOTE: AD5X is intentionally NOT skipped — it's only ever installed via the
    # ZMOD chroot, which ships Mainsail/Fluidd.
    if [ "$platform" = "ad5m" ]; then
        log_info "Skipping Moonraker update_manager on AD5M (typically no web UI)"
        return 0
    fi

    # A payload install writes NOTHING to any Moonraker conf unless the
    # operator opted in with --auto-update. The stanza arms Moonraker's
    # NetDeploy against `path:` (its update flow rmtree()s the path), and on a
    # mod host the payload's lifecycle belongs to the mod's OTA, not to a
    # second updater. With the opt-in the stanza lands in the mod's
    # user.moonraker.conf - find_moonraker_conf's mod-host answer - never in
    # the mod's git-tracked conf.
    if [ "${HELIX_MOD_PAYLOAD:-}" = "1" ] && [ "${HELIX_MOD_PAYLOAD_UPDATES:-}" != "1" ]; then
        log_info "Payload install: skipping Moonraker update_manager"
        log_info "(pass --auto-update to write the stanza into the mod's user.moonraker.conf)"
        return 0
    fi

    # --auto-update is refused for every payload install, wherever the root
    # sits. Moonraker's type:web updater rmtree()s `path:` before extracting
    # persistent_files matches only top-level names of path:, so nothing can
    # keep the config/ and platform/ preservation the payload contract exists
    # to provide - and config/ lives inside the root. Where the root sits changes
    # nothing: a payload's lifecycle belongs to the mod's OTA, not to a second
    # updater that cannot see the contract. The option is refused, not the
    # install; completing without the updater armed is the safe outcome,
    # because nothing remote-triggered can then touch the root.
    if [ "${HELIX_MOD_PAYLOAD_UPDATES:-}" = "1" ] \
       && [ "${HELIX_MOD_PAYLOAD:-}" = "1" ]; then
        log_error "--auto-update refused: this is a payload install."
        log_error "  ${INSTALL_DIR}"
        log_error "Moonraker's type:web updater replaces the whole root on update, destroying"
        log_error "the config/ and platform/ preservation the payload contract provides."
        log_error "The mod's own OTA updates a payload install; arm nothing else against it."
        return 0
    fi

    log_info "Configuring Moonraker update_manager..."

    # Write release_info.json if not already present (fallback for older tarballs)
    write_release_info

    local conf
    conf=$(find_moonraker_conf)

    if [ -z "$conf" ]; then
        log_warn "Could not find moonraker.conf in any known location:"
        local probed_dir
        probed_dir="$(klipper_config_dir)"
        if [ -n "$probed_dir" ]; then
            log_warn "  ${probed_dir}/moonraker.conf"
        fi
        for tried in $MOONRAKER_CONF_PATHS; do
            log_warn "  $tried"
        done
        log_warn "To enable web UI updates, manually add to your moonraker.conf:"
        echo ""
        generate_update_manager_config
        echo ""
        return 0
    fi

    log_info "Using moonraker.conf at: $conf"

    # The stanza may sit in a file the conf includes rather than in the conf.
    # Every check and rewrite below goes to the files Moonraker reads it from;
    # appending one to the conf would leave two for Moonraker to merge. A
    # stanza in the firmware mod's files counts as present but is never edited.
    # When conf is the user conf a mod's own conf includes, the search starts
    # at the mod's conf, which reaches the user conf and everything beside it.
    local mod_conf stanza_files editable_files stanza_conf
    mod_conf=$(mod_conf_including_user_conf "$conf")
    stanza_files=$(update_manager_stanza_files "${mod_conf:-$conf}")
    editable_files=$(update_manager_editable_stanza_files "${mod_conf:-$conf}")
    stanza_conf=$(printf '%s\n' "$editable_files" | sed -n 1p)
    # The upgrade case: an earlier install appended the stanza to the mod's
    # conf. The user conf still takes one when no editable file holds it, so
    # the updater keeps working once the mod's file is restored. Moonraker
    # merges a section that appears in more than one file and rejects a
    # repeat only within one file.
    local mod_stanza_files=""
    if [ -n "$mod_conf" ] && [ -n "$stanza_files" ]; then
        mod_stanza_files=$(printf '%s\n' "$stanza_files" | while IFS= read -r _mcs_file; do
            if host_path_is_mod_owned "$_mcs_file"; then printf '%s\n' "$_mcs_file"; fi
        done)
        [ -n "$editable_files" ] || stanza_files=""
    fi

    # Gate on Moonraker's asset_name support before arming the one-click
    # updater (prestonbrown/helixscreen#993). On a Moonraker that ignores
    # asset_name, the Update button in Mainsail/Fluidd rmtree()s the install
    # directory and then fails on a non-zip asset -- strictly worse than no
    # button at all.
    local mr_support
    mr_support=$(moonraker_asset_name_support)

    if [ "$mr_support" = "unsupported" ]; then
        log_warn "This Moonraker predates release_info.json asset_name support."
        log_warn "Its update_manager would download the WRONG release asset and"
        log_warn "DELETE ${INSTALL_DIR} before failing. Skipping the"
        log_warn "[update_manager helixscreen] section for your own safety."
        log_warn "To get the in-UI update button: upgrade Moonraker to v0.10.0 or"
        log_warn "newer, then re-run this installer."
        log_warn "Either way, HelixScreen's built-in updater is unaffected:"
        log_warn "  Settings -> Updates, inside HelixScreen."

        # The gun may already be loaded from an earlier install that ran before
        # this gate existed -- unload it.
        local removed_stale=0
        if [ -n "$editable_files" ]; then
            log_warn "Removing the existing [update_manager helixscreen] section."
            local stale_file
            while IFS= read -r stale_file; do
                _remove_update_manager_section_from "$stale_file" && removed_stale=1
            done <<STALE_FILES
$editable_files
STALE_FILES
        fi

        # These two are orthogonal to the updater and must not regress just
        # because we skipped the stanza: the buildroot key silences a warning
        # caused by ANY update_manager section (mainsail/fluidd have their own),
        # and the asvc allowlist is what lets a user restart HelixScreen from
        # Mainsail's service list.
        disable_system_updates_on_buildroot "$conf"
        ensure_moonraker_asvc "$conf"

        if [ "$removed_stale" -eq 1 ]; then
            restart_moonraker
        fi
        return 0
    fi

    # undetermined: no recognisable Moonraker source on disk (vendor layout we
    # don't know, remote/containerised Moonraker). Deliberately preserve the
    # pre-gate behaviour and write the stanza -- refusing here would regress
    # every install we simply can't reason about.
    if [ "$mr_support" = "undetermined" ]; then
        log_warn "Could not locate the Moonraker source to verify asset_name support."
        log_warn "Configuring the updater anyway. If Moonraker is older than v0.10.0,"
        log_warn "use HelixScreen's built-in updater (Settings -> Updates) instead of"
        log_warn "the Update button in Mainsail/Fluidd."
    fi

    # Migrate old git_repo or zip config to type: web
    # (type: zip shows perpetual UP-TO-DATE in Mainsail — see mainsail-crew/mainsail#2444)
    if [ -n "$stanza_conf" ] \
        && { has_old_git_repo_section "$stanza_conf" || has_old_zip_section "$stanza_conf"; }; then
        migrate_to_web_type "$stanza_conf" || true
        # Where the OS has no package manager, silence the provider warnings.
        disable_system_updates_on_buildroot "$conf"
        ensure_moonraker_asvc "$conf"
        restart_moonraker
        return 0
    fi

    if [ -n "$stanza_files" ]; then
        # Set by the sync_* rewrites below; Moonraker reads its config only at
        # startup, so a rewritten stanza needs a restart before Mainsail sees it.
        _UPDATE_MANAGER_SYNCED=""
        local stanza_file
        while IFS= read -r stanza_file; do
            [ -n "$stanza_file" ] || continue
            log_info "update_manager section already exists in $stanza_file"
            # path: is only ever written when the section is first added, so an
            # install that has moved leaves it naming the tree we left behind.
            sync_update_manager_path "$stanza_file"
            # channel: likewise — rewrite it to whatever this update resolved to.
            sync_update_manager_channel "$stanza_file"
            # Remove managed_services and install_script (unread by type: web,
            # warned about as unparsed) and persistent_files (read, but it matches
            # top-level names only and cannot preserve our config/ layout).
            cleanup_unsupported_options "$stanza_file"
        done <<STANZA_FILES
$editable_files
STANZA_FILES
        warn_mod_conf_stanzas "$mod_stanza_files" "$stanza_conf"
        # Where the OS has no package manager, silence the provider warnings.
        disable_system_updates_on_buildroot "$conf"
        # Still ensure asvc is correct even if section already exists
        ensure_moonraker_asvc "$conf"
        if [ "$_UPDATE_MANAGER_SYNCED" = "yes" ]; then
            restart_moonraker
        fi
        return 0
    fi

    add_update_manager_section "$conf"
    warn_mod_conf_stanzas "$mod_stanza_files" "$conf"
    # Where the OS has no package manager, silence the provider warnings.
    disable_system_updates_on_buildroot "$conf"
    ensure_moonraker_asvc "$conf"
    restart_moonraker
}

# Remove [update_manager helixscreen] from conf @1, or with no argument from
# every file Moonraker reads it from: find_moonraker_conf and the files its
# [include] lines reach. A mod-owned file is left in place with a warning.
# Called during uninstallation
# Returns: 1 when a file needed the edit and could not take it (left whole)
remove_update_manager_section() {
    if [ -n "${1:-}" ]; then
        _remove_update_manager_section_from "$1"
        return
    fi
    local conf stanza_file rc=0
    conf=$(find_moonraker_conf)
    [ -n "$conf" ] || return 0
    while IFS= read -r stanza_file; do
        [ -n "$stanza_file" ] || continue
        _remove_update_manager_section_from "$stanza_file" || rc=1
    done <<STANZA_FILES
$(update_manager_editable_stanza_files "$conf")
STANZA_FILES
    return $rc
}

# Remove [update_manager helixscreen] and the comment block the installer
# writes above it from conf @1.
# Returns: 1 when the file needed the edit and could not take it (left whole)
_remove_update_manager_section_from() {
    local conf="$1"

    if ! has_update_manager_section "$conf"; then
        return 0
    fi

    log_info "Removing update_manager section from $conf..."

    # Create backup
    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen-uninstall" 2>/dev/null || true

    # Remove the section (from [update_manager helixscreen] to the next section
    # or EOF) together with the comment block generate_update_manager_config()
    # writes above it.
    #
    # The comment block is matched structurally, not by literal text. Deleting
    # named lines instead meant the two patterns here had to track every line
    # the generator emits, and they did not: the generator writes five comment
    # lines and only two were deleted, so each install/uninstall cycle orphaned
    # the other three (seen on a CC1, where "mainsail#2444" ended up in
    # moonraker.conf twice).
    #
    # Lines are buffered until something that is not a comment or blank arrives.
    # On reaching our section, the trailing run of comment lines plus at most one
    # blank line before it is dropped, which is exactly the generator's output;
    # anything earlier is another line's comment and is printed back.
    local prog='
        skip {
            if ($0 ~ /^\[/) { skip = 0 } else { next }
        }
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { buf[++n] = $0; next }
        /^\[update_manager helixscreen\]/ {
            i = n
            while (i >= 1 && buf[i] ~ /^[[:space:]]*#/) i--
            if (i >= 1 && buf[i] ~ /^[[:space:]]*$/) i--
            for (j = 1; j <= i; j++) print buf[j]
            n = 0
            skip = 1
            next
        }
        {
            for (j = 1; j <= n; j++) print buf[j]
            n = 0
            print
        }
        END { for (j = 1; j <= n; j++) print buf[j] }
    '
    if ! helix_rewrite_conf "$conf" "$prog" "$fs"; then
        log_warn "Could not remove the update_manager section from $conf - edit it by hand, backup at ${conf}.bak.helixscreen-uninstall"
        return 1
    fi

    log_success "Removed update_manager section from $conf"
}
