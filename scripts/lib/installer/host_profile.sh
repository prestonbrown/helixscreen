#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: host_profile
# Host capability profile: probe the firmware mod once, answer path questions
#
# Reads: HELIX_MOD_PAYLOAD (set by --mod-payload parsing)
# Writes: HOST_* globals, HELIX_MOD_PAYLOAD (defaults it when unset)

# Source guard
[ -n "${_HELIX_HOST_PROFILE_SOURCED:-}" ] && return 0
_HELIX_HOST_PROFILE_SOURCED=1

# Host capability profile. Probes ONCE (host_profile_probe, called from main()
# before set_install_paths) and exports answers; downstream code asks these
# instead of testing vendor markers. The mod's own .shell/platform.sh is the
# source of truth for its presence — reading it survives their refactors of
# everything around it.
#
# Candidate roots are env-overridable so the BATS suite can point the probe at
# a sandbox tree instead of the real /usr/data (same convention as
# HELIX_STATE_VAR_LIB in common.sh). Production leaves them unset.
HELIX_MOD_PAYLOAD="${HELIX_MOD_PAYLOAD:-}"

HOST_MOD_ROOT=""
HOST_MOD_CHROOT=""
HOST_CHROOT_STATE="none"
HOST_SERVICE_MECHANISM="systemd"
# shellcheck disable=SC2034  # consumed by set_install_paths (install-root selection)
HOST_INSTALL_ROOT=""
# shellcheck disable=SC2034  # consumed by set_install_paths (config-dir selection)
HOST_CONFIG_DIR=""
# shellcheck disable=SC2034  # consumed by moonraker.conf discovery on mod hosts
HOST_MOONRAKER_USER_CONF=""
# shellcheck disable=SC2034  # consumed by install_platform_hooks (hook key)
HOST_PLATFORM_HOOK_KEY=""
# shellcheck disable=SC2034  # consumed by stop_competing_uis (mod owns the sweep)
HOST_OWNS_COMPETING_UIS=0

host_profile_probe() {
    local cand
    # shellcheck disable=SC2086  # word splitting is the point: a candidate path list
    for cand in ${HELIX_MOD_TREE_CANDIDATES:-/usr/data/config/mod /opt/config/mod}; do
        if [ -f "$cand/.shell/platform.sh" ]; then HOST_MOD_ROOT="$cand"; break; fi
    done
    # shellcheck disable=SC2086  # word splitting is the point: a candidate path list
    for cand in ${HELIX_MOD_CHROOT_CANDIDATES:-/usr/data/.mod/.forge-x /usr/data/.mod/.zmod}; do
        if [ -d "$cand/usr/bin" ]; then HOST_MOD_CHROOT="$cand"; break; fi
    done
    if [ -n "$HOST_MOD_CHROOT" ]; then
        # Inside the chroot, "/" IS the chroot root: same device:inode pair.
        # Both stats must succeed before the comparison counts — on a host
        # without a usable stat(1) two empty strings compare equal and would
        # report "inside", the wrong answer for the AD5X chroot guard.
        local root_id chroot_id
        root_id=$(stat -c %d:%i / 2>/dev/null) || root_id=""
        chroot_id=$(stat -c %d:%i "$HOST_MOD_CHROOT" 2>/dev/null) || chroot_id=""
        # shellcheck disable=SC2034  # consumed by the AD5X chroot-context gate
        if [ -n "$root_id" ] && [ -n "$chroot_id" ] && [ "$root_id" = "$chroot_id" ]; then
            HOST_CHROOT_STATE="inside"
        else
            HOST_CHROOT_STATE="outside:$HOST_MOD_CHROOT"
        fi
    fi
    # shellcheck disable=SC2034  # consumed by set_install_paths / stop_competing_uis /
    # shellcheck disable=SC2034  # install_platform_hooks / moonraker.conf discovery
    if [ -n "$HOST_MOD_ROOT" ]; then
        HOST_SERVICE_MECHANISM="mod-managed"
        HOST_OWNS_COMPETING_UIS=1
        HOST_INSTALL_ROOT="$HOST_MOD_ROOT/.bin/helixscreen"
        # mod_data is a sibling of the mod tree on every layout: /usr/data on
        # the AD5X (Z-Mod), /opt on the AD5M (Forge-X) — derive, never pin.
        HOST_CONFIG_DIR="$(dirname "$HOST_MOD_ROOT")/mod_data/helixscreen/config"
        HOST_MOONRAKER_USER_CONF="$(dirname "$HOST_MOD_ROOT")/mod_data/user.moonraker.conf"
        HOST_PLATFORM_HOOK_KEY="ad5x-forgex"
    fi
}

# True when path (symlinks resolved) is managed by the mod: the probed tree,
# the probed chroot, or one of the canonical mod roots. Never mv, rm, or chmod
# these outside --mod-payload's in-place contract.
host_path_is_mod_owned() {
    [ -n "$1" ] || return 1
    local p
    p=$(readlink -f "$1" 2>/dev/null) || p="$1"
    # Each probed root matches only when the probe found one — an empty
    # "$HOST_MOD_ROOT"/* pattern degenerates to /* and would claim every
    # absolute path on a host with no mod.
    if [ -n "$HOST_MOD_ROOT" ]; then
        case "$p" in
            "$HOST_MOD_ROOT"|"$HOST_MOD_ROOT"/*) return 0 ;;
        esac
    fi
    if [ -n "$HOST_MOD_CHROOT" ]; then
        case "$p" in
            "$HOST_MOD_CHROOT"|"$HOST_MOD_CHROOT"/*) return 0 ;;
        esac
    fi
    # The canonical roots are hard-coded, like HELIX_INSTALL_DIRS: those
    # namespaces are the mod's whether or not a probe found them. The probe's
    # marker (.shell/platform.sh) is refactorable, and a half-uninstall can
    # remove it while the payload still sits in the tree — recognition must
    # not depend on it.
    case "$p" in
        /usr/data/.mod|/usr/data/.mod/*)                 return 0 ;;
        /usr/data/config/mod|/usr/data/config/mod/*)     return 0 ;;
        /opt/config/mod|/opt/config/mod/*)               return 0 ;;
    esac
    return 1
}

# The one mod-payload exemption test: true when this run must NOT touch the
# path destructively — it is mod-owned and --mod-payload (the in-place update
# contract) was not given. The fatal guard and the uninstall sweeps both route
# through here so the exemption lives in exactly one place.
host_mod_destruct_blocked() {
    [ "$HELIX_MOD_PAYLOAD" != "1" ] && host_path_is_mod_owned "$1"
}

# $1=what the caller was about to do, $2=path — call before any destructive
# step. Exits 1 when the path is mod-owned and this is not a payload update.
host_refuse_mod_owned() {
    if host_mod_destruct_blocked "$2"; then
        log_error "refusing ${1} on mod-owned path: $2"
        log_error "this tree belongs to the firmware mod; --mod-payload updates it in place"
        exit 1
    fi
}
