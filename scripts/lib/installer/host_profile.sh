#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: host_profile
# Host capability profile: probe the firmware mod once, answer path questions
#
# Reads: HELIX_MOD_PAYLOAD (set by the payload contract: auto-detected on
#        verified mod hosts, or the --mod-payload compat alias in parsing)
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
#
# THE BOTH-PLACES RULE: a mod tree location appears in this file TWICE, in the
# probe candidate lists right here (env-overridable, marker-gated) AND in the
# hard-coded canonical literals inside host_path_is_mod_owned below (not
# overridable, marker-free). The two shapes are deliberate: the probe may be
# redirected or miss a half-uninstalled marker, while ownership of the
# namespace must not depend on either. Adding a new mod location means adding
# it to BOTH lists -- one without the other is either a path the guard does
# not recognize or a path the probe can never find.
# Blank at source time on purpose: this variable is the ONE switch that arms
# the mod-owned destruct exemption (host_mod_destruct_blocked below), so it
# must never be inherited from the environment - a stale HELIX_MOD_PAYLOAD=1
# exported by an old self-update or a user shell would silently license every
# destructive step against the mod's tree. Only two legitimate setters exist,
# both reached from an explicit command line: parse_installer_args /
# mod_payload_autodetect in main.sh (install direction) and the uninstaller
# bundle's --mod-payload parse (uninstall direction).
HELIX_MOD_PAYLOAD=""

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
# The pre-payload standalone install an AD5M Forge-X rig may still carry
# (our own installer put ad5m+forge_x at /opt/helixscreen with an
# S90helixscreen service). Empty when the rig has none; the adopt-or-warn
# offer in main.sh's mod_payload_mode_block consumes both.
# shellcheck disable=SC2034  # consumed by payload_legacy_adopt_or_warn (main.sh)
HOST_LEGACY_INSTALL_ROOT=""
# shellcheck disable=SC2034  # consumed by payload_legacy_adopt_or_warn (main.sh)
HOST_LEGACY_INIT_SCRIPT=""

