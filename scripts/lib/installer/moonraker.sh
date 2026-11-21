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
    local sub

    # Dynamic: the detected Klipper user's home first (same precedence as
    # find_moonraker_conf), then the static fallback list.
    for root in ${KLIPPER_HOME:+"${KLIPPER_HOME}/moonraker"} $MOONRAKER_SRC_PATHS; do
        [ -n "$root" ] || continue
        for sub in "$root/moonraker/components/update_manager" \
                   "$root/components/update_manager" \
                   "$root/moonraker/moonraker/components/update_manager"; do
            if [ -d "$sub" ]; then
                echo "$sub"
                return 0
            fi
        done
    done

    echo ""
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
    local um
    um=$(find_moonraker_update_manager_dir)

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
# Z-Mod on the AD5X keeps its moonraker.conf git-tracked in its own tree and
# links it into the config dir, so a stanza written there dirties the mod's
# checkout and Moonraker then refuses the mod's own updates. For a conf in the
# mod's tree the answer is the user conf it includes ([include
# ...user.moonraker.conf]); the mod's file is never returned.
find_moonraker_conf() {
    # Dynamic: the platform's own config dir first -- KLIPPER_CONFIG_DIR when a
    # firmware declared one (COSMOS), else <KLIPPER_HOME>/printer_data/config.
    local config_dir
    config_dir="$(klipper_config_dir)"
    if [ -n "$config_dir" ] && moonraker_conf_answer "${config_dir}/moonraker.conf"; then
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

# Z-Mod's AD5X git tree (Moonraker's [update_manager Z-Mod] path), bind-mounted
# at the same path inside its chroot. .shell/zmod.sh is Z-Mod's own entry
# script; Forge-X ships no such file. Prints the tree, or nothing.
zmod_tree_root() {
    local cand
    # shellcheck disable=SC2086  # word splitting is the point: a candidate path list
    for cand in ${HELIX_ZMOD_TREE_CANDIDATES:-/usr/data/zmod/zmod}; do
        if [ -f "$cand/.shell/zmod.sh" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 0
}

# True when path @1 (symlinks resolved) is under root @2 (symlinks resolved).
moonraker_path_under() {
    [ -n "$2" ] || return 1
    local p root
    case "$1" in "$2"|"$2"/*) return 0 ;; esac
    p=$(readlink -f "$1" 2>/dev/null) || p=""
    [ -n "$p" ] || p="$1"
    root=$(readlink -f "$2" 2>/dev/null) || root=""
    [ -n "$root" ] || root="$2"
    case "$p" in "$root"|"$root"/*) return 0 ;; esac
    return 1
}

# True when Moonraker config file @1 belongs to the firmware mod: it lies in
# Z-Mod's probed tree or under /usr/data/zmod. Such a file is never edited.
moonraker_conf_mod_owned() {
    [ -n "$1" ] || return 1
    local tree
    tree=$(zmod_tree_root)
    if [ -n "$tree" ] && moonraker_path_under "$1" "$tree"; then
        return 0
    fi
    moonraker_path_under "$1" /usr/data/zmod
}

# Print what find_moonraker_conf answers for candidate conf @1: @1 itself, or
# for a mod-owned @1 the user conf it includes (mod_user_moonraker_conf).
# Returns 1, printing nothing, when @1 is missing or neither is ours to edit.
moonraker_conf_answer() {
    [ -f "$1" ] || return 1
    if ! moonraker_conf_mod_owned "$1"; then
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
        /*) ;;
        *) inc="$(dirname "$1")/$inc" ;;
    esac
    moonraker_conf_mod_owned "$inc" && return 1
    printf '%s\n' "$inc"
}

# Print the mod-owned moonraker.conf that includes user conf @1, from the
# candidates find_moonraker_conf walks. Empty when @1 is no mod's user conf.
# A stanza an earlier install appended to the mod's conf is reachable only
# from there: the user conf includes nothing back.
mod_conf_including_user_conf() {
    local config_dir cand
    config_dir="$(klipper_config_dir)"
    # shellcheck disable=SC2086  # MOONRAKER_CONF_PATHS is a word list
    for cand in ${config_dir:+"${config_dir}/moonraker.conf"} $MOONRAKER_CONF_PATHS; do
        [ -f "$cand" ] && moonraker_conf_mod_owned "$cand" || continue
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

# Print every Moonraker config file reachable from @1 through [include] lines,
# @1 first, one per line, each once. A relative include resolves against the
# directory of the file that names it, as the path was given (a symlinked
# moonraker.conf resolves against the link's directory). A glob expands the way
# Moonraker expands it. @2, when set, is prefixed to an absolute include: the
# root Moonraker runs under, seen from here. A file is walked once however
# many paths lead to it, so an include cycle ends. Paths holding blanks are
# not supported; no Moonraker layout uses them.
moonraker_conf_files() {
    _mcf_root="${2:-}"
    _mcf_todo="$1"
    _mcf_seen=" "
    while [ -n "$_mcf_todo" ]; do
        _mcf_file="${_mcf_todo%% *}"
        case "$_mcf_todo" in
            *" "*) _mcf_todo="${_mcf_todo#* }" ;;
            *) _mcf_todo="" ;;
        esac
        [ -n "$_mcf_file" ] || continue
        [ -f "$_mcf_file" ] || continue
        _mcf_key=$(readlink -f "$_mcf_file" 2>/dev/null) || _mcf_key=""
        [ -n "$_mcf_key" ] || _mcf_key="$_mcf_file"
        case "$_mcf_seen" in *" $_mcf_key "*) continue ;; esac
        _mcf_seen="$_mcf_seen$_mcf_key "
        printf '%s\n' "$_mcf_file"
        _mcf_dir=$(dirname "$_mcf_file")
        # Trailing blanks after "]" include the CR of a file saved on Windows.
        _mcf_incs=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]]*[^][:space:]]\)[[:space:]]*\][[:space:]]*$/\1/p' "$_mcf_file" 2>/dev/null)
        while IFS= read -r _mcf_pat; do
            [ -n "$_mcf_pat" ] || continue
            case "$_mcf_pat" in
                /*) _mcf_pat="$_mcf_root$_mcf_pat" ;;
                *) _mcf_pat="$_mcf_dir/$_mcf_pat" ;;
            esac
            # Unquoted on purpose: the pattern expands here, against its own directory.
            for _mcf_inc in $_mcf_pat; do
                if [ -f "$_mcf_inc" ]; then
                    _mcf_todo="${_mcf_todo:+$_mcf_todo }$_mcf_inc"
                fi
            done
        done <<MCF_INCLUDES
$_mcf_incs
MCF_INCLUDES
    done
    return 0
}

# Print each file holding [update_manager helixscreen] among moonraker.conf @1
# and the files its [include] lines reach, one per line; nothing when none does.
# Some firmware keeps the web UIs' updater stanzas in an included mainsail.conf.
update_manager_stanza_files() {
    moonraker_conf_files "$1" | while IFS= read -r _umsf_file; do
        if has_update_manager_section "$_umsf_file"; then
            printf '%s\n' "$_umsf_file"
        fi
    done
    return 0
}

# Warn that @2 is in @1, which the firmware mod owns, and succeed; fail
# silently when @1 is ours to edit.
conf_left_to_mod() {
    moonraker_conf_mod_owned "$1" || return 1
    log_warn "$2 is in $1, which the firmware mod owns - leaving it in place"
}

# update_manager_stanza_files @1 without the firmware mod's files, each of
# which is warned about and left out: the stanza files the installer may edit.
update_manager_editable_stanza_files() {
    update_manager_stanza_files "$1" | while IFS= read -r _umes_file; do
        conf_left_to_mod "$_umes_file" "[update_manager helixscreen]" || printf '%s\n' "$_umes_file"
    done
}

# Warn that the [update_manager helixscreen] stanza in mod-owned conf @1 now
# duplicates the one in user conf @2, and say how to remove it. The mod's conf
# is a file in the mod's git tree: the entry marks that tree modified, and
# Moonraker refuses to update a modified git_repo, so the mod's own updates
# stay blocked until the file is restored. That file is the mod's to restore.
warn_mod_conf_stanza() {
    local real
    real=$(readlink -f "$1" 2>/dev/null) || real=""
    [ -n "$real" ] || real="$1"
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
    fs=$(file_sudo "$conf")

    # Create backup
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    # Append configuration
    generate_update_manager_config | $fs tee -a "$conf" >/dev/null

    log_success "Added update_manager section to $conf"
    log_info "You can now update HelixScreen from the Mainsail/Fluidd web interface!"
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

    # Remove old section
    remove_update_manager_section "$conf" 2>/dev/null || true

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

    log_success "Disabled OS package updates in $conf (no OS package manager)"
}

# Remove unsupported options from the helixscreen update_manager section.
# type: web only supports: type, channel, repo, path.
# Options like persistent_files, managed_services, and install_script are
# not supported and cause Moonraker to log "unparsed config option" warnings.
# Args: $1 = moonraker.conf path
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
# Moonraker reads the allowlist only at startup, so _MOONRAKER_ASVC_CHANGED is
# set to "yes" when this call appended to it and to "" otherwise.
#
# Args: $1 = moonraker.conf path (used to derive the data dir holding the asvc)
ensure_moonraker_asvc() {
    local conf="$1"
    _MOONRAKER_ASVC_CHANGED=""
    local asvc
    asvc=$(moonraker_asvc_file "$conf")

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
    _MOONRAKER_ASVC_CHANGED=yes
    log_success "Added helixscreen to Moonraker service allowlist"
}

# Print the moonraker.asvc that sits beside the config dir of moonraker.conf
# @1: the data dir is two levels up from <config dir>/moonraker.conf. For the
# user conf a mod's own conf includes (one level deeper, in mod_data/), the
# config dir is the mod's conf's.
moonraker_asvc_file() {
    local conf
    conf=$(mod_conf_including_user_conf "$1")
    [ -n "$conf" ] || conf="$1"
    printf '%s/moonraker.asvc\n' "$(dirname "$(dirname "$conf")")"
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
    local asvc
    asvc=$(moonraker_asvc_file "$conf")

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

# Restart Moonraker to pick up configuration changes.
#
# The init script name varies by firmware, and knowing only systemd plus the K1's
# S56moonraker_service meant this fell off the end silently on everything else.
# On a CC1 that made the moonraker.conf edit take effect at the user's next
# reboot instead of at install time, which reads as "an update spontaneously
# changed my printer" (verified: the CC1 has no systemctl, no
# S56moonraker_service, and its script is /etc/init.d/moonraker).
#
# HELIX_INITD_DIR is the same seam camera.sh uses, so the BATS suite can point
# this at a temp dir rather than needing a writable /etc/init.d.
# Always returns 0 — a failed restart must never abort an install.
restart_moonraker() {
    local initd script

    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet moonraker 2>/dev/null; then
        log_info "Restarting Moonraker to apply configuration..."
        $SUDO systemctl restart moonraker || true
        return 0
    fi

    # SysV/procd firmware. S56moonraker_service is Creality K1/Simple AF; a plain
    # "moonraker" covers COSMOS (CC1) and the rest.
    initd="${HELIX_INITD_DIR:-/etc/init.d}"
    for script in "${initd}/S56moonraker_service" "${initd}/moonraker" \
                  "/etc/init.d/S56moonraker_service" "/etc/init.d/moonraker"; do
        if [ -x "$script" ] || [ -f "$script" ]; then
            log_info "Restarting Moonraker to apply configuration..."
            if ! $SUDO "$script" restart 2>/dev/null; then
                log_warn "Could not restart Moonraker - you may need to restart it manually"
            fi
            return 0
        fi
    done

    log_warn "Could not find a way to restart Moonraker - restart it manually for the configuration change to take effect."
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
            if moonraker_conf_mod_owned "$_mcs_file"; then printf '%s\n' "$_mcs_file"; fi
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
        if [ -n "$stanza_files" ]; then
            log_warn "Removing the existing [update_manager helixscreen] section."
            remove_update_manager_section
            removed_stale=1
        fi

        # These two are orthogonal to the updater and must not regress just
        # because we skipped the stanza: the buildroot key silences a warning
        # caused by ANY update_manager section (mainsail/fluidd have their own),
        # and the asvc allowlist is what lets a user restart HelixScreen from
        # Mainsail's service list.
        disable_system_updates_on_buildroot "$conf"
        ensure_moonraker_asvc "$conf"

        if [ "$removed_stale" -eq 1 ] || [ "$_MOONRAKER_ASVC_CHANGED" = "yes" ]; then
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
        migrate_to_web_type "$stanza_conf"
        # Where the OS has no package manager, silence the provider warnings.
        disable_system_updates_on_buildroot "$conf"
        ensure_moonraker_asvc "$conf"
        restart_moonraker
        return 0
    fi

    if [ -n "$stanza_files" ]; then
        # Set by sync_update_manager_channel; Moonraker reads its config only at
        # startup, so a rewritten stanza needs a restart before Mainsail sees it.
        _UPDATE_MANAGER_SYNCED=""
        local stanza_file
        while IFS= read -r stanza_file; do
            [ -n "$stanza_file" ] || continue
            log_info "update_manager section already exists in $stanza_file"
            # channel: is only ever written when the section is first added, so
            # rewrite it to whatever this update resolved to.
            sync_update_manager_channel "$stanza_file"
            # Remove options not supported by type: web (persistent_files,
            # managed_services, install_script) that cause Moonraker warnings.
            cleanup_unsupported_options "$stanza_file"
        done <<STANZA_FILES
$editable_files
STANZA_FILES
        warn_mod_conf_stanzas "$mod_stanza_files" "$stanza_conf"
        # Where the OS has no package manager, silence the provider warnings.
        disable_system_updates_on_buildroot "$conf"
        # Still ensure asvc is correct even if section already exists
        ensure_moonraker_asvc "$conf"
        if [ "$_UPDATE_MANAGER_SYNCED" = "yes" ] || [ "$_MOONRAKER_ASVC_CHANGED" = "yes" ]; then
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
# [include] lines reach.
# Called during uninstallation
remove_update_manager_section() {
    if [ -n "${1:-}" ]; then
        _remove_update_manager_section_from "$1"
        return
    fi
    local conf stanza_file
    conf=$(find_moonraker_conf)
    [ -n "$conf" ] || return 0
    while IFS= read -r stanza_file; do
        [ -n "$stanza_file" ] || continue
        _remove_update_manager_section_from "$stanza_file"
    done <<STANZA_FILES
$(update_manager_stanza_files "$conf")
STANZA_FILES
}

# Remove [update_manager helixscreen] and the comment block the installer
# writes above it from conf @1.
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
    # The program is passed as an argument rather than interpolated into the
    # -c string, so its $0 and $1 stay awk's and are never expanded by a shell.
    $fs sh -c 'awk "$1" "$2" > "$2.helixtmp" && mv "$2.helixtmp" "$2"' sh "$prog" "$conf"

    log_success "Removed update_manager section from $conf"
}