host_profile_probe() {
    local cand legacy_root
    # The probe owns these answers: reset before probing so a second call (or
    # a caller that pre-set them) can never leave a stale answer behind.
    HOST_MOD_ROOT=""
    HOST_MOD_CHROOT=""
    HOST_CHROOT_STATE="none"
    HOST_SERVICE_MECHANISM="systemd"
    HOST_INSTALL_ROOT=""
    HOST_CONFIG_DIR=""
    HOST_MOONRAKER_USER_CONF=""
    HOST_PLATFORM_HOOK_KEY=""
    HOST_OWNS_COMPETING_UIS=0
    HOST_LEGACY_INSTALL_ROOT=""
    HOST_LEGACY_INIT_SCRIPT=""

    # shellcheck disable=SC2086  # word splitting is the point: a candidate path list
    for cand in ${HELIX_MOD_TREE_CANDIDATES:-/usr/data/config/mod /opt/config/mod}; do
        # Two descriptor spellings exist: upstream 1.4.2 ships .shell/common.sh
        # (a flat AD5M file - S99root sources it), while the AD5X port fork
        # renamed/extended it into .shell/platform.sh (per-board blocks).
        # Either marks a live mod tree; keying on one fork's spelling made the
        # probe blind to every upstream AD5M install.
        if [ -f "$cand/.shell/common.sh" ] || [ -f "$cand/.shell/platform.sh" ]; then
            HOST_MOD_ROOT="$cand"; break
        fi
    done
    # The chroot is the mod's Buildroot rootfs, one derivation off each
    # board's DATA_MNT in the mod's own descriptor (.shell/platform.sh):
    # /usr/data on the AD5X, /data on the AD5M. Z-Mod's chroot shares the
    # AD5X location; no /data/.mod/.zmod arm on purpose - the AD5M Z-Mod
    # population keeps its standalone flow until its shape is verified.
    # shellcheck disable=SC2086  # word splitting is the point: a candidate path list
    for cand in ${HELIX_MOD_CHROOT_CANDIDATES:-/usr/data/.mod/.forge-x /usr/data/.mod/.zmod /data/.mod/.forge-x}; do
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
    # The payload-contract answers are scoped to the mod's own shape: the
    # tree WITH its Buildroot chroot, which both Forge-X layouts carry (each
    # board's DATA_MNT). A tree without a chroot is a mod mid-install or
    # half-removed - still recognized above (flavor detection, the forgex
    # takeover paths, mod-owned guarding), but nothing verified that shape
    # can run a payload, so the contract stays available there by explicit
    # --payload-root only.
    # shellcheck disable=SC2034  # consumed by set_install_paths / stop_competing_uis /
    # shellcheck disable=SC2034  # install_platform_hooks / moonraker.conf discovery
    if [ -n "$HOST_MOD_ROOT" ] && [ -n "$HOST_MOD_CHROOT" ]; then
        HOST_SERVICE_MECHANISM="mod-managed"
        HOST_OWNS_COMPETING_UIS=1
        HOST_INSTALL_ROOT="$HOST_MOD_ROOT/.bin/helixscreen"
        # mod_data is a sibling of the mod tree on every layout: /usr/data on
        # the AD5X (Z-Mod), /opt on the AD5M (Forge-X) — derive, never pin.
        HOST_CONFIG_DIR="$(dirname "$HOST_MOD_ROOT")/mod_data/helixscreen/config"
        HOST_MOONRAKER_USER_CONF="$(dirname "$HOST_MOD_ROOT")/mod_data/user.moonraker.conf"
        # The hook key names the RIG, not the mod: the two payload layouts
        # differ (the AD5M hook's cache paths assume the host's own /data,
        # which the AD5X chroot does not have). The split follows the mod's
        # own descriptor rule - .shell/platform.sh selects its block by
        # uname, mips -> AD5X, everything else -> AD5M.
        case "$(uname -m)" in
            mips*) HOST_PLATFORM_HOOK_KEY="ad5x-forgex" ;;
            *)     HOST_PLATFORM_HOOK_KEY="ad5m-forgex" ;;
        esac
        # The legacy standalone population: before the payload contract, our
        # own installer put ad5m+forge_x rigs at /opt/helixscreen with an
        # S90helixscreen service. The payload install must see that root
        # before it can offer to adopt it (main.sh) - and an AD5X rig must
        # never answer one, since its /opt is the bind of /usr/data and a
        # /opt/helixscreen there is a data-partition path, not an install to
        # adopt. Candidates are env-overridable like the probe lists above.
        if [ "$HOST_PLATFORM_HOOK_KEY" = "ad5m-forgex" ]; then
            legacy_root="${HELIX_LEGACY_INSTALL_ROOT:-/opt/helixscreen}"
            if [ -d "$legacy_root" ]; then
                HOST_LEGACY_INSTALL_ROOT="$legacy_root"
                HOST_LEGACY_INIT_SCRIPT="${HELIX_LEGACY_INIT_SCRIPT:-/etc/init.d/S90helixscreen}"
            fi
        fi
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
        /data/.mod|/data/.mod/*)                         return 0 ;;
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

# mod_data as a sibling of the mod tree on every layout: /usr/data on the AD5X
# (Z-Mod), /opt on the AD5M (Forge-X) — the same rule host_profile_probe
# applies to HOST_CONFIG_DIR. Derived, never pinned; forgex.sh's
# forgex_mod_data() delegates here so installer state files share one path.
host_mod_data() {
    printf '%s\n' "$(dirname "${HOST_MOD_ROOT:-/opt/config/mod}")/mod_data"
}

# The mod's data mount (its descriptor's DATA_MNT): the parent of the .mod
# namespace — /usr/data on the AD5X, /data on the AD5M. The one location per
# board where a payload root outside the mod's git tree both exists and
# survives an OTA, which is why the OD1 escape-hatch example derives from
# here rather than a hard-coded AD5X path. Echoes nothing when the probe
# found no chroot (callers keep their own fallback).
host_mod_data_mount() {
    [ -n "${HOST_MOD_CHROOT:-}" ] || return 0
    printf '%s\n' "$(dirname "$(dirname "$HOST_MOD_CHROOT")")"
}

# Where the payload root of the LAST payload install is recorded, beside the
# display-mode record. An install can land outside the probed default
# (--payload-root, the OTA-durable seam); without this note a later armed
# uninstall removes the default while the real payload sits where the
# operator put it. Latest install wins — current state, not history.
host_payload_root_record() {
    printf '%s\n' "$(host_mod_data)/helixscreen_payload_root"
}

# Record the payload root this install actually used (payload contract only).
record_payload_root() {
    # mod_data exists on any host the probe recognized; mkdir -p covers a
    # half-built sandbox and costs nothing where it already stands.
    $SUDO mkdir -p "$(host_mod_data)" 2>/dev/null
    printf '%s\n' "$1" | $SUDO tee "$(host_payload_root_record)" >/dev/null 2>/dev/null \
        || log_warn "Could not record the payload root ($(host_payload_root_record))"
}

# The recorded payload root, or empty when no payload install left one. Never
# fails: callers capture its output, and a failing command substitution aborts
# them under the bundles' set -e.
read_payload_root_record() {
    [ -f "$(host_payload_root_record)" ] || return 0
    cat "$(host_payload_root_record)" 2>/dev/null || true
}

# Resolve the payload root this run's uninstall acts on: the --payload-root
# flag, else the root the install recorded in mod_data, else the probed
# default (INSTALL_DIR). ONE resolver for every uninstall entry point — the
# standalone arm and install.sh's HELIX_INSTALL_DIRS sweep. A caller that
# resolves from the probe or flag alone misses a --payload-root install: it
# removes the probed default while the real payload, and its updater clone,
# survives and the record goes stale.
#
# Echoes the resolved path (empty when nothing resolves). Returns 1 to
# REFUSE: a flag or a corrupted record can name an arbitrary existing
# directory, and that must fail loudly with the offending source named —
# never removed, never silently fallen back from; the operator re-runs with
# an explicit flag. Resolution happens once per run: the cached answer
# (HOST_PAYLOAD_ROOT) keeps the arm and the sweep on the same root.
resolve_payload_root() {
    if [ -n "${HOST_PAYLOAD_ROOT:-}" ]; then
        printf '%s\n' "$HOST_PAYLOAD_ROOT"
        return 0
    fi

    rpr_root="${MOD_PAYLOAD_ROOT:-}"
    rpr_src="the --payload-root flag"
    if [ -z "$rpr_root" ]; then
        rpr_root=$(read_payload_root_record 2>/dev/null || true)
        rpr_src="the payload-root record ($(host_payload_root_record))"
    fi

    if [ -z "$rpr_root" ]; then
        # The probed default already passed set_install_paths' own validate
        # gate in every entry point that reaches here armed.
        rpr_root="${INSTALL_DIR:-}"
    elif ! _user_dir_name_ok "$rpr_root" '*helixscreen*' 2>/dev/null; then
        # A missing gate helper fails the test too (rc 127): refusing without
        # it is fail-safe, acting without it is not.
        log_error "Refusing to uninstall the payload root named by ${rpr_src}:"
        log_error "  ${rpr_root}"
        log_error "Its last path component must contain 'helixscreen' - the same name"
        log_error "gate every install root passes. Re-run with an explicit --payload-root."
        return 1
    fi

    HOST_PAYLOAD_ROOT="$rpr_root"
    printf '%s\n' "$rpr_root"
    return 0
}

# $1=what the caller was about to do, $2=path — call before any destructive
# step. Exits 1 when the path is mod-owned and this is not a payload update.
host_refuse_mod_owned() {
    if host_mod_destruct_blocked "$2"; then
        log_error "refusing ${1} on mod-owned path: $2"
        log_error "this tree belongs to the firmware mod; the payload contract updates"
        log_error "it in place (a bare install here, or --payload-root to name a root)"
        exit 1
    fi
}
